#!/usr/bin/env python3
"""Discover ARM7 code and autoload regions from a locally extracted NDS ARM7 image.

The input and generated images are ROM-derived. Keep generated output below an
ignored directory (normally ``build/gen``); this tool never writes into the
tracked source tree except for this original analysis utility.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from bisect import bisect_right
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

try:
    from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, CS_MODE_THUMB, Cs
    from capstone.arm import (
        ARM_INS_ADD,
        ARM_INS_B,
        ARM_INS_BL,
        ARM_INS_BLX,
        ARM_INS_BX,
        ARM_INS_CMP,
        ARM_INS_LDR,
        ARM_INS_LDRB,
        ARM_INS_LDRH,
        ARM_INS_MOV,
        ARM_INS_POP,
        ARM_INS_PUSH,
        ARM_INS_SUB,
        ARM_OP_IMM,
        ARM_OP_MEM,
        ARM_OP_REG,
    )
except ImportError as error:  # pragma: no cover - exercised by CLI diagnostics
    raise RuntimeError("Capstone is required; use the project .venv Python environment") from error


K_MODULE_ARM7_MAIN = -4
K_MODULE_ARM7_FIRST_AUTOLOAD = -5


class AnalysisError(ValueError):
    """Malformed input or an ARM7 autoload table that cannot be represented."""


@dataclass(frozen=True)
class AutoloadBlock:
    destination: int
    load_size: int
    bss_size: int
    source_offset: int
    source_bytes: bytes

    @property
    def end(self) -> int:
        return self.destination + self.load_size + self.bss_size


@dataclass(frozen=True)
class AutoloadLayout:
    params_offset: int
    list_start: int
    list_end: int
    source_address: int
    blocks: tuple[AutoloadBlock, ...]
    static_bss_start: int | None
    static_bss_end: int | None


@dataclass
class ModuleState:
    name: str
    module_id: int
    base: int
    image: bytes
    loaded_size: int
    executable_ranges: list[tuple[int, int]]
    data_ranges: list[tuple[int, int, str]] = field(default_factory=list)
    functions: dict[tuple[int, int, bool], dict[str, Any]] = field(default_factory=dict)
    function_code: dict[tuple[int, int, bool], set[int]] = field(default_factory=dict)
    function_labels: dict[tuple[int, int, bool], set[tuple[int, bool]]] = field(default_factory=dict)
    function_calls: dict[tuple[int, int, bool], set[tuple[int, int, int]]] = field(default_factory=dict)
    function_evidence: dict[tuple[int, int, bool], set[str]] = field(default_factory=dict)
    function_data: dict[tuple[int, int, bool], set[int]] = field(default_factory=dict)
    invalid_reachable: dict[tuple[int, int, bool], set[int]] = field(default_factory=dict)
    routine_names: dict[int, str] = field(default_factory=dict)

    def contains(self, address: int, *, code: bool = False) -> bool:
        offset = address - self.base
        if not 0 <= offset < self.loaded_size:
            return False
        if not code:
            return True
        return (
            any(start <= offset < end for start, end in self.executable_ranges)
            and not any(start <= offset < end for start, end, _ in self.data_ranges)
        )

    def add_data_range(self, start_address: int, size: int, reason: str) -> None:
        if size <= 0:
            return
        start = max(start_address, self.base)
        end = min(start_address + size, self.base + len(self.image))
        if start < end:
            self.data_ranges.append((start - self.base, end - self.base, reason))


@dataclass
class AnalysisResult:
    modules: list[ModuleState]
    report: str


def _word(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _half(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _image_offset(address: int, base: int, size: int) -> int | None:
    offset = address - base
    return offset if 0 <= offset < size else None


def _image_boundary_offset(address: int, base: int, size: int) -> int | None:
    offset = address - base
    return offset if 0 <= offset <= size else None


def _valid_range(start: int, end: int, base: int, size: int) -> bool:
    return base <= start <= end <= base + size


def parse_autoload_layout(image: bytes, base: int) -> AutoloadLayout:
    """Find the crt0-referenced ``{list begin, list end, source}`` parameters.

    NitroSDK ARM7 crt0 uses three absolute pointers followed by the static BSS
    start/end words. Since SDK revisions move the parameter block, locate it by
    validating the list and its contiguous source payload instead of relying on
    a fixed file offset.
    """
    if len(image) < 12:
        raise AnalysisError("ARM7 image is too small to contain module parameters")

    candidates: list[tuple[int, int, AutoloadLayout]] = []
    referenced_addresses = {_word(image, offset) for offset in range(0, len(image) - 3, 4)}
    for params_offset in range(0, len(image) - 11, 4):
        list_start_addr, list_end_addr, source_address = struct.unpack_from("<III", image, params_offset)
        list_start = _image_offset(list_start_addr, base, len(image))
        list_end = _image_boundary_offset(list_end_addr, base, len(image))
        source_offset = _image_offset(source_address, base, len(image))
        if list_start is None or list_end is None or source_offset is None:
            continue
        if list_end <= list_start or list_end - list_start > 0x1000 or (list_end - list_start) % 12:
            continue
        entry_count = (list_end - list_start) // 12
        if entry_count == 0 or entry_count > 128:
            continue

        cursor = source_offset
        blocks: list[AutoloadBlock] = []
        valid = True
        previous_ranges: list[tuple[int, int]] = []
        for entry_offset in range(list_start, list_end, 12):
            destination, load_size, bss_size = struct.unpack_from("<III", image, entry_offset)
            if destination == 0 or load_size == 0 or destination + load_size + bss_size > 0x100000000:
                valid = False
                break
            if cursor > len(image) or load_size > len(image) - cursor:
                valid = False
                break
            block_end = destination + load_size + bss_size
            if any(destination < old_end and old_start < block_end for old_start, old_end in previous_ranges):
                valid = False
                break
            previous_ranges.append((destination, block_end))
            blocks.append(AutoloadBlock(destination, load_size, bss_size, cursor, image[cursor : cursor + load_size]))
            cursor += load_size
        if not valid or cursor > list_start:
            continue

        static_bss_start: int | None = None
        static_bss_end: int | None = None
        if params_offset + 20 <= len(image):
            candidate_start, candidate_end = struct.unpack_from("<II", image, params_offset + 12)
            if candidate_start <= candidate_end and base <= candidate_start <= base + len(image) and base <= candidate_end <= base + len(image):
                static_bss_start, static_bss_end = candidate_start, candidate_end

        # Prefer exact payload/list adjacency, then parameter addresses that
        # occur as aligned words anywhere in the supplied image.
        score = 100 if cursor == list_start else 0
        params_address = base + params_offset
        if params_address in referenced_addresses:
            score += 20
        if static_bss_start is not None:
            score += 5
        layout = AutoloadLayout(
            params_offset=params_offset,
            list_start=base + list_start,
            list_end=base + list_end,
            source_address=source_address,
            blocks=tuple(blocks),
            static_bss_start=static_bss_start,
            static_bss_end=static_bss_end,
        )
        candidates.append((score, params_offset, layout))

    if not candidates:
        raise AnalysisError("could not locate a valid ARM7 autoload list/module-parameter block")
    candidates.sort(key=lambda item: (-item[0], item[1]))
    best_score = candidates[0][0]
    best = [candidate for candidate in candidates if candidate[0] == best_score]
    if len(best) > 1:
        # Equivalent overlapping sightings are unusual; require a unique
        # descriptor rather than silently copying the wrong payload.
        offsets = ", ".join(f"0x{candidate[1]:X}" for candidate in best)
        raise AnalysisError(f"ambiguous ARM7 module-parameter blocks at offsets {offsets}")
    return candidates[0][2]


def build_modules(image: bytes, layout: AutoloadLayout, base: int) -> list[ModuleState]:
    """Materialize guest-visible main and autoload memory regions."""
    main_image = bytearray(image)
    if layout.static_bss_start is not None and layout.static_bss_end is not None:
        if _valid_range(layout.static_bss_start, layout.static_bss_end, base, len(main_image)):
            start = layout.static_bss_start - base
            end = layout.static_bss_end - base
            main_image[start:end] = bytes(end - start)

    source_offset = layout.source_address - base
    main = ModuleState(
        name="arm7main",
        module_id=K_MODULE_ARM7_MAIN,
        base=base,
        image=bytes(main_image),
        loaded_size=len(main_image),
        executable_ranges=[(0, len(main_image))],
    )
    # Packed autoload bytes are data at their source address. Do not assume
    # that unrelated bytes after the payload are non-executable.
    source_size = sum(block.load_size for block in layout.blocks)
    main.add_data_range(base + source_offset, source_size, "autoload source payload")
    main.add_data_range(layout.list_start, layout.list_end - layout.list_start, "autoload block descriptors")
    params_size = 20 if layout.static_bss_start is not None else 12
    main.add_data_range(base + layout.params_offset, params_size, "autoload/module parameter words")
    if layout.static_bss_start is not None and layout.static_bss_end is not None:
        main.add_data_range(layout.static_bss_start, layout.static_bss_end - layout.static_bss_start, "crt0-cleared static BSS")

    modules = [main]
    for index, block in enumerate(layout.blocks):
        name = f"arm7autoload_{block.destination:08x}"
        module_id = K_MODULE_ARM7_FIRST_AUTOLOAD - index
        region = block.source_bytes + bytes(block.bss_size)
        module = ModuleState(
            name=name,
            module_id=module_id,
            base=block.destination,
            image=region,
            loaded_size=block.load_size,
            executable_ranges=[(0, block.load_size)],
        )
        module.add_data_range(block.destination + block.load_size, block.bss_size, "autoload block BSS zeroed by crt0")
        modules.append(module)
    return modules


def _module_for_address(modules: Sequence[ModuleState], address: int, *, code: bool = False) -> tuple[int, ModuleState] | None:
    for index, module in enumerate(modules):
        if module.contains(address, code=code):
            return index, module
    return None


def _instruction_mode(thumb: bool) -> int:
    return CS_MODE_THUMB if thumb else CS_MODE_ARM


def _decoder(thumb: bool) -> Cs:
    decoder = Cs(CS_ARCH_ARM, CS_MODE_LITTLE_ENDIAN | _instruction_mode(thumb))
    decoder.detail = True
    return decoder


def _read_guest_word(modules: Sequence[ModuleState], address: int) -> int | None:
    located = _module_for_address(modules, address)
    if located is None:
        return None
    _, module = located
    offset = address - module.base
    if offset & 3 or offset + 4 > len(module.image):
        return None
    return _word(module.image, offset)


def _read_guest_byte(modules: Sequence[ModuleState], address: int) -> int | None:
    located = _module_for_address(modules, address)
    if located is None:
        return None
    _, module = located
    return module.image[address - module.base]


def _entry_bytes(module: ModuleState, address: int, thumb: bool) -> bytes:
    offset = address - module.base
    alignment = 2 if thumb else 4
    if offset < 0 or offset >= module.loaded_size or offset % alignment:
        return b""
    return module.image[offset : min(module.loaded_size, offset + 24)]


def _is_prologue(module: ModuleState, address: int, thumb: bool) -> bool:
    offset = address - module.base
    if offset < 0 or offset >= module.loaded_size:
        return False
    if thumb:
        if offset + 2 > module.loaded_size:
            return False
        half = _half(module.image, offset)
        if (half & 0xFE00) == 0xB400:
            # A PUSH of only caller-saved registers is not a reliable
            # prologue: it is also common as an in-function stack temporary.
            return bool((half & 0x0100) or (half & 0x00F0))
        return (half & 0xFF80) == 0xB080
    if offset + 4 > module.loaded_size:
        return False
    word = _word(module.image, offset)
    return (word & 0xFFFF0000) == 0xE92D0000 or (word & 0xFFFFF000) == 0xE24DD000


def _plausible_entry(module: ModuleState, address: int, thumb: bool) -> bool:
    if not _is_prologue(module, address, thumb):
        return False
    code = _entry_bytes(module, address, thumb)
    decoded = list(_decoder(thumb).disasm(code, address, count=8))
    if not decoded or decoded[0].address != address:
        return False
    # Reject prologue-shaped literal/data words that immediately decode into an
    # invalid/truncated instruction stream. A short leaf return is acceptable.
    count = 0
    for instruction in decoded:
        if instruction.address != address + sum(item.size for item in decoded[:count]):
            break
        count += 1
        if instruction.id in (ARM_INS_BX,) and instruction.op_str.strip() in ("lr", "r14"):
            break
        if instruction.id == ARM_INS_POP and "pc" in instruction.op_str:
            break
    return count >= 2


def _plausible_leaf(module: ModuleState, address: int, thumb: bool) -> bool:
    """Validate a no-prologue ARM entry by decoding a bounded body to an
    explicit return or tail branch. Callback targets need not be short thunks.
    """
    if thumb:
        return False
    offset = address - module.base
    if offset < 0 or offset % 4 or offset + 4 > module.loaded_size:
        return False
    lookahead = 32
    code = module.image[offset : min(module.loaded_size, offset + 4 * lookahead)]
    decoded = list(_decoder(False).disasm(code, address, count=lookahead))
    if not decoded or decoded[0].address != address or _is_conditional(decoded[0]):
        return False
    for index, instruction in enumerate(decoded):
        if instruction.address != address + 4 * index:
            return False
        if instruction.id == ARM_INS_BX and not _is_conditional(instruction):
            return True
        if instruction.id == ARM_INS_POP and "pc" in instruction.op_str:
            return True
        if instruction.id == ARM_INS_B and not _is_conditional(instruction):
            return True
    return False


def _literal_address(instruction: Any, thumb: bool) -> int | None:
    if instruction.id not in (ARM_INS_LDR, ARM_INS_LDRB) or len(instruction.operands) < 2:
        return None
    operand = instruction.operands[1]
    if operand.type != ARM_OP_MEM or instruction.reg_name(operand.mem.base) != "pc" or operand.mem.index:
        return None
    pc = ((instruction.address + 4) & ~3) if thumb else instruction.address + 8
    return pc + operand.mem.disp


def _written_registers(instruction: Any) -> set[str]:
    try:
        _, written = instruction.regs_access()
    except Exception:
        return set()
    return {instruction.reg_name(register) for register in written}


def _operand_register(instruction: Any, operand_index: int = 0) -> str | None:
    if len(instruction.operands) <= operand_index:
        return None
    operand = instruction.operands[operand_index]
    return instruction.reg_name(operand.reg) if operand.type == ARM_OP_REG else None


def _operand_immediate(instruction: Any, operand_index: int = 0) -> int | None:
    if len(instruction.operands) <= operand_index:
        return None
    operand = instruction.operands[operand_index]
    return operand.imm if operand.type == ARM_OP_IMM else None


def _normalize_code_target(modules: Sequence[ModuleState], value: int, default_thumb: bool) -> tuple[int, bool, int] | None:
    thumb = bool(value & 1) if value & 1 else default_thumb
    address = value & ~1
    located = _module_for_address(modules, address, code=True)
    if located is None:
        return None
    module_index, _ = located
    alignment = 2 if thumb else 4
    if address % alignment:
        return None
    return module_index, address, thumb


def _branch_immediate(instruction: Any) -> int | None:
    for operand in instruction.operands:
        if operand.type == ARM_OP_IMM:
            return operand.imm
    return None


def _is_conditional(instruction: Any) -> bool:
    return instruction.cc not in (0, 15)


def _relative_branch_target(word: int, address: int) -> int | None:
    if word & 0x0E000000 != 0x0A000000:
        return None
    displacement = word & 0x00FFFFFF
    if displacement & 0x00800000:
        displacement -= 0x01000000
    return (address + 8 + (displacement << 2)) & 0xFFFFFFFF


def _candidate_switch_words(
    modules: Sequence[ModuleState],
    table_address: int,
    base: int,
    limit: int = 64,
    *,
    exact_count: bool = False,
) -> list[tuple[int, int]]:
    targets: list[tuple[int, int]] = []
    for index in range(limit):
        slot = table_address + index * 4
        word = _read_guest_word(modules, slot)
        if word is None:
            break
        candidates = [word]
        signed = word if word < 0x80000000 else word - 0x100000000
        candidates.extend((base + signed, slot + 8 + (signed << 2)))
        target_info = None
        for candidate in candidates:
            target_info = _normalize_code_target(modules, candidate, False)
            if target_info is not None:
                break
        if target_info is None:
            branch_target = _relative_branch_target(word, slot)
            if branch_target is not None:
                target_info = _normalize_code_target(modules, branch_target, False)
        if target_info is None:
            if exact_count:
                continue
            break
        module_index, target, thumb = target_info
        targets.append((module_index, target | int(thumb), slot))
    return targets


class Arm7Analyzer:
    """Recursive-descent ARM/Thumb analyzer for the manifest's loaded regions."""

    def __init__(self, modules: list[ModuleState]) -> None:
        self.modules = modules
        self.decoders = {False: _decoder(False), True: _decoder(True)}
        self.pending: deque[tuple[int, int, bool, str]] = deque()
        self.queued_functions: set[tuple[int, int, bool]] = set()
        self.scheduled: dict[tuple[int, int, bool], set[str]] = defaultdict(set)
        self.visited_functions: set[tuple[int, int, bool]] = set()
        self.unresolved_targets: set[tuple[int, int, str]] = set()
        self.decode_anomalies: list[tuple[int, int, bool, int]] = []
        self.overlaps: list[tuple[int, int, bool, int, bool]] = []

    def _mark_data(self, address: int, size: int, reason: str) -> None:
        located = _module_for_address(self.modules, address)
        if located is None:
            return
        _, module = located
        module.add_data_range(address, size, reason)

    def schedule(self, module_index: int, address: int, thumb: bool, evidence: str) -> None:
        module = self.modules[module_index]
        alignment = 2 if thumb else 4
        if address & 1 or address % alignment or not module.contains(address, code=True):
            return
        key = (module_index, address, thumb)
        if evidence not in self.scheduled[key]:
            self.scheduled[key].add(evidence)
        if key not in self.visited_functions and key not in self.queued_functions:
            self.pending.append((module_index, address, thumb, evidence))
            self.queued_functions.add(key)

    def _locate_target(self, value: int, thumb_default: bool) -> tuple[int, int, bool] | None:
        return _normalize_code_target(self.modules, value, thumb_default)

    def _scan_pointer_tables(self) -> None:
        seen: set[tuple[int, int, bool]] = set(self.scheduled)
        validated: set[tuple[int, int, bool]] = set()
        for source_index, source in enumerate(self.modules):
            scan_end = min(source.loaded_size, len(source.image))
            for offset in range(0, scan_end - 3, 4):
                value = _word(source.image, offset)
                target = self._locate_target(value, False)
                if target is None:
                    continue
                target_index, target_address, thumb = target
                key = (target_index, target_address, thumb)
                if key not in validated:
                    if not (
                        _plausible_entry(self.modules[target_index], target_address, thumb)
                        or _plausible_leaf(self.modules[target_index], target_address, thumb)
                    ):
                        continue
                    validated.add(key)
                # Every validated occurrence is data, even when its target
                # was already scheduled by another module's pointer table.
                source.add_data_range(source.base + offset, 4, "validated function-pointer table entry")
                if key not in seen:
                    self.schedule(target_index, target_address, thumb, f"validated function pointer at 0x{source.base + offset:08X}")
                    seen.add(key)

    def _scan_prologues(self) -> None:
        for module_index, module in enumerate(self.modules):
            data_ranges = _merge_ranges((start, end) for start, end, _ in module.data_ranges)
            data_starts = [start for start, _ in data_ranges]
            for start, end in module.executable_ranges:
                end = min(end, module.loaded_size)
                for offset in range(start, end - 1, 2):
                    data_index = bisect_right(data_starts, offset) - 1
                    if data_index >= 0 and offset < data_ranges[data_index][1]:
                        continue
                    address = module.base + offset
                    if offset % 4 == 0 and _is_prologue(module, address, False):
                        if _plausible_entry(module, address, False):
                            self.schedule(module_index, address, False, "validated ARM prologue scan")
                    if _is_prologue(module, address, True) and _plausible_entry(module, address, True):
                        self.schedule(module_index, address, True, "validated Thumb prologue scan")

    def _read_literal(self, address: int, size: int = 4) -> int | None:
        located = _module_for_address(self.modules, address)
        if located is None:
            return None
        _, module = located
        offset = address - module.base
        if offset < 0 or offset + size > len(module.image):
            return None
        if size == 1:
            return module.image[offset]
        if size == 2:
            return _half(module.image, offset)
        return _word(module.image, offset)

    def _schedule_value(self, value: int, default_thumb: bool, evidence: str) -> tuple[int, int, bool] | None:
        target = self._locate_target(value, default_thumb)
        if target is None:
            return None
        module_index, address, thumb = target
        self.schedule(module_index, address, thumb, evidence)
        return target

    def _switch_entry_count(self, module_index: int, address: int) -> int | None:
        module = self.modules[module_index]
        offset = address - module.base
        if offset < 4:
            return None
        raw = module.image[offset - 4 : offset]
        compare = next(self.decoders[False].disasm(raw, address - 4, count=1), None)
        if compare is None or compare.id != ARM_INS_CMP:
            return None
        immediate = next(
            (operand.imm for operand in compare.operands[1:] if operand.type == ARM_OP_IMM),
            None,
        )
        return immediate + 1 if immediate is not None and 0 <= immediate < 64 else None

    def _switch_targets(self, module_index: int, instruction: Any, thumb: bool, registers: dict[str, Any]) -> list[tuple[int, int, bool]]:
        targets: list[tuple[int, int, bool]] = []
        if not thumb and instruction.id == ARM_INS_ADD and _operand_register(instruction, 0) == "pc":
            if len(instruction.operands) >= 3 and instruction.reg_name(instruction.operands[1].reg) == "pc":
                # `add pc, pc, rX, lsl #2`: each slot at pc+8 is itself an
                # executable instruction (normally `b case`), so the slots
                # are the jump targets and stay code.
                table = instruction.address + 8
                count = self._switch_entry_count(module_index, instruction.address)
                if count is None:
                    count = 0
                    while count < 256:
                        word = _read_guest_word(self.modules, table + count * 4)
                        if word is None or (word >> 24) != 0xEA:
                            break
                        count += 1
                if count == 0:
                    # Computed jump into straight-line code (unrolled divide):
                    # continue decoding linearly from pc+8.
                    count = 1
                for index in range(count):
                    target = _normalize_code_target(self.modules, table + index * 4, False)
                    if target is not None:
                        targets.append(target)
        # Thumb/GCC switch idiom: add rBase, pc; ldrb rOff,[rBase,rIndex];
        # add pc,rOff. The table's base is propagated as a symbolic byte-table value.
        if thumb and instruction.id == ARM_INS_ADD and _operand_register(instruction, 0) == "pc" and len(instruction.operands) >= 2:
            source = _operand_register(instruction, 1)
            value = registers.get(source or "")
            if isinstance(value, tuple) and value and value[0] == "byte-table":
                table_address = value[1]
                pc = instruction.address + 4
                entries: list[int] = []
                for slot in range(256):
                    byte_value = self._read_literal(table_address + slot, 1)
                    if byte_value is None:
                        break
                    candidate = pc + byte_value
                    target = self._locate_target(candidate, True)
                    if target is None:
                        break
                    entries.append(byte_value)
                    targets.append(target)
                if entries:
                    self._mark_data(table_address, len(entries), "Thumb byte-offset switch table")
        return targets

    def _walk_function(self, module_index: int, start: int, thumb: bool) -> None:
        module = self.modules[module_index]
        key = (module_index, start, thumb)
        self.visited_functions.add(key)
        module.function_code[key] = set()
        module.function_data[key] = set()
        module.function_labels[key] = set()
        module.function_calls[key] = set()
        module.function_evidence[key] = set(self.scheduled[key])
        module.invalid_reachable[key] = set()

        # Join states conservatively at control-flow merges. This removes values
        # that vary in loops instead of enumerating an unbounded counter range.
        work: deque[tuple[int, dict[str, Any]]] = deque([(start, {})])
        joined_states: dict[int, dict[str, Any]] = {}
        instruction_count = 0
        while work and instruction_count < 100000:
            pc, constants = work.popleft()
            if not module.contains(pc, code=True):
                continue
            if pc in joined_states:
                previous = joined_states[pc]
                merged = {register: value for register, value in previous.items() if constants.get(register) == value}
                if merged == previous:
                    continue
                joined_states[pc] = merged
                constants = merged.copy()
            else:
                joined_states[pc] = constants.copy()
            offset = pc - module.base
            raw = module.image[offset : min(module.loaded_size, offset + 4)]
            decoded = next(self.decoders[thumb].disasm(raw, pc, count=1), None)
            if decoded is None or decoded.address != pc:
                module.invalid_reachable[key].add(pc)
                self.decode_anomalies.append((module_index, start, thumb, pc))
                continue
            instruction_count += 1
            instruction_end = pc + decoded.size
            module.function_code[key].update(range(pc, instruction_end))

            literal_address = _literal_address(decoded, thumb)
            tracked_writes: set[str] = set()
            if literal_address is not None:
                literal_size = 1 if decoded.id == ARM_INS_LDRB else 4
                loaded_value = self._read_literal(literal_address, literal_size)
                self._mark_data(literal_address, literal_size, "PC-relative literal load")
                literal_owner = _module_for_address(self.modules, literal_address)
                if literal_owner is not None and literal_owner[0] == module_index:
                    module.function_data[key].update(range(literal_address, literal_address + literal_size))
                destination = _operand_register(decoded, 0)
                if destination and destination != "pc":
                    if decoded.id == ARM_INS_LDRB and loaded_value is None:
                        constants[destination] = ("byte-table", literal_address)
                        tracked_writes.add(destination)
                    elif loaded_value is not None:
                        constants[destination] = loaded_value
                        tracked_writes.add(destination)
                    else:
                        constants.pop(destination, None)

            # Track only values established by instructions we fully understand.
            if decoded.id == ARM_INS_MOV and len(decoded.operands) > 1:
                destination = _operand_register(decoded, 0)
                if destination and destination != "pc":
                    if decoded.operands[1].type == ARM_OP_IMM:
                        constants[destination] = decoded.operands[1].imm
                        tracked_writes.add(destination)
                    elif decoded.operands[1].type == ARM_OP_REG:
                        source = decoded.reg_name(decoded.operands[1].reg)
                        if source in constants:
                            constants[destination] = constants[source]
                            tracked_writes.add(destination)
                        else:
                            constants.pop(destination, None)
                    else:
                        constants.pop(destination, None)
            elif decoded.id in (ARM_INS_ADD, ARM_INS_SUB) and len(decoded.operands) >= 2:
                destination = _operand_register(decoded, 0)
                source = _operand_register(decoded, 1)
                immediate = _operand_immediate(decoded, 2)
                if destination and destination != "pc":
                    pc_relative = (
                        decoded.id == ARM_INS_ADD
                        and source == "pc"
                        and (len(decoded.operands) < 3 or decoded.operands[2].type == ARM_OP_IMM)
                    )
                    if pc_relative:
                        pc_value = ((pc + 4) & ~3) if thumb else pc + 8
                        constants[destination] = pc_value + (immediate or 0)
                        tracked_writes.add(destination)
                    elif source in constants and isinstance(constants[source], int) and immediate is not None:
                        delta = immediate if decoded.id == ARM_INS_ADD else -immediate
                        constants[destination] = constants[source] + delta
                        tracked_writes.add(destination)
                    else:
                        constants.pop(destination, None)
            elif decoded.id == ARM_INS_LDRB and literal_address is None and len(decoded.operands) > 1:
                destination = _operand_register(decoded, 0)
                memory = decoded.operands[1]
                if destination and destination != "pc" and memory.type == ARM_OP_MEM:
                    base_register = decoded.reg_name(memory.mem.base)
                    base_value = constants.get(base_register)
                    if isinstance(base_value, int) and memory.mem.index:
                        constants[destination] = ("byte-table", base_value + memory.mem.disp)
                        tracked_writes.add(destination)
                    else:
                        constants.pop(destination, None)

            for register in _written_registers(decoded):
                if register in ("pc", "cpsr", "apsr") or register in tracked_writes:
                    continue
                constants.pop(register, None)

            # Calls are separate functions; execution continues after the call.
            if decoded.id in (ARM_INS_BL, ARM_INS_BLX):
                immediate = _branch_immediate(decoded)
                if immediate is not None:
                    call_thumb = (not thumb) if decoded.id == ARM_INS_BLX else thumb
                    value = immediate | int(call_thumb)
                    target = self._schedule_value(value, call_thumb, f"direct call from 0x{pc:08X}")
                    if target is None:
                        self.unresolved_targets.add((pc, immediate, "call"))
                    else:
                        target_index, target_address, target_thumb = target
                        module.function_calls[key].add((pc, target_address | int(target_thumb), self.modules[target_index].module_id))
                elif decoded.id == ARM_INS_BLX:
                    source = _operand_register(decoded, 0)
                    value = constants.get(source or "")
                    if isinstance(value, int):
                        target = self._schedule_value(value, False, f"indirect call from 0x{pc:08X}")
                        if target:
                            target_index, target_address, target_thumb = target
                            module.function_calls[key].add((pc, target_address | int(target_thumb), self.modules[target_index].module_id))
                        else:
                            self.unresolved_targets.add((pc, value & ~1, "indirect call"))
                for register in ("r0", "r1", "r2", "r3", "r12", "ip", "lr"):
                    constants.pop(register, None)
                work.append((instruction_end, constants.copy()))
                continue

            # Conditional PC writes fall through; `mov lr, pc; bx rX` is an
            # indirect call that also continues after the BX.
            falls_through = _is_conditional(decoded)
            if decoded.id == ARM_INS_BX and not thumb and pc - 4 >= module.base:
                falls_through = falls_through or _word(module.image, pc - 4 - module.base) == 0xE1A0E00F
            elif decoded.id == ARM_INS_BX and thumb and pc - 2 >= module.base:
                falls_through = falls_through or _half(module.image, pc - 2 - module.base) == 0x46FE
            if falls_through and ("pc" in _written_registers(decoded) or decoded.id == ARM_INS_BX):
                work.append((instruction_end, {}))

            # Return instructions terminate a path. BX/BLX register veneers are
            # resolved only when preceding literal loads or simple arithmetic
            # established a concrete target.
            if decoded.id == ARM_INS_BX:
                source = _operand_register(decoded, 0)
                if source in ("lr", "r14"):
                    continue
                value = constants.get(source or "")
                if isinstance(value, int):
                    target = self._schedule_value(value, False, f"indirect branch from 0x{pc:08X}")
                    if target is None:
                        self.unresolved_targets.add((pc, value & ~1, "indirect branch"))
                    else:
                        target_index, target_address, target_thumb = target
                        if target_index == module_index:
                            module.function_labels[key].add((target_address, target_thumb))
                elif source == "pc":
                    value = ((pc + 4) & ~3) if thumb else pc + 8
                    target = self._schedule_value(value, False, f"BX PC veneer at 0x{pc:08X}")
                    if target:
                        module.function_labels[key].add((target[1], target[2]))
                continue

            # Loads into PC and POP/LDM returns need explicit treatment because
            # Capstone may classify them as generic writes to the PC.
            if decoded.id == ARM_INS_LDR and _operand_register(decoded, 0) == "pc":
                target_value = self._read_literal(literal_address, 4) if literal_address is not None else None
                if target_value is not None:
                    target = self._schedule_value(target_value, thumb, f"literal branch at 0x{pc:08X}")
                    if target:
                        module.function_labels[key].add((target[1], target[2]))
                else:
                    table_address = None
                    if len(decoded.operands) > 1 and decoded.operands[1].type == ARM_OP_MEM:
                        memory = decoded.operands[1].mem
                        if decoded.reg_name(memory.base) == "pc":
                            pc_base = ((pc + 4) & ~3) if thumb else pc + 8
                            table_address = pc_base + memory.disp
                    if table_address is not None:
                        discovered = _candidate_switch_words(self.modules, table_address, pc)
                        if discovered:
                            self._mark_data(table_address, len(discovered) * 4, "ARM PC-indexed switch table")
                        for target_index, tagged, slot in discovered:
                            address, target_thumb = tagged & ~1, bool(tagged & 1)
                            module.function_labels[key].add((address, target_thumb))
                            if target_index == module_index:
                                work.append((address, constants.copy()))
                            else:
                                self.schedule(target_index, address, target_thumb, f"switch target at 0x{slot:08X}")
                continue
            if decoded.id == ARM_INS_POP and "pc" in decoded.op_str:
                continue

            # Resolve classic ARM absolute/relative switch dispatches.
            switch_targets = self._switch_targets(module_index, decoded, thumb, constants)
            if switch_targets:
                for target_index, target_address, target_thumb in switch_targets:
                    module.function_labels[key].add((target_address, target_thumb))
                    if target_index == module_index:
                        work.append((target_address, constants.copy()))
                    else:
                        self.schedule(target_index, target_address, target_thumb, f"switch target at 0x{pc:08X}")
                continue

            # Conditional branches have both paths; unconditional branches have
            # only the target. Direct targets remain labels in the containing fn.
            is_branch = decoded.id == ARM_INS_B
            if is_branch:
                target = _branch_immediate(decoded)
                if target is None:
                    continue
                conditional = decoded.cc not in (0, 15) or decoded.mnemonic not in ("b", "bal")
                branch_thumb = thumb
                normalized = _normalize_code_target(self.modules, target | int(branch_thumb), branch_thumb)
                if normalized is not None:
                    target_index, target_address, target_thumb = normalized
                    target_key = (target_index, target_address, target_thumb)
                    if target_index == module_index:
                        module.function_labels[key].add((target_address, target_thumb))
                        if target_address == start and target_thumb == thumb:
                            pass
                        elif target_key in self.scheduled or _plausible_entry(module, target_address, target_thumb):
                            self.schedule(target_index, target_address, target_thumb, f"tail branch from 0x{pc:08X}")
                        else:
                            work.append((target_address, constants.copy()))
                    else:
                        self.schedule(target_index, target_address, target_thumb, f"cross-module branch from 0x{pc:08X}")
                elif target != 0:
                    self.unresolved_targets.add((pc, target, "branch"))
                if conditional:
                    work.append((instruction_end, constants.copy()))
                continue

            # A non-linking explicit PC write terminates or redispatches control.
            writes_pc = "pc" in _written_registers(decoded)
            if writes_pc:
                for switch_index, target_address, target_thumb in switch_targets:
                    self.schedule(switch_index, target_address, target_thumb, f"PC write from 0x{pc:08X}")
                continue

            work.append((instruction_end, constants.copy()))

        if instruction_count >= 100000:
            self.decode_anomalies.append((module_index, start, thumb, start))

    def discover(self, seeds: Iterable[tuple[int, bool, str]]) -> None:
        for address, thumb, evidence in seeds:
            located = _module_for_address(self.modules, address & ~1, code=True)
            if located is not None:
                self.schedule(located[0], address & ~1, thumb or bool(address & 1), evidence)
        # Discovered direct calls are the primary source. Tables and valid entry
        # prologues provide roots for callback-driven SDK routines.
        self._scan_pointer_tables()
        self._scan_prologues()
        rounds = 0
        while self.pending and rounds < 100000:
            module_index, address, thumb, _ = self.pending.popleft()
            key = (module_index, address, thumb)
            self.queued_functions.discard(key)
            if key in self.visited_functions:
                continue
            self._walk_function(module_index, address, thumb)
            rounds += 1
        # Drain any roots deferred by the safety cap above.
        while self.pending:
            module_index, address, thumb, _ = self.pending.popleft()
            key = (module_index, address, thumb)
            self.queued_functions.discard(key)
            if key not in self.visited_functions:
                self._walk_function(module_index, address, thumb)

    def finalize(self) -> None:
        owners: dict[tuple[int, int], list[tuple[int, bool]]] = defaultdict(list)
        for module_index, module in enumerate(self.modules):
            for key, code_addresses in module.function_code.items():
                address, thumb = key[1], key[2]
                if code_addresses:
                    covered = code_addresses | module.function_data.get(key, set())
                    end = max(covered) + 1
                    for code_address in code_addresses:
                        owners[(module_index, code_address)].append((address, thumb))
                    size = max(1, end - address)
                else:
                    size = 0
                module.functions[key] = {
                    "addr": address,
                    "thumb": thumb,
                    "size": size,
                    "name": function_name(module, address),
                }
            for (owner_module, code_address), function_keys in owners.items():
                if owner_module != module_index or len(function_keys) < 2:
                    continue
                distinct = sorted(set(function_keys))
                for first, second in zip(distinct, distinct[1:]):
                    if first[0] != second[0] or first[1] != second[1]:
                        self.overlaps.append((module_index, first[0], first[1], second[0], second[1]))
                        break


def function_name(module: ModuleState, address: int) -> str:
    return module.routine_names.get(address, f"func_{address:08X}")


def _merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for start, end in sorted((start, end) for start, end in ranges if start < end):
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged]


def _coverage(module: ModuleState) -> tuple[int, int, int, list[tuple[int, int]]]:
    code = set()
    for addresses in module.function_code.values():
        code.update(addresses)
    code_offsets = {address - module.base for address in code if module.base <= address < module.base + len(module.image)}
    data_ranges = _merge_ranges((start, end) for start, end, _ in module.data_ranges)
    data_offsets: set[int] = set()
    for start, end in data_ranges:
        for offset in range(start, end):
            if offset not in code_offsets:
                data_offsets.add(offset)
    unknown = set(range(len(module.image))) - code_offsets - data_offsets
    gaps = _merge_ranges((start, end) for start, end in _offset_runs(unknown))
    return len(code_offsets), len(data_offsets), len(unknown), gaps


def _offset_runs(offsets: set[int]) -> list[tuple[int, int]]:
    if not offsets:
        return []
    ordered = sorted(offsets)
    result: list[tuple[int, int]] = []
    start = previous = ordered[0]
    for offset in ordered[1:]:
        if offset != previous + 1:
            result.append((start, previous + 1))
            start = offset
        previous = offset
    result.append((start, previous + 1))
    return result


def _code_looking(module: ModuleState, start: int, end: int) -> tuple[bool, str]:
    length = end - start
    sample_end = min(end, start + 48)
    if _is_prologue(module, module.base + start, False) or _is_prologue(module, module.base + start, True):
        return True, "begins with a stack-saving function prologue; no discovered root reaches it"
    bytes_to_decode = module.image[start:sample_end]
    for thumb in (False, True):
        decoded = list(_decoder(thumb).disasm(bytes_to_decode, module.base + start, count=12))
        if len(decoded) >= 6 and any(insn.id in (ARM_INS_B, ARM_INS_BL, ARM_INS_BLX) for insn in decoded):
            return True, "a short ARM/Thumb decode contains several instructions and control flow, but no validated entry was found"
    if length >= 16 and bytes(module.image[start:sample_end]).count(0) >= length * 3 // 4:
        return False, "mostly zero-filled storage, consistent with unclassified BSS or alignment"
    return False, "not reachable from discovered code roots and not a validated function-pointer/prologue target; retained as unknown data or padding"


def _format_words(data: bytes, offset: int, size: int) -> str:
    chunk = data[offset : min(offset + size, offset + 16)]
    padded = chunk[: len(chunk) - (len(chunk) % 4)]
    words = [f"{value:08X}" for value in struct.unpack("<" + "I" * (len(padded) // 4), padded)] if padded else []
    tail = chunk[len(padded) :]
    if tail:
        words.append(tail.hex().upper())
    return " ".join(words) if words else "(empty)"


def _manifest(modules: list[ModuleState], image_paths: dict[str, Path]) -> dict[str, Any]:
    output_modules: list[dict[str, Any]] = []
    for module in modules:
        data_ranges = _merge_ranges((start, end) for start, end, _ in module.data_ranges)

        def scan_only(key: tuple[int, int, bool]) -> bool:
            evidence = module.function_evidence.get(key, set())
            return bool(evidence) and all("prologue scan" in item for item in evidence)

        strong_code: dict[bool, set[int]] = {False: set(), True: set()}
        for key, addresses in module.function_code.items():
            if not scan_only(key):
                strong_code[key[2]].update(addresses)
        # Prologue-scan-only hits inside literal pools/tables, or inside code
        # that a better-evidenced function decodes in the other instruction
        # set, are not functions.
        functions = sorted(
            (fn for key, fn in module.functions.items()
             if not scan_only(key) or (
                 not any(start <= fn["addr"] - module.base < end for start, end in data_ranges)
                 and fn["addr"] not in strong_code[not fn["thumb"]])),
            key=lambda fn: (fn["addr"], fn["thumb"]),
        )
        labels: set[tuple[int, bool]] = set()
        for key, targets in module.function_labels.items():
            labels.update((address, thumb) for address, thumb in targets if address != key[1])
        calls_by_target: dict[tuple[int, int], set[int]] = defaultdict(set)
        for calls in module.function_calls.values():
            for call_from, target, module_id in calls:
                calls_by_target[(call_from, target)].add(module_id)
        code_ranges: list[tuple[int, int]] = []
        for start, end in module.executable_ranges:
            end = min(end, module.loaded_size)
            cursor = start
            for data_start, data_end in data_ranges:
                if data_end <= cursor:
                    continue
                if data_start >= end:
                    break
                if cursor < data_start:
                    code_ranges.append((cursor, data_start))
                cursor = max(cursor, data_end)
                if cursor >= end:
                    break
            if cursor < end:
                code_ranges.append((cursor, end))
        output_modules.append(
            {
                "name": module.name,
                "module_id": module.module_id,
                "base": module.base,
                "image": str(image_paths[module.name].resolve()),
                "functions": functions,
                "labels": [{"addr": address, "thumb": thumb} for address, thumb in sorted(labels)],
                "data_ranges": [[module.base + start, module.base + end] for start, end in data_ranges],
                "code_ranges": [[module.base + start, module.base + end] for start, end in code_ranges],
                "calls": [
                    {"from": call_from, "to": target, "modules": sorted(module_ids)}
                    for (call_from, target), module_ids in sorted(calls_by_target.items())
                ],
            }
        )
    return {"cpu": "arm7", "arch": "v4t", "modules": output_modules}


def _build_report(
    image_path: Path,
    base: int,
    entry: int,
    callback: int,
    layout: AutoloadLayout,
    modules: list[ModuleState],
    analyzer: Arm7Analyzer,
) -> str:
    lines = [
        "ARM7 static analysis report",
        "===========================",
        f"Input image: {image_path.resolve()}",
        f"Loaded image base: 0x{base:08X}  size: 0x{len(modules[0].image):X} ({len(modules[0].image)} bytes)",
        f"Entry: 0x{entry:08X}  autoload callback: 0x{callback:08X}",
        "Architecture: ARMv4T (ARM and Thumb); recursive-descent disassembly with Capstone.",
        "",
        "Autoload layout",
        "----------------",
        f"Parameter block: file offset 0x{layout.params_offset:X} (guest 0x{base + layout.params_offset:08X})",
        f"Autoload list: 0x{layout.list_start:08X}..0x{layout.list_end:08X} ({len(layout.blocks)} blocks)",
        f"Payload source: 0x{layout.source_address:08X}",
    ]
    if layout.static_bss_start is not None and layout.static_bss_end is not None:
        lines.append(f"Static BSS cleared by crt0: 0x{layout.static_bss_start:08X}..0x{layout.static_bss_end:08X} ({layout.static_bss_end - layout.static_bss_start} bytes)")
    else:
        lines.append("Static BSS range: not identified from the parameter block")
    for index, block in enumerate(layout.blocks):
        lines.append(
            f"  block {index}: destination 0x{block.destination:08X}, load 0x{block.load_size:X}, "
            f"BSS 0x{block.bss_size:X}, image range 0x{block.destination:08X}..0x{block.end:08X}, "
            f"source offset 0x{block.source_offset:X}"
        )
    lines.extend(
        [
            "",
            "Module coverage",
            "---------------",
            "Coverage is a byte union: reachable decoded instructions, recognized literals/tables/BSS, then unknown.",
        ]
    )
    total_functions = 0
    for module in modules:
        function_count = len(module.functions)
        total_functions += function_count
        code_bytes, data_bytes, unknown_bytes, gaps = _coverage(module)
        lines.append(
            f"{module.name} (id {module.module_id}, base 0x{module.base:08X}, image {len(module.image)} bytes): "
            f"{function_count} functions; code {code_bytes} bytes ({100 * code_bytes / max(1, len(module.image)):.1f}%), "
            f"data {data_bytes} bytes ({100 * data_bytes / max(1, len(module.image)):.1f}%), "
            f"unknown {unknown_bytes} bytes ({100 * unknown_bytes / max(1, len(module.image)):.1f}%)."
        )
        if gaps:
            lines.append("  Remaining unknown gaps (address, size, first words; justification):")
            for start, end in gaps:
                code_looking, reason = _code_looking(module, start, end)
                kind = "code-looking" if code_looking else "not code-looking"
                lines.append(
                    f"    0x{module.base + start:08X} +0x{end - start:X}: "
                    f"{_format_words(module.image, start, end - start)} [{kind}: {reason}]"
                )
        else:
            lines.append("  No unknown gaps remain.")
        function_names = [fn["name"] for fn in module.functions.values() if fn["name"] != f"func_{fn['addr']:08X}"]
        if function_names:
            lines.append("  Named functions: " + ", ".join(sorted(set(function_names))))

    lines.extend(["", f"Total discovered functions: {total_functions}"])
    if analyzer.overlaps:
        unique_overlaps = sorted(set(analyzer.overlaps))
        lines.append("Function overlaps / explicit mid-function entries:")
        for module_index, first, first_thumb, second, second_thumb in unique_overlaps:
            lines.append(
                f"  {modules[module_index].name}: 0x{first:08X} ({'Thumb' if first_thumb else 'ARM'}) and "
                f"0x{second:08X} ({'Thumb' if second_thumb else 'ARM'}) reach shared instruction bytes; review as possible mid-function entry."
            )
    else:
        lines.append("Function overlaps: none among decoded instruction ownership sets.")

    if analyzer.decode_anomalies:
        lines.append(f"Reachable decode anomalies: {len(analyzer.decode_anomalies)}")
        for module_index, function_start, thumb, pc in sorted(set(analyzer.decode_anomalies)):
            lines.append(f"  {modules[module_index].name}: function 0x{function_start:08X} ({'Thumb' if thumb else 'ARM'}) reaches undecodable 0x{pc:08X}.")
    else:
        lines.append("Reachable decode anomalies: none recorded.")

    lines.extend(["", "Root provenance", "---------------"])
    for (module_index, address, thumb), evidence in sorted(analyzer.scheduled.items()):
        lines.append(
            f"  {modules[module_index].name}: 0x{address:08X} "
            f"({'Thumb' if thumb else 'ARM'}): " + "; ".join(sorted(evidence))
        )
    if analyzer.unresolved_targets:
        lines.extend(["", "Unresolved control-flow targets", "-------------------------------"])
        for source, target, kind in sorted(analyzer.unresolved_targets):
            lines.append(f"  {kind} from 0x{source:08X} to 0x{target:08X} is outside the supplied loaded code regions.")
    return "\n".join(lines) + "\n"


def analyze_image(
    image: bytes,
    *,
    base: int,
    entry: int,
    callback: int,
    image_path: Path | str = Path("arm7.bin"),
    extra_seeds: Sequence[tuple[int, bool, str]] = (),
    data_ranges: Sequence[tuple[int, int, str]] = (),
    routine_names: dict[int, str] | None = None,
) -> AnalysisResult:
    if base < 0 or base + len(image) > 0x100000000:
        raise AnalysisError("loaded image must fit within the 32-bit address space")
    layout = parse_autoload_layout(image, base)
    modules = build_modules(image, layout, base)
    for start, end, reason in data_ranges:
        if not 0 <= start < end <= 0x100000000:
            raise AnalysisError("metadata data ranges must have increasing 32-bit boundaries")
        for module in modules:
            module.add_data_range(start, end - start, reason)
    for address, name in (routine_names or {}).items():
        located = _module_for_address(modules, address & ~1)
        if located is None:
            raise AnalysisError(f"named routine 0x{address:08X} is outside the supplied modules")
        located[1].routine_names[address & ~1] = name
    seeds: list[tuple[int, bool, str]] = [
        (entry, bool(entry & 1), "caller-supplied ARM7 entry point"),
        (callback, bool(callback & 1), "caller-supplied autoload callback"),
    ]
    for address, thumb, reason in seeds:
        if _normalize_code_target(modules, address, thumb) is None:
            raise AnalysisError(f"{reason} is not an aligned address in a supplied code region")
    seeds.extend(extra_seeds)
    analyzer = Arm7Analyzer(modules)
    analyzer.discover(seeds)
    analyzer.finalize()
    report = _build_report(image_path, base, entry, callback, layout, modules, analyzer)
    return AnalysisResult(modules, report)


def write_outputs(result: AnalysisResult, out_dir: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    image_dir = out_dir / "arm7_images"
    image_dir.mkdir(parents=True, exist_ok=True)
    image_paths: dict[str, Path] = {}
    for module in result.modules:
        image_path = image_dir / f"{module.name}.bin"
        image_path.write_bytes(module.image)
        image_paths[module.name] = image_path
    manifest = _manifest(result.modules, image_paths)
    manifest_path = out_dir / "arm7.manifest.json"
    report_path = out_dir / "arm7_analysis.txt"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    report_path.write_text(result.report, encoding="utf-8")
    return manifest_path, report_path


def _parse_address(text: str) -> int:
    return int(text, 0)


def _read_metadata(path: Path) -> dict[str, Any]:
    """Read caller-owned layout hints; addresses may be integers or hex strings.

    Optional keys: base, entry, autoload_callback, extra_roots (address, thumb,
    reason), data_ranges (start, end, reason), and routine_names (address-to-name
    mapping). No layout hints or routine identities are built into this tool.
    """
    metadata = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(metadata, dict):
        raise AnalysisError("analysis metadata must be a JSON object")

    def address(value: Any) -> int:
        if isinstance(value, str):
            return _parse_address(value)
        if type(value) is int:
            return value
        raise AnalysisError("metadata addresses must be integers or integer strings")

    try:
        result = {
            key: address(metadata[key])
            for key in ("base", "entry", "autoload_callback") if key in metadata
        }
        result["extra_seeds"] = [
            (address(root["address"]), bool(root.get("thumb", False)), str(root.get("reason", "caller-supplied root")))
            for root in metadata.get("extra_roots", [])
        ]
        result["data_ranges"] = [
            (address(region["start"]), address(region["end"]), str(region.get("reason", "caller-supplied data range")))
            for region in metadata.get("data_ranges", [])
        ]
        result["routine_names"] = {
            address(key): name for key, name in metadata.get("routine_names", {}).items()
        }
        if any(not isinstance(name, str) or not name for name in result["routine_names"].values()):
            raise AnalysisError("routine names must be nonempty strings")
    except (KeyError, TypeError, AttributeError) as error:
        raise AnalysisError(f"invalid analysis metadata: {error}") from error
    return result


def executed_seeds(result: AnalysisResult, pcs: Sequence[tuple[int, bool]]) -> list[tuple[int, bool, str]]:
    """Function roots for executed instructions no discovered function decodes.

    Each maximal run of executed, undiscovered PCs (same mode, consecutive)
    starts at an instruction the reference run reached by a jump or call;
    BIOS addresses (< 0x4000) are ignored."""
    decoded: set[tuple[int, bool]] = set()
    for module in result.modules:
        for key, addresses in module.function_code.items():
            decoded.update((address, key[2]) for address in addresses)
    missing = sorted((pc, thumb) for pc, thumb in set(pcs) if pc >= 0x4000 and (pc, thumb) not in decoded)
    missing_set = set(missing)
    return [
        (pc | int(thumb), thumb, f"executed in the reference run (run start 0x{pc:08X})")
        for pc, thumb in missing
        if (pc - (2 if thumb else 4), thumb) not in missing_set
    ]


def _read_pcs(path: Path) -> list[tuple[int, bool]]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] in ("a", "t"):
            rows.append((int(parts[0], 16), parts[1] == "t"))
    return rows


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Analyze an extracted NitroSDK ARM7 image and emit an ARM7 recompiler manifest.")
    parser.add_argument("--arm7", required=True, type=Path, help="path to extracted ARM7 binary")
    parser.add_argument("--out", required=True, type=Path, help="output directory (use ignored build/gen)")
    parser.add_argument("--base", type=_parse_address, help="caller-supplied guest load base; required unless present in --metadata")
    parser.add_argument("--entry", type=_parse_address, help="caller-supplied guest entry address; required unless present in --metadata")
    parser.add_argument("--autoload-callback", type=_parse_address, help="caller-supplied autoload callback; required unless present in --metadata")
    parser.add_argument("--metadata", type=Path, help="private JSON layout hints: base, entry, autoload_callback, extra_roots, data_ranges, routine_names")
    parser.add_argument("--seed-pcs", type=Path, help="executed-PC list (`addr_hex a|t`) from a reference run; undiscovered runs become function roots")
    args = parser.parse_args(argv)
    try:
        metadata = _read_metadata(args.metadata) if args.metadata else {}
        values = {}
        for key in ("base", "entry", "autoload_callback"):
            value = getattr(args, key)
            if value is None:
                value = metadata.get(key)
            if value is None:
                parser.error(f"--{key.replace('_', '-')} is required unless supplied by --metadata")
            values[key] = value
        image = args.arm7.read_bytes()
        seeds = list(metadata.get("extra_seeds", ()))
        pcs = _read_pcs(args.seed_pcs) if args.seed_pcs else []
        for _ in range(8):
            result = analyze_image(
                image,
                base=values["base"],
                entry=values["entry"],
                callback=values["autoload_callback"],
                image_path=args.arm7,
                extra_seeds=seeds,
                data_ranges=metadata.get("data_ranges", ()),
                routine_names=metadata.get("routine_names"),
            )
            new = [seed for seed in executed_seeds(result, pcs) if seed not in seeds]
            if not new:
                break
            seeds.extend(new)
        if seeds:
            print(f"Additional roots supplied or discovered: {len(seeds)}")
        manifest_path, report_path = write_outputs(result, args.out)
    except (OSError, AnalysisError, ValueError, struct.error) as error:
        print(f"ARM7 analysis failed: {error}", file=sys.stderr)
        return 2
    print(f"Manifest: {manifest_path.resolve()}")
    print(f"Report: {report_path.resolve()}")
    print(f"Modules: {len(result.modules)}; functions: {sum(len(module.functions) for module in result.modules)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
