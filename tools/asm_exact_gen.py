#!/usr/bin/env python3
"""Generate byte-exact naked-`__asm` candidate sources from DSD assembly snapshots.

For a function in a DSD gap-object assembly file (`_dsd_gap@<module>_<n>.s`, as snapshotted by the
author helpers under extracted/dqix-decomp-eu/build/scratch/*/asm) this writes

    extern "C" __attribute__((naked)) void <name>(void) { __asm { ... } }

with the original instructions. The result is byte-exact assembly, NOT a C++ decompilation: use it only
when the project owner accepts asm-exact bodies.
Conversions: `ldr rd, .L_pool` -> `ldr rd, =<value or symbol>` (MWCC re-creates the literal pool), local
code labels `.L_xxxxxxxx` -> `L_xxxxxxxx`. Thumb functions are emitted by the runner's `nofralloc` form. Anything else it cannot reproduce (data inside the function or ARM/Thumb-mixed code) raises `Unsupported` and the function is skipped.

Usage (from the repository root, `.venv/bin/python`):
    tools/asm_exact_gen.py --asm-dir DIR [--asm-dir DIR ...] --symbols FILE [...] NAME... --out DIR
Pure functions (`parse_functions`, `convert`) are unit tested in tests/test_asm_exact_gen.py.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

LABEL_RE = re.compile(r"^(\.L_[0-9a-fA-F]+):\s*(.*)$")
START_RE = re.compile(r"^\s*(arm|thumb)_func_start\s+(\S+)")
END_RE = re.compile(r"^\s*(arm|thumb)_func_end\s+(\S+)")
NAME_LABEL_RE = re.compile(r"^(\S+):\s*;\s*0x([0-9a-fA-F]+)\s*$")
WORD_RE = re.compile(r"^\.word\s+(\S+)\s*$")
LDR_POOL_RE = re.compile(r"^(ldr\w*)\s+(r\d+|sp|lr|ip|fp|sl),\s*(\.L_[0-9a-fA-F]+)\s*$")
BRANCH_RE = re.compile(r"^(b\w*)\s+(\.L_[0-9a-fA-F]+)\s*$")
BRANCH_MNEMONIC = r"(?:blx|bl|b)(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?"
RAW_BRANCH_RE = re.compile(rf"^({BRANCH_MNEMONIC})\s+#([+-]?(?:0x[0-9a-fA-F]+|\d+))$", re.I)
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class Unsupported(Exception):
    pass


@dataclass
class Function:
    name: str
    mode: str  # arm | thumb
    lines: list[str] = field(default_factory=list)
    address: int | None = None


def parse_functions(text: str) -> dict[str, Function]:
    """Split a DSD assembly file into functions (raw lines between start and end markers)."""
    found: dict[str, Function] = {}
    current: Function | None = None
    for raw in text.splitlines():
        match = START_RE.match(raw)
        if match:
            current = Function(match.group(2), match.group(1))
            continue
        if END_RE.match(raw):
            if current is not None:
                found[current.name] = current
            current = None
            continue
        if current is not None:
            label = NAME_LABEL_RE.match(raw.strip())
            if label and label.group(1) == current.name:
                current.address = int(label.group(2), 16)
            current.lines.append(raw.rstrip())
    return found


def _instruction_size(mode: str, text: str) -> int:
    if mode == "arm":
        return 4
    mnemonic = text.split(None, 1)[0].lower()
    return 4 if mnemonic in {"bl", "blx"} else 2


def convert(
    func: Function,
    strip_frame: bool = True,
    branch_symbols: dict[int, str] | None = None,
) -> tuple[list[str], set[str]]:
    """Return (asm body lines, symbol names referenced). Raises Unsupported.

    DSD prints branch immediates as byte deltas from the branch instruction address. These are
    converted to local labels or symbols so MWCC does not reinterpret them as assembler immediates.
    `strip_frame=True` drops the leading push/trailing pop for the naked form (MWCC re-creates its own frame);
    `strip_frame=False` keeps every original instruction for the `nofralloc` form."""
    thumb = func.mode == "thumb"
    if thumb and strip_frame:
        raise Unsupported("thumb function needs the nofralloc form")
    pool: dict[str, str] = {}
    code_labels: set[str] = set()
    labels_by_address: dict[int, list[str]] = {}
    items: list[tuple[str, str, int | None]] = []  # (kind, text, address)
    pc = func.address
    for raw in func.lines:
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        name_label = NAME_LABEL_RE.match(line)
        if name_label:
            if pc is None:
                pc = int(name_label.group(2), 16)
            continue
        line = line.split(";", 1)[0].rstrip()  # trailing comments (`b .L_x ; case 3`, `.L_x: ; jump table`)
        if not line:
            continue
        label = LABEL_RE.match(line)
        if label:
            pc = int(label.group(1)[3:], 16)
            rest = label.group(2).strip()
            if not rest:
                code_labels.add(label.group(1))
                labels_by_address.setdefault(pc, []).append(label.group(1))
                items.append(("label", label.group(1), pc))
                continue
            word = WORD_RE.match(rest)
            if not word:
                raise Unsupported(f"data directive in function: {rest.split()[0]}")
            pool[label.group(1)] = word.group(1)
            pc += 4
            continue
        if line.startswith("."):
            raise Unsupported(f"directive in function: {line.split()[0]}")
        if pc is None and RAW_BRANCH_RE.match(line):
            raise Unsupported("raw branch needs a function address")
        items.append(("insn", line, pc))
        if pc is not None:
            pc += _instruction_size(func.mode, line)
    if not any(kind == "insn" for kind, _, _ in items):
        raise Unsupported("no instructions")
    symbols: set[str] = set()
    used_pool: set[str] = set()
    instruction_addresses = {address for kind, _, address in items if kind == "insn" and address is not None}
    synthetic_labels: set[int] = set()
    raw_branches: dict[int, tuple[str, str]] = {}
    branch_symbols = branch_symbols or {}
    for kind, text, address in items:
        if kind != "insn":
            continue
        raw_branch = RAW_BRANCH_RE.match(text)
        if raw_branch:
            if address is None:
                raise Unsupported("raw branch needs an instruction address")
            target = (address + int(raw_branch.group(2), 0)) & 0xFFFFFFFF
            if target in instruction_addresses:
                existing = labels_by_address.get(target)
                label = existing[0][1:] if existing else f"L_{target:08x}"
                if not existing:
                    synthetic_labels.add(target)
                raw_branches[address] = (raw_branch.group(1), label)
                continue
            symbol = branch_symbols.get(target)
            if symbol is None or not IDENT_RE.match(symbol):
                raise Unsupported(f"raw branch to unknown target 0x{target:08x}")
            symbols.add(symbol)
            raw_branches[address] = (raw_branch.group(1), symbol)
    body: list[str] = []
    for kind, text, address in items:
        if kind == "label":
            body.append(f"{text[1:]}:")
            continue
        if address in synthetic_labels:
            body.append(f"L_{address:08x}:")
        if address in raw_branches:
            mnemonic, target = raw_branches[address]
            body.append(f"{mnemonic} {target}")
            continue
        ldr = LDR_POOL_RE.match(text)
        if ldr:
            if ldr.group(3) not in pool:
                raise Unsupported(f"pc-relative load of non-pool label {ldr.group(3)}")
            value = pool[ldr.group(3)]
            used_pool.add(ldr.group(3))
            base = re.split(r"[+-]", value, maxsplit=1)[0]
            if IDENT_RE.match(base) and not base.startswith("0x"):
                symbols.add(base)
            body.append(f"{ldr.group(1)} {ldr.group(2)}, ={value}")
            continue
        branch = BRANCH_RE.match(text)
        if branch:
            if branch.group(2) not in code_labels:
                target = int(branch.group(2)[3:], 16)
                symbol = branch_symbols.get(target)
                if symbol is None or not IDENT_RE.match(symbol):
                    raise Unsupported(f"branch to unknown label {branch.group(2)}")
                symbols.add(symbol)
                body.append(f"{branch.group(1)} {symbol}")
                continue
            body.append(f"{branch.group(1)} {branch.group(2)[1:]}")
            continue
        if ".L_" in text:
            raise Unsupported(f"unhandled local label reference: {text}")
        call = re.match(r"^(bl?x?\w*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$", text)
        if call and call.group(2) not in {"lr", "sp", "pc"} and not re.match(r"^r\d+$", call.group(2)):
            symbols.add(call.group(2))
        body.append(text)
    if used_pool != set(pool):
        raise Unsupported("pool words not loaded by ldr (shared or data pool)")
    if thumb:
        body = [_thumb_mnemonic(text) for text in body]
    return (_strip_frame(body) if strip_frame else body), symbols

THUMB_FLAG_OPS = {"adds", "subs", "movs", "ands", "orrs", "eors", "bics", "muls", "lsls", "lsrs", "asrs", "rors",
                  "negs", "mvns", "adcs", "sbcs"}


def _thumb_mnemonic(text: str) -> str:
    """Convert DSD's canonical Thumb spellings to MWCC's inline-assembler syntax."""
    stack_adjust = re.match(r"^(add|sub)\s+sp,\s*sp,\s*(#\S+)$", text)
    if stack_adjust:
        return f"{stack_adjust.group(1)} sp, {stack_adjust.group(2)}"
    head, _, rest = text.partition(" ")
    if head in THUMB_FLAG_OPS:
        return f"{head[:-1]} {rest}".rstrip()
    return text


PUSH_RE = re.compile(r"^stmdb\s+sp!,\s*\{[^}]*\blr\}$")
POP_RE = re.compile(r"^ldm\w*ia\s+sp!,\s*\{[^}]*\bpc\}$")


def _strip_frame(body: list[str]) -> list[str]:
    """MWCC emits its own register save/restore around a naked-`__asm` body (the saved set follows the
    registers the body uses), so drop the original leading push and trailing pop-to-pc."""
    insns = [i for i, text in enumerate(body) if not text.endswith(":")]
    if not insns or not PUSH_RE.match(body[insns[0]]):
        return body
    last = insns[-1]
    if not POP_RE.match(body[last]):
        raise Unsupported("push without a matching final pop-to-pc")
    return [text for i, text in enumerate(body) if i not in (insns[0], last)]


def render(name: str, body: list[str], symbols: set[str], kinds: dict[str, str], nofralloc: bool = False, thumb: bool = False) -> str:
    """C++ source text. `kinds` maps symbol -> 'function' | 'data' (default data for unknown names)."""
    decls = []
    for symbol in sorted(symbols - {name}):
        if kinds.get(symbol) == "function":
            decls.append(f'extern "C" void {symbol}(void);')
        else:
            decls.append(f'extern "C" unsigned {symbol}[];')
    if nofralloc and thumb:
        decls = ["#pragma thumb on"] + decls
    if nofralloc:  # MWCC asm function without compiler prologue/epilogue: every original instruction is kept
        lines = decls + [f'extern "C" asm void {name}(void) {{', "    nofralloc"]
        lines += [f"    {text}" for text in body]
        lines += ["}", ""]
        return "\n".join(lines)
    lines = decls + [f'extern "C" __attribute__((naked)) void {name}(void) {{', "    __asm {"]
    lines += [f"        {text}" for text in body]
    lines += ["    }", "}", ""]
    return "\n".join(lines)


def load_kinds(symbol_files: list[Path]) -> dict[str, str]:
    kinds: dict[str, str] = {}
    for path in symbol_files:
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1].startswith("kind:"):
                kinds[parts[0]] = "function" if parts[1].startswith("kind:function") else "data"
    return kinds


def load_symbol_addresses(symbol_files: list[Path]) -> dict[int, str]:
    """Map exact function addresses to an available assembler symbol name."""
    addresses: dict[int, str] = {}
    for path in symbol_files:
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) < 2 or not parts[1].startswith("kind:function"):
                continue
            address = re.search(r"\baddr:(0x[0-9a-fA-F]+)", line)
            if address:
                addresses.setdefault(int(address.group(1), 16), parts[0])
    return addresses


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--asm-dir", action="append", required=True, type=Path)
    parser.add_argument("--symbols", action="append", default=[], type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("names", nargs="+")
    args = parser.parse_args(argv)

    functions: dict[str, Function] = {}
    for directory in args.asm_dir:
        for path in sorted(directory.glob("*.s")):
            functions.update(parse_functions(path.read_text()))
    kinds = load_kinds(args.symbols)
    branch_symbols = load_symbol_addresses(args.symbols)
    args.out.mkdir(parents=True, exist_ok=True)
    written = skipped = 0
    for name in args.names:
        func = functions.get(name)
        if func is None:
            print(f"skip {name}: not in asm snapshot")
            skipped += 1
            continue
        try:
            body, symbols = convert(func, branch_symbols=branch_symbols)
        except Unsupported as error:
            print(f"skip {name}: {error}")
            skipped += 1
            continue
        (args.out / f"{name}.cpp").write_text(render(name, body, symbols, kinds))
        written += 1
    print(f"written={written} skipped={skipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
