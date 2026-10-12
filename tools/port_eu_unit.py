#!/usr/bin/env python3
"""Plan, and optionally apply, the DSD config edits that let a compiled unit link as `complete`.

Given an MWCC ELF object and a DSD project, the tool places every allocatable section of
the object in the target module images (relocation-masked byte search for code, then the
module relocation table for data/bss), derives the target address of every symbol the
object defines or references, and diffs that against the module symbols.txt files.  The
result is the symbols.txt renames/additions/removals and the delinks.txt `complete` block
that `dsd lcf` + `mwldarm` need to link the object in place of the delinked bytes.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Collection, Iterable, Sequence

import yaml

ELF_HEADER = struct.Struct("<HHIIIIIHHHHHH")
SECTION_HEADER = struct.Struct("<IIIIIIIIII")
SYMBOL = struct.Struct("<IIIBBH")
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3
STT_FILE = 4
STB_LOCAL = 0
STB_GLOBAL = 1

R_ARM_PC24 = 1
R_ARM_ABS32 = 2
R_ARM_THM_CALL = 10
R_ARM_CALL = 28
R_ARM_JUMP24 = 29
ARM_BRANCHES = {R_ARM_PC24, R_ARM_CALL, R_ARM_JUMP24}
THUMB_BRANCHES = {R_ARM_THM_CALL}
CALL_KINDS = {"arm_call", "arm_call_thumb", "thumb_call", "thumb_call_arm", "arm_branch", "thumb_branch"}

AUTO_NAME = re.compile(r"^(?:func|data|bss)_(?:ov\d{3}_)?[0-9a-f]{8}$")
SYMBOL_LINE = re.compile(r"^(?P<name>\S+)\s+kind:(?P<kind>\S+)\s+addr:(?P<addr>0x[0-9a-fA-F]+)(?P<extra>(?:\s+\S+)*)\s*$")
GAP_ANCHOR_PREFIX = "__dsd_gap_anchor_"
RELOC_LINE = re.compile(
    r"^from:(?P<src>0x[0-9a-fA-F]+)\s+kind:(?P<kind>\S+)\s+to:(?P<dst>0x[0-9a-fA-F]+)"
    r"(?:\s+add:(?P<add>0x[0-9a-fA-F]+))?\s+module:(?P<module>\S+)\s*$")
SECTION_LINE = re.compile(r"^\s*(?P<name>\.\S+)\s+start:(?P<start>0x[0-9a-fA-F]+)\s+end:(?P<end>0x[0-9a-fA-F]+)(?P<extra>.*)$")


class PortError(Exception):
    """Raised when the object cannot be mapped onto the target modules."""


# --------------------------------------------------------------------------- ELF objects


@dataclass(frozen=True)
class ObjSection:
    index: int
    name: str
    kind: int
    flags: int
    size: int
    align: int
    data: bytes


@dataclass(frozen=True)
class ObjSymbol:
    index: int
    name: str
    value: int
    size: int
    type: int
    bind: int
    shndx: int


@dataclass(frozen=True)
class ObjReloc:
    offset: int
    type: int
    symbol: ObjSymbol
    addend: int


@dataclass
class ObjectFile:
    path: Path
    sections: list[ObjSection]
    symbols: list[ObjSymbol]
    relocs: dict[int, list[ObjReloc]]

    def alloc_sections(self) -> list[ObjSection]:
        return [s for s in self.sections if s.flags & SHF_ALLOC and s.size > 0]


def _cstring(blob: bytes, offset: int) -> str:
    end = blob.find(b"\0", offset)
    return blob[offset:end if end >= 0 else len(blob)].decode("utf-8", errors="replace")


def parse_object(path: Path) -> ObjectFile:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise PortError(f"{path}: expected an ELF32 little-endian object")
    (_type, machine, _version, _entry, _phoff, shoff, _flags, _ehsize, _phentsize, _phnum,
     shentsize, shnum, shstrndx) = ELF_HEADER.unpack_from(data, 16)
    if machine != 40:
        raise PortError(f"{path}: not an ARM object")
    headers = [SECTION_HEADER.unpack_from(data, shoff + i * shentsize) for i in range(shnum)]
    shstr = headers[shstrndx]
    names = data[shstr[4]:shstr[4] + shstr[5]]
    sections = []
    for index, (name, kind, flags, _addr, offset, size, _link, _info, align, _entsize) in enumerate(headers):
        content = b"" if kind == SHT_NOBITS else data[offset:offset + size]
        sections.append(ObjSection(index, _cstring(names, name), kind, flags, size, max(align, 1), content))

    symbols: list[ObjSymbol] = []
    symtab_index = None
    for index, header in enumerate(headers):
        if header[1] != SHT_SYMTAB:
            continue
        symtab_index = index
        strings = sections[header[6]].data
        stride = header[9] or SYMBOL.size
        for number, offset in enumerate(range(0, header[5], stride)):
            name, value, size, info, _other, shndx = SYMBOL.unpack_from(sections[index].data, offset)
            symbols.append(ObjSymbol(number, _cstring(strings, name), value, size, info & 0xF, info >> 4, shndx))
        break
    if symtab_index is None:
        raise PortError(f"{path}: no symbol table")

    relocs: dict[int, list[ObjReloc]] = {}
    for index, header in enumerate(headers):
        kind, target, stride = header[1], header[7], header[9]
        if kind not in (SHT_REL, SHT_RELA) or header[6] != symtab_index:
            continue
        target_section = sections[target]
        if not target_section.flags & SHF_ALLOC:
            continue
        stride = stride or (12 if kind == SHT_RELA else 8)
        entries = relocs.setdefault(target, [])
        for offset in range(0, header[5], stride):
            if kind == SHT_RELA:
                r_offset, r_info, addend = struct.unpack_from("<IIi", sections[index].data, offset)
            else:
                r_offset, r_info = struct.unpack_from("<II", sections[index].data, offset)
                addend = _implicit_addend(target_section.data, r_offset, r_info & 0xFF)
            if r_info & 0xFF == 0:
                continue
            entries.append(ObjReloc(r_offset, r_info & 0xFF, symbols[r_info >> 8], addend))
    return ObjectFile(path, sections, symbols, relocs)


def _implicit_addend(content: bytes, offset: int, reloc_type: int) -> int:
    if reloc_type == R_ARM_ABS32:
        return struct.unpack_from("<i", content, offset)[0]
    if reloc_type in ARM_BRANCHES:
        imm = struct.unpack_from("<I", content, offset)[0] & 0xFFFFFF
        return ((imm ^ 0x800000) - 0x800000) << 2
    return 0


def symbol_target_adjust(reloc: ObjReloc) -> int:
    """Return the value added to the symbol address to obtain the relocation's target."""
    if reloc.type in ARM_BRANCHES:
        return reloc.addend + 8
    if reloc.type in THUMB_BRANCHES:
        return reloc.addend + 4
    return reloc.addend


# --------------------------------------------------------------------------- DSD project


@dataclass
class SymbolEntry:
    line: int
    name: str
    kind: str
    address: int
    extra: list[str]

    @property
    def category(self) -> str:
        if self.kind.startswith("function"):
            return "function"
        if self.kind.startswith("bss"):
            return "bss"
        if self.kind.startswith("data"):
            return "data"
        if self.kind.startswith("label"):
            return "label"
        return "other"

    @property
    def local(self) -> bool:
        return "local" in self.extra

    @property
    def size(self) -> int | None:
        match = re.search(r"size=(0x[0-9a-fA-F]+)", self.kind)
        return int(match.group(1), 16) if match else None


@dataclass(frozen=True)
class TargetReloc:
    source: int
    kind: str
    target: int
    module: str
    addend: int
    line: int

    @property
    def effective_target(self) -> int:
        return self.target + self.addend


@dataclass(frozen=True)
class RelocationRebase:
    source_module: str
    relocation: TargetReloc
    target: int
    addend: int
    symbol: str


@dataclass(frozen=True)
class RelocationAddition:
    source_module: str
    source: int
    kind: str
    target: int
    target_module: str
    addend: int = 0


@dataclass
class TargetModule:
    name: str
    data: bytes
    delinks_path: Path
    symbols_path: Path
    relocs_path: Path
    sections: dict[str, tuple[int, int]]
    symbols: list[SymbolEntry]
    relocs: dict[int, TargetReloc]
    base: int = 0
    by_address: dict[int, list[SymbolEntry]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.base = min(start for start, _end in self.sections.values())
        self.index_symbols()

    def index_symbols(self) -> None:
        self.by_address = {}
        for entry in self.symbols:
            self.by_address.setdefault(entry.address, []).append(entry)

    def section_of(self, address: int) -> str | None:
        for name, (start, end) in self.sections.items():
            if start <= address < end:
                return name
        return None


def parse_symbols(path: Path) -> list[SymbolEntry]:
    entries = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines()):
        if not raw.strip():
            continue
        match = SYMBOL_LINE.match(raw.strip())
        if match is None:
            raise PortError(f"{path}:{number + 1}: malformed symbol line")
        entries.append(SymbolEntry(number, match["name"], match["kind"], int(match["addr"], 16),
                                   match["extra"].split()))
    return entries


def parse_relocs(path: Path) -> dict[int, TargetReloc]:
    relocs = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines()):
        match = RELOC_LINE.match(raw.strip())
        if match:
            source = int(match["src"], 16)
            relocs[source] = TargetReloc(
                source=source,
                kind=match["kind"],
                target=int(match["dst"], 16),
                module=match["module"],
                addend=int(match["add"], 16) if match["add"] else 0,
                line=number,
            )
    return relocs


def parse_delinks_header(path: Path) -> dict[str, tuple[int, int]]:
    sections: dict[str, tuple[int, int]] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.strip() and not raw[0].isspace():
            break
        match = SECTION_LINE.match(raw)
        if match:
            sections[match["name"]] = (int(match["start"], 16), int(match["end"], 16))
    return sections


def pristine_image(project: Path, version: str, name: str,
                   keep_configured_image: Collection[str] = ()) -> Path | None:
    """Return an extracted module image unless the caller requests its configured image."""
    if name in keep_configured_image:
        return None
    root = project / "extract" / version
    if name == "main":
        path = root / "arm9" / "arm9.bin"
    elif name in ("itcm", "dtcm"):
        path = root / "arm9" / f"{name}.bin"
    else:
        path = root / "arm9_overlays" / f"{name}.bin"
    return path if path.is_file() else None


def load_project(project: Path, version: str,
                 keep_configured_image: Collection[str] = ()) -> dict[str, TargetModule]:
    """Load module metadata and images for placement.

    Prefer extracted images over potentially stale linked build outputs. Modules named
    in ``keep_configured_image`` use their configured ``object`` instead. If no extracted
    image exists, the configured object is used regardless of that collection.
    """
    config_path = project / "config" / version / "arm9" / "config.yaml"
    config = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    base_dir = config_path.parent
    entries = [config["main_module"], *config.get("autoloads", []), *config.get("overlays", [])]
    modules = {}
    for entry in entries:
        name = entry["name"]
        delinks = base_dir / entry["delinks"]
        image = pristine_image(project, version, name, keep_configured_image) or (base_dir / entry["object"]).resolve()
        modules[name] = TargetModule(
            name=name,
            data=image.read_bytes(),
            delinks_path=delinks,
            symbols_path=base_dir / entry["symbols"],
            relocs_path=base_dir / entry["relocations"],
            sections=parse_delinks_header(delinks),
            symbols=parse_symbols(base_dir / entry["symbols"]),
            relocs=parse_relocs(base_dir / entry["relocations"]),
        )
    return modules


def module_from_ref(ref: str) -> str | None:
    if ref in ("main", "itcm", "dtcm"):
        return ref
    match = re.fullmatch(r"overlay\((\d+)\)", ref)
    return f"ov{int(match.group(1)):03d}" if match else None


# --------------------------------------------------------------------------- placement


def find_masked(haystack: bytes, pattern: bytes, masked: set[int], start: int, end: int) -> list[int]:
    """Offsets in haystack[start:end] where every unmasked pattern byte matches."""
    runs = []
    index = 0
    while index < len(pattern):
        if index in masked:
            index += 1
            continue
        stop = index
        while stop < len(pattern) and stop not in masked:
            stop += 1
        runs.append((index, pattern[index:stop]))
        index = stop
    if not runs:
        return []
    anchor_offset, anchor = max(runs, key=lambda run: len(run[1]))
    hits = []
    position = haystack.find(anchor, start + anchor_offset, end)
    while position != -1:
        begin = position - anchor_offset
        if start <= begin and begin + len(pattern) <= end and all(
                haystack[begin + offset:begin + offset + len(run)] == run for offset, run in runs):
            hits.append(begin)
        position = haystack.find(anchor, position + 1, end)
    return hits


@dataclass
class Group:
    name: str
    members: list[tuple[ObjSection, int]]
    size: int
    module: str | None = None
    address: int | None = None
    method: str = ""
    stripped: list[ObjSection] = field(default_factory=list)


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def build_groups(obj: ObjectFile) -> dict[str, Group]:
    groups: dict[str, Group] = {}
    for section in obj.alloc_sections():
        group = groups.setdefault(section.name, Group(section.name, [], 0))
        offset = _align(group.size, section.align)
        group.members.append((section, offset))
        group.size = offset + section.size
    return groups


def section_pattern(obj: ObjectFile, section: ObjSection) -> tuple[bytes, set[int]]:
    masked: set[int] = set()
    for reloc in obj.relocs.get(section.index, []):
        masked.update(range(reloc.offset, min(reloc.offset + 4, section.size)))
    return section.data, masked


def _matches_at(module: TargetModule, address: int, pattern: bytes, masked: set[int]) -> bool:
    offset = address - module.base
    if offset < 0 or offset + len(pattern) > len(module.data):
        return False
    window = module.data[offset:offset + len(pattern)]
    return all(window[i] == pattern[i] for i in range(len(pattern)) if i not in masked)


def place_chain(obj: ObjectFile, group: Group, candidates: Sequence[TargetModule],
                start: tuple[str, int] | None = None) -> None:
    """Place a code group allowing linker-stripped members (dead or duplicate weak code).

    Every kept member must follow the previous kept member at the next aligned address;
    the chain covering the most bytes wins and ties between different locations fail.
    """
    members = [section for section, _offset in group.members]
    patterns = [section_pattern(obj, section) for section in members]

    def region(module: TargetModule) -> tuple[int, int] | None:
        if group.name not in module.sections:
            return None
        lo, hi = module.sections[group.name]
        return lo, min(hi, module.base + len(module.data))

    def extend(module: TargetModule, anchor: int, address: int) -> list[tuple[int, int]]:
        kept = [(anchor, address)]
        cursor = address + members[anchor].size
        for index in range(anchor + 1, len(members)):
            candidate = _align(cursor, members[index].align)
            if _matches_at(module, candidate, *patterns[index]):
                kept.append((index, candidate))
                cursor = candidate + members[index].size
        first = address
        for index in range(anchor - 1, -1, -1):
            candidate = first - members[index].size
            if candidate % members[index].align == 0 and _matches_at(module, candidate, *patterns[index]):
                kept.insert(0, (index, candidate))
                first = candidate
        return kept

    anchors: list[tuple[TargetModule, int, int]] = []
    if start is not None:
        module = next(m for m in candidates if m.name == start[0])
        for index, (pattern, masked) in enumerate(patterns):
            if _matches_at(module, start[1], pattern, masked):
                anchors.append((module, index, start[1]))
                break
    else:
        best_hits: list[tuple[TargetModule, int, int]] | None = None
        for index in sorted(range(len(members)), key=lambda i: -members[i].size):
            pattern, masked = patterns[index]
            hits = []
            for module in candidates:
                bounds = region(module)
                if bounds is None:
                    continue
                for offset in find_masked(module.data, pattern, masked, bounds[0] - module.base,
                                          bounds[1] - module.base):
                    hits.append((module, index, module.base + offset))
            if hits and (best_hits is None or len(hits) < len(best_hits)):
                best_hits = hits
            if best_hits is not None and len(best_hits) == 1:
                break
        anchors = best_hits or []
    if not anchors:
        raise PortError(f"{group.name}: no member matches the target modules")

    chains = []
    for module, index, address in anchors:
        kept = extend(module, index, address)
        chains.append((sum(members[i].size for i, _ in kept), module, kept))
    best = max(score for score, _module, _kept in chains)
    winners = {(module.name, kept[0][1]): (module, kept) for score, module, kept in chains if score == best}
    if len(winners) != 1:
        shown = ", ".join(f"{name}@{address:#010x}" for name, address in list(winners)[:8])
        raise PortError(f"{group.name} placement is ambiguous between {shown}")
    module, kept = next(iter(winners.values()))
    kept = _prefer_referenced_twins(obj, module, members, patterns, kept)
    base = kept[0][1]
    kept_indices = {index for index, _address in kept}
    group.members = [(members[index], address - base) for index, address in kept]
    group.stripped = [section for index, section in enumerate(members) if index not in kept_indices]
    group.size = kept[-1][1] + members[kept[-1][0]].size - base
    group.module, group.address, group.method = module.name, base, "bytes"


def _prefer_referenced_twins(obj: ObjectFile, module: TargetModule, members: list[ObjSection],
                             patterns: list[tuple[bytes, set[int]]],
                             kept: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Swap a kept member for a stripped byte-identical twin (C1/C2, D1/D2 constructor
    aliases) when only the twin is referenced by the object or named at that address."""
    referenced = {reloc.symbol.shndx for relocs in obj.relocs.values() for reloc in relocs}

    def score(index: int, address: int) -> tuple[bool, bool]:
        names = {s.name for s in obj.symbols if s.shndx == members[index].index and s.name}
        named_here = {entry.name for entry in module.by_address.get(address, [])}
        return members[index].index in referenced, bool(names & named_here)

    def twin_key(index: int) -> tuple[bytes, tuple[tuple[int, int, str], ...]]:
        relocs = tuple(sorted((r.offset, r.type, r.symbol.name) for r in obj.relocs.get(members[index].index, [])))
        return patterns[index][0], relocs

    kept_indices = {index for index, _address in kept}
    result = []
    for index, address in kept:
        twins = [other for other in range(len(members))
                 if other not in kept_indices and twin_key(other) == twin_key(index)]
        best = max([index, *twins], key=lambda candidate: score(candidate, address))
        if best != index:
            kept_indices.discard(index)
            kept_indices.add(best)
        result.append((best, address))
    order = sorted(result, key=lambda item: item[0])
    if [address for _index, address in order] != sorted(address for _index, address in result):
        return kept
    return order


def group_pattern(obj: ObjectFile, group: Group) -> tuple[bytes, set[int]]:
    pattern = bytearray(group.size)
    masked = set(range(group.size))
    for section, offset in group.members:
        pattern[offset:offset + section.size] = section.data
        masked.difference_update(range(offset, offset + section.size))
        for reloc in obj.relocs.get(section.index, []):
            masked.update(range(offset + reloc.offset, offset + reloc.offset + 4))
    return bytes(pattern), masked


def section_offsets(groups: dict[str, Group]) -> dict[int, tuple[Group, int]]:
    return {section.index: (group, offset) for group in groups.values() for section, offset in group.members}


def pad_text_end(group: Group, module: TargetModule) -> bool:
    """Extend a `.text` range that ends on a halfword (Thumb) to the next word boundary.

    The delinks range would otherwise leave a 2-byte `_dsd_gap` object between this unit and the next
    one; DSD emits it as a data/padding object that objdiff cannot parse (`objdiff report` panics at
    arm.rs:130).  mwld aligns the next object to its 4-byte `.text` alignment itself, so the linked bytes are
    unchanged as long as the image bytes in the padding are zero. Left alone when anything else owns the
    padding (another symbol, the section end, non-zero bytes); `integrate_batch._planned_thumb_unit_end`
    rejects the candidate in that case.
    """
    if group.address is None:
        return False
    end = group.address + group.size
    padded = _align(end, 4)
    if padded == end:
        return False
    low, high = module.sections.get(group.name, (0, 0))
    if padded > high or padded > module.base + len(module.data):
        return False
    for entry in module.symbols:
        start = entry.address & ~1
        if group.address <= start < end:
            continue                      # the unit's own symbols
        if start < padded and start + max(entry.size or 0, 1) > end:
            return False
    if any(module.data[address - module.base] for address in range(end, padded)):
        return False
    group.size += padded - end
    return True


def place(obj: ObjectFile, modules: dict[str, TargetModule], module_hint: str | None = None,
          text_address: int | None = None) -> dict[str, Group]:
    groups = build_groups(obj)
    candidates = [modules[module_hint]] if module_hint else list(modules.values())

    def byte_search(group: Group, restrict: list[TargetModule]) -> list[tuple[str, int]]:
        pattern, masked = group_pattern(obj, group)
        hits = []
        for module in restrict:
            if group.name not in module.sections:
                continue
            start, end = module.sections[group.name]
            lo, hi = start - module.base, min(end - module.base, len(module.data))
            for offset in find_masked(module.data, pattern, masked, lo, hi):
                hits.append((module.name, module.base + offset))
        return hits

    text = groups.get(".text")
    if text is None:
        raise PortError("object has no .text")
    if text_address is not None:
        owner = module_hint or next((m.name for m in candidates if m.section_of(text_address) == ".text"), None)
        if owner is None:
            raise PortError(f"no module has .text at {text_address:#x}")
        place_chain(obj, text, candidates, (owner, text_address))
        text.method = "given"
    else:
        place_chain(obj, text, candidates)
    pad_text_end(text, modules[text.module])
    where = section_offsets(groups)

    module = modules[text.module]
    progress = True
    while progress:
        progress = False
        # Forward: relocations in placed sections that point at unplaced groups.
        for group in [g for g in groups.values() if g.address is not None]:
            for section, offset in group.members:
                for reloc in obj.relocs.get(section.index, []):
                    target_index = reloc.symbol.shndx
                    if target_index not in where:
                        continue
                    target_group, target_offset = where[target_index]
                    if target_group.address is not None:
                        continue
                    found = module.relocs.get(group.address + offset + reloc.offset)
                    if found is None:
                        continue
                    target = found.effective_target
                    if reloc.type in ARM_BRANCHES | THUMB_BRANCHES:
                        target &= ~1
                    target_group.address = target - symbol_target_adjust(reloc) - target_offset - reloc.symbol.value
                    target_group.module, target_group.method = module.name, "reloc"
                    progress = True
        # Reverse: relocations inside unplaced groups that point at placed symbols.
        for group in [g for g in groups.values() if g.address is None and g.name in module.sections]:
            options: set[int] | None = None
            for section, offset in group.members:
                for reloc in obj.relocs.get(section.index, []):
                    source_index = reloc.symbol.shndx
                    if source_index not in where or where[source_index][0].address is None:
                        continue
                    source_group, source_offset = where[source_index]
                    expected = source_group.address + source_offset + reloc.symbol.value + symbol_target_adjust(reloc)
                    start, end = module.sections[group.name]
                    sites = {r.source - offset - reloc.offset for r in module.relocs.values()
                             if r.effective_target == expected and start <= r.source < end}
                    options = sites if options is None else options & sites
            if options is not None and len(options) == 1:
                group.address, group.module, group.method = options.pop(), module.name, "reverse-reloc"
                progress = True
        # Byte search inside the same module for remaining initialised data/code.
        for group in [g for g in groups.values() if g.address is None]:
            if all(section.kind == SHT_NOBITS for section, _ in group.members):
                continue
            hits = byte_search(group, [module])
            if len(hits) == 1:
                group.module, group.address = hits[0]
                group.method = "bytes"
                progress = True
    unplaced = [g.name for g in groups.values() if g.address is None]
    if unplaced:
        raise PortError(f"could not place {', '.join(unplaced)}")
    return groups


# --------------------------------------------------------------------------- symbol planning


@dataclass
class Wanted:
    name: str
    module: str
    address: int
    category: str
    size: int | None
    local: bool
    defined: bool


@dataclass
class Plan:
    unit: str
    groups: dict[str, Group]
    wanted: list[Wanted] = field(default_factory=list)
    renames: list[tuple[str, SymbolEntry, str]] = field(default_factory=list)   # module, entry, new name
    resizes: list[tuple[str, SymbolEntry, int]] = field(default_factory=list)
    relocal: list[tuple[str, SymbolEntry, bool]] = field(default_factory=list)
    additions: list[tuple[str, str]] = field(default_factory=list)             # module, line
    removals: list[tuple[str, SymbolEntry]] = field(default_factory=list)
    relocation_rebases: list[RelocationRebase] = field(default_factory=list)
    relocation_additions: list[RelocationAddition] = field(default_factory=list)
    defines: dict[str, str] = field(default_factory=dict)
    conflicts: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    obj: ObjectFile | None = None

    @property
    def module(self) -> str:
        return self.groups[".text"].module or ""


def _section_symbols(plan: Plan, section: ObjSection) -> list[ObjSymbol]:
    symbols = plan.obj.symbols if plan.obj else []
    return [s for s in symbols if s.shndx == section.index and _is_named_symbol(s)]


def _is_named_symbol(symbol: ObjSymbol) -> bool:
    return bool(symbol.name) and symbol.type in (STT_FUNC, STT_OBJECT, 0) \
        and not symbol.name.startswith(("$", ".dwarf")) and symbol.type not in (STT_SECTION, STT_FILE)


def plan_unit(obj: ObjectFile, modules: dict[str, TargetModule], unit: str, groups: dict[str, Group]) -> Plan:
    plan = Plan(unit, groups, obj=obj)
    where = section_offsets(groups)
    module = modules[plan.module]
    wanted: dict[str, Wanted] = {}

    def want(item: Wanted) -> None:
        previous = wanted.get(item.name)
        if previous is None:
            wanted[item.name] = item
        elif (previous.module, previous.address) != (item.module, item.address):
            plan.conflicts.append(
                f"{item.name}: resolves to both {previous.module}@{previous.address:#010x} "
                f"and {item.module}@{item.address:#010x}")

    for symbol in obj.symbols:
        if symbol.shndx not in where or not _is_named_symbol(symbol):
            continue
        group, offset = where[symbol.shndx]
        category = "function" if symbol.type == STT_FUNC else ("bss" if group.name in (".bss", ".sbss") else "data")
        want(Wanted(symbol.name, module.name, group.address + offset + (symbol.value & ~1 if category == "function" else symbol.value),
                    category, symbol.size or None, symbol.bind == STB_LOCAL, True))

    for group in groups.values():
        for section, offset in group.members:
            for reloc in obj.relocs.get(section.index, []):
                site = group.address + offset + reloc.offset
                source_owner = modules[group.module or plan.module]
                found = source_owner.relocs.get(site)
                if found is None:
                    resolved = _resolve_relocation_target(reloc, where, modules)
                    kind = _object_relocation_kind(section, reloc)
                    if resolved is not None and kind is not None:
                        owner, address, category = resolved
                        symbol_address = address
                        addend = symbol_target_adjust(reloc)
                        if addend < 0:
                            address += addend
                            addend = 0
                        plan.relocation_additions.append(RelocationAddition(
                            source_owner.name, site, kind, address, _module_ref(owner), addend))
                        if reloc.symbol.shndx == 0 and reloc.symbol.name:
                            want(Wanted(reloc.symbol.name, owner, symbol_address, category, None, False, False))
                        continue
                    plan.conflicts.append(
                        f"no {source_owner.name} relocation at {site:#010x} for "
                        f"{reloc.symbol.name or 'section'}")
                    continue
                target = found.effective_target
                if found.kind in CALL_KINDS:
                    target &= ~1
                address = target - symbol_target_adjust(reloc)
                if reloc.symbol.shndx in where:
                    target_group, target_offset = where[reloc.symbol.shndx]
                    expected = target_group.address + target_offset + reloc.symbol.value
                    if found.kind in CALL_KINDS:
                        expected &= ~1
                    if expected != address:
                        plan.conflicts.append(
                            f"relocation at {site:#010x} targets {found.effective_target:#010x}, object expects "
                            f"{expected + symbol_target_adjust(reloc):#010x}")
                    continue
                if not reloc.symbol.name or reloc.symbol.type == STT_SECTION:
                    continue
                if reloc.symbol.shndx != 0 and reloc.symbol.bind == STB_LOCAL:
                    plan.conflicts.append(f"{reloc.symbol.name}: local symbol in a stripped section is referenced")
                    continue
                owner = module_from_ref(found.module)
                if owner is None:
                    ambiguous = re.fullmatch(r"overlays\((\d+(?:,\d+)+)\)", found.module)
                    resolved = _explicit_overlay_target(reloc.symbol, modules)
                    if ambiguous and resolved is not None:
                        named_owner, named_address, named_category = resolved
                        allowed = {f"ov{int(number):03d}" for number in ambiguous.group(1).split(",")}
                        if (named_owner in allowed and named_address == address
                                and (found.kind not in CALL_KINDS or named_category == "function")):
                            owner = named_owner
                if owner is None or owner not in modules:
                    plan.conflicts.append(f"{reloc.symbol.name}: ambiguous target module {found.module}")
                    continue
                target_module = modules[owner]
                section_name = target_module.section_of(address) or ""
                if found.kind in CALL_KINDS:
                    address &= ~1
                    category = "function"
                else:
                    existing = [e for e in target_module.by_address.get(address, []) if e.category != "label"]
                    if not existing and address & 1:
                        thumb_target = address & ~1
                        thumb_existing = [
                            e for e in target_module.by_address.get(thumb_target, [])
                            if e.category == "function"
                        ]
                        if thumb_existing:
                            address = thumb_target
                            existing = thumb_existing
                    category = existing[0].category if existing else ("bss" if section_name == ".bss" else "data")
                want(Wanted(reloc.symbol.name, owner, address, category, None, False, False))

    _reject_stripped_definitions(plan, module)
    plan.wanted = list(wanted.values())
    _promote_external_local_data_symbols(plan, modules)
    _diff_symbols(plan, modules)
    add_symbol_less_gap_anchors(plan, module)
    _plan_relocation_rebases(plan, modules)
    _external_references(plan, modules)
    return plan


def _complete_text_ranges(path: Path) -> list[tuple[int, int]] | None:
    if not path.is_file():
        return None
    ranges = []
    in_unit, complete, has_text = False, False, False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line[0].isspace():
            in_unit, complete = line.rstrip().endswith(":"), False
            continue
        if line.strip() == "complete":
            complete = True
            continue
        match = SECTION_LINE.match(line)
        if match is None or match["name"] != ".text":
            continue
        if not in_unit:
            has_text = True
        elif complete:
            ranges.append((int(match["start"], 16), int(match["end"], 16)))
    return ranges if has_text else None


def _gap_has_linkable_symbol(module: TargetModule, low: int, high: int) -> bool:
    for entry in module.symbols:
        address = entry.address & ~1 if entry.category == "function" else entry.address
        if not low <= address < high:
            continue
        if entry.category == "function":
            return True
        if (entry.category in ("data", "bss", "label") and not entry.local
                and not entry.name.startswith(".L_")):
            return True
    return False


def _gap_anchor_mode(module: TargetModule, unit_start: int, unit_end: int) -> str:
    """Use the candidate's instruction set for its neighboring source-less gap anchors."""
    functions = [
        entry for entry in module.symbols
        if entry.category == "function" and unit_start <= (entry.address & ~1) < unit_end
    ]
    if functions:
        return "thumb" if functions[0].kind.startswith("function(thumb") else "arm"
    return "arm"


def add_symbol_less_gap_anchors(plan: Plan, module: TargetModule) -> None:
    """Add global label anchors for new `.text` gaps with no existing linkable symbol."""
    group = plan.groups.get(".text")
    section = module.sections.get(".text")
    if group is None or group.address is None or group.size <= 0 or section is None:
        return
    text_start, text_end = section
    unit_start, unit_end = group.address, group.address + group.size
    if not text_start <= unit_start < unit_end <= text_end:
        return
    ranges = _complete_text_ranges(module.delinks_path)
    if ranges is None:
        return
    before = max((end for _start, end in ranges if end <= unit_start), default=text_start)
    after = min((start for start, _end in ranges if start >= unit_end), default=text_end)
    for low, high in ((before, unit_start), (unit_end, after)):
        if low >= high or _gap_has_linkable_symbol(module, low, high):
            continue
        name = f"{GAP_ANCHOR_PREFIX}{module.name}_{low:08x}"
        existing = next((entry for entry in module.symbols if entry.name == name), None)
        if existing is not None:
            if existing.address != low:
                plan.conflicts.append(f"{name}: existing gap anchor is at {existing.address:#010x}, not {low:#010x}")
            continue
        mode = _gap_anchor_mode(module, unit_start, unit_end)
        plan.additions.append((module.name, f"{name} kind:label({mode}) addr:{low:#010x}"))


def _reject_stripped_definitions(plan: Plan, module: TargetModule) -> None:
    """Flag global functions in sections that placement dropped but the module still defines elsewhere.

    `place_chain` tolerates stripped members for dead or duplicate weak code.  A strong function that
    the module symbol table places outside the unit's range is real code, not a stripped duplicate: its
    bytes stay in the leftover `_dsd_gap` object and the link reports it multiply defined.  The usual
    cause is defining the functions in an order other than ascending address.
    """
    obj = plan.obj
    if obj is None:
        return
    for group in plan.groups.values():
        if group.address is None or group.module != module.name:
            continue
        low, high = group.address, group.address + group.size
        stripped = {section.index for section in group.stripped}
        for symbol in obj.symbols:
            if symbol.shndx not in stripped or symbol.type != STT_FUNC or symbol.bind != STB_GLOBAL \
                    or not symbol.name:
                continue
            for entry in module.symbols:
                if entry.name == symbol.name and entry.category == "function" \
                        and not low <= (entry.address & ~1) < high:
                    plan.conflicts.append(
                        f"{symbol.name}: not placed in the unit range {low:#010x}-{high:#010x} but the module "
                        f"defines it at {entry.address:#010x}; define the functions in ascending address order")
                    break


def _object_relocation_kind(section: ObjSection, reloc: ObjReloc) -> str | None:
    if reloc.type == R_ARM_ABS32:
        return "load"
    if reloc.type == R_ARM_CALL:
        return "arm_call"
    if reloc.type == R_ARM_JUMP24:
        return "arm_branch"
    if reloc.type == R_ARM_PC24:
        instruction = struct.unpack_from("<I", section.data, reloc.offset)[0]
        # ARM branch encoding bit 24 selects link (BL) rather than plain branch (B).
        return "arm_call" if instruction & 0x01000000 else "arm_branch"
    return None


def _module_ref(module: str) -> str:
    if module.startswith("ov") and module[2:].isdigit():
        return f"overlay({int(module[2:])})"
    return module


def _explicit_overlay_target(symbol: ObjSymbol, modules: dict[str, TargetModule]) -> tuple[str, int, str] | None:
    """Resolve a qualified auto-name from its exact owner declaration, not its suffix."""
    match = re.fullmatch(r"(func|data|bss)_(ov\d{3})_[0-9a-f]{8}", symbol.name)
    if match is None:
        return None
    prefix, owner = match.groups()
    module = modules.get(owner)
    if module is None:
        return None
    if symbol.type == STT_FUNC and prefix != "func":
        return None
    if symbol.type == STT_OBJECT and prefix == "func":
        return None
    entries = [entry for entry in module.symbols if entry.name == symbol.name]
    if not entries:
        return None
    resolved = {(entry.address, entry.category, entry.size) for entry in entries}
    if len(resolved) != 1:
        return None
    address, category, size = next(iter(resolved))
    if (prefix == "func" and category != "function"
            or prefix == "bss" and category != "bss"
            or prefix == "data" and category not in ("data", "bss")
            or category not in ("function", "data", "bss")):
        return None
    section = module.section_of(address)
    if (section is None
            or (category == "function" and section != ".text")
            or (category == "function" and (size is None or size <= 0))
            or (symbol.size and size != symbol.size)
            or (size is not None and address + size > module.sections[section][1])
            or (category != "function" and section not in (".data", ".rodata", ".bss", ".sbss"))):
        return None
    return owner, address, category


def _resolve_relocation_target(reloc: ObjReloc, where: dict[int, tuple[Group, int]],
                               modules: dict[str, TargetModule]) -> tuple[str, int, str] | None:
    symbol = reloc.symbol
    if symbol.shndx in where:
        group, offset = where[symbol.shndx]
        if group.module is None or group.address is None:
            return None
        category = "function" if symbol.type == STT_FUNC else (
            "bss" if group.name in (".bss", ".sbss") else "data")
        address = group.address + offset + (symbol.value & ~1 if category == "function" else symbol.value)
        return group.module, address, category
    if symbol.shndx != 0 or not symbol.name:
        return None
    if re.match(r"^(?:func|data|bss)_ov", symbol.name):
        return _explicit_overlay_target(symbol, modules)
    matches = {
        (module.name, entry.address, entry.category)
        for module in modules.values()
        for entry in module.symbols
        if entry.name == symbol.name and entry.category in ("function", "data", "bss")
    }
    if len(matches) != 1:
        return None
    owner, address, category = next(iter(matches))
    return owner, address, category


def _promote_external_local_data_symbols(plan: Plan, modules: dict[str, TargetModule]) -> None:
    """Export local data that existing code already references outside this unit."""
    unit_module = plan.module
    ranges = [(group.address, group.address + group.size) for group in plan.groups.values()
              if group.address is not None]
    local_data = [
        item for item in plan.wanted
        if item.defined and item.module == unit_module and item.local and item.category in ("data", "bss")
    ]
    if not local_data:
        return
    for source_module in modules.values():
        for relocation in source_module.relocs.values():
            if module_from_ref(relocation.module) != unit_module:
                continue
            if source_module.name == unit_module and any(
                    start <= relocation.source < end for start, end in ranges):
                continue
            if relocation.kind in CALL_KINDS:
                continue
            target = relocation.effective_target
            matches = [
                item for item in local_data
                if item.address == target or (
                    item.size is not None and item.address < target < item.address + item.size
                )
            ]
            if len(matches) == 1:
                matches[0].local = False


def _plan_relocation_rebases(plan: Plan, modules: dict[str, TargetModule]) -> None:
    """Retarget references to removed interior data symbols through their surviving root."""
    removed = {
        entry.address: entry
        for module, entry in plan.removals
        if module == plan.module and entry.category in ("data", "bss")
    }
    if not removed:
        return

    roots: list[tuple[Wanted, int]] = []
    data_symbols = [
        item for item in plan.wanted
        if item.defined and item.module == plan.module and item.category in ("data", "bss")
    ]
    for item in data_symbols:
        group = next(
            (group for group in plan.groups.values()
             if group.address is not None and group.address <= item.address < group.address + group.size),
            None,
        )
        if group is None:
            continue
        group_category = "bss" if group.name in (".bss", ".sbss") else "data"
        if item.category != group_category:
            continue
        group_end = group.address + group.size
        if item.size:
            end = min(item.address + item.size, group_end)
        else:
            next_address = min(
                (other.address for other in data_symbols
                 if other.module == item.module and other.category == item.category
                 and item.address < other.address < group_end),
                default=group_end,
            )
            end = next_address
        if item.address < end:
            roots.append((item, end))

    for source_module in modules.values():
        for relocation in source_module.relocs.values():
            if module_from_ref(relocation.module) != plan.module:
                continue
            stale = removed.get(relocation.target)
            if stale is None:
                continue
            effective_target = relocation.effective_target
            candidates = [
                (item, end) for item, end in roots
                if item.address <= effective_target < end
            ]
            if not candidates:
                plan.conflicts.append(
                    f"{source_module.name}@{relocation.source:#010x} targets removed data symbol "
                    f"{stale.name} at {relocation.target:#010x}, but no named unit data symbol covers "
                    f"{effective_target:#010x}"
                )
                continue
            item, _end = max(candidates, key=lambda root: (root[0].address, root[0].name))
            plan.relocation_rebases.append(RelocationRebase(
                source_module=source_module.name,
                relocation=relocation,
                target=item.address,
                addend=effective_target - item.address,
                symbol=item.name,
            ))


def _compatible(entry: SymbolEntry, category: str) -> bool:
    if category == "function":
        return entry.category == "function"
    return entry.category in ("data", "bss")


def _symbol_line(item: Wanted, module: TargetModule) -> str:
    if item.category == "function":
        kind = f"function(arm,size={item.size:#x})" if item.size else "function(arm)"
    elif item.category == "bss":
        kind = "bss"
    elif item.category == "label":
        kind = "label(arm)"
    else:
        kind = "data(any)"
    return f"{item.name} kind:{kind} addr:{item.address:#010x}" + (" local" if item.local else "")


def _diff_symbols(plan: Plan, modules: dict[str, TargetModule]) -> None:
    unit_module = modules[plan.module]
    ranges = [(g.address, g.address + g.size) for g in plan.groups.values()]
    in_unit = {id(entry) for entry in unit_module.symbols
               if (entry.category in ("function", "data", "bss")
                   or entry.name.startswith(GAP_ANCHOR_PREFIX))
               and any(lo <= entry.address < hi for lo, hi in ranges)}
    names: dict[str, list[tuple[str, SymbolEntry]]] = {}
    for module in modules.values():
        for entry in module.symbols:
            names.setdefault(entry.name, []).append((module.name, entry))
    claimed: set[int] = set()
    for item in plan.wanted:
        module = modules[item.module]
        existing = [e for e in module.by_address.get(item.address, []) if _compatible(e, item.category)]
        same = [e for e in existing if e.name == item.name]
        # In-unit entries are all renamed or removed, so their current names never block.
        elsewhere = [(m, e) for m, e in names.get(item.name, [])
                     if (m, e.address) != (item.module, item.address) and id(e) not in in_unit]
        auto_reference = not item.defined and AUTO_NAME.match(item.name)
        if elsewhere and not (item.local and item.defined):
            if auto_reference and existing:
                plan.defines[item.name] = existing[0].name
                continue
            where = ", ".join(f"{m}@{e.address:#010x}" for m, e in elsewhere)
            plan.conflicts.append(f"{item.name}: already defined at {where}")
            continue
        if same:
            entry = same[0]
        elif existing:
            entry = existing[0]
            if auto_reference:
                plan.defines[item.name] = entry.name
                continue
            if not AUTO_NAME.match(entry.name) and not item.defined:
                plan.conflicts.append(
                    f"{item.name}: {item.module}@{item.address:#010x} is already named {entry.name}")
                continue
            plan.renames.append((item.module, entry, item.name))
        else:
            if auto_reference:
                prefix = "func" if item.category == "function" else "data"
                overlay = f"{item.module}_" if item.module.startswith("ov") else ""
                auto = f"{prefix}_{overlay}{item.address:08x}"
                plan.defines[item.name] = auto
                plan.additions.append((item.module, _symbol_line(
                    Wanted(auto, item.module, item.address, item.category, None, False, False), module)))
            else:
                plan.additions.append((item.module, _symbol_line(item, module)))
            continue
        claimed.add(id(entry))
        if item.defined:
            if item.category == "function" and item.size and entry.size != item.size:
                plan.resizes.append((item.module, entry, item.size))
            if entry.local != item.local:
                plan.relocal.append((item.module, entry, item.local))
    # Stale symbols inside the unit's ranges that the object does not define.
    for entry in unit_module.symbols:
        if id(entry) in in_unit and id(entry) not in claimed:
            plan.removals.append((unit_module.name, entry))
    _add_data_section_boundary_symbols(plan, unit_module)


def _add_data_section_boundary_symbols(plan: Plan, module: TargetModule) -> None:
    """Keep the final unsized data symbol inside its delinked section."""
    removed_ids = {
        id(entry) for owner, entry in plan.removals if owner == module.name
    }
    marker_addresses: set[int] = set()
    for group in plan.groups.values():
        if group.name not in (".data", ".rodata", ".bss", ".sbss") or group.address is None:
            continue
        category = "bss" if group.name in (".bss", ".sbss") else "data"
        end = group.address + group.size
        terminal = max(
            (
                item for item in plan.wanted
                if item.defined and item.module == module.name and item.category == category
                and group.address <= item.address < end
            ),
            key=lambda item: item.address,
            default=None,
        )
        if terminal is None:
            continue
        existing = [
            entry for entry in module.by_address.get(terminal.address, [])
            if entry.category == category
        ]
        size = existing[0].size if existing else None
        if size is not None:
            if terminal.address + size > end:
                plan.conflicts.append(
                    f"{module.name}@{terminal.address:#010x} {terminal.name} extends beyond "
                    f"{group.name} section end {end:#010x}"
                )
            continue
        next_addresses = [
            entry.address for entry in module.symbols
            if entry.category == category and entry.address > terminal.address
            and id(entry) not in removed_ids
        ]
        for added_module, line in plan.additions:
            match = SYMBOL_LINE.match(line.strip()) if added_module == module.name else None
            if match and match["kind"].startswith(category) and int(match["addr"], 16) > terminal.address:
                next_addresses.append(int(match["addr"], 16))
        next_address = min(next_addresses, default=None)
        if next_address is not None and next_address <= end:
            continue
        if any(entry.address == end and entry.category == category for entry in module.symbols):
            continue
        if end in marker_addresses:
            continue
        marker_addresses.add(end)
        prefix = "bss" if category == "bss" else "data"
        name = f"{prefix}_{end:08x}"
        marker = Wanted(name, module.name, end, category, None, True, False)
        plan.additions.append((module.name, _symbol_line(marker, module)))


def _external_references(plan: Plan, modules: dict[str, TargetModule]) -> None:
    """Every relocation from outside the unit into it must hit a global object symbol.

    References into the middle of a global data object (e.g. vtable+8) are reported as
    warnings: they resolve as symbol+addend once the containing symbol is named.
    """
    unit_module = plan.module
    exported = {w.address for w in plan.wanted if w.defined and not w.local}
    spans = [(w.address, w.address + w.size) for w in plan.wanted
             if w.defined and not w.local and w.category != "function" and w.size]
    ranges = [(g.address, g.address + g.size) for g in plan.groups.values()]

    def inside(address: int) -> bool:
        return any(lo <= address < hi for lo, hi in ranges)

    for module in modules.values():
        for reloc in module.relocs.values():
            if module.name == unit_module and inside(reloc.source):
                continue
            if module_from_ref(reloc.module) != unit_module:
                continue
            target = reloc.effective_target & ~1 if reloc.kind in CALL_KINDS else reloc.effective_target
            if (reloc.kind not in CALL_KINDS and target & 1
                    and any(entry.address == (target & ~1) and entry.category == "function"
                            and entry.kind.startswith("function(thumb")
                            for entry in modules[unit_module].symbols)):
                target &= ~1
            if not inside(target) or target in exported:
                continue
            message = (f"{module.name}@{reloc.source:#010x} references {target:#010x} inside the unit, "
                       f"which is not a global symbol of the object")
            if reloc.kind not in CALL_KINDS and any(lo <= target < hi for lo, hi in spans):
                plan.warnings.append(message)
            else:
                plan.conflicts.append(message)


def delinks_block(plan: Plan, modules: dict[str, TargetModule]) -> str:
    module = modules[plan.module]
    order = list(module.sections)
    lines = [f"{plan.unit}:", "    complete"]
    for name in sorted(plan.groups, key=lambda n: order.index(n) if n in order else len(order)):
        group = plan.groups[name]
        lines.append(f"    {name} start:{group.address:#010x} end:{group.address + group.size:#010x}")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------- applying


def _rebased_reloc_line(line: str, target: int, addend: int, expected_source: int) -> str:
    match = RELOC_LINE.match(line.strip())
    if match is None or int(match["src"], 16) != expected_source:
        raise PortError(f"relocation at {expected_source:#010x} changed before it could be rebased")
    replacement = f"to:{target:#010x}" + (f" add:{addend:#x}" if addend else "")
    return re.sub(
        r"to:0x[0-9a-fA-F]+(?:\s+add:0x[0-9a-fA-F]+)?",
        replacement,
        line,
        count=1,
    )


def apply_plan(plan: Plan, modules: dict[str, TargetModule]) -> list[Path]:
    touched: dict[str, list[str]] = {}

    def lines_of(module: str) -> list[str]:
        if module not in touched:
            touched[module] = modules[module].symbols_path.read_text(encoding="utf-8").splitlines()
        return touched[module]

    for module, entry, new_name in plan.renames:
        lines = lines_of(module)
        lines[entry.line] = new_name + lines[entry.line][len(entry.name):]
    for module, entry, size in plan.resizes:
        lines = lines_of(module)
        lines[entry.line] = re.sub(r"size=0x[0-9a-fA-F]+", f"size={size:#x}", lines[entry.line], count=1)
    for module, entry, local in plan.relocal:
        lines = lines_of(module)
        text = re.sub(r"\s+local\b", "", lines[entry.line])
        lines[entry.line] = text + " local" if local else text
    removed = {(module, entry.line) for module, entry in plan.removals}
    for module, _entry in plan.removals:
        lines_of(module)
    for module, line in plan.additions:
        lines_of(module)
    for module, lines in touched.items():
        kept = [text for number, text in enumerate(lines) if (module, number) not in removed]
        for added_module, line in plan.additions:
            if added_module != module:
                continue
            address = int(SYMBOL_LINE.match(line)["addr"], 16)
            position = len(kept)
            for number, text in enumerate(kept):
                match = SYMBOL_LINE.match(text.strip())
                if match and int(match["addr"], 16) > address:
                    position = number
                    break
            kept.insert(position, line)
        modules[module].symbols_path.write_text("\n".join(kept) + "\n", encoding="utf-8")

    module = modules[plan.module]
    text = module.delinks_path.read_text(encoding="utf-8")
    block = delinks_block(plan, modules)
    pattern = re.compile(rf"^{re.escape(plan.unit)}:\n(?:[ \t]+\S.*\n?)*", re.MULTILINE)
    if pattern.search(text):
        text = pattern.sub(block, text, count=1)
    else:
        text = text.rstrip("\n") + "\n\n" + block
    module.delinks_path.write_text(text, encoding="utf-8")
    relocation_lines: dict[str, list[str]] = {}
    for rebase in plan.relocation_rebases:
        source_module = rebase.source_module
        if source_module not in relocation_lines:
            relocation_lines[source_module] = modules[source_module].relocs_path.read_text(
                encoding="utf-8"
            ).splitlines()
        lines = relocation_lines[source_module]
        line = rebase.relocation.line
        if line >= len(lines):
            raise PortError(
                f"{source_module}@{rebase.relocation.source:#010x}: relocation line disappeared"
            )
        lines[line] = _rebased_reloc_line(
            lines[line], rebase.target, rebase.addend, rebase.relocation.source
        )
    for addition in plan.relocation_additions:
        source_module = addition.source_module
        if source_module not in relocation_lines:
            relocation_lines[source_module] = modules[source_module].relocs_path.read_text(
                encoding="utf-8"
            ).splitlines()
        lines = relocation_lines[source_module]
        addend = f" add:{addition.addend:#x}" if addition.addend else ""
        line = (
            f"from:{addition.source:#010x} kind:{addition.kind} to:{addition.target:#010x}"
            f"{addend} module:{addition.target_module}"
        )
        existing = None
        for raw in lines:
            match = RELOC_LINE.match(raw.strip())
            if match and int(match["src"], 16) == addition.source:
                existing = match
                break
        if existing is not None:
            match = existing
            if (match["kind"] != addition.kind or int(match["dst"], 16) != addition.target
                    or (int(match["add"], 16) if match["add"] else 0) != addition.addend
                    or match["module"] != addition.target_module):
                raise PortError(
                    f"{source_module}@{addition.source:#010x}: relocation changed before it could be added"
                )
            continue
        position = len(lines)
        for index, raw in enumerate(lines):
            match = RELOC_LINE.match(raw.strip())
            if match and int(match["src"], 16) > addition.source:
                position = index
                break
        lines.insert(position, line)
    for source_module, lines in relocation_lines.items():
        modules[source_module].relocs_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    return ([modules[m].symbols_path for m in touched]
            + [modules[m].relocs_path for m in relocation_lines]
            + [module.delinks_path])


# --------------------------------------------------------------------------- CLI


def describe(plan: Plan, modules: dict[str, TargetModule]) -> dict[str, object]:
    return {
        "unit": plan.unit,
        "module": plan.module,
        "sections": {name: {"start": f"{g.address:#010x}", "end": f"{g.address + g.size:#010x}", "method": g.method}
                     for name, g in plan.groups.items()},
        "stripped": [f"{name}[{section.index}] size {section.size:#x}: "
                     + ", ".join(s.name for s in _section_symbols(plan, section))
                     for name, g in plan.groups.items() for section in g.stripped],
        "renames": [f"{m}@{e.address:#010x}: {e.name} -> {new}" for m, e, new in plan.renames],
        "resizes": [f"{m}@{e.address:#010x}: {e.name} size {e.size} -> {size:#x}" for m, e, size in plan.resizes],
        "locality": [f"{m}@{e.address:#010x}: {e.name} local={local}" for m, e, local in plan.relocal],
        "additions": [f"{m}: {line}" for m, line in plan.additions],
        "removals": [f"{m}@{e.address:#010x}: {e.name}" for m, e in plan.removals],
        "source_defines": [f"#define {k} {v}" for k, v in sorted(plan.defines.items())],
        "conflicts": plan.conflicts,
        "warnings": plan.warnings,
        "relocation_rebases": [
            f"{rebase.source_module}@{rebase.relocation.source:#010x}: "
            f"{rebase.relocation.target:#010x} add:{rebase.relocation.addend:#x} -> "
            f"{rebase.target:#010x} add:{rebase.addend:#x} "
            f"module:{rebase.relocation.module} ({rebase.symbol})"
            for rebase in plan.relocation_rebases
        ],
        "relocation_additions": [
            f"{addition.source_module}@{addition.source:#010x}: {addition.kind} -> "
            f"{addition.target_module}@{addition.target:#010x}"
            + (f" add:{addition.addend:#x}" if addition.addend else "")
            for addition in plan.relocation_additions
        ],
        "delinks": delinks_block(plan, modules),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("object", type=Path, help="compiled unit object (ELF)")
    parser.add_argument("unit", help="delinks unit name, e.g. src/System/Mutex.cpp")
    parser.add_argument("--project", type=Path, default=Path("."), help="DSD project root")
    parser.add_argument("--version", default="eur", help="config/<version> to edit")
    parser.add_argument("--module", help="restrict the .text search to one module")
    parser.add_argument("--keep-configured-image", action="append", default=[], metavar="MODULE",
                        help="use this module's configured object instead of its extracted image (repeatable)")
    parser.add_argument("--text-address", type=lambda v: int(v, 0), help="skip the .text search")
    parser.add_argument("--apply", action="store_true", help="write symbols.txt and delinks.txt edits")
    parser.add_argument("--allow-defines", action="store_true",
                        help="apply even if the source still needs the listed #define aliases")
    args = parser.parse_args(argv)
    try:
        modules = load_project(args.project, args.version, args.keep_configured_image)
        obj = parse_object(args.object)
        groups = place(obj, modules, args.module, args.text_address)
        plan = plan_unit(obj, modules, args.unit, groups)
    except (OSError, PortError, KeyError, struct.error) as error:
        print(f"port_eu_unit.py: {error}", file=sys.stderr)
        return 2
    print(json.dumps(describe(plan, modules), indent=2))
    if plan.conflicts or (plan.defines and not args.allow_defines):
        return 1
    if args.apply:
        for path in apply_plan(plan, modules):
            print(f"updated {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
