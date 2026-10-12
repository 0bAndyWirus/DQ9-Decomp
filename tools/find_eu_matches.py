#!/usr/bin/env python3
"""Find compiled ARM ELF functions in raw module images, masking relocations."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


ELF_HEADER = struct.Struct("<HHIIIIIHHHHHH")
SECTION_HEADER = struct.Struct("<IIIIIIIIII")
SYMBOL = struct.Struct("<IIIBBH")
REL = struct.Struct("<II")
RELA = struct.Struct("<IIi")
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
STT_FUNC = 2


@dataclass(frozen=True)
class Section:
    index: int
    name: str
    kind: int
    address: int
    offset: int
    size: int
    link: int
    info: int
    entry_size: int
    data: bytes


@dataclass(frozen=True)
class Function:
    name: str
    section: Section
    offset: int
    size: int


@dataclass(frozen=True)
class Module:
    name: str
    base: int
    path: Path
    data: bytes


def _cstring(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        return ""
    end = data.find(b"\0", offset)
    if end < 0:
        end = len(data)
    return data[offset:end].decode("utf-8", errors="replace")


def _slice(data: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"{label} is outside the ELF file")
    return data[offset : offset + size]


def parse_elf_object(path: Path) -> tuple[list[Section], list[Function], dict[int, set[int]]]:
    """Read ELF32 little-endian ARM symbols and relocation sites by target section."""
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise ValueError(f"{path}: expected ELF32 little-endian input")

    (elf_type, machine, _version, _entry, _phoff, shoff, _flags, _ehsize,
     _phentsize, _phnum, shentsize, shnum, shstrndx) = ELF_HEADER.unpack_from(data, 16)
    if elf_type != 1 or machine != 40:
        raise ValueError(f"{path}: expected a relocatable ARM ELF object")
    if shentsize < SECTION_HEADER.size or shstrndx >= shnum:
        raise ValueError(f"{path}: invalid ELF section table")

    raw_sections = []
    for index in range(shnum):
        start = shoff + index * shentsize
        values = SECTION_HEADER.unpack_from(_slice(data, start, SECTION_HEADER.size, "section header"))
        raw_sections.append(values)
    strings_header = raw_sections[shstrndx]
    section_names = _slice(data, strings_header[4], strings_header[5], "section-name table")

    sections = []
    for index, values in enumerate(raw_sections):
        (name_offset, kind, _flags, address, offset, size, link, info,
         _alignment, entry_size) = values
        content = b"" if kind == 8 else _slice(data, offset, size, f"section {index}")
        sections.append(Section(index, _cstring(section_names, name_offset), kind,
                                address, offset, size, link, info, entry_size, content))

    by_index = {section.index: section for section in sections}
    relocation_sites: dict[int, set[int]] = {}
    symbols_by_table: dict[int, list[tuple[str, int, int, int, int]]] = {}
    for section in sections:
        if section.kind == SHT_SYMTAB:
            if section.link not in by_index:
                raise ValueError(f"{path}: symbol table {section.name} has an invalid string table")
            strings = by_index[section.link].data
            stride = section.entry_size or SYMBOL.size
            if stride < SYMBOL.size or section.size % stride:
                raise ValueError(f"{path}: malformed symbol table {section.name}")
            symbols = []
            for offset in range(0, section.size, stride):
                name_offset, value, size, info, _other, section_index = SYMBOL.unpack_from(section.data, offset)
                symbols.append((_cstring(strings, name_offset), value, size, info, section_index))
            symbols_by_table[section.index] = symbols
        elif section.kind in (SHT_REL, SHT_RELA):
            if section.info >= len(sections):
                raise ValueError(f"{path}: relocation section {section.name} has an invalid target")
            stride = section.entry_size or (RELA.size if section.kind == SHT_RELA else REL.size)
            entry = RELA if section.kind == SHT_RELA else REL
            if stride < entry.size or section.size % stride:
                raise ValueError(f"{path}: malformed relocation section {section.name}")
            sites = relocation_sites.setdefault(section.info, set())
            for offset in range(0, section.size, stride):
                reloc_offset, reloc_info = entry.unpack_from(section.data, offset)[:2]
                reloc_type = reloc_info & 0xFF
                if reloc_type != 0:
                    sites.add(reloc_offset)

    functions = []
    seen = set()
    for symbols in symbols_by_table.values():
        for name, value, size, info, section_index in symbols:
            if info & 0xF != STT_FUNC or size == 0 or section_index not in by_index:
                continue
            section = by_index[section_index]
            start = value & ~1
            if section.kind == 8 or start + size > section.size:
                raise ValueError(f"{path}: function {name!r} extends outside {section.name}")
            key = (name, section_index, start, size)
            if key not in seen:
                functions.append(Function(name or f"<anonymous@{start:#x}>", section, start, size))
                seen.add(key)
    functions.sort(key=lambda function: (function.section.index, function.offset, function.name))
    return sections, functions, relocation_sites


def _masked_offsets(relocations: set[int], start: int, size: int) -> set[int]:
    """Conservatively mask the relocated word; clip at a function/section boundary."""
    masked = set()
    for relocation in relocations:
        relative = relocation - start
        if 0 <= relative < size:
            masked.update(range(relative, min(relative + 4, size)))
    return masked


def find_pattern_matches(haystack: bytes, pattern: bytes, masked: set[int] | None = None) -> list[int]:
    """Return every start offset matching all non-relocated bytes (overlaps included)."""
    masked = masked or set()
    if not pattern or len(pattern) > len(haystack):
        return []
    runs = []
    index = 0
    while index < len(pattern):
        if index in masked:
            index += 1
            continue
        end = index + 1
        while end < len(pattern) and end not in masked:
            end += 1
        runs.append((index, pattern[index:end]))
        index = end

    if not runs:
        return list(range(len(haystack) - len(pattern) + 1))

    anchor_offset, anchor = max(runs, key=lambda run: len(run[1]))
    matches = []
    candidate = haystack.find(anchor)
    last_start = len(haystack) - len(pattern)
    while candidate != -1:
        start = candidate - anchor_offset
        if 0 <= start <= last_start and all(
            haystack[start + offset : start + offset + len(run)] == run
            for offset, run in runs
        ):
            matches.append(start)
        candidate = haystack.find(anchor, candidate + 1)
    return matches


def scan_object(object_path: Path, modules: Sequence[Module]) -> dict[str, object]:
    sections, functions, relocations = parse_elf_object(object_path)
    text_sections = [section for section in sections if section.name == ".text" or section.name.startswith(".text.")]

    def hits_for(section: Section, start: int, size: int) -> list[dict[str, object]]:
        pattern = section.data[start : start + size]
        masked = _masked_offsets(relocations.get(section.index, set()), start, size)
        hits = []
        for module in modules:
            for offset in find_pattern_matches(module.data, pattern, masked):
                hits.append({"module": module.name, "offset": offset, "address": module.base + offset})
        return hits

    section_results = []
    for section in text_sections:
        hits = hits_for(section, 0, section.size)
        section_results.append({
            "name": section.name,
            "size": section.size,
            "relocations": len(relocations.get(section.index, set())),
            "hits": hits,
            "unique": len(hits) == 1,
        })

    function_results = []
    for function in functions:
        masked = _masked_offsets(relocations.get(function.section.index, set()),
                                 function.offset, function.size)
        hits = hits_for(function.section, function.offset, function.size)
        function_results.append({
            "name": function.name,
            "section": function.section.name,
            "offset": function.offset,
            "size": function.size,
            "relocated_bytes_masked": len(masked),
            "hits": hits,
            "unique": len(hits) == 1,
        })

    text_section = text_sections[0] if len(text_sections) == 1 else None
    section_match = section_results[0] if len(section_results) == 1 else None
    all_functions_unique = bool(function_results) and all(
        function["unique"] for function in function_results
    )
    contiguous_match = False
    matched_module = None
    if text_section is not None and section_match is not None and section_match["unique"] and all_functions_unique:
        section_hit = section_match["hits"][0]
        matched_module = section_hit["module"]
        contiguous_match = all(
            function["section"] == text_section.name
            and function["hits"][0]["module"] == matched_module
            and function["hits"][0]["address"] == section_hit["address"] + function["offset"]
            for function in function_results
        )

    return {
        "object": str(object_path),
        "sections": section_results,
        "functions": function_results,
        "summary": {
            "function_count": len(function_results),
            "uniquely_matched_functions": sum(item["unique"] for item in function_results),
            "uniquely_matched_function_bytes": sum(
                item["size"] for item in function_results if item["unique"]
            ),
            "contiguous_sections": sum(item["unique"] for item in section_results),
            "all_functions_unique": all_functions_unique,
            "contiguous_text_match": contiguous_match,
            "module": matched_module if contiguous_match else None,
        },
    }


def _module(value: list[str]) -> Module:
    name, base, path = value
    file_path = Path(path)
    return Module(name, int(base, 0), file_path, file_path.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Match functions from an ELF32 ARM object against raw EU module images."
    )
    parser.add_argument("object", type=Path, help="compiled USA ELF object")
    parser.add_argument(
        "--module", action="append", nargs=3, metavar=("NAME", "BASE", "PATH"), required=True,
        help="EU raw module image and its load address; repeat for every module to search",
    )
    args = parser.parse_args()
    try:
        modules = [_module(value) for value in args.module]
        result = scan_object(args.object, modules)
    except (OSError, ValueError, struct.error) as error:
        print(f"find_eu_matches.py: {error}", file=sys.stderr)
        return 2
    json.dump(result, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
