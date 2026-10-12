#!/usr/bin/env python3
"""Generate MWCC nofralloc candidates while preserving literal pools and inline byte/word data.

Unlike :mod:`asm_exact_gen`, this emits data directives at their original positions. It intentionally
rejects fixed-offset branch operands: MWCC inline assembly rejects them as instructions, while DCD
encodes them as data and therefore fails the relocation-aware instruction comparison.

Usage from the repository root::
    .venv/bin/python tools/asm_exact_special.py --asm-dir DIR --symbols FILE --out DIR NAME...
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:  # package import in tests; sibling import when run as a script
    from . import asm_exact_gen as gen
except ImportError:  # pragma: no cover - exercised by CLI
    import asm_exact_gen as gen

LABEL_RE = re.compile(r"^(\.L_[0-9a-fA-F]+):\s*(.*)$")
WORD_RE = re.compile(r"^\.word\s+(.+?)\s*$")
BYTE_RE = re.compile(r"^\.byte\s+(.+?)\s*$")
FIXED_BRANCH_RE = re.compile(r"^b(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?\s+#?-?(0x[0-9a-fA-F]+|[0-9]+)\s*$", re.I)
LDR_POOL_LABEL_RE = re.compile(r"^ldr\s+(\w+),\s*(\.L_[0-9a-fA-F]+)\s*$", re.I)


def _symbol_base(operand: str) -> str | None:
    """Return an external symbol in a word expression, excluding numeric/local labels."""
    base = re.split(r"[+-]", operand.strip(), maxsplit=1)[0]
    if gen.IDENT_RE.fullmatch(base) and not base.lower().startswith("0x"):
        return base
    return None


def _pack_byte_row(text: str) -> list[str]:
    try:
        values = [int(part.strip(), 0) for part in text.split(",")]
    except ValueError as error:
        raise gen.Unsupported(f"non-constant inline byte data: {text}") from error
    if any(value < 0 or value > 255 for value in values):
        raise gen.Unsupported(f"inline byte value out of range: {text}")
    if len(values) % 4:
        raise gen.Unsupported(f"inline byte row is not word-sized; MWCC has no usable byte directive: {text}")
    words = [
        sum(values[index + offset] << (8 * offset) for offset in range(4))
        for index in range(0, len(values), 4)
    ]
    return [f"dcd 0x{word:08x}" for word in words]


def convert_special(func: gen.Function) -> tuple[list[str], set[str]]:
    """Return source-ordered nofralloc body rows and external symbol references.

    Inline byte rows are packed into little-endian words, preserving their exact bytes without
    relying on unsupported DCB syntax. Terminal word literal pools become ``ldr reg, =value``:
    MWCC emits the pool after the asm body, at the original end position. Mid-function pools and
    fixed-offset branches are rejected rather than silently changing placement or instruction/data
    classification.
    """
    if func.mode != "arm":
        raise gen.Unsupported("special inline-data form currently supports ARM functions only")

    raw_items: list[tuple[str, str]] = []
    data_labels: set[str] = set()
    local_labels: set[str] = set()
    word_labels: dict[str, str] = {}
    last_label: str | None = None
    for raw in func.lines:
        line = raw.strip()
        if not line or line.startswith(";") or gen.NAME_LABEL_RE.match(line):
            continue
        line = line.split(";", 1)[0].rstrip()
        if not line:
            continue
        label = LABEL_RE.match(line)
        if label:
            name, rest = label.group(1), label.group(2).strip()
            local_labels.add(name)
            raw_items.append(("label", name))
            last_label = name
            if not rest:
                continue
            line = rest
            word = WORD_RE.match(line)
            byte = BYTE_RE.match(line)
            if word:
                data_labels.add(name)
                word_labels[name] = word.group(1)
                raw_items.append(("word", word.group(1)))
                last_label = None
                continue
            if byte:
                data_labels.add(name)
                raw_items.append(("byte", byte.group(1)))
                last_label = None
                continue
            raise gen.Unsupported(f"unsupported inline data at {name}: {line.split()[0]}")
        if line.startswith("."):
            word = WORD_RE.match(line)
            byte = BYTE_RE.match(line)
            if word:
                raw_items.append(("word", word.group(1)))
                if last_label is not None:
                    data_labels.add(last_label)
                    word_labels[last_label] = word.group(1)
                last_label = None
                continue
            if byte:
                raw_items.append(("byte", byte.group(1)))
                if last_label is not None:
                    data_labels.add(last_label)
                last_label = None
                continue
            raise gen.Unsupported(f"unsupported directive in function: {line.split()[0]}")
        raw_items.append(("insn", line))
        last_label = None

    if not any(kind == "insn" for kind, _ in raw_items):
        raise gen.Unsupported("no instructions")

    pools_to_move: dict[str, str] = {}
    pool_word_indices: set[int] = set()
    for kind, text in raw_items:
        if kind != "insn":
            continue
        ldr = LDR_POOL_LABEL_RE.match(text)
        if not ldr or ldr.group(2) not in word_labels:
            continue
        label = ldr.group(2)
        pool_index = next(
            (
                pool_index
                for pool_index, item in enumerate(raw_items)
                if item == ("word", word_labels[label])
                and pool_index > 0
                and raw_items[pool_index - 1] == ("label", label)
            ),
            None,
        )
        if pool_index is None:
            raise gen.Unsupported(f"literal pool label {label} does not own its word")
        if any(item_kind in {"insn", "word", "byte"} for item_kind, _ in raw_items[pool_index + 1 :]):
            raise gen.Unsupported(f"literal pool {label} is not terminal; MWCC would move its position")
        pools_to_move[label] = word_labels[label]
        pool_word_indices.add(pool_index)

    body: list[str] = []
    symbols: set[str] = set()
    for item_index, (kind, text) in enumerate(raw_items):
        if kind == "label":
            if text not in pools_to_move:
                body.append(f"{text[1:]}:")
        elif kind == "word":
            if item_index in pool_word_indices:
                # The terminal pool is emitted by the assembler for its `ldr =value` use.
                continue
            body.append(f"dcd {text}")
            symbol = _symbol_base(text)
            if symbol:
                symbols.add(symbol)
        elif kind == "byte":
            body.extend(_pack_byte_row(text))
        else:
            ldr = LDR_POOL_LABEL_RE.match(text)
            if ldr and ldr.group(2) in pools_to_move:
                body.append(f"ldr {ldr.group(1)}, ={pools_to_move[ldr.group(2)]}")
                symbol = _symbol_base(pools_to_move[ldr.group(2)])
                if symbol:
                    symbols.add(symbol)
                continue
            ldr = gen.LDR_POOL_RE.match(text)
            if ldr:
                label = ldr.group(3)
                if label not in data_labels:
                    raise gen.Unsupported(f"pc-relative load of non-data label {label}")
                body.append(f"{ldr.group(1)} {ldr.group(2)}, {label[1:]}")
                continue
            branch = gen.BRANCH_RE.match(text)
            if branch:
                label = branch.group(2)
                if label.startswith(".L_") and label not in local_labels:
                    body.append(f"{branch.group(1)} {label[1:]}")
                    symbols.add(label[1:])
                    continue
                if label not in local_labels or label in data_labels:
                    raise gen.Unsupported(f"branch to non-code label {label}")
                body.append(f"{branch.group(1)} {label[1:]}")
                continue
            if FIXED_BRANCH_RE.match(text):
                raise gen.Unsupported(
                    f"fixed-offset branch {text!r}: MWCC requires a label; DCD emits data, not an instruction"
                )
            if ".L_" in text:
                raise gen.Unsupported(f"unhandled local label reference: {text}")
            call = re.match(r"^(bl?x?\w*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$", text)
            if call and call.group(2) not in {"lr", "sp", "pc"} and not re.fullmatch(r"r\d+", call.group(2)):
                symbols.add(call.group(2))
            body.append(text)

    return body, symbols


def render_special(name: str, func: gen.Function, body: list[str], symbols: set[str], kinds: dict[str, str]) -> str:
    """Render with explicit original prologue/epilogue and no compiler frame synthesis."""
    for symbol in symbols:
        if re.fullmatch(r"L_[0-9a-fA-F]+", symbol):
            kinds[symbol] = "function"
    return gen.render(name, body, symbols, kinds, nofralloc=True, thumb=False)


def generate(text: str, name: str, kinds: dict[str, str]) -> str:
    """Generate one named function from an assembly snapshot."""
    func = gen.parse_functions(text).get(name)
    if func is None:
        raise gen.Unsupported("not in assembly snapshot")
    body, symbols = convert_special(func)
    return render_special(name, func, body, symbols, kinds)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--asm-dir", action="append", required=True, type=Path)
    parser.add_argument("--symbols", action="append", default=[], type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("names", nargs="+")
    args = parser.parse_args(argv)

    functions: dict[str, gen.Function] = {}
    for directory in args.asm_dir:
        for path in sorted(directory.glob("*.s")):
            functions.update(gen.parse_functions(path.read_text()))
    kinds = gen.load_kinds(args.symbols)
    args.out.mkdir(parents=True, exist_ok=True)
    written = skipped = 0
    for name in args.names:
        func = functions.get(name)
        if func is None:
            print(f"skip {name}: not in asm snapshot")
            skipped += 1
            continue
        try:
            body, symbols = convert_special(func)
        except gen.Unsupported as error:
            print(f"skip {name}: {error}")
            skipped += 1
            continue
        source = render_special(name, func, body, symbols, kinds)
        (args.out / f"{name}.cpp").write_text(source)
        written += 1
    print(f"written={written} skipped={skipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
