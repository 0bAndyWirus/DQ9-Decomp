"""Build a runtime manifest from DSD's ARM9 analysis configuration."""

from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, NamedTuple

import yaml

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, CS_MODE_THUMB, Cs, CsInsn
from capstone.arm_const import (
    ARM_CC_AL,
    ARM_CC_INVALID,
    ARM_OP_IMM,
    ARM_OP_MEM,
    ARM_OP_REG,
    ARM_REG_LR,
    ARM_REG_PC,
    ARM_SFT_LSL,
)


_SYMBOL_RE = re.compile(
    r"^(?P<name>\S+)\s+kind:(?P<kind>\S+)\s+addr:(?P<address>0x[0-9a-fA-F]+|[0-9]+)"
    r"(?:\s+local)?\s*$"
)
_FUNCTION_RE = re.compile(
    r"^function\((?P<isa>arm|thumb)(?:,size=(?P<size>0x[0-9a-fA-F]+|[0-9]+))?\)$"
)
_RELOC_RE = re.compile(
    r"^from:(?P<from>0x[0-9a-fA-F]+|[0-9]+)\s+kind:(?P<kind>\S+)\s+"
    r"to:(?P<to>0x[0-9a-fA-F]+|[0-9]+)\s+module:(?P<module>\S+)\s*$"
)
_MODULE_IDS = {"main": -1, "itcm": -2, "dtcm": -3}
_SECTION_RE = re.compile(
    r"^\s+(?P<name>\S+)\s+start:(?P<start>0x[0-9a-fA-F]+|[0-9]+)\s+"
    r"end:(?P<end>0x[0-9a-fA-F]+|[0-9]+)\s+kind:(?P<kind>\w+)\b"
)
_COVERAGE_RE = re.compile(r"^(?P<address>(?:0x)?[0-9a-fA-F]+)\s+(?P<mode>[at])$")
_BIOS_START = 0xFFFF0000
MAIN_RAM_BASE = 0x02000000  # Nintendo DS main RAM hardware-map base.

# Instruction classes (capstone base names) that an ARM946E-S (ARMv5TE) can
# execute. Anything else decoded inside a gap marks it as data.
_ARM_INSNS = frozenset({
    "adc", "add", "adr", "and", "asr", "b", "bic", "bl", "blx", "bx", "clz", "cmn", "cmp",
    "eor", "ldm", "ldmda", "ldmdb", "ldmib", "ldr", "ldrb", "ldrbt", "ldrd", "ldrh",
    "ldrsb", "ldrsh", "ldrt", "lsl", "lsr", "mcr", "mla", "mov", "mrc", "mrs", "msr",
    "mul", "mvn", "orr", "pld", "pop", "push", "qadd", "qdadd", "qdsub", "qsub", "ror",
    "rrx", "rsb", "rsc", "sbc", "smlabb", "smlabt", "smlatb", "smlatt", "smlal",
    "smlalbb", "smlalbt", "smlaltb", "smlaltt", "smlawb", "smlawt", "smulbb", "smulbt",
    "smultb", "smultt", "smull", "smulwb", "smulwt", "stm", "stmda", "stmdb", "stmib",
    "str", "strb", "strbt", "strd", "strh", "strt", "sub", "svc", "swp", "swpb", "teq",
    "tst", "umlal", "umull",
})
_THUMB_INSNS = frozenset({
    "adc", "add", "adr", "and", "asr", "b", "bic", "bl", "blx", "bx", "cmn", "cmp", "eor",
    "ldm", "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "lsl", "lsr", "mov", "movs", "mul",
    "mvn", "orr", "pop", "push", "ror", "rsb", "sbc", "stm", "str", "strb", "strh", "sub",
    "svc", "tst",
})
_DATA_WRITERS = frozenset({
    "adc", "add", "and", "asr", "bic", "eor", "lsl", "lsr", "mov", "movs", "mvn", "orr",
    "ror", "rrx", "rsb", "rsc", "sbc", "sub",
})
_LITERAL_LOADS = {"ldr": 4, "ldrb": 1, "ldrh": 2, "ldrsb": 1, "ldrsh": 2, "ldrd": 8}
_BLOCK_LOADS = frozenset({"pop", "ldm", "ldmda", "ldmdb", "ldmib"})
_BLOCK_STORES = frozenset({"push", "stm", "stmda", "stmdb", "stmib"})


def _number(value: str) -> int:
    return int(value, 0)


def _read_symbols(path: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[list[int]]]:
    functions: list[dict[str, Any]] = []
    labels: list[dict[str, Any]] = []
    data_addresses: list[int] = []
    all_addresses: set[int] = set()
    if not path.is_file():
        return functions, labels, []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = _SYMBOL_RE.fullmatch(line.strip())
        if match is None:
            continue
        name, kind, address_text = match.group("name", "kind", "address")
        address = _number(address_text)
        all_addresses.add(address)
        function = _FUNCTION_RE.fullmatch(kind)
        if function is not None and function.group("size") is not None:
            size = _number(function.group("size"))
            if size > 0:
                thumb = function.group("isa") == "thumb"
                functions.append({"addr": address & ~1, "thumb": thumb, "size": size, "name": name})
        elif kind.startswith("label("):
            thumb_match = re.match(r"^label\((arm|thumb)\)$", kind)
            if thumb_match is not None:
                labels.append({"addr": address & ~1, "thumb": thumb_match.group(1) == "thumb"})
        elif kind.startswith("data("):
            data_addresses.append(address)

    sorted_addresses = sorted(all_addresses)
    next_address = {addr: sorted_addresses[i + 1] for i, addr in enumerate(sorted_addresses[:-1])}
    ranges = [
        [address, next_address[address]]
        for address in sorted(set(data_addresses))
        if address in next_address and next_address[address] > address
    ]
    functions.sort(key=lambda function: (function["addr"], function["name"]))
    labels.sort(key=lambda label: label["addr"])
    ranges.sort()
    return functions, labels, ranges


def _module_ids(reference: str) -> list[int] | None:
    if reference in _MODULE_IDS:
        return [_MODULE_IDS[reference]]
    if reference == "none":
        return []
    single = re.fullmatch(r"overlay\((\d+)\)", reference)
    if single:
        return [int(single.group(1))]
    multiple = re.fullmatch(r"overlays\((\d+(?:,\d+)*)\)", reference)
    if multiple:
        return [int(item) for item in multiple.group(1).split(",")]
    return None


def _read_calls(path: Path) -> list[dict[str, Any]]:
    calls: list[dict[str, Any]] = []
    if not path.is_file():
        return calls
    for line in path.read_text(encoding="utf-8").splitlines():
        match = _RELOC_RE.fullmatch(line.strip())
        if match is None or match.group("kind") not in {"arm_call", "thumb_call"}:
            continue
        modules = _module_ids(match.group("module"))
        if modules is not None:
            calls.append({"from": _number(match.group("from")), "to": _number(match.group("to")), "modules": modules})
    return calls


def _load_yaml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    with path.open("r", encoding="utf-8") as stream:
        value = yaml.safe_load(stream)
    return value if isinstance(value, dict) else {}


def _metadata_path(config_dir: Path, config: dict[str, Any]) -> Path:
    build_value = config.get("build_path", "build")
    return (config_dir / str(build_value) / "build").resolve()


def _module(config_dir: Path, entry: dict[str, Any], module_id: int, name: str,
            base_address: int) -> dict[str, Any]:
    image_value = entry.get("object")
    if not isinstance(image_value, str) or not image_value:
        raise ValueError(f"DSD module {name!r} is missing an object image path")
    symbols_path = config_dir / str(entry.get("symbols", ""))
    relocation_path = config_dir / str(entry.get("relocations", ""))
    functions, labels, data_ranges = _read_symbols(symbols_path)
    return {
        "name": name,
        "module_id": module_id,
        "base": base_address,
        "image": str((config_dir / image_value).resolve()),
        "functions": functions,
        "labels": labels,
        "data_ranges": data_ranges,
        "code_ranges": [list(region) for region in
                        _read_code_sections(config_dir / str(entry.get("delinks", "")))],
        "calls": _read_calls(relocation_path),
    }


def _read_code_sections(path: Path) -> list[tuple[int, int]]:
    """Return the [start, end) ranges of ``kind:code`` sections in a DSD delinks header."""
    sections: list[tuple[int, int]] = []
    if not path.is_file():
        return sections
    for line in path.read_text(encoding="utf-8").splitlines():
        # The module header is the indented block before the first blank line or unit.
        if not line.strip() or not line[:1].isspace():
            break
        match = _SECTION_RE.match(line)
        if match is not None and match.group("kind") == "code":
            start, end = _number(match.group("start")), _number(match.group("end"))
            if end > start:
                sections.append((start, end))
    return sections


def _function_name(module: dict[str, Any], address: int) -> str:
    module_id = int(module["module_id"])
    if module_id >= 0:
        return f"func_ov{module_id:03d}_{address:08x}"
    return f"func_{address:08x}"


class _Effect(NamedTuple):
    """Control-flow role of one validated instruction."""

    flow: str  # "next", "call", "branch" (direct b) or "exit" (indirect pc write / return)
    conditional: bool
    target: int | None = None
    literal: tuple[int, int] | None = None  # (address, width) of a pc-relative load


_DISASSEMBLERS: dict[bool, Cs] = {}


def _disassembler(thumb: bool) -> Cs:
    engine = _DISASSEMBLERS.get(thumb)
    if engine is None:
        engine = Cs(CS_ARCH_ARM, (CS_MODE_THUMB if thumb else CS_MODE_ARM) | CS_MODE_LITTLE_ENDIAN)
        engine.detail = True
        _DISASSEMBLERS[thumb] = engine
    return engine


def _effect(insn: CsInsn, thumb: bool) -> _Effect | None:
    """Classify a decoded instruction, or return None if it is not plausible ARMv5TE code."""
    name = insn.insn_name()
    if name not in (_THUMB_INSNS if thumb else _ARM_INSNS):
        return None
    if thumb and insn.size != 2 and name not in {"bl", "blx"}:
        return None
    if insn.mnemonic.startswith(("movw", "movt")):
        return None
    conditional = insn.cc not in (ARM_CC_AL, ARM_CC_INVALID)
    operands = insn.operands
    registers = [operand.reg for operand in operands if operand.type == ARM_OP_REG]
    if name in {"mcr", "mrc"}:
        # The ARM946E-S only has CP15; other coprocessor numbers are data.
        return _Effect("next", False) if insn.op_str.startswith("p15,") and not conditional else None
    if name == "svc":
        number = operands[0].imm if operands else -1
        valid = 0 <= number < 0x40 if thumb else (number & 0xFFFF) == 0 and number >> 16 < 0x40
        return _Effect("next", False) if valid and not conditional else None
    if name in {"b", "bl", "blx"} and operands and operands[0].type == ARM_OP_IMM:
        return _Effect("branch" if name == "b" else "call", conditional, operands[0].imm & ~1)
    if name == "blx":
        return _Effect("next", conditional)
    if name == "bx":
        return _Effect("exit", conditional)
    if name in _BLOCK_LOADS or name in _BLOCK_STORES:
        stack = name in {"pop", "push"}
        if not stack and registers and registers[0] == ARM_REG_PC:
            return None
        if ARM_REG_PC in (registers if stack else registers[1:]):
            return _Effect("exit", conditional) if name in _BLOCK_LOADS else None
        return _Effect("next", conditional)
    destination_pc = bool(operands) and operands[0].type == ARM_OP_REG and operands[0].reg == ARM_REG_PC
    memory = next((operand.mem for operand in operands if operand.type == ARM_OP_MEM), None)
    if memory is not None:
        if memory.base == ARM_REG_PC:
            if insn.writeback:
                return None
            if memory.index:
                # Only `ldr pc, [pc, rN, lsl #2]` jump tables index off pc.
                return _Effect("exit", conditional) if name == "ldr" and destination_pc else None
            width = _LITERAL_LOADS.get(name)
            if width is None:
                return None
            base = (insn.address + 4) & ~3 if thumb else insn.address + 8
            flow = "exit" if destination_pc else "next"
            return _Effect(flow, conditional, literal=(base + memory.disp, width))
        if destination_pc:
            return _Effect("exit", conditional) if name == "ldr" else None
        return _Effect("next", conditional)
    if destination_pc:
        return _Effect("exit", conditional) if name in _DATA_WRITERS else None
    return _Effect("next", conditional)


def _inside(ranges: list[tuple[int, int]], address: int) -> bool:
    # Overlay sections overlap each other, so a sorted bisect alone is not enough.
    return any(start <= address < end for start, end in ranges)


class _GapScanner:
    """Conservative linear decoder that decides whether an uncovered code range is code."""

    def __init__(self, module: dict[str, Any], image: bytes, sections: list[tuple[int, int]],
                 known_entries: set[int], all_sections: list[tuple[int, int]],
                 external_targets: set[int]) -> None:
        self.module = module
        self.base = int(module["base"])
        self.image = image
        self.sections = sorted(sections)
        self.known_entries = known_entries
        self.all_sections = all_sections
        self.external_targets = external_targets
        self.calls_by_source: dict[int, list[dict[str, Any]]] = {}
        for call in module.get("calls", []):
            self.calls_by_source.setdefault(int(call["from"]) & ~1, []).append(call)

    def _value(self, address: int, size: int) -> int | None:
        offset = address - self.base
        if offset < 0 or offset + size > len(self.image):
            return None
        return int.from_bytes(self.image[offset : offset + size], "little")

    def is_padding(self, address: int, thumb: bool) -> bool:
        if thumb:
            return self._value(address, 2) in (0, 0x46C0)  # zero / mov r8, r8
        return self._value(address, 4) in (0, 0xE1A00000)  # zero / mov r0, r0

    def looks_like_prologue(self, address: int, thumb: bool) -> bool:
        if thumb:
            value = self._value(address, 2)
            # push {..., lr} / sub sp, #imm
            return value is not None and ((value & 0xFF00) == 0xB500 or (value & 0xFF80) == 0xB080)
        value = self._value(address, 4)
        # stmdb sp!, {..., lr} / str lr, [sp, #-4]! / sub sp, sp, #imm
        return value is not None and (
            (value & 0xFFFF4000) == 0xE92D4000 or value == 0xE52DE004 or (value & 0xFFFFF000) == 0xE24DD000
        )

    def _instruction(self, address: int, end: int, thumb: bool) -> CsInsn | None:
        offset = address - self.base
        limit = min(end, address + 4) - self.base
        if offset < 0 or limit > len(self.image) or limit <= offset:
            return None
        return next(_disassembler(thumb).disasm(self.image[offset:limit], address, 1), None)

    def scan(self, start: int, end: int) -> tuple[list[tuple[int, int, bool]], str]:
        """Decode a gap found by the static scan; returns added functions or a rejection reason."""
        if start % 4:
            return self.decode(start, end, True)
        functions, reason = self.decode(start, end, False)
        if functions or reason == "padding":
            return functions, reason
        first = start
        while first < end and self.is_padding(first, True):
            first += 2
        if first < end and self.looks_like_prologue(first, True):
            thumb_functions, thumb_reason = self.decode(start, end, True)
            if thumb_functions:
                return thumb_functions, ""
            reason = f"{reason}; thumb: {thumb_reason}"
        return [], reason

    def decode(self, start: int, end: int, thumb: bool,
               relaxed: bool = False) -> tuple[list[tuple[int, int, bool]], str]:
        """Linearly decode [start, end) into back-to-back functions.

        Strict mode (static gap scan) requires every instruction to be valid ARMv5TE code,
        every function to begin unconditionally, direct branches to stay in the gap or reach
        known entries, literal loads to point past the code into the gap, and the code to end
        in an unconditional return/tail exit followed only by literal pool words or padding.
        Relaxed mode (seeded by an executed PC) additionally accepts branches/literals that
        leave the gap within the module and trailing undecodable data after the last exit.
        """
        unit = 2 if thumb else 4
        mode = "thumb" if thumb else "arm"
        if start % unit:
            return [], f"start 0x{start:08x} is not {mode}-aligned"
        pos = start
        while pos < end and self.is_padding(pos, thumb):
            pos += unit
        if pos >= end:
            return [], "padding"
        first = pos
        functions: list[tuple[int, int, bool]] = []
        function_start = pos
        function_targets: set[int] = set()
        all_targets: set[int] = set()
        code: set[int] = set()
        literal: set[int] = set()
        boundary = False  # previous instruction was an unconditional exit
        separated = False  # padding or a literal pool followed that exit
        while pos < end:
            if pos in literal:
                if not boundary:
                    return [], f"literal pool at 0x{pos:08x} is reached by straight-line {mode} code"
                pos += unit
                separated = True
                continue
            if boundary and not any(target >= pos for target in function_targets):
                if self.is_padding(pos, thumb):
                    pos += unit
                    separated = True
                    continue
                if separated or pos in self.external_targets or self.looks_like_prologue(pos, thumb):
                    functions.append((function_start, pos, thumb))
                    function_start = pos
                    function_targets = set()
                pending = False
            else:
                pending = True
            insn = self._instruction(pos, end, thumb)
            effect = _effect(insn, thumb) if insn is not None else None
            if effect is None:
                if relaxed and boundary and not pending:
                    break  # trailing data after the final exit stays inside the function
                return [], f"invalid {mode} instruction at 0x{pos:08x}"
            if pos == function_start and effect.conditional and not relaxed:
                return [], f"{mode} function candidate 0x{pos:08x} starts with a conditional instruction"
            if effect.literal is not None:
                address, width = effect.literal
                if pos + insn.size <= address and address + width <= end:
                    literal.update(range(address & ~(unit - 1), address + width, unit))
                elif not (relaxed and not first <= address < end and self._value(address, width) is not None):
                    return [], f"literal load at 0x{pos:08x} reads 0x{address:08x} outside the trailing pool"
            if effect.flow == "branch":
                target = effect.target
                if first <= target < end:
                    if target % unit:
                        return [], f"branch at 0x{pos:08x} targets misaligned 0x{target:08x}"
                    function_targets.add(target)
                    all_targets.add(target)
                elif target not in self.known_entries and not (relaxed and _inside(self.sections, target)):
                    return [], f"branch at 0x{pos:08x} leaves the gap to unknown 0x{target:08x}"
            elif effect.flow == "call":
                target = effect.target
                if not (first <= target < end or target in self.known_entries
                        or _inside(self.all_sections, target)):
                    return [], f"call at 0x{pos:08x} targets non-code 0x{target:08x}"
            code.add(pos)
            boundary = effect.flow in {"branch", "exit"} and not effect.conditional
            if boundary:
                separated = False
            pos += insn.size
        if not boundary:
            return [], f"{mode} code from 0x{function_start:08x} runs off the gap end without a return"
        stray = sorted(all_targets - code)
        if stray:
            return [], f"branch into non-code 0x{stray[0]:08x}"
        functions.append((function_start, end, thumb))
        return functions, ""


def _code_gaps(module: dict[str, Any], sections: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Return code-section byte ranges covered by neither a function nor a data symbol."""
    covered = sorted(
        [(int(fn["addr"]), int(fn["addr"]) + int(fn["size"])) for fn in module["functions"]]
        + [(int(low), int(high)) for low, high in module["data_ranges"]]
    )
    gaps: list[tuple[int, int]] = []
    for start, end in sorted(sections):
        cursor = start
        for low, high in covered:
            if low >= end:
                break
            if high <= cursor:
                continue
            if low > cursor:
                gaps.append((cursor, low))
            cursor = max(cursor, high)
        if cursor < end:
            gaps.append((cursor, end))
    return gaps


def _branch_targets(image: bytes, base: int, sections: list[tuple[int, int]],
                    gaps: list[tuple[int, int]]) -> set[int]:
    """Collect ARM b/bl/blx immediate targets anywhere in the module that land in a gap."""
    starts = [start for start, _ in gaps]
    targets: set[int] = set()
    for start, end in sections:
        low = max(0, ((start + 3) & ~3) - base)
        high = min(len(image), end - base) & ~3
        if high <= low:
            continue
        for index, (word,) in enumerate(struct.iter_unpack("<I", image[low:high])):
            if (word >> 25) & 7 != 5:
                continue
            offset = word & 0xFFFFFF
            if offset & 0x800000:
                offset -= 0x1000000
            target = base + low + index * 4 + 8 + (offset << 2)
            if word >> 28 == 0xF:
                target += (word >> 23) & 2  # blx immediate H bit
            slot = bisect.bisect_right(starts, target) - 1
            if slot >= 0 and target < gaps[slot][1]:
                targets.add(target)
    return targets
def _literal_code_pointers(module: dict[str, Any], function: dict[str, Any], image: bytes,
                           sections: list[tuple[int, int]]) -> set[tuple[int, bool]]:
    """Collect same-module code pointers loaded from PC-relative literal words."""
    base = int(module["base"])
    start = int(function["addr"])
    end = min(start + int(function["size"]), base + len(image))
    thumb = bool(function["thumb"])
    unit = 2 if thumb else 4
    literal_words: set[int] = set()
    targets: set[tuple[int, bool]] = set()
    pos = start
    while pos < end:
        if pos in literal_words:
            pos += unit
            continue
        offset = pos - base
        if offset < 0 or offset + unit > len(image):
            pos += unit
            continue
        insn = next(_disassembler(thumb).disasm(image[offset : min(offset + 4, len(image))], pos, 1),
                    None)
        if insn is None:
            pos += unit
            continue
        effect = _effect(insn, thumb)
        if effect is not None and effect.literal is not None:
            address, width = effect.literal
            if width == 4 and effect.flow == "next":
                literal_offset = address - base
                if 0 <= literal_offset and literal_offset + 4 <= len(image):
                    pointer = int.from_bytes(image[literal_offset : literal_offset + 4], "little")
                    target = pointer & ~1
                    target_thumb = bool(pointer & 1)
                    target_unit = 2 if target_thumb else 4
                    if target % target_unit == 0 and _inside(sections, target):
                        targets.add((target, target_thumb))
            if start <= address and address + width <= end:
                literal_words.update(range(address & ~(unit - 1), address + width, unit))
        pos += insn.size
    return targets


def _pc_relative_assignment(insn: CsInsn, thumb: bool) -> tuple[int, int] | None:
    """Return a destination register and value for a PC-relative address formation."""
    operands = insn.operands
    if not operands or operands[0].type != ARM_OP_REG or operands[0].reg == ARM_REG_PC:
        return None
    name = insn.insn_name()
    if name == "adr" and len(operands) == 2 and operands[1].type == ARM_OP_IMM:
        if thumb:
            halfword = int.from_bytes(insn.bytes, "little")
            if (halfword & 0xF800) != 0xA000:
                return None
            pc = (insn.address + 4) & ~3
            return operands[0].reg, (pc + ((halfword & 0xFF) << 2)) & 0xFFFFFFFF
        return operands[0].reg, int(operands[1].imm) & 0xFFFFFFFF
    if (name not in {"add", "sub"} or len(operands) not in {3, 4}
            or operands[1].type != ARM_OP_REG or operands[1].reg != ARM_REG_PC
            or operands[2].type != ARM_OP_IMM
            or (len(operands) == 4 and operands[3].type != ARM_OP_IMM)):
        return None
    pc = insn.address + (4 if thumb else 8)
    if thumb:
        pc &= ~3
        immediate = int(operands[2].imm)
    else:
        raw = int.from_bytes(insn.bytes, "little")
        rotation = ((raw >> 8) & 0xF) * 2
        immediate = int(operands[2].imm) if not rotation else (
            ((raw & 0xFF) >> rotation) | ((raw & 0xFF) << (32 - rotation))
        ) & 0xFFFFFFFF
    value = pc + immediate if name == "add" else pc - immediate
    return operands[0].reg, value & 0xFFFFFFFF


def _indirect_entry_targets(
    instructions: list[tuple[int, CsInsn, _Effect]],
    thumb: bool,
    sections: list[tuple[int, int]],
    is_local_call: Callable[[int], bool],
) -> dict[tuple[int, bool], str]:
    """Resolve register-indirect code transfers fed by PC-relative addresses."""
    values: dict[int, set[int]] = {}
    entries: dict[tuple[int, bool], str] = {}

    previous_end: int | None = None
    for _, insn, effect in instructions:
        if previous_end is not None and insn.address != previous_end:
            values.clear()
        previous_end = insn.address + insn.size
        operands = insn.operands
        name = insn.insn_name()
        source: int | None = None
        transfer_kind: str | None = None
        if name in {"bx", "blx"} and operands and operands[0].type == ARM_OP_REG:
            source = operands[0].reg
            transfer_kind = "indirect-call" if name == "blx" else "indirect"
            if name == "blx" and not is_local_call(insn.address):
                transfer_kind = None
        elif (name == "mov" and (thumb or not insn.update_flags) and len(operands) == 2
              and operands[0].type == ARM_OP_REG and operands[0].reg == ARM_REG_PC
              and operands[1].type == ARM_OP_REG):
            source = operands[1].reg
            transfer_kind = "indirect"

        if source is not None and transfer_kind is not None:
            for value in values.get(source, ()):
                if name in {"bx", "blx"}:
                    target_thumb = bool(value & 1)
                    target = value & ~(1 if target_thumb else 3)
                else:
                    target_thumb = thumb
                    target = value & ~(1 if thumb else 3)
                target_unit = 2 if target_thumb else 4
                if target % target_unit == 0 and _inside(sections, target):
                    entries.setdefault((target, target_thumb), transfer_kind)

        conditional = insn.cc not in (ARM_CC_AL, ARM_CC_INVALID)
        assignment = _pc_relative_assignment(insn, thumb)
        copied: tuple[int, set[int]] | None = None
        if (name == "mov" and len(operands) == 2 and operands[0].type == ARM_OP_REG
                and operands[0].reg != ARM_REG_PC and operands[1].type == ARM_OP_REG
                and operands[1].shift.type == 0):
            source_values = values.get(operands[1].reg)
            if source_values:
                copied = (operands[0].reg, source_values)
        if not values and assignment is None and copied is None:
            continue
        writes = set(insn.regs_access()[1])

        for register in writes:
            if not conditional:
                values.pop(register, None)
        if assignment is not None:
            register, value = assignment
            if conditional:
                values.setdefault(register, set()).add(value)
            else:
                values[register] = {value}
        elif copied is not None:
            register, source_values = copied
            if conditional:
                values.setdefault(register, set()).update(source_values)
            else:
                values[register] = set(source_values)
        if effect.flow in {"branch", "exit"} and not effect.conditional:
            values.clear()
    return entries

def _relative_offset_table_targets(
    scanner: _GapScanner,
    start: int,
    end: int,
    thumb: bool,
    instructions: list[tuple[int, CsInsn, _Effect]],
) -> tuple[set[int], set[int]]:
    """Resolve ADD-PC-relative indexed word tables and return targets/table slots."""
    if thumb:
        return set(), set()
    targets: set[int] = set()
    slots: set[int] = set()
    instruction_starts = {address for address, _, _ in instructions}
    for index, (_, insn, _) in enumerate(instructions):
        operands = insn.operands
        if (insn.insn_name() != "add" or len(operands) < 3
                or operands[0].type != ARM_OP_REG or operands[0].reg != ARM_REG_PC
                or operands[1].type != ARM_OP_REG or operands[2].type != ARM_OP_REG):
            continue
        source_regs = {operands[1].reg, operands[2].reg}
        load_index: int | None = None
        value_reg: int | None = None
        base_reg: int | None = None
        for candidate in range(index - 1, max(-1, index - 4), -1):
            candidate_insn = instructions[candidate][1]
            candidate_operands = candidate_insn.operands
            if (candidate_insn.insn_name() != "ldr" or candidate_insn.writeback
                    or not candidate_operands or candidate_operands[0].type != ARM_OP_REG):
                continue
            memory = next(
                (operand for operand in candidate_operands if operand.type == ARM_OP_MEM), None
            )
            if memory is None or not memory.mem.index or memory.mem.disp != 0:
                continue
            shift = int(memory.mem.lshift)
            if memory.shift.type == ARM_SFT_LSL:
                shift = int(memory.shift.value)
            candidate_base = memory.mem.base
            candidate_value = candidate_operands[0].reg
            if (shift != 2 or candidate_base not in source_regs
                    or candidate_value not in source_regs or candidate_base == candidate_value):
                continue
            if any(
                candidate_value in instructions[middle][1].regs_access()[1]
                or candidate_base in instructions[middle][1].regs_access()[1]
                for middle in range(candidate + 1, index)
            ):
                continue
            load_index = candidate
            value_reg = candidate_value
            base_reg = candidate_base
            break
        if load_index is None or value_reg is None or base_reg is None:
            continue

        table_base: int | None = None
        for candidate in range(load_index - 1, -1, -1):
            prior = instructions[candidate][1]
            writes = set(prior.regs_access()[1])
            if base_reg not in writes:
                continue
            assignment = _pc_relative_assignment(prior, False)
            if (assignment is not None and assignment[0] == base_reg
                    and prior.cc in (ARM_CC_AL, ARM_CC_INVALID)):
                table_base = assignment[1]
            break
        if table_base is None or table_base % 4 or not start <= table_base < end:
            continue

        image_offset = table_base - scanner.base
        if image_offset < 0 or image_offset + 4 > len(scanner.image):
            continue
        for slot in range(table_base, end - 3, 4):
            offset = slot - scanner.base
            if offset < 0 or offset + 4 > len(scanner.image):
                break
            relative = int.from_bytes(scanner.image[offset:offset + 4], "little")
            target = (table_base + relative) & 0xFFFFFFFF
            if (target % 4 or not start <= target < end
                    or target not in instruction_starts):
                break
            target_insn = scanner._instruction(target, end, False)
            if target_insn is None or _effect(target_insn, False) is None:
                break
            if table_base <= target < slot + 4:
                break
            targets.add(target)
            slots.add(slot)
    return targets, slots


def _region_dispatch_analysis(scanner: _GapScanner, function: dict[str, Any]
                              ) -> tuple[set[int], list[list[int]], set[int]]:
    """Find dispatch entries, table targets, and ranges to skip in an inferred region."""
    start = int(function["addr"])
    end = min(start + int(function["size"]), scanner.base + len(scanner.image))
    thumb = bool(function["thumb"])
    unit = 2 if thumb else 4
    instructions: list[tuple[int, CsInsn, _Effect]] = []
    literal_words: set[int] = set()
    invalid_words: set[int] = set()
    declared_data_words: set[int] = set()
    for low, high in scanner.module.get("data_ranges", []):
        first = max(start, int(low)) & ~(unit - 1)
        limit = min(end, int(high))
        declared_data_words.update(range(first, limit, unit))
    pos = start
    while pos < end:
        if pos in literal_words or pos in declared_data_words:
            pos += unit
            continue
        insn = scanner._instruction(pos, end, thumb)
        effect = _effect(insn, thumb) if insn is not None else None
        if effect is None:
            invalid_words.add(pos)
            pos += unit
            continue
        instructions.append((pos, insn, effect))
        if effect.literal is not None:
            address, width = effect.literal
            if start <= address and address + width <= end:
                literal_words.update(range(address & ~(unit - 1), address + width, unit))
        pos += insn.size

    table_targets, table_words = _relative_offset_table_targets(
        scanner, start, end, thumb, instructions
    )
    data_words = literal_words | table_words | declared_data_words | invalid_words
    instruction_starts = {address for address, _, _ in instructions} - data_words
    entries: set[int] = {start, *table_targets}
    values: dict[int, set[int]] = {}
    previous_end: int | None = None
    for address, insn, effect in instructions:
        if previous_end is not None and address != previous_end:
            values.clear()
        previous_end = address + insn.size
        if address in data_words:
            values.clear()
            continue
        name = insn.insn_name()
        operands = insn.operands
        if effect.flow in {"branch", "call"} and effect.target is not None:
            target_thumb = thumb != (name == "blx")
            target = effect.target & ~(1 if target_thumb else 3)
            if target_thumb == thumb and start <= target < end:
                entries.add(target)
        if (name in {"add", "sub"} and len(operands) == 3
                and operands[0].type == ARM_OP_REG and operands[0].reg == ARM_REG_PC):
            def operand_values(operand: Any) -> set[int]:
                if operand.type == ARM_OP_IMM:
                    return {int(operand.imm) & 0xFFFFFFFF}
                if operand.type != ARM_OP_REG or operand.shift.type:
                    return set()
                if operand.reg == ARM_REG_PC:
                    pc = (address + (4 if thumb else 8)) & 0xFFFFFFFF
                    if thumb:
                        pc &= ~3
                    return {pc}
                return values.get(operand.reg, set())

            left_values = operand_values(operands[1])
            right_values = operand_values(operands[2])
            for left in left_values:
                for right in right_values:
                    target = (left + right if name == "add" else left - right) & 0xFFFFFFFF
                    target &= ~(1 if thumb else 3)
                    if target in instruction_starts:
                        entries.add(target)
        if (effect.flow == "call"
                or (effect.flow in {"branch", "exit"} and effect.conditional)):
            next_address = address + insn.size
            if start <= next_address < end:
                entries.add(next_address)
        source: int | None = None
        if name in {"bx", "blx"} and operands and operands[0].type == ARM_OP_REG:
            source = operands[0].reg
        elif (name == "mov" and (thumb or not insn.update_flags) and len(operands) == 2
              and operands[0].type == ARM_OP_REG and operands[0].reg == ARM_REG_PC
              and operands[1].type == ARM_OP_REG):
            source = operands[1].reg
        if source is not None:
            for value in values.get(source, ()):
                target_thumb = bool(value & 1) if name in {"bx", "blx"} else thumb
                target = value & ~(1 if target_thumb else 3)
                if target_thumb == thumb and start <= target < end:
                    entries.add(target)
        if effect.flow in {"branch", "exit"} and values.get(ARM_REG_LR):
            entries.update(value & ~(1 if thumb else 3) for value in values[ARM_REG_LR])

        conditional = insn.cc not in (ARM_CC_AL, ARM_CC_INVALID)
        assignment = _pc_relative_assignment(insn, thumb)
        if (assignment is None and name == "mov" and len(operands) == 2
                and operands[0].type == ARM_OP_REG and operands[0].reg != ARM_REG_PC
                and operands[1].type == ARM_OP_REG and operands[1].reg == ARM_REG_PC):
            pc = (address + (4 if thumb else 8)) & 0xFFFFFFFF
            assignment = (operands[0].reg, pc | (1 if thumb else 0))
        copied: tuple[int, set[int]] | None = None
        if (name == "mov" and len(operands) == 2
                and operands[0].type == ARM_OP_REG and operands[0].reg != ARM_REG_PC
                and operands[1].type == ARM_OP_REG and operands[1].shift.type == 0):
            source_values = values.get(operands[1].reg)
            if source_values:
                copied = (operands[0].reg, source_values)
        if values or assignment is not None or copied is not None:
            writes = set(insn.regs_access()[1])
            for register in writes:
                if not conditional:
                    values.pop(register, None)
            if assignment is not None:
                register, value = assignment
                if conditional:
                    values.setdefault(register, set()).add(value)
                else:
                    values[register] = {value}
            elif copied is not None:
                register, source_values = copied
                if conditional:
                    values.setdefault(register, set()).update(source_values)
                else:
                    values[register] = set(source_values)
        if effect.flow in {"branch", "exit"} and not effect.conditional:
            values.clear()


    dispatch_instructions = [
        record for record in instructions if record[0] not in data_words
    ]
    entries.update(
        address for (address, target_thumb), _source in _indirect_entry_targets(
            dispatch_instructions, thumb, scanner.sections, lambda _address: True
        ).items()
        if target_thumb == thumb and start <= address < end
    )
    valid_entries: set[int] = set()
    for address in entries:
        if address % unit or not start <= address < end or address not in instruction_starts:
            continue
        valid_entries.add(address)
    data_ranges: list[list[int]] = []
    for address in sorted(data_words):
        if data_ranges and address <= data_ranges[-1][1]:
            data_ranges[-1][1] = max(data_ranges[-1][1], address + unit)
        else:
            data_ranges.append([address, address + unit])
    return valid_entries, data_ranges, table_targets

def _region_dispatch_entries(scanner: _GapScanner, function: dict[str, Any]
                             ) -> tuple[set[int], list[list[int]]]:
    """Find in-region dispatch entries and ranges that a linear emitter must skip."""
    entries, data_ranges, _ = _region_dispatch_analysis(scanner, function)
    return entries, data_ranges



def _function_entry_targets(scanner: _GapScanner, function: dict[str, Any]
                            ) -> tuple[dict[tuple[int, bool], str], set[int]]:
    """Find direct callees, indirect callees, and return-separated entries."""
    module = scanner.module
    module_id = int(module["module_id"])
    start = int(function["addr"])
    end = min(start + int(function["size"]), scanner.base + len(scanner.image))
    thumb = bool(function["thumb"])
    unit = 2 if thumb else 4
    literal_words: set[int] = set()
    call_targets: set[tuple[int, bool]] = set()
    branch_targets: set[int] = set()
    instructions: list[tuple[int, CsInsn, _Effect]] = []
    end_boundaries: set[int] = set()

    def is_local_call(address: int) -> bool:
        records = scanner.calls_by_source.get(address, [])
        return not records or any(
            module_id in {int(target) for target in record.get("modules", [])}
            for record in records
        )
    pos = start
    after_exit = False
    while pos < end:
        if pos in literal_words or (after_exit and scanner.is_padding(pos, thumb)):
            pos += unit
            continue
        insn = scanner._instruction(pos, end, thumb)
        effect = _effect(insn, thumb) if insn is not None else None
        if effect is None:
            if after_exit and not any(target >= pos for target in branch_targets):
                end_boundaries.add(pos)
                break
            after_exit = False
            pos += unit
            continue
        instructions.append((pos, insn, effect))
        if effect.literal is not None:
            address, width = effect.literal
            if start <= address and address + width <= end:
                literal_words.update(range(address & ~(unit - 1), address + width, unit))
        if effect.flow == "call" and effect.target is not None and is_local_call(pos):
            target_thumb = thumb != (insn.insn_name() == "blx")
            target_unit = 2 if target_thumb else 4
            if (effect.target % target_unit == 0
                    and _inside(scanner.sections, effect.target)):
                call_targets.add((effect.target, target_thumb))
        elif effect.flow == "branch" and effect.target is not None:
            if start <= effect.target < end:
                branch_targets.add(effect.target)
        after_exit = effect.flow == "exit" and not effect.conditional
        pos += insn.size
    if after_exit:
        end_boundaries.add(end)

    entries: dict[tuple[int, bool], str] = {}
    for address, insn, effect in instructions:
        if effect.flow == "call" and effect.target is not None and is_local_call(address):
            target_thumb = thumb != (insn.insn_name() == "blx")
            target = (effect.target, target_thumb)
            if target in call_targets:
                entries[target] = "call"
        if effect.flow != "exit" or effect.conditional:
            continue

        boundary = address + insn.size
        candidate = boundary
        while candidate < end and (
            candidate in literal_words or scanner.is_padding(candidate, thumb)
        ):
            candidate += unit
        call_targets_at_boundary = {
            target for target in call_targets if target[0] == candidate
        }
        is_prologue = candidate < end and scanner.looks_like_prologue(candidate, thumb)
        for target in call_targets_at_boundary:
            entries[target] = "call"
        if is_prologue and not call_targets_at_boundary:
            entries.setdefault((candidate, thumb), "post-return")

        if candidate >= end or not any(target >= boundary for target in branch_targets):
            end_boundaries.add(boundary)
        if call_targets_at_boundary or is_prologue:
            break
        if not any(target >= boundary for target in branch_targets):
            break
    entries.update(_indirect_entry_targets(instructions, thumb, scanner.sections, is_local_call))
    return entries, end_boundaries


def _function_key(function: dict[str, Any]) -> tuple[int, int, bool]:
    return int(function["addr"]), int(function["size"]), bool(function["thumb"])


def _function_covers(function: dict[str, Any], address: int) -> bool:
    start = int(function["addr"])
    return start <= address < start + int(function["size"])


def _exact_function(module: dict[str, Any], address: int, thumb: bool) -> bool:
    return any(int(function["addr"]) == address and bool(function["thumb"]) == thumb
               for function in module["functions"])


def _function_gap(module: dict[str, Any], sections: list[tuple[int, int]],
                  address: int) -> tuple[int, int] | None:
    return next(((start, end) for start, end in _code_gaps(module, sections)
                 if start <= address < end), None)


@dataclass
class _Gap:
    scanner: _GapScanner
    start: int
    end: int
    reason: str


class _GapDiscovery:
    """Adds inferred functions for undescribed code inside DSD code sections."""

    def __init__(self, modules: list[dict[str, Any]], sections: list[list[tuple[int, int]]]) -> None:
        self.modules = modules
        self.known_entries = {int(fn["addr"]) for module in modules for fn in module["functions"]}
        self.known_entries.update(int(label["addr"]) for module in modules for label in module["labels"])
        all_sections = sorted(section for module_sections in sections for section in module_sections)
        self.order = {int(module["module_id"]): position for position, module in enumerate(modules)}
        self.inferred_entries: set[tuple[int, int, bool]] = set()
        self.code_gaps = 0
        self.added: list[dict[str, Any]] = []
        self.rejected: list[_Gap] = []
        for module, module_sections in zip(modules, sections):
            image_path = Path(module["image"])
            if not module_sections or not image_path.is_file():
                continue
            image = image_path.read_bytes()
            gaps = _code_gaps(module, module_sections)
            if not gaps:
                continue
            self.code_gaps += len(gaps)
            scanner = _GapScanner(module, image, module_sections, self.known_entries, all_sections,
                                  _branch_targets(image, int(module["base"]), module_sections, gaps))
            for start, end in gaps:
                functions, reason = scanner.scan(start, end)
                if functions:
                    self._add(scanner, functions, "gap")
                else:
                    self.rejected.append(_Gap(scanner, start, end, reason))
            self._discover_entries(scanner)
            self._build_dispatch_regions(scanner)
            self._trim_rejected(module)


    def _is_inferred(self, module: dict[str, Any], function: dict[str, Any]) -> bool:
        return (
            int(module["module_id"]), int(function["addr"]), bool(function["thumb"])
        ) in self.inferred_entries

    def _build_dispatch_regions(self, scanner: _GapScanner) -> None:
        module = scanner.module
        module["dispatch_regions"] = []
        inferred = [
            function for function in module["functions"]
            if self._is_inferred(module, function) and int(function["size"]) > 0
        ]
        dsd_functions = [
            function for function in module["functions"]
            if not self._is_inferred(module, function)
        ]

        for thumb in (False, True):
            functions = sorted(
                (function for function in inferred if bool(function["thumb"]) == thumb),
                key=lambda function: int(function["addr"]),
            )
            group: list[dict[str, Any]] = []
            group_end = 0

            def add_group() -> None:
                if not group:
                    return
                start = min(int(function["addr"]) for function in group)
                end = max(int(function["addr"]) + int(function["size"]) for function in group)
                if not any(low <= start and end <= high for low, high in scanner.sections):
                    return
                if any(
                    bool(function["thumb"]) == thumb
                    and int(function["addr"]) < end
                    and start < int(function["addr"]) + int(function["size"])
                    for function in dsd_functions
                ):
                    return
                region = {"addr": start, "size": end - start, "thumb": thumb}
                entries = {int(function["addr"]) for function in group}
                dispatch_entries, data_ranges = _region_dispatch_entries(scanner, region)
                entries.update(dispatch_entries)
                region["data_ranges"] = data_ranges
                entries = {
                    address for address in entries
                    if start <= address < end
                    and not any(
                        bool(function["thumb"]) == thumb and _function_covers(function, address)
                        for function in dsd_functions
                    )
                }
                module["dispatch_regions"].append({
                    **region,
                    "entries": sorted(entries),
                })

            for function in functions:
                start = int(function["addr"])
                end = start + int(function["size"])
                if group and start < group_end:
                    group.append(function)
                    group_end = max(group_end, end)
                else:
                    add_group()
                    group = [function]
                    group_end = end
            add_group()

        module["dispatch_regions"].sort(
            key=lambda region: (int(region["addr"]), bool(region["thumb"]))
        )

    def _add(self, scanner: _GapScanner, functions: list[tuple[int, int, bool]], source: str) -> None:
        module = scanner.module
        module_id = int(module["module_id"])
        for start, end, thumb in functions:
            record = {
                "addr": start,
                "thumb": thumb,
                "size": end - start,
                "name": _function_name(module, start),
            }
            module["functions"].append(record)
            self.known_entries.add(start)
            self.inferred_entries.add((module_id, start, thumb))
            self.added.append({"module": module["name"], "module_id": module["module_id"], **record,
                               "source": source})
        module["functions"].sort(key=lambda function: (function["addr"], function["name"]))

    def _discover_entries(self, scanner: _GapScanner) -> None:
        module = scanner.module
        scanned_functions: set[tuple[int, int, bool]] = set()
        handled_targets: set[tuple[int, bool]] = set()
        pending_functions = list(module["functions"])
        while pending_functions:
            function = pending_functions.pop()
            key = _function_key(function)
            if key in scanned_functions:
                continue
            scanned_functions.add(key)

            targets = _literal_code_pointers(module, function, scanner.image, scanner.sections)
            for address, thumb in sorted(targets - handled_targets):
                handled_targets.add((address, thumb))
                count = len(module["functions"])
                self._infer_entry(scanner, address, thumb, "literal", set())
                if len(module["functions"]) != count:
                    pending_functions.extend(module["functions"])

            entries, _ = _function_entry_targets(scanner, function)
            if not self._is_inferred(module, function):
                entries = {
                    target: source for target, source in entries.items()
                    if source in {"indirect", "indirect-call"}
                }
            for (address, thumb), source in sorted(entries.items()):
                count = len(module["functions"])
                self._infer_entry(scanner, address, thumb, source, set())
                if len(module["functions"]) != count:
                    pending_functions.extend(module["functions"])

    def _infer_dispatch_region(self, scanner: _GapScanner, address: int, thumb: bool,
                               end: int, source: str) -> bool:
        region = {"addr": address, "size": end - address, "thumb": thumb}
        entries, _, table_targets = _region_dispatch_analysis(scanner, region)
        if address not in entries or not table_targets:
            return False
        self._add(scanner, [(address, end, thumb)], source)
        return True

    def _infer_branch_stub(self, scanner: _GapScanner, address: int, thumb: bool,
                           end: int, source: str,
                           visited: set[tuple[int, bool]]) -> bool:
        unit = 2 if thumb else 4
        insn = scanner._instruction(address, end, thumb)
        effect = _effect(insn, thumb) if insn is not None else None
        if effect is None or effect.flow != "branch" or effect.conditional or effect.target is None:
            return False
        target = effect.target
        if target % unit or not _inside(scanner.sections, target):
            return False
        if not self._infer_entry(scanner, target, thumb, source, visited):
            return False
        stub, _ = scanner.decode(address, address + insn.size, thumb, relaxed=True)
        if not stub or stub[0][0] != address:
            return False
        self._add(scanner, stub, source)
        return True

    def _infer_entry(self, scanner: _GapScanner, address: int, thumb: bool, source: str,
                     visited: set[tuple[int, bool]]) -> bool:
        module = scanner.module
        unit = 2 if thumb else 4
        key = (address, thumb)
        if address % unit or not _inside(scanner.sections, address) or key in visited:
            return False
        visited.add(key)
        if _exact_function(module, address, thumb):
            return True

        covering = [function for function in module["functions"]
                    if _function_covers(function, address)]
        if covering:
            if any(not self._is_inferred(module, function) for function in covering):
                owner = min(covering, key=lambda function: int(function["size"]))
                end = min(
                    int(owner["addr"]) + int(owner["size"]),
                    scanner.base + len(scanner.image),
                )
                return self._infer_branch_stub(scanner, address, thumb, end, source, visited)
            return self._infer_secondary_entry(scanner, address, thumb, source)

        gap = _function_gap(module, scanner.sections, address)
        if gap is None:
            return False
        functions, _ = scanner.decode(address, gap[1], thumb, relaxed=True)
        if functions and functions[0][0] == address:
            self._add(scanner, functions, source)
            return True
        # A mixed code/data gap may defeat linear decoding even when an indexed
        # relative jump table proves that the direct entry roots a dispatch region.
        if self._infer_dispatch_region(scanner, address, thumb, gap[1], source):
            return True

        return self._infer_branch_stub(scanner, address, thumb, gap[1], source, visited)

    def _infer_secondary_entry(self, scanner: _GapScanner, address: int, thumb: bool,
                               source: str) -> bool:
        module = scanner.module
        if _exact_function(module, address, thumb):
            return True
        covering = [function for function in module["functions"]
                    if _function_covers(function, address)]
        if any(not self._is_inferred(module, function) for function in covering):
            return False
        owners = [function for function in covering if self._is_inferred(module, function)]
        if not owners:
            return False
        owner = min(owners, key=lambda function: int(function["size"]))
        end = min(int(owner["addr"]) + int(owner["size"]), scanner.base + len(scanner.image))
        if address >= end:
            return False

        seeded = {"addr": address, "thumb": thumb, "size": end - address}
        _, boundaries = _function_entry_targets(scanner, seeded)
        ends = {boundary for boundary in boundaries if address < boundary <= end}
        unit = 2 if thumb else 4
        # Inferred owners can span adjacent code; test the next recognizable
        # prologue as a bound before unrelated bytes make the decode fail.
        ends.update(
            candidate for candidate in range(address + unit, end, unit)
            if scanner.looks_like_prologue(candidate, thumb)
        )
        ends.add(end)
        for candidate_end in sorted(ends):
            functions, _ = scanner.decode(address, candidate_end, thumb, relaxed=True)
            if functions and functions[0][0] == address and functions[0][1] > address:
                self._add(scanner, [functions[0]], source)
                return True
        if self._infer_dispatch_region(scanner, address, thumb, end, source):
            return True

        return False

    def _trim_rejected(self, module: dict[str, Any]) -> None:
        covered = sorted(
            [(int(function["addr"]), int(function["addr"]) + int(function["size"]))
             for function in module["functions"]]
            + [(int(low), int(high)) for low, high in module["data_ranges"]]
        )
        remaining: list[_Gap] = []
        for gap in self.rejected:
            if gap.scanner.module is not module:
                remaining.append(gap)
                continue
            cursor = gap.start
            for low, high in covered:
                if high <= cursor:
                    continue
                if low >= gap.end:
                    break
                if low > cursor:
                    remaining.append(_Gap(gap.scanner, cursor, min(low, gap.end), gap.reason))
                cursor = max(cursor, high)
                if cursor >= gap.end:
                    break
            if cursor < gap.end:
                remaining.append(_Gap(gap.scanner, cursor, gap.end, gap.reason))
        self.rejected = remaining

    def seed(self, pc: int, thumb: bool) -> int:
        """Retry a rejected gap holding an executed PC; returns the number of functions added."""
        unit = 2 if thumb else 4
        if pc % unit:
            return 0
        for gap in list(self.rejected):
            if not gap.start <= pc < gap.end:
                continue
            first = (gap.start + unit - 1) & ~(unit - 1)
            candidates = [first]
            candidates += [address for address in range(pc, first, -unit)
                           if gap.scanner.looks_like_prologue(address, thumb)]
            candidates.append(pc)
            for start in dict.fromkeys(candidates):
                functions, _ = gap.scanner.decode(start, gap.end, thumb, relaxed=True)
                if not any(low <= pc < high for low, high, _ in functions):
                    continue
                self._add(gap.scanner, functions, "coverage")
                self.rejected.remove(gap)
                if functions[0][0] > gap.start:
                    self.rejected.append(_Gap(gap.scanner, gap.start, functions[0][0], gap.reason))
                self._discover_entries(gap.scanner)
                self._build_dispatch_regions(gap.scanner)
                self._trim_rejected(gap.scanner.module)
                return len(functions)
        return 0

    def summary(self) -> dict[str, Any]:
        return {
            "code_gaps": self.code_gaps,
            "added": sorted(self.added, key=lambda record: (self.order[record["module_id"]], record["addr"])),
            "dispatch_regions": sorted(
                (
                    {"module": module["name"], "module_id": module["module_id"], **region}
                    for module in self.modules
                    for region in module.get("dispatch_regions", [])
                ),
                key=lambda record: (self.order[record["module_id"]], record["addr"], record["thumb"]),
            ),
            "rejected": [
                {"module": gap.scanner.module["name"], "start": gap.start, "end": gap.end, "reason": gap.reason}
                for gap in sorted(self.rejected,
                                  key=lambda gap: (self.order[int(gap.scanner.module["module_id"])], gap.start))
            ],
        }


def _read_coverage(path: Path) -> tuple[int, set[tuple[int, bool]]]:
    rows = 0
    pcs: set[tuple[int, bool]] = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        text = " ".join(line.split())
        if not text or text.startswith("#"):
            continue
        match = _COVERAGE_RE.fullmatch(text)
        if match is None:
            raise ValueError(f"{path}:{number}: expected 'addr_hex a|t', got {line!r}")
        rows += 1
        pcs.add((int(match.group("address"), 16), match.group("mode") == "t"))
    return rows, pcs


class _CoverageIndex:
    """Union of all module function ranges, per instruction set."""

    def __init__(self, modules: list[dict[str, Any]]) -> None:
        merged: dict[bool, list[list[int]]] = {False: [], True: []}
        for thumb in (False, True):
            ranges = sorted(
                (int(fn["addr"]), int(fn["addr"]) + int(fn["size"]))
                for module in modules for fn in module["functions"] if bool(fn["thumb"]) == thumb
            )
            for start, end in ranges:
                if merged[thumb] and start <= merged[thumb][-1][1]:
                    merged[thumb][-1][1] = max(merged[thumb][-1][1], end)
                else:
                    merged[thumb].append([start, end])
        self.ranges = merged
        self.starts = {thumb: [start for start, _ in merged[thumb]] for thumb in merged}

    def covers(self, pc: int, thumb: bool) -> bool:
        index = bisect.bisect_right(self.starts[thumb], pc) - 1
        return index >= 0 and pc < self.ranges[thumb][index][1]


def _check_coverage(modules: list[dict[str, Any]], discovery: _GapDiscovery, path: Path) -> dict[str, Any]:
    """Require every executed non-BIOS PC to lie in a same-mode function of some module."""
    rows, pcs = _read_coverage(path)
    bios = sum(1 for pc, _ in pcs if pc >= _BIOS_START)
    executed = sorted(item for item in pcs if item[0] < _BIOS_START)
    index = _CoverageIndex(modules)
    seeded = 0
    for pc, thumb in [item for item in executed if not index.covers(*item)]:
        if not index.covers(pc, thumb):
            added = discovery.seed(pc, thumb)
            if added:
                seeded += added
                index = _CoverageIndex(modules)
    misses = [item for item in executed if not index.covers(*item)]
    summary = {
        "rows": rows,
        "unique_pcs": len(pcs),
        "covered": len(executed) - len(misses),
        "bios_ignored": bios,
        "seeded_functions": seeded,
        "misses": len(misses),
    }
    if misses:
        groups: list[list[Any]] = []
        for pc, thumb in misses:
            reason = (f"inside a {'arm' if thumb else 'thumb'} function (mode mismatch)"
                      if index.covers(pc, not thumb) else "not inside any function")
            last = groups[-1] if groups else None
            if last is not None and last[2] == thumb and last[3] == reason and pc - last[1] <= (2 if thumb else 4):
                last[1] = pc
                last[4] += 1
            else:
                groups.append([pc, pc, thumb, reason, 1])
        lines = [
            f"  0x{low:08x}-0x{high:08x} {'thumb' if thumb else 'arm'} ({count} pcs): {reason}"
            for low, high, thumb, reason, count in groups
        ]
        raise ValueError(
            f"{path}: {len(misses)} executed PCs are not covered by the manifest "
            f"(rows={rows} unique_pcs={len(pcs)} bios_ignored={bios} seeded_functions={seeded}):\n"
            + "\n".join(lines)
        )
    return summary


def build_arm9_manifest(config_path: str | Path, *, coverage_path: str | Path | None = None) -> dict[str, Any]:
    """Build the ARM9 static-recompiler manifest for a DSD config YAML file.

    Code-section ranges that DSD left without a function or data symbol are decoded and,
    when they hold valid code, added as ``func_*`` functions (reported under ``gap_scan``).
    With ``coverage_path`` (rows ``addr_hex a|t``) every executed non-BIOS PC must lie in a
    same-mode function of some module; misses raise ValueError.
    """
    config_file = Path(config_path).resolve()
    config = _load_yaml(config_file)
    if not isinstance(config.get("main_module"), dict):
        raise ValueError(f"{config_file}: expected a DSD ARM9 config with main_module")

    config_dir = config_file.parent
    metadata_dir = _metadata_path(config_dir, config)
    main_meta = _load_yaml(metadata_dir / "arm9.yaml")
    modules: list[dict[str, Any]] = []
    sections: list[list[tuple[int, int]]] = []

    def add_module(entry: dict[str, Any], module_id: int, name: str, base_address: int) -> None:
        modules.append(_module(config_dir, entry, module_id, name, base_address))
        sections.append(_read_code_sections(config_dir / str(entry.get("delinks", ""))))

    add_module(config["main_module"], -1, "main", int(main_meta.get("base_address", MAIN_RAM_BASE)))

    for item in config.get("autoloads", []) or []:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name", "")).lower()
        if name not in {"itcm", "dtcm"}:
            kind = str(item.get("kind", "")).lower()
            name = "itcm" if "itcm" in kind else "dtcm" if "dtcm" in kind else name
        if name not in {"itcm", "dtcm"}:
            continue
        metadata = _load_yaml(metadata_dir / f"{name}.yaml")
        add_module(item, _MODULE_IDS[name], name, int(metadata.get("base_address", 0)))

    overlay_meta = _load_yaml(metadata_dir / "arm9_overlays.yaml")
    metadata_by_id = {
        int(item["id"]): item for item in overlay_meta.get("overlays", [])
        if isinstance(item, dict) and "id" in item
    }
    for item in config.get("overlays", []) or []:
        if not isinstance(item, dict) or "id" not in item:
            continue
        overlay_id = int(item["id"])
        name = str(item.get("name", f"ov{overlay_id:03d}"))
        metadata = metadata_by_id.get(overlay_id, {})
        add_module(item, overlay_id, name, int(metadata.get("base_address", 0)))

    discovery = _GapDiscovery(modules, sections)
    coverage = _check_coverage(modules, discovery, Path(coverage_path)) if coverage_path is not None else None
    manifest: dict[str, Any] = {"cpu": "arm9", "arch": "v5te", "modules": modules,
                                "gap_scan": discovery.summary()}
    if coverage is not None:
        manifest["coverage"] = coverage
    return manifest


def write_arm9_manifest(config_path: str | Path, output_path: str | Path, *,
                        coverage_path: str | Path | None = None) -> dict[str, Any]:
    """Build and write the manifest as stable, human-readable JSON."""
    manifest = build_arm9_manifest(config_path, coverage_path=coverage_path)
    destination = Path(output_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build an ARM9 manifest from locally supplied DSD analysis files")
    parser.add_argument(
        "--config",
        required=True,
        help="DSD ARM9 config YAML",
    )
    parser.add_argument(
        "--out",
        default="build/gen/arm9.manifest.json",
        help="manifest JSON path",
    )
    parser.add_argument(
        "--coverage",
        help="executed-PC list (rows 'addr_hex a|t') that the manifest functions must cover",
    )
    args = parser.parse_args(argv)
    try:
        manifest = write_arm9_manifest(args.config, args.out, coverage_path=args.coverage)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    gap_scan = manifest["gap_scan"]
    for record in gap_scan["added"]:
        print(f"gap function: {record['module']} 0x{record['addr']:08x} "
              f"{'thumb' if record['thumb'] else 'arm'} size=0x{record['size']:x} "
              f"{record['name']} ({record['source']})")
    padding = 0
    for gap in gap_scan["rejected"]:
        if gap["reason"] == "padding":
            padding += 1
        else:
            print(f"rejected gap: {gap['module']} 0x{gap['start']:08x}-0x{gap['end']:08x}: {gap['reason']}")
    print(f"gap scan: {gap_scan['code_gaps']} code gaps, {len(gap_scan['added'])} functions added, "
          f"{len(gap_scan['rejected'])} gaps rejected ({padding} padding)")
    coverage = manifest.get("coverage")
    if coverage is not None:
        print(f"coverage: rows={coverage['rows']} unique_pcs={coverage['unique_pcs']} "
              f"covered={coverage['covered']} bios_ignored={coverage['bios_ignored']} "
              f"seeded_functions={coverage['seeded_functions']} misses={coverage['misses']}")
    function_count = sum(len(module["functions"]) for module in manifest["modules"])
    print(f"wrote {args.out}: {len(manifest['modules'])} modules, {function_count} functions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
