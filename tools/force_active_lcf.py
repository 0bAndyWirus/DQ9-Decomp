#!/usr/bin/env python3
"""Write a copy of an LCF with a FORCE_ACTIVE block listing the given symbols.

mwldarm.exe aborts with an empty message ("Errors caused tool to abort.") when the argument of the command-line
option `-force_active` is longer than about 256 characters (13 names/247 chars link, 14 short names/249 chars link,
13 names/288 chars abort). Every newly integrated function that nothing references adds a name to that list, so the
list outgrew the limit. A FORCE_ACTIVE block in the linker command file has no such limit (787 characters tested)
and gives a byte-identical link output.

usage: force_active_lcf.py IN.lcf NAMES.txt OUT.lcf     (NAMES.txt: one symbol per line)
"""

import os
import re
import sys
from pathlib import Path

NAME = re.compile(r"[A-Za-z0-9_@.$<>]+\Z")


def insert_force_active(lcf_text: str, names: list[str]) -> str:
    if re.search(r"^\s*FORCE_ACTIVE\s*\{", lcf_text, re.MULTILINE):
        raise ValueError("the LCF already has a FORCE_ACTIVE block")
    bad = [name for name in names if not NAME.match(name)]
    if bad:
        raise ValueError(f"invalid symbol name(s): {bad[:3]}")
    match = re.search(r"^SECTIONS\s*\{", lcf_text, re.MULTILINE)
    if match is None:
        raise ValueError("the LCF has no SECTIONS block")
    if not names:
        return lcf_text
    block = "FORCE_ACTIVE {\n" + ",\n".join(f"    {name}" for name in names) + "\n}\n\n"
    return lcf_text[:match.start()] + block + lcf_text[match.start():]


def gap_first_symbols(text: tuple[int, int], unit_ranges: list[tuple[int, int]],
                     symbols: list[tuple[str, int]]) -> set[str]:
    """Name one externally visible symbol in every `.text` gap.

    DSD turns each gap into one `_dsd_gap` object whose `.text` is a single linker section, and mwld dead-strips
    it when nothing references any symbol in it. Delinking a unit can leave code or data with no function symbol;
    callers must add a global label anchor for those symbol-less remainders. Forcing the lowest symbol in every
    gap keeps the complete section live, including any unlabelled bytes around that symbol.
    """
    start, end = text
    gaps = []
    cursor = start
    for low, high in sorted(unit_ranges):
        if low > cursor:
            gaps.append((cursor, low))
        cursor = max(cursor, high)
    if cursor < end:
        gaps.append((cursor, end))
    names = set()
    for low, high in gaps:
        inside = [(address & ~1, name) for name, address in symbols if low <= (address & ~1) < high]
        if inside:
            names.add(min(inside)[1])
    return names


def gap_text_active_symbols(delinks_path: Path) -> set[str]:
    """Find the first linkable symbol in each gap described by one module's config files."""
    text = None
    unit_ranges = []
    in_header, complete = True, False
    for line in delinks_path.read_text(encoding="utf-8").splitlines():
        if line and not line[0].isspace():
            in_header, complete = False, False
            continue
        fields = line.split()
        if len(fields) >= 3 and fields[0] == ".text" and fields[1].startswith("start:"):
            span = (int(fields[1].split(":", 1)[1], 0), int(fields[2].split(":", 1)[1], 0))
            if in_header:
                text = span
            elif complete:
                unit_ranges.append(span)
        elif line.strip() == "complete":
            complete = True
    symbols_path = delinks_path.with_name("symbols.txt")
    if text is None or not symbols_path.is_file():
        return set()
    symbols = []
    for line in symbols_path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 3 or not fields[2].startswith("addr:"):
            continue
        kind, name = fields[1], fields[0]
        function = kind.startswith("kind:function(")
        exported_nonfunction = (
            kind.startswith(("kind:data", "kind:bss", "kind:label"))
            and "local" not in fields[3:]
            and not name.startswith(".L_")
        )
        if function or exported_nonfunction:
            symbols.append((name, int(fields[2].split(":", 1)[1], 0)))
    return gap_first_symbols(text, unit_ranges, symbols)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    source, names_file, output = (Path(arg) for arg in argv[1:])
    try:
        names = sorted({line.strip() for line in names_file.read_text().splitlines() if line.strip()})
        text = insert_force_active(source.read_text(), names)
    except (OSError, ValueError) as error:
        print(f"force_active_lcf.py: {error}", file=sys.stderr)
        return 1
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(text)
    os.replace(temporary, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
