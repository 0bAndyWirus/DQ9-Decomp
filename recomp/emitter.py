"""Control-flow reconstruction and C++ emission for ARM/Thumb guest code."""

from __future__ import annotations

from bisect import bisect_right
from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any

from capstone.arm_const import ARM_CC_GE, ARM_CC_GT, ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

from .decoder import (
    Decoder,
    DecodedInstruction,
    operand_reg,
    reg_index,
    shift_kind,
    unsupported_for_arch,
)


_U32 = 0xFFFFFFFF
# Data/delay SWIs charge one generated cycle; runtime HLE supplies the rest.
# Control-flow SWIs retain their cooperative halt/wait accounting.
_ARM9_HLE_TIMED_SWIS = frozenset({
    0x03, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18,
})


def _u32(value: int) -> str:
    return f"0x{value & _U32:08x}u"


def _immediate_value(insn: DecodedInstruction, operand: Any) -> int:
    """Decode an ARM rotated 8-bit data-processing immediate."""
    value = int(operand.imm) & _U32
    if insn.thumb or not (insn.raw & (1 << 25)):
        return value
    rotation = ((insn.raw >> 8) & 0xF) * 2
    if not rotation:
        return value
    immediate = insn.raw & 0xFF
    return ((immediate >> rotation) | (immediate << (32 - rotation))) & _U32


def _module_tag(cpu: str, module_id: int) -> str:
    fixed = {-1: "main", -2: "itcm", -3: "dtcm", -4: "arm7main", -5: "arm7wram"}
    name = fixed.get(module_id, f"ov{module_id:03d}" if module_id >= 0 else f"m{-module_id}")
    return f"{cpu}_{name}"


def _entry_suffix(cpu: str, module_id: int, address: int) -> str:
    return f"{_module_tag(cpu, module_id)}_{address:08x}"


def _read_json_manifest(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("cpu") not in {"arm9", "arm7"}:
        raise ValueError(f"manifest {path} must name cpu 'arm9' or 'arm7'")
    if data.get("arch") not in {"v5te", "v4t"}:
        raise ValueError(f"manifest {path} must name arch 'v5te' or 'v4t'")
    if not isinstance(data.get("modules"), list):
        raise ValueError(f"manifest {path} has no modules list")
    return data


def _module_image(module: dict[str, Any]) -> bytes:
    image_path = Path(module["image"])
    if not image_path.is_absolute():
        raise ValueError(f"module image must be absolute: {image_path}")
    return image_path.read_bytes()


@dataclass
class Entry:
    module: dict[str, Any]
    function: dict[str, Any]
    address: int
    thumb: bool
    name: str
    extra: bool = False

    @property
    def module_id(self) -> int:
        return int(self.module["module_id"])

    @property
    def end(self) -> int:
        return int(self.function["addr"]) + int(self.function["size"])

    @property
    def size(self) -> int:
        return max(0, self.end - self.address)

    @property
    def slot(self) -> str:
        return "p_" + _entry_suffix(self._cpu, self.module_id, self.address) + self._slot_mode

    @property
    def body_symbol(self) -> str:
        return "rc_" + self.slot.removeprefix("p_")

    _cpu: str = "arm9"
    _slot_mode: str = ""


@dataclass
class Block:
    address: int
    instructions: list[DecodedInstruction]


@dataclass
class DispatchRegion:
    module: dict[str, Any]
    address: int
    size: int
    thumb: bool
    entries: list[Entry]
    data_ranges: list[tuple[int, int]]
    body_entry: Entry

    @property
    def module_id(self) -> int:
        return int(self.module["module_id"])

    @property
    def body_symbol(self) -> str:
        mode = "thumb" if self.thumb else "arm"
        return f"rc_shared_{_entry_suffix(self.body_entry._cpu, self.module_id, self.address)}_{mode}"


class FunctionEmitter:
    def __init__(
        self,
        cpu: str,
        arch: str,
        module: dict[str, Any],
        image: bytes,
        entry: Entry,
        address_entries: dict[int, list[Entry]],
        calls: dict[tuple[int, int], list[int]],
        stats: dict[str, Any],
        module_entries: dict[tuple[int, int], Entry],
        decoder: Decoder,
        dispatch_entries: list[Entry] | None = None,
        data_ranges: list[tuple[int, int]] | None = None,
        module_mode_entries: dict[tuple[int, int, bool], Entry] | None = None,
    ):
        self.dispatch_entries = sorted(dispatch_entries, key=lambda item: item.address) if dispatch_entries is not None else None
        self.data_ranges = sorted(data_ranges or [])
        self.module_mode_entries = module_mode_entries or {}
        self.cpu = cpu
        self.arch = arch
        self.module = module
        self.image = image
        self.entry = entry
        self.address_entries = address_entries
        self.calls = calls
        self.stats = stats
        self.module_entries = module_entries
        self.base = int(module["base"])
        self.begin = entry.address
        self.end = entry.end
        self.thumb = entry.thumb
        self.decoder = decoder
        seed_addresses = [item.address for item in self.dispatch_entries] if self.dispatch_entries is not None else [entry.address]
        self.insns: dict[int, DecodedInstruction] = {}
        self.leaders: set[int] = set(seed_addresses)
        self.loop_headers: set[int] = set()

    def _inside(self, addr: int) -> bool:
        return self.begin <= addr < self.end

    def _decode_at(self, address: int) -> DecodedInstruction | None:
        insn = self.decoder.decode(address)
        if insn is None or address + insn.size > self.end:
            return None
        # Shared-region records mark literal pools and relative tables that
        # must not be decoded as instructions.
        if any(start < address + insn.size and address < end for start, end in self.data_ranges):
            return None
        return insn

    @staticmethod
    def _branch_name(insn: DecodedInstruction) -> str:
        return insn.mnemonic.split(".")[0]

    def _branch_target(self, insn: DecodedInstruction) -> int | None:
        name = self._branch_name(insn)
        if name not in {"b", "bl", "blx"}:
            return None
        for operand in insn.operands:
            if operand.type == ARM_OP_IMM:
                return int(operand.imm) & _U32
        return None

    def _is_dynamic_terminator(self, insn: DecodedInstruction) -> bool:
        name = self._branch_name(insn)
        if name in {"bx", "bxj", "pop", "ldm", "ldmia", "ldmib", "ldmda", "ldmdb"}:
            if name == "pop":
                return any(reg_index(insn, op.reg) == 15 for op in insn.operands if op.type == ARM_OP_REG)
            if name.startswith("ldm"):
                return any(reg_index(insn, op.reg) == 15 for op in insn.operands[1:] if op.type == ARM_OP_REG)
            return True
        if name.startswith(("ldr", "mov", "add", "sub", "rsb", "adc", "sbc", "and", "orr", "eor", "bic", "mvn")):
            return bool(insn.operands and insn.operands[0].type == ARM_OP_REG and reg_index(insn, insn.operands[0].reg) == 15)
        return False

    def _terminator(self, insn: DecodedInstruction) -> bool:
        name = self._branch_name(insn)
        if name in {"b", "bx", "bxj"}:
            return True
        if name == "blx":
            return False
        if name == "bl":
            return False
        return self._is_dynamic_terminator(insn)

    def _preceding(self, insn: DecodedInstruction, limit: int) -> list[DecodedInstruction]:
        previous: list[DecodedInstruction] = []
        cursor = insn.address
        for address, item in reversed(sorted((addr, item) for addr, item in self.insns.items() if addr < insn.address)):
            if item.next_address != cursor:
                break
            previous.append(item)
            cursor = address
            if len(previous) >= limit:
                break
        previous.reverse()
        return previous

    def _jump_case_count(self, insn: DecodedInstruction, index_reg: int, default: int | None) -> int | None:
        """Upper bound of a computed jump's index from the compare that guards it.

        `subs idx, src, #base` before the jump rebases the bound of an earlier
        `cmp src, #limit` to indices 0..limit-base. An unrelated comparison
        of the destination register must not replace that source bound.
        Other writers of the index register keep the historical scan.
        """
        register, base = index_reg, 0
        for prior in reversed(self._preceding(insn, 20)):
            name = prior.mnemonic.split(".")[0]
            if name.rstrip("s") != "cmp":
                ops = prior.operands
                if (name.rstrip("s") == "sub" and prior.condition == 14 and base == 0 and len(ops) == 3
                        and ops[0].type == ARM_OP_REG and operand_reg(prior, ops[0]) == register
                        and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM):
                    register, base = operand_reg(prior, ops[1]), _immediate_value(prior, ops[2])
                continue
            ops = prior.operands
            if len(ops) == 2 and ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_IMM:
                if operand_reg(prior, ops[0]) == register:
                    bound = int(ops[1].imm)
                    if bound == 0 and insn.condition in {ARM_CC_GE - 1, ARM_CC_GT - 1}:
                        continue
                    return min(256, max(0, bound - base + 1))
            if len(ops) >= 3 and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
                if operand_reg(prior, ops[1]) == register:
                    bound = int(ops[2].imm)
                    if bound == 0 and insn.condition in {ARM_CC_GE - 1, ARM_CC_GT - 1}:
                        continue
                    return min(256, max(0, bound - base + 1))
        return min(256, max(0, default)) if default is not None else None

    def _function_contains(self, address: int) -> bool:
        return self.begin <= address < self.end

    def _external_table_slots(self, insn: DecodedInstruction) -> list[int]:
        """Slots of a bounded ARM ``add pc, pc, rX, lsl #2`` table outside this body.

        DSD sometimes splits the branch slots that follow such an ADD into
        separate tiny functions. The computed transfer then lands on those
        slots (possibly mid-"function"), so each one needs a dispatchable entry.
        """
        if insn.thumb or self._branch_name(insn) != "add" or len(insn.operands) < 3:
            return []
        dst, base_op, index_op = insn.operands[:3]
        if (dst.type != ARM_OP_REG or operand_reg(insn, dst) != 15
                or base_op.type != ARM_OP_REG or operand_reg(insn, base_op) != 15
                or index_op.type != ARM_OP_REG):
            return []
        shift_type, by_register, _ = shift_kind(index_op.shift.type)
        if shift_type != 0 or by_register or int(index_op.shift.value) != 2:
            return []
        count = self._jump_case_count(insn, operand_reg(insn, index_op), None)
        if not count:
            return []
        pc = (insn.address + 8) & _U32
        return [pc + index * 4 for index in range(count) if not self._function_contains(pc + index * 4)]

    def _thumb_halfword_table_info(self, insn: DecodedInstruction) -> dict[str, Any] | None:
        ops = insn.operands
        if len(ops) != 2 or ops[0].type != ARM_OP_REG or ops[1].type != ARM_OP_REG:
            return None
        if operand_reg(insn, ops[0]) != 15:
            return None
        target_reg = operand_reg(insn, ops[1])
        previous = self._preceding(insn, 24)
        load_pos = -1
        load = None
        mem = None
        for pos in range(len(previous) - 1, -1, -1):
            candidate = previous[pos]
            if candidate.mnemonic.split(".")[0] not in {"ldrh", "ldrsh"} or len(candidate.operands) < 2:
                continue
            if candidate.operands[0].type != ARM_OP_REG or operand_reg(candidate, candidate.operands[0]) != target_reg:
                continue
            memop = next((operand for operand in candidate.operands if operand.type == ARM_OP_MEM), None)
            if memop is None or not memop.mem.base or memop.mem.index:
                continue
            load_pos, load, mem = pos, candidate, memop.mem
            break
        if load is None or mem is None:
            return None

        base_reg = reg_index(load, mem.base)
        add_pos = -1
        for pos in range(load_pos - 1, -1, -1):
            candidate = previous[pos]
            candidate_ops = candidate.operands
            if candidate.mnemonic.split(".")[0].rstrip("s") != "add" or len(candidate_ops) != 2:
                continue
            if (
                candidate_ops[0].type == ARM_OP_REG
                and candidate_ops[1].type == ARM_OP_REG
                and operand_reg(candidate, candidate_ops[0]) == base_reg
                and operand_reg(candidate, candidate_ops[1]) == 15
            ):
                add_pos = pos
                break
        if add_pos < 0:
            return None

        key_reg = None
        for candidate in reversed(previous[:add_pos]):
            candidate_ops = candidate.operands
            if not candidate_ops or candidate_ops[0].type != ARM_OP_REG:
                continue
            if operand_reg(candidate, candidate_ops[0]) != base_reg:
                continue
            name = candidate.mnemonic.split(".")[0].rstrip("s")
            if name == "add" and len(candidate_ops) >= 3 and candidate_ops[1].type == ARM_OP_REG and candidate_ops[2].type == ARM_OP_REG:
                lhs = operand_reg(candidate, candidate_ops[1])
                rhs = operand_reg(candidate, candidate_ops[2])
                if lhs == rhs:
                    key_reg = lhs
            elif name == "add" and len(candidate_ops) == 2 and candidate_ops[1].type == ARM_OP_REG:
                lhs = operand_reg(candidate, candidate_ops[1])
                if lhs == base_reg:
                    key_reg = lhs
            elif name == "lsl" and len(candidate_ops) >= 3 and candidate_ops[1].type == ARM_OP_REG:
                key_reg = operand_reg(candidate, candidate_ops[1])
            break

        signed = load.mnemonic.split(".")[0] == "ldrsh"
        shifted_high = False
        for candidate in previous[load_pos + 1 :]:
            candidate_ops = candidate.operands
            name = candidate.mnemonic.split(".")[0].rstrip("s")
            if name == "sxth" and candidate_ops and candidate_ops[0].type == ARM_OP_REG and operand_reg(candidate, candidate_ops[0]) == target_reg:
                signed = True
            if (
                name == "lsl"
                and len(candidate_ops) >= 3
                and candidate_ops[0].type == ARM_OP_REG
                and candidate_ops[1].type == ARM_OP_REG
                and candidate_ops[2].type == ARM_OP_IMM
                and operand_reg(candidate, candidate_ops[0]) == target_reg
                and operand_reg(candidate, candidate_ops[1]) == target_reg
                and int(candidate_ops[2].imm) == 16
            ):
                shifted_high = True
            if (
                shifted_high
                and name == "asr"
                and len(candidate_ops) >= 3
                and candidate_ops[0].type == ARM_OP_REG
                and candidate_ops[1].type == ARM_OP_REG
                and candidate_ops[2].type == ARM_OP_IMM
                and operand_reg(candidate, candidate_ops[0]) == target_reg
                and operand_reg(candidate, candidate_ops[1]) == target_reg
                and int(candidate_ops[2].imm) == 16
            ):
                signed = True

        table_base_pc = previous[add_pos].address + 4
        table = (table_base_pc + int(mem.disp)) & _U32
        table_offset = table - self.base
        available = min(256, max(0, (len(self.image) - table_offset) // 2)) if table_offset >= 0 else 0
        if available == 0:
            return None

        branch_pc = insn.address + 4
        count = self._jump_case_count(insn, key_reg, None) if key_reg is not None else None
        if count is None:
            # The case code follows the table: stop at the first entry whose
            # address reaches the lowest target seen so far.
            count = 0
            lowest = None
            for index in range(available):
                entry_address = table + index * 2
                if lowest is not None and entry_address >= lowest:
                    break
                raw = self._literal(insn, entry_address, 2)
                if raw is None:
                    break
                offset = raw - 0x10000 if signed and raw & 0x8000 else raw
                target = (branch_pc + offset) & ~1
                if not self._function_contains(target):
                    break
                lowest = target if lowest is None else min(lowest, target)
                count += 1
        else:
            count = min(count, available)

        cases: list[tuple[int, int]] = []
        seen: set[int] = set()
        for index in range(count):
            raw = self._literal(insn, table + index * 2, 2)
            if raw is None:
                break
            offset = raw - 0x10000 if signed and raw & 0x8000 else raw
            target = (branch_pc + offset) & ~1
            if self._function_contains(target):
                key = offset & _U32
                if key not in seen:
                    cases.append((key, target))
                    seen.add(key)
        if not cases:
            return None
        return {"kind": "thumb_halfword", "index": target_reg, "base": branch_pc, "cases": cases}

    def _relative_jump_table_info(self, insn: DecodedInstruction) -> dict[str, Any] | None:
        """Recognize a PC-relative base, indexed LDR, and ADD-PC table sequence."""
        if self.dispatch_entries is None:
            return None
        if insn.thumb or self._branch_name(insn) != "add" or len(insn.operands) < 3:
            return None
        destination, left, right = insn.operands[:3]
        if destination.type != ARM_OP_REG or operand_reg(insn, destination) != 15:
            return None
        if left.type != ARM_OP_REG or right.type != ARM_OP_REG:
            return None
        source_regs = {operand_reg(insn, left), operand_reg(insn, right)}
        previous = self._preceding(insn, 24)
        if len(previous) < 2:
            return None

        load = previous[-1]
        if self._branch_name(load) != "ldr" or len(load.operands) < 2 or load.condition != insn.condition:
            return None
        if load.operands[0].type != ARM_OP_REG:
            return None
        memop = next((operand for operand in load.operands if operand.type == ARM_OP_MEM), None)
        if memop is None or not memop.mem.base or not memop.mem.index or load.writeback:
            return None
        if int(memop.mem.disp) != 0:
            return None
        shift_type, by_register, _ = shift_kind(int(getattr(memop.shift, "type", 0)))
        shift = int(getattr(memop.shift, "value", 0)) or int(memop.mem.lshift)
        if shift_type != 0 or by_register or shift != 2:
            return None
        base_reg = reg_index(load, memop.mem.base)
        index_reg = reg_index(load, memop.mem.index)
        value_reg = operand_reg(load, load.operands[0])
        if base_reg not in source_regs or value_reg not in source_regs or base_reg == value_reg:
            return None

        setup = previous[-2]
        ops = setup.operands
        if (
            not ops or ops[0].type != ARM_OP_REG or operand_reg(setup, ops[0]) != base_reg
            or self._branch_name(setup) not in {"add", "sub"}
            or len(ops) not in {3, 4} or ops[1].type != ARM_OP_REG
            or operand_reg(setup, ops[1]) != 15 or ops[2].type != ARM_OP_IMM
            or (len(ops) == 4 and ops[3].type != ARM_OP_IMM)
            or setup.condition != load.condition
        ):
            return None

        pc_value = setup.address + 8
        displacement = _immediate_value(setup, ops[2])
        base = (pc_value + (displacement if self._branch_name(setup) == "add" else -displacement)) & _U32
        table_offset = base - self.base
        available = min(256, max(0, (len(self.image) - table_offset) // 4)) if table_offset >= 0 else 0
        if table_offset >= 0 and base < self.end:
            available = min(available, max(0, (self.end - base) // 4))
        count = self._jump_case_count(insn, index_reg, None)
        count = min(available, count) if count is not None else available
        cases: list[tuple[int, int]] = []
        for index in range(count):
            offset = self._literal(insn, base + index * 4, 4)
            if offset is None:
                break
            target = (base + offset) & _U32 & ~3
            if not self._function_contains(target) or self._decode_at(target) is None:
                break
            cases.append((index, target))
        if not cases:
            return None
        return {
            "kind": "relative_table",
            "index": index_reg,
            "value": value_reg,
            "table": base,
            "base": base,
            "shift": 2,
            "cases": cases,
        }


    def _jump_table_info(self, insn: DecodedInstruction) -> dict[str, Any] | None:
        name = insn.mnemonic.split(".")[0]
        pc = (insn.address + (4 if insn.thumb else 8)) & _U32
        if name == "ldr" and insn.operands and insn.operands[0].type == ARM_OP_REG:
            if operand_reg(insn, insn.operands[0]) != 15:
                return None
            memop = next((op for op in insn.operands if op.type == ARM_OP_MEM), None)
            if memop is None or not memop.mem.base or reg_index(insn, memop.mem.base) != 15 or not memop.mem.index:
                return None
            index_reg = reg_index(insn, memop.mem.index)
            table = (pc + int(memop.mem.disp)) & _U32
            shift = int(memop.mem.lshift)
            table_offset = table - self.base
            available = min(256, max(0, (len(self.image) - table_offset) // 4)) if table_offset >= 0 else 0
            count = self._jump_case_count(insn, index_reg, None)
            if count is None:
                count = 0
                lowest = None
                for index in range(available):
                    entry_address = table + index * 4
                    if lowest is not None and entry_address >= lowest:
                        break
                    literal = self._literal(insn, entry_address, 4)
                    if literal is None:
                        break
                    target_thumb = bool(literal & 1) if self.arch != "v4t" else self.thumb
                    target = literal & ~1
                    if target_thumb != self.thumb or not self._function_contains(target):
                        break
                    if target > entry_address:
                        lowest = target if lowest is None else min(lowest, target)
                    count += 1
            else:
                count = min(count, available)
            cases: list[tuple[int, int]] = []
            for index in range(count):
                literal = self._literal(insn, table + index * 4, 4)
                if literal is None:
                    break
                target_thumb = bool(literal & 1) if self.arch != "v4t" else self.thumb
                target = literal & ~1
                if target_thumb == self.thumb and self._function_contains(target):
                    cases.append((index, target))
            if cases:
                return {"kind": "ldr", "index": index_reg, "table": table, "shift": shift, "cases": cases}
            return None

        if name != "add":
            return None
        relative = self._relative_jump_table_info(insn)
        if relative is not None:
            return relative
        if insn.thumb:
            return self._thumb_halfword_table_info(insn)
        if len(insn.operands) < 3:
            return None
        dst, base_op, index_op = insn.operands[:3]
        if dst.type != ARM_OP_REG or operand_reg(insn, dst) != 15:
            return None
        if base_op.type != ARM_OP_REG or operand_reg(insn, base_op) != 15 or index_op.type != ARM_OP_REG:
            return None
        shift_type, by_register, _ = shift_kind(index_op.shift.type)
        if shift_type != 0 or by_register:
            return None
        index_reg = operand_reg(insn, index_op)
        shift = int(index_op.shift.value) if index_op.shift.type else 0
        count = self._jump_case_count(insn, index_reg, None)
        if shift == 2:
            # The nearest compare can be a lower-bound check (`cmp index,#0`)
            # for a conditional ADD, not an upper bound for the table. Prefer
            # the actual contiguous run of ARM B slots at the computed PC.
            branch_count = 0
            while branch_count < 256:
                address = pc + branch_count * 4
                word = self._literal(insn, address, 4)
                if word is None or (word >> 24) != 0xEA or not self._function_contains(address):
                    break
                branch_count += 1
            if branch_count:
                count = branch_count if count is None else min(count, branch_count)
        if not count:
            # Computed jump into straight-line code: every instruction of
            # the linear run at pc is a possible target.
            cases = []
            address = pc
            step = 1 << shift
            while self._function_contains(address) and address - pc < 0x1000:
                decoded = self._decode_at(address)
                if decoded is None:
                    break
                if (address - pc) % step == 0:
                    cases.append(((address - pc) >> shift, address))
                if self._terminator(decoded) and decoded.condition == 14:
                    break
                address = decoded.next_address
            if not cases:
                return None
            return {"kind": "add", "index": index_reg, "base": pc, "shift": shift, "cases": cases}
        cases = []
        for index in range(count):
            target = (pc + ((index << shift) & _U32)) & _U32 & ~3
            if self._function_contains(target):
                cases.append((index, target))
        if not cases:
            return None
        return {"kind": "add", "index": index_reg, "base": pc, "shift": shift, "cases": cases}


    def _jump_table(self, insn: DecodedInstruction) -> str | None:
        info = self._jump_table_info(insn)
        if info is None:
            return None
        if self.dispatch_entries is not None:
            # Shared regions route the live computed target through their one
            # instruction-boundary dispatcher instead of specializing sites.
            self.stats["jump_tables"] += 1
            return None
        index = int(info["index"])
        link = self._manual_link(insn)
        if info["kind"] == "relative_table":
            return self._cond(insn, self._relative_table_body(insn, info, link))
        case_lines = []
        for key, case_target in info["cases"]:
            if link is None:
                case_body = f"goto L_{case_target:08x};"
            else:
                case_body = self._call_with_link(
                    insn, _u32(case_target), link, count_site=False
                )
            case_lines.append(f"case {_u32(key)}: {{\n{_indent(case_body)}\n}}")
        cases = "\n".join(case_lines)
        if info["kind"] == "ldr":
            target = f"::dsr::ldr32(c, {_u32(info['table'])} + (jump_index << {info['shift']}u))"
        elif info["kind"] == "thumb_halfword":
            target = f"(({_u32(info['base'])} + c->r[{index}]) | 1u)"
        else:
            target = f"({_u32(info['base'])} + (jump_index << {info['shift']}u))"
            target = f"({target} & ~3u)"
        if link is not None:
            call = self._call_with_link(insn, "jump_target", link, count_site=False)
            fallback = f"std::uint32_t jump_target = {target};\n{call}"
        elif info["kind"] == "add":
            # Resolve wrapped index arithmetic back to known in-function table
            # slots before falling back to the global function-entry dispatcher.
            targets = "\n".join(
                f"case {_u32(address)}: goto L_{address:08x};"
                for address in sorted({address for _, address in info["cases"]})
            )
            fallback = (
                f"std::uint32_t jump_target = {target};\n"
                f"switch (jump_target) {{\n{_indent(targets)}\n"
                "default: {\n"
                "    ::dsr::dsr_call(c, jump_target);\n"
                "    return;\n"
                "}\n}"
            )
        else:
            fallback = (
                f"std::uint32_t jump_target = {target};\n"
                "::dsr::dsr_call(c, jump_target);\n"
                "return;"
            )
        body = (
            f"std::uint32_t jump_index = c->r[{index}];\n"
            f"switch (jump_index) {{\n{_indent(cases)}\n"
            f"default: {{\n{_indent(fallback)}\n}}"
            "\n}"
        )
        self.stats["jump_tables"] += 1
        self.stats["dsr_call_sites"] += 1
        return self._cond(insn, body)

    def _relative_table_body(
        self, insn: DecodedInstruction, info: dict[str, Any], link: int | None
    ) -> str:
        """Transfer to the ADD-PC sum of the already-loaded offset and base.

        The indexed LDR before the ADD may overwrite its own index register
        (``ldr ip, [r5, ip, lsl #2]``), so the target is computed from the live
        operand registers at the transfer, never by re-reading the table.
        """
        left, right = (operand_reg(insn, operand) for operand in insn.operands[1:3])
        target = f"((c->r[{left}] + c->r[{right}]) & ~3u)"
        if link is not None:
            call = self._call_with_link(insn, "jump_target", link, count_site=False)
            body = f"std::uint32_t jump_target = {target};\n{call}"
        else:
            targets = "\n".join(
                f"case {_u32(address)}: goto L_{address:08x};"
                for address in sorted({address for _, address in info["cases"]})
            )
            body = (
                f"std::uint32_t jump_target = {target};\n"
                f"switch (jump_target) {{\n{_indent(targets)}\n"
                "default: {\n"
                "    ::dsr::dsr_call(c, jump_target);\n"
                "    return;\n"
                "}\n}"
            )
        self.stats["jump_tables"] += 1
        self.stats["dsr_call_sites"] += 1
        return body

    def _manual_link(self, insn: DecodedInstruction) -> int | None:
        """Find a PC-derived LR continuation that turns this transfer into a call."""
        link = self._manual_link_value(insn)
        if link is not None and self._branch_name(insn) == "b" and not self._inside(link & ~1):
            # A direct B only acts as a call when LR names a continuation in
            # this body. An external LR (crt0 seeding the return address of a
            # tail-jumped main) leaves the B an ordinary local branch.
            return None
        return link

    def _manual_link_value(self, insn: DecodedInstruction) -> int | None:
        name = self._branch_name(insn)
        ops = insn.operands
        if name == "b":
            if self._branch_target(insn) is None:
                return None
        elif not ops or ops[0].type != ARM_OP_REG:
            return None
        if name == "bx":
            if operand_reg(insn, ops[0]) == 14:
                return None
        elif name in {"mov", "ldr", "add"}:
            if operand_reg(insn, ops[0]) != 15:
                return None
            if name == "mov" and (len(ops) < 2 or ops[1].type != ARM_OP_REG):
                return None
            if name == "mov" and operand_reg(insn, ops[1]) == 14:
                return None
            if name == "add" and (
                len(ops) < 3 or ops[1].type != ARM_OP_REG or ops[2].type != ARM_OP_REG
            ):
                return None
            if name == "ldr":
                mem = next((x for x in ops if x.type == ARM_OP_MEM), None)
                if mem is None or (mem.mem.base and reg_index(insn, mem.mem.base) == 13):
                    return None
        elif name != "b":
            return None
        shared = self.dispatch_entries is not None
        limit = 24 if shared else 3
        for prior in reversed(self._preceding(insn, limit)):
            pops = prior.operands
            pname = self._branch_name(prior)
            if shared:
                writes_lr = any(
                    prior.detail.reg_name(reg).lower() in {"lr", "r14"}
                    for reg in prior.detail.regs_access()[1]
                )
            else:
                writes_lr = (
                    bool(pops) and pops[0].type == ARM_OP_REG
                    and operand_reg(prior, pops[0]) == 14
                )
            if not shared:
                if pname in {"b", "bl", "blx", "bx"} or prior.condition != 14:
                    return None
            else:
                if prior.condition == 15:
                    continue
                if pname in {"bl", "blx"}:
                    return None
                if self._terminator(prior):
                    if prior.condition == 14 or writes_lr:
                        return None
                    continue
                if prior.condition != 14:
                    if writes_lr:
                        return None
                    continue
            if not writes_lr:
                continue
            pc_value = prior.address + (4 if prior.thumb else 8)
            if pname == "mov" and len(pops) == 2 and pops[1].type == ARM_OP_REG \
                    and operand_reg(prior, pops[1]) == 15:
                return pc_value | (1 if prior.thumb else 0)
            if shared and pname == "adr" and len(pops) == 2 and pops[1].type == ARM_OP_IMM:
                return int(pops[1].imm) & _U32
            if pname in {"add", "sub"} and (len(pops) == 3 or (shared and len(pops) == 4)) \
                    and pops[1].type == ARM_OP_REG and operand_reg(prior, pops[1]) == 15 \
                    and pops[2].type == ARM_OP_IMM \
                    and (len(pops) == 3 or pops[3].type == ARM_OP_IMM):
                displacement = _immediate_value(prior, pops[2]) if shared else int(pops[2].imm)
                return (pc_value + (displacement if pname == "add" else -displacement)) & _U32
            if pname == "ldr":
                mem = next((x for x in pops if x.type == ARM_OP_MEM), None)
                if mem is not None:
                    literal_address = self._mem_literal_address(prior, mem.mem, False)
                    if literal_address is not None:
                        return self._literal(prior, literal_address, 4)
            return None
        return None

    def _call_with_link(
        self, insn: DecodedInstruction, target_expr: str, link: int, *, count_site: bool = True
    ) -> str:
        """Emit a transfer whose explicit LR value supplies its continuation."""
        if count_site:
            self.stats["dsr_call_sites"] += 1
        body = [
            f"::dsr::dsr_call(c, {target_expr});",
            f"DSR_POSTCALL(c, {_u32(link)});",
        ]
        cont = link & ~1
        if self.dispatch_entries is not None:
            if self._inside(cont) and bool(link & 1) == self.thumb:
                body.extend([
                    f"dispatch_target = {_u32(link)};",
                    "goto L_region_dispatch;",
                ])
            else:
                body.append(f"::dsr::dsr_call(c, {_u32(link)});")
                body.append("return;")
        elif self._inside(cont) and cont in self.leaders:
            body.append(f"goto L_{cont:08x};")
        else:
            body.append(f"::dsr::dsr_call(c, {_u32(link)});")
            body.append("return;")
        return "\n".join(body)

    def _discover(self) -> None:
        seeds = [item.address for item in self.dispatch_entries] if self.dispatch_entries is not None else [self.entry.address]
        pending = list(seeds)
        visited: set[int] = set()
        while pending:
            address = pending.pop()
            if address in visited or not self._inside(address):
                continue
            visited.add(address)
            cursor = address
            while self._inside(cursor) and cursor not in self.insns:
                insn = self._decode_at(cursor)
                if insn is None:
                    break
                self.insns[cursor] = insn
                name = self._branch_name(insn)
                target = self._branch_target(insn)
                if name in {"bl", "blx"}:
                    # A local BL subroutine may lie past the caller's return.
                    # Discover it so it can receive its own callable entry.
                    if name == "bl" and target is not None and self._inside(target & ~1):
                        self.leaders.add(target & ~1)
                        pending.append(target & ~1)
                    next_pc = insn.next_address
                    if self._inside(next_pc):
                        self.leaders.add(next_pc)
                        pending.append(next_pc)
                    break
                if name == "b":
                    link = self._manual_link(insn)
                    if link is not None:
                        if self._inside(link & ~1):
                            self.leaders.add(link & ~1)
                            pending.append(link & ~1)
                        if insn.condition != 14 and self._inside(insn.next_address):
                            self.leaders.add(insn.next_address)
                            pending.append(insn.next_address)
                        break
                    if target is not None:
                        if target <= insn.address:
                            self.loop_headers.add(target)
                        if self._inside(target):
                            self.leaders.add(target)
                            pending.append(target)
                    if insn.condition != 14 and self._inside(insn.next_address):
                        self.leaders.add(insn.next_address)
                        pending.append(insn.next_address)
                    break
                if self._terminator(insn) or name in {"swi", "svc", "bkpt", "udf"}:
                    jump_info = self._jump_table_info(insn) if self._terminator(insn) else None
                    if jump_info is not None:
                        for _, jump_target in jump_info["cases"]:
                            if jump_target < insn.address:
                                self.loop_headers.add(jump_target)
                            self.leaders.add(jump_target)
                            pending.append(jump_target)
                    link = self._manual_link(insn) if self._terminator(insn) else None
                    if link is not None and self._inside(link & ~1):
                        self.leaders.add(link & ~1)
                        pending.append(link & ~1)
                    if insn.condition != 14 and self._inside(insn.next_address):
                        self.leaders.add(insn.next_address)
                        pending.append(insn.next_address)
                    break
                cursor = insn.next_address

    def _decode_shared_region(self) -> None:
        """Sweep shared code linearly so every decoded instruction is dispatchable."""
        cursor = self.begin
        unit = 2 if self.thumb else 4
        while cursor < self.end:
            data_end = next((end for start, end in self.data_ranges if start <= cursor < end), None)
            if data_end is not None:
                cursor = min(self.end, data_end)
                continue
            insn = self.insns.get(cursor) or self._decode_at(cursor)
            if insn is None:
                cursor += unit
                continue
            self.insns[cursor] = insn
            self.leaders.add(cursor)
            name = self._branch_name(insn)
            target = self._branch_target(insn)
            manual_link = self._manual_link(insn) if name == "b" or self._terminator(insn) else None
            if name == "b":
                if manual_link is not None:
                    if self._inside(manual_link & ~1):
                        self.leaders.add(manual_link & ~1)
                elif target is not None and self._inside(target):
                    self.leaders.add(target)
                    if target <= cursor:
                        self.loop_headers.add(target)
                if insn.condition != 14 and self._inside(insn.next_address):
                    self.leaders.add(insn.next_address)
            elif self._terminator(insn):
                jump_info = self._jump_table_info(insn)
                if jump_info is not None:
                    for _, jump_target in jump_info["cases"]:
                        self.leaders.add(jump_target)
                        if jump_target < cursor:
                            self.loop_headers.add(jump_target)
                if manual_link is not None and self._inside(manual_link & ~1):
                    self.leaders.add(manual_link & ~1)
                if insn.condition != 14 and self._inside(insn.next_address):
                    self.leaders.add(insn.next_address)
            cursor = insn.next_address

    def _blocks(self) -> list[Block]:
        if self.dispatch_entries is None:
            if self.entry.address not in self.insns:
                return []
        elif not any(entry.address in self.insns for entry in self.dispatch_entries):
            return []
        leaders = sorted(a for a in self.leaders if a in self.insns)
        if self.dispatch_entries is None and (not leaders or leaders[0] != self.entry.address):
            leaders.insert(0, self.entry.address)
        blocks: list[Block] = []
        for pos, address in enumerate(leaders):
            stop = leaders[pos + 1] if pos + 1 < len(leaders) else self.end
            cursor = address
            instructions: list[DecodedInstruction] = []
            while cursor < stop and cursor in self.insns:
                insn = self.insns[cursor]
                instructions.append(insn)
                cursor = insn.next_address
                if self._terminator(insn) or self._branch_name(insn) == "b":
                    break
            if instructions:
                blocks.append(Block(address, instructions))
        return blocks


    def _pc_value(self, insn: DecodedInstruction, reg: int, role: str = "read") -> str:
        if reg != 15:
            return f"c->r[{reg}]"
        if self.thumb:
            value = insn.address + 4
        else:
            value = insn.address + (12 if role in {"store", "shift"} else 8)
        return _u32(value)

    def _reg_value(self, insn: DecodedInstruction, operand: Any, role: str = "read") -> str:
        return self._pc_value(insn, operand_reg(insn, operand), role)

    def _cpp_reg(self, reg: int, insn: DecodedInstruction, role: str = "read") -> str:
        return self._pc_value(insn, reg, role)

    def _literal(self, insn: DecodedInstruction, address: int, width: int) -> int | None:
        offset = address - self.base
        if offset < 0 or offset + width > len(self.image):
            return None
        if width == 4:
            address &= ~3
            offset = address - self.base
            if offset < 0 or offset + width > len(self.image):
                return None
        return int.from_bytes(self.image[offset : offset + width], "little")

    def _emit_bad(self, insn: DecodedInstruction, why: str) -> str:
        self.stats["unsupported_sites"].append(
            {"address": insn.address, "module": self.module["name"], "instruction": insn.text, "reason": why}
        )
        self.stats["undefined_sites"] += 1
        return f"::dsr::dsr_undefined(c, {_u32(insn.address)}, {_u32(insn.raw)});"

    def _cond(self, insn: DecodedInstruction, body: str) -> str:
        if insn.condition == 14:
            return body
        if insn.condition == 15:
            return "/* NV condition: instruction does not execute. */"
        return f"if (::dsr::condition_passed(c, {insn.condition}u)) {{\n{_indent(body)}\n}}"


    def _shifted_operand(self, insn: DecodedInstruction, operand: Any, temp: str) -> tuple[list[str], str, str]:
        """Return setup statements, value expression, shifter carry expression."""
        if operand.type == ARM_OP_IMM:
            value = _immediate_value(insn, operand)
            if not insn.thumb and (insn.raw & (1 << 25)) and ((insn.raw >> 8) & 15):
                return [], _u32(value), f"(({_u32(value)} >> 31) & 1u)"
            return [], _u32(value), "c->cf"
        if operand.type != ARM_OP_REG:
            raise ValueError(f"unsupported shifter operand at 0x{insn.address:08x}")
        shift_type, by_reg, rrx = shift_kind(operand.shift.type)
        value = self._reg_value(insn, operand, "shift" if by_reg else "read")
        if operand.shift.type == 0:
            return [], value, "c->cf"
        if rrx:
            amount_expr = "0u"
        elif by_reg:
            try:
                amount_expr = self._cpp_reg(reg_index(insn, int(operand.shift.value)), insn)
            except (ValueError, TypeError):
                amount_expr = "0u"
        else:
            amount_expr = f"{int(operand.shift.value)}u"
        return [
            f"std::uint32_t {temp}_carry = c->cf;",
            f"std::uint32_t {temp} = ::dsr::shift_carry({value}, {shift_type}u, {amount_expr}, c->cf, {'true' if by_reg else 'false'}, &{temp}_carry);",
        ], temp, f"{temp}_carry"

    def _alu_flags(self, value: str, carry: str | None = None, overflow: str | None = None) -> str:
        pieces = [f"c->n = ({value} >> 31) & 1u;", f"c->z = ({value} == 0u);"]
        if carry is not None:
            pieces.append(f"c->cf = {carry};")
        if overflow is not None:
            pieces.append(f"c->v = {overflow};")
        return "\n".join(pieces)

    def _alu(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic
        base = name.rstrip("s") if name.endswith("s") else name
        # Capstone separates condition codes and S bit, not as mnemonic suffix.
        supported = {
            "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
            "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn",
            "lsl", "lsr", "asr", "ror", "rrx",
        }
        if base not in supported:
            return None
        ops = insn.operands
        if not ops or ops[0].type != ARM_OP_REG:
            return None
        dest = operand_reg(insn, ops[0])
        tests = base in {"tst", "teq", "cmp", "cmn"}
        # Capstone marks ADC/SBC/RSC as flag-updating even without S: carry
        # input is not flag output. ARM data-processing encodings own the S bit.
        set_flags = insn.update_flags if insn.thumb else bool(insn.raw & (1 << 20))
        if base in {"lsl", "lsr", "asr", "ror", "rrx"}:
            if len(ops) < 2:
                return None
            typ = {"lsl": 0, "lsr": 1, "asr": 2, "ror": 3, "rrx": 3}[base]
            if ops[1].type == ARM_OP_REG and ops[1].shift.type != 0 and len(ops) == 2:
                setup, shifted, shcarry = self._shifted_operand(insn, ops[1], "shifted")
                statements = setup + [f"std::uint32_t value = {shifted};"]
            else:
                by_register = False
                if insn.thumb and len(ops) == 2 and base != "rrx":
                    src = self._cpp_reg(dest, insn)
                    if ops[1].type == ARM_OP_IMM:
                        amount_expr = f"{int(ops[1].imm) & 0xFF}u"
                    elif ops[1].type == ARM_OP_REG:
                        amount_expr = self._reg_value(insn, ops[1])
                        by_register = True
                    else:
                        return None
                else:
                    src = self._reg_value(insn, ops[1], "shift" if len(ops) == 3 and ops[2].type == ARM_OP_REG else "read")
                    amount_expr = "0u" if base == "rrx" or len(ops) == 2 else str(int(ops[2].imm) & 0xFF) + "u" if ops[2].type == ARM_OP_IMM else self._reg_value(insn, ops[2])
                    by_register = len(ops) == 3 and ops[2].type == ARM_OP_REG
                statements = ["std::uint32_t shcarry = c->cf;", f"std::uint32_t value = ::dsr::shift_carry({src}, {typ}u, {amount_expr}, c->cf, {'true' if by_register else 'false'}, &shcarry);"]
                shcarry = "shcarry"
            if dest == 15:
                if set_flags:
                    statements.extend(self._exception_return("value"))
                else:
                    statements.extend(self._pc_transfer(insn, "value", same_mode=True))
            else:
                statements.append(f"c->r[{dest}] = value;")
                if set_flags:
                    statements.append(self._alu_flags("value", shcarry))
            return self._cond(insn, "\n".join(statements))
        if base == "mov" or base == "mvn":
            if len(ops) < 2:
                return None
            setup, value, carry = self._shifted_operand(insn, ops[1], "op2")
            if base == "mvn":
                value = f"~({value})"
            statements = setup + [f"std::uint32_t result = {value};"]
            if dest == 15:
                if set_flags:
                    statements.extend(self._exception_return("result"))
                elif base == "mov" and ops[1].type == ARM_OP_REG and operand_reg(insn, ops[1]) == 14:
                    if self.dispatch_entries is None:
                        statements.extend(["DSR_RET(c, result);", "return;"])
                    else:
                        target_expr = f"(result {'| 1u' if self.thumb else '& ~3u'})"
                        statements.extend(self._shared_region_return(target_expr, "result"))
                else:
                    statements.extend(self._pc_transfer(insn, "result", same_mode=True))
            else:
                statements.append(f"c->r[{dest}] = result;")
                if set_flags:
                    statements.append(self._alu_flags("result", carry))
            return self._cond(insn, "\n".join(statements))
        if tests:
            if len(ops) < 2:
                return None
            left_op, right_op = ops[0], ops[1]
        elif len(ops) >= 3:
            left_op, right_op = ops[1], ops[2]
        elif len(ops) == 2:
            left_op, right_op = ops[0], ops[1]
        else:
            return None
        left = self._reg_value(insn, left_op, "shift" if shift_kind(right_op.shift.type)[1] else "read")
        setup, right, shcarry = self._shifted_operand(insn, right_op, "op2")
        statements = setup
        arithmetic = base in {"add", "adc", "sub", "sbc", "rsb", "rsc", "cmp", "cmn"}
        if base in {"add", "adc", "cmn"}:
            carryin = "c->cf" if base == "adc" else "0u"
            statements.append(f"::dsr::AluResult alu = ::dsr::add_with_carry({left}, {right}, {carryin});")
        elif base in {"sub", "cmp"}:
            statements.append(f"::dsr::AluResult alu = ::dsr::sub_with_carry({left}, {right}, 1u);")
        elif base == "rsb":
            statements.append(f"::dsr::AluResult alu = ::dsr::sub_with_carry({right}, {left}, 1u);")
        elif base == "sbc":
            statements.append(f"::dsr::AluResult alu = ::dsr::sub_with_carry({left}, {right}, c->cf);")
        elif base == "rsc":
            statements.append(f"::dsr::AluResult alu = ::dsr::sub_with_carry({right}, {left}, c->cf);")
        elif base in {"tst", "teq"}:
            op = "&" if base == "tst" else "^"
            statements.append(f"std::uint32_t result = {left} {op} {right};")
        else:
            op = {"and": "&", "eor": "^", "orr": "|", "bic": "& ~"}[base]
            statements.append(f"std::uint32_t result = {left} {op} {right};")
        if tests:
            if arithmetic:
                statements.append(self._alu_flags("alu.value", "alu.carry", "alu.overflow"))
            else:
                statements.append(self._alu_flags("result", shcarry))
            return self._cond(insn, "\n".join(statements))
        value = "alu.value" if arithmetic else "result"
        if dest == 15:
            if set_flags:
                statements.extend(self._exception_return(value))
            else:
                statements.extend(self._pc_transfer(insn, value, same_mode=True))
        else:
            statements.append(f"c->r[{dest}] = {value};")
            if set_flags:
                if arithmetic:
                    statements.append(self._alu_flags("alu.value", "alu.carry", "alu.overflow"))
                else:
                    statements.append(self._alu_flags("result", shcarry))
        return self._cond(insn, "\n".join(statements))

    def _exception_return(self, target: str) -> list[str]:
        # Exception-return data-processing instructions take CPSR from SPSR.
        return ["::dsr::dsr_exception_return(c);", f"DSR_RET(c, {target});", "return;"]

    def _slot_for(self, target: int, source: DecodedInstruction) -> Entry | None:
        key = (self.entry.module_id, source.address)
        address = target & ~1
        target_thumb = bool(target & 1) if target & 1 else self.thumb
        if key in self.calls:
            candidates = [
                self.module_mode_entries.get((module_id, address, target_thumb))
                or self.module_entries.get((module_id, address))
                for module_id in self.calls[key]
            ]
            found = [entry for entry in candidates if entry is not None and entry.thumb == target_thumb]
            return found[0] if len(found) == 1 else None
        exact = [
            entry for entry in self.address_entries.get(address, [])
            if entry.thumb == target_thumb
        ]
        if len(exact) == 1:
            return exact[0]
        exact = self.module_mode_entries.get((self.entry.module_id, address, target_thumb))
        if exact is not None:
            return exact
        fallback = self.module_entries.get((self.entry.module_id, address))
        return fallback if fallback is not None and fallback.thumb == target_thumb else None

    @staticmethod
    def _direct_slot_call(entry: Entry) -> str:
        # The EU camera builder needs per-machine dispatch; its generated slot
        # is shared by every machine and must not be replaced at runtime.
        if entry._cpu == "arm9" and entry.module_id == -1 and entry.address == 0x020c28b0:
            return f"::dsr::dsr_call(c, {_u32(entry.address | int(entry.thumb))});"
        return f"(*::dsr::{entry.slot})(c);"


    def _call_direct(self, insn: DecodedInstruction, target: int, tail: bool = False) -> str | None:
        entry = self._slot_for(target, insn)
        if entry is None:
            return None
        if entry.thumb != self.thumb:
            return None
        call = self._direct_slot_call(entry)
        self.stats["direct_calls"] += 1
        if tail:
            self.stats["tail_calls"] += 1
            # Tail call: the callee's own return is this function's return.
            return f"{call}\nreturn;"
        ret = insn.next_address
        link = ret | (1 if self.thumb else 0)
        return f"c->r[14] = {_u32(link)};\n{call}\nDSR_POSTCALL(c, {_u32(link)});"

    def _shared_region_return(self, target_expr: str, return_expr: str) -> list[str]:
        # Internal returns stay in the shared body; external targets leave through DSR_RET.
        return [
            f"dispatch_target = {target_expr};",
            f"if ((dispatch_target & 1u) == {1 if self.thumb else 0}u && "
            f"(dispatch_target & ~1u) >= {_u32(self.begin)} && "
            f"(dispatch_target & ~1u) < {_u32(self.end)}) goto L_region_dispatch;",
            f"DSR_RET(c, {return_expr});",
            "return;",
        ]

    def _pc_transfer(self, insn: DecodedInstruction, value: str, static: int | None = None, same_mode: bool = False) -> list[str]:
        link = self._manual_link(insn)
        target_expr = _u32(static) if static is not None else f"({value})"
        if same_mode:
            target_expr = f"({target_expr} {'| 1u' if self.thumb else '& ~3u'})"
        if self.dispatch_entries is not None:
            if link is None:
                self.stats["dsr_call_sites"] += 1
                self.stats["dynamic_exits"] += 1
                return [
                    f"dispatch_target = {target_expr};",
                    "goto L_region_dispatch;",
                ]
            self.stats["dsr_call_sites"] += 1
            cont = link & ~1
            body = [
                f"dispatch_target = {target_expr};",
                f"if ((dispatch_target & 1u) == {1 if self.thumb else 0}u && "
                f"(dispatch_target & ~1u) >= {_u32(self.begin)} && "
                f"(dispatch_target & ~1u) < {_u32(self.end)}) goto L_region_dispatch;",
                "::dsr::dsr_call(c, dispatch_target);",
                f"DSR_POSTCALL(c, {_u32(link)});",
            ]
            if self._inside(cont) and bool(link & 1) == self.thumb:
                body.extend([
                    f"dispatch_target = {_u32(link)};",
                    "goto L_region_dispatch;",
                ])
            else:
                body.extend([
                    f"::dsr::dsr_call(c, {_u32(link)});",
                    "return;",
                ])
            return body
        if link is not None:
            return [self._call_with_link(insn, target_expr, link)]
        if static is not None:
            static_address = static & ~1
            target_thumb = bool(static & 1) if self.arch != "v4t" else self.thumb
            if self._inside(static_address) and target_thumb == self.thumb:
                return [f"goto L_{static_address:08x};"]
            direct = self._call_direct(insn, static, tail=True)
            if direct is not None:
                return [direct]
        self.stats["dsr_call_sites"] += 1
        self.stats["dynamic_exits"] += 1
        return [
            f"std::uint32_t pc_target = {target_expr};",
            "::dsr::dsr_call(c, pc_target);",
            "return;",
        ]

    def _memory_offset(self, insn: DecodedInstruction, mem: Any, post_operand: Any | None, mem_subtracted: bool = False, mem_shift: Any | None = None) -> tuple[int, str, str, str, bool]:
        base = reg_index(insn, mem.base) if mem.base else 0
        base_value = self._cpp_reg(base, insn)
        pieces: list[str] = []
        subtract = bool(mem_subtracted) or int(mem.disp) < 0
        if post_operand is not None:
            if post_operand.type == ARM_OP_REG:
                subtract = subtract or bool(getattr(post_operand, "subtracted", False))
                index_value = self._reg_value(insn, post_operand)
                shift_obj = post_operand.shift
                shift_code = int(getattr(shift_obj, "type", 0))
                shift_type, by_register, rrx = shift_kind(shift_code)
                shift_amount = int(getattr(shift_obj, "value", 0))
                if by_register:
                    shift_amount_expr = self._cpp_reg(reg_index(insn, shift_amount), insn)
                else:
                    shift_amount_expr = f"{shift_amount}u"
                if shift_amount or rrx or by_register or shift_type != 0:
                    index_value = (
                        f"::dsr::shift_value({index_value}, {shift_type}u, "
                        f"{shift_amount_expr}, c->cf, {'true' if by_register else 'false'})"
                    )
                pieces.append(f"({index_value})")
            elif post_operand.type == ARM_OP_IMM:
                post_offset = int(post_operand.imm)
                subtract = subtract or post_offset < 0 or bool(getattr(post_operand, "subtracted", False))
                if post_offset:
                    pieces.append(_u32(abs(post_offset)))
        else:
            offset = int(mem.disp)
            if mem.index:
                index = reg_index(insn, mem.index)
                index_value = self._cpp_reg(index, insn)
                shift_obj = mem_shift
                shift_code = int(getattr(shift_obj, "type", 0))
                shift_type, by_register, rrx = shift_kind(shift_code)
                shift_amount = int(getattr(shift_obj, "value", 0))
                if by_register:
                    shift_amount_expr = self._cpp_reg(reg_index(insn, shift_amount), insn)
                else:
                    if not rrx and shift_code == 0:
                        shift_amount = int(mem.lshift)
                    shift_amount_expr = f"{shift_amount}u"
                if shift_amount or rrx or by_register or shift_type != 0:
                    index_value = (
                        f"::dsr::shift_value({index_value}, {shift_type}u, "
                        f"{shift_amount_expr}, c->cf, {'true' if by_register else 'false'})"
                    )
                pieces.append(f"({index_value})")
            if offset:
                pieces.append(_u32(abs(offset)))
        offset_value = " + ".join(pieces) if pieces else "0u"
        if subtract and pieces:
            offset_value = f"(-({offset_value}))"
        is_post = post_operand is not None
        address = base_value if is_post else f"({base_value} + ({offset_value}))"
        updated = f"({base_value} + ({offset_value}))"
        return base, address, updated, offset_value, is_post

    def _mem_literal_address(self, insn: DecodedInstruction, mem: Any, post: bool) -> int | None:
        if post or mem.index or not mem.base or reg_index(insn, mem.base) != 15:
            return None
        pc = ((insn.address + 4) & ~3) if insn.thumb else insn.address + 8
        return (pc + int(mem.disp)) & _U32

    def _memory_instruction(self, insn: DecodedInstruction) -> str | None:
        encoded_name = insn.mnemonic.split(".")[0]
        names = {"ldrt": "ldr", "ldrbt": "ldrb", "strt": "str", "strbt": "strb"}
        name = names.get(encoded_name, encoded_name)
        if name not in {"ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd", "str", "strb", "strh", "strd"}:
            return None
        ops = insn.operands
        if len(ops) < 2:
            return None
        if name in {"ldrd", "strd"}:
            return self._double_memory(insn, name)
        destination = operand_reg(insn, ops[0])
        mem_index = next((i for i, x in enumerate(ops) if x.type == ARM_OP_MEM), None)
        if mem_index is None:
            return None
        memop = ops[mem_index]
        postop = next((x for x in ops[mem_index + 1 :] if x.type in {ARM_OP_IMM, ARM_OP_REG}), None)
        is_post = postop is not None
        base, address, updated, _, _ = self._memory_offset(
            insn, memop.mem, postop,
            bool(getattr(memop, "subtracted", False)),
            getattr(memop, "shift", None),
        )
        literal_address = self._mem_literal_address(insn, memop.mem, is_post)
        if literal_address is not None:
            # PC-relative effective addresses are known at translation time,
            # but the guest memory value must remain live at execution time.
            address = _u32(literal_address)
        load = name.startswith("ldr")
        statements: list[str] = []
        if load:
            fn = {"ldr": "ldr32", "ldrb": "ldrb", "ldrh": "ldrh", "ldrsb": "ldrsb", "ldrsh": "ldrsh"}[name]
            statements.append(f"std::uint32_t loaded = ::dsr::{fn}(c, {address});")
            if (insn.writeback or is_post) and base != 15:
                statements.append(f"c->r[{base}] = {updated};")
            if destination == 15 and base == 13 and is_post:
                # `ldr pc, [sp], #4` is an epilogue: a static return.
                statements.extend(["DSR_RET(c, loaded);", "return;"])
            elif destination == 15:
                statements.extend(self._pc_transfer(insn, "loaded", same_mode=self.arch == "v4t"))
            else:
                statements.append(f"c->r[{destination}] = loaded;")
        else:
            fn = {"str": "wr32", "strb": "wr8", "strh": "wr16"}[name]
            source = self._reg_value(insn, ops[0], "store")
            statements.append(f"::dsr::{fn}(c, {address}, {source}, {_u32(insn.address)});")
            if (insn.writeback or is_post) and base != 15:
                statements.append(f"c->r[{base}] = {updated};")
        return self._cond(insn, "\n".join(statements))
    def _double_memory(self, insn: DecodedInstruction, name: str) -> str | None:
        ops = insn.operands
        if len(ops) < 2 or ops[0].type != ARM_OP_REG:
            return None
        low = operand_reg(insn, ops[0])
        mem_index = next((i for i, x in enumerate(ops) if x.type == ARM_OP_MEM), None)
        if mem_index is None:
            return None
        memop = ops[mem_index]
        postop = next((x for x in ops[mem_index + 1 :] if x.type in {ARM_OP_IMM, ARM_OP_REG}), None)
        base, address, updated, _, _ = self._memory_offset(
            insn, memop.mem, postop if postop is not None and insn.writeback else None,
            bool(getattr(memop, "subtracted", False)),
            getattr(memop, "shift", None),
        )
        if name == "ldrd":
            code = [
                f"std::uint32_t pair0 = ::dsr::rd32(c, {address});",
                f"std::uint32_t pair1 = ::dsr::rd32(c, ({address}) + 4u);",
            ]
            if insn.writeback and base != 15:
                code.append(f"c->r[{base}] = {updated};")
            code.extend((f"c->r[{low}] = pair0;", f"c->r[{(low + 1) & 15}] = pair1;"))
        else:
            code = [
                f"::dsr::wr32(c, {address}, c->r[{low}], {_u32(insn.address)});",
                f"::dsr::wr32(c, ({address}) + 4u, c->r[{(low + 1) & 15}], {_u32(insn.address)});",
            ]
            if insn.writeback and base != 15:
                code.append(f"c->r[{base}] = {updated};")
        return self._cond(insn, "\n".join(code))


    def _block_transfer(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        if name not in {"ldm", "ldmia", "ldmib", "ldmda", "ldmdb", "stm", "stmia", "stmib", "stmda", "stmdb", "push", "pop"}:
            return None
        load = name.startswith("ldm") or name == "pop"
        ops = insn.operands
        if name in {"push", "pop"}:
            base = 13
            regs = sorted(operand_reg(insn, x) for x in ops if x.type == ARM_OP_REG)
            mode = "DB" if name == "push" else "IA"
            wb = True
        else:
            if not ops or ops[0].type != ARM_OP_REG:
                return None
            base = operand_reg(insn, ops[0])
            regs = sorted(operand_reg(insn, x) for x in ops[1:] if x.type == ARM_OP_REG)
            mode_suffix = name[-2:].upper()
            mode = mode_suffix if mode_suffix in {"IA", "IB", "DA", "DB"} else "IA"
            wb = bool(insn.writeback)
        if not regs:
            return None
        count = len(regs)
        base_value = self._cpp_reg(base, insn)
        offset_start = {
            "IA": 0,
            "IB": 4,
            "DA": -4 * count + 4,
            "DB": -4 * count,
        }[mode]
        start_expr = f"({base_value} + ({offset_start}))" if offset_start < 0 else f"({base_value} + {offset_start}u)"
        wb_expr = f"({base_value} {'-' if mode in {'DA', 'DB'} else '+'} {4 * count}u)"
        s_bit = bool(not insn.thumb and ((insn.raw >> 22) & 1))
        has_pc = 15 in regs
        statements = [f"std::uint32_t transfer_base = {base_value};", f"std::uint32_t transfer_addr = {start_expr};"]
        for reg in regs:
            if load:
                statements.append(f"std::uint32_t transfer_{reg} = ::dsr::rd32(c, transfer_addr);")
                statements.append("transfer_addr += 4u;")
            else:
                source = self._cpp_reg(reg, insn, "store")
                if s_bit and not has_pc and reg != 15:
                    source = f"::dsr::dsr_user_reg_read(c, {reg}u)"
                statements.append(f"::dsr::wr32(c, transfer_addr, {source}, {_u32(insn.address)});")
                statements.append("transfer_addr += 4u;")
        if wb:
            statements.append(f"c->r[{base}] = {wb_expr};")
        if load:
            for reg in regs:
                if reg == 15:
                    continue
                if s_bit and not has_pc:
                    statements.append(f"::dsr::dsr_user_reg_write(c, {reg}u, transfer_{reg});")
                else:
                    statements.append(f"c->r[{reg}] = transfer_{reg};")
            if has_pc:
                if s_bit:
                    statements.extend(["::dsr::dsr_exception_return(c);", "DSR_RET(c, transfer_15);", "return;"])
                else:
                    if base in (13, 11):
                        # Epilogue pop / APCS `ldmdb fp, {.., pc}`: static return.
                        if self.dispatch_entries is not None:
                            statements.extend(self._shared_region_return("transfer_15", "transfer_15"))
                        else:
                            statements.extend(["DSR_RET(c, transfer_15);", "return;"])
                    else:
                        statements.extend(self._pc_transfer(insn, "transfer_15", same_mode=self.arch == "v4t"))
        return self._cond(insn, "\n".join(statements))

    def _mul(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        if insn.update_flags and name.endswith("s"):
            name = name[:-1]
        ops = insn.operands
        if name in {"mul", "mla", "mls"}:
            if len(ops) == 2 and name == "mul":
                d = operand_reg(insn, ops[0])
                lhs, rhs = self._reg_value(insn, ops[0]), self._reg_value(insn, ops[1])
            elif len(ops) >= 3:
                d = operand_reg(insn, ops[0])
                lhs, rhs = self._reg_value(insn, ops[1]), self._reg_value(insn, ops[2])
            else:
                return None
            value = f"({lhs} * {rhs})"
            if name in {"mla", "mls"} and len(ops) >= 4:
                acc = self._reg_value(insn, ops[3])
                value = f"({value} {'-' if name == 'mls' else '+'} {acc})"
            statements = [f"std::uint32_t product = {value};"]
            if d == 15:
                statements.extend(self._pc_transfer(insn, "product", same_mode=True))
            else:
                statements.append(f"c->r[{d}] = product;")
                if insn.update_flags:
                    statements.append(self._alu_flags("product"))
            return self._cond(insn, "\n".join(statements))
        if name in {"umull", "umlal", "smull", "smlal"}:
            if len(ops) < 4:
                return None
            lo, hi = operand_reg(insn, ops[0]), operand_reg(insn, ops[1])
            lhs, rhs = self._reg_value(insn, ops[2]), self._reg_value(insn, ops[3])
            signed = name.startswith("s")
            if name.endswith("lal"):
                multiply = f"::dsr::{'smull' if signed else 'umull'}({lhs}, {rhs})"
                expr = f"({multiply} + ((static_cast<std::uint64_t>(c->r[{hi}]) << 32) | c->r[{lo}]))"
            else:
                expr = f"::dsr::{'smull' if signed else 'umull'}({lhs}, {rhs})"
            statements = [f"std::uint64_t wide = {expr};", f"c->r[{lo}] = static_cast<std::uint32_t>(wide);", f"c->r[{hi}] = static_cast<std::uint32_t>(wide >> 32);"]
            if insn.update_flags:
                statements += [f"c->n = (wide >> 63) & 1u;", f"c->z = (wide == 0u);"]
            return self._cond(insn, "\n".join(statements))
        # ARMv5TE signed halfword/word multiply family.
        if name.startswith(("smla", "smul", "smlal")):
            if len(ops) < 3:
                return None
            if name.startswith("smlal") and len(ops) >= 4:
                lo, hi = operand_reg(insn, ops[0]), operand_reg(insn, ops[1])
                lhs, rhs = self._reg_value(insn, ops[2]), self._reg_value(insn, ops[3])
                high_a = name[-2] == "t"
                high_b = name[-1] == "t"
                accumulate = f"((static_cast<std::uint64_t>(c->r[{hi}]) << 32) | c->r[{lo}])"
                statements = [
                    f"std::uint64_t wide = ::dsr::smlalxy({accumulate}, {lhs}, {rhs}, {'true' if high_a else 'false'}, {'true' if high_b else 'false'});",
                    f"c->r[{lo}] = static_cast<std::uint32_t>(wide);",
                    f"c->r[{hi}] = static_cast<std::uint32_t>(wide >> 32);",
                ]
            elif name.startswith("smlaw") and len(ops) >= 4:
                d = operand_reg(insn, ops[0])
                lhs, rhs, acc = (self._reg_value(insn, x) for x in ops[1:4])
                statements = [f"c->r[{d}] = ::dsr::smlawy(c, {lhs}, {rhs}, {acc}, {'true' if name.endswith('t') else 'false'});"]
            elif name.startswith("smla") and len(ops) >= 4:
                d = operand_reg(insn, ops[0])
                lhs, rhs, acc = (self._reg_value(insn, x) for x in ops[1:4])
                high_a = name[-2] == "t"
                high_b = name[-1] == "t"
                statements = [f"c->r[{d}] = ::dsr::smla_xy(c, {lhs}, {rhs}, {acc}, {'true' if high_a else 'false'}, {'true' if high_b else 'false'});"]
            elif name.startswith("smulw") and len(ops) >= 3:
                d = operand_reg(insn, ops[0])
                lhs, rhs = self._reg_value(insn, ops[1]), self._reg_value(insn, ops[2])
                statements = [f"c->r[{d}] = ::dsr::smulwy({lhs}, {rhs}, {'true' if name.endswith('t') else 'false'});"]
            elif name.startswith("smul") and len(ops) >= 3:
                d = operand_reg(insn, ops[0])
                lhs, rhs = self._reg_value(insn, ops[1]), self._reg_value(insn, ops[2])
                high_a = name[-2] == "t"
                high_b = name[-1] == "t"
                statements = [f"c->r[{d}] = ::dsr::smulxy({lhs}, {rhs}, {'true' if high_a else 'false'}, {'true' if high_b else 'false'});"]
            else:
                return None
            return self._cond(insn, "\n".join(statements))
        return None

    def _saturating(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        if name not in {"qadd", "qsub", "qdadd", "qdsub"}:
            return None
        if len(insn.operands) < 3:
            return None
        d = operand_reg(insn, insn.operands[0])
        a = self._reg_value(insn, insn.operands[1])
        b = self._reg_value(insn, insn.operands[2])
        return self._cond(insn, f"c->r[{d}] = ::dsr::{name}(c, {a}, {b});")

    def _multiply_misc(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        ops = insn.operands
        if name == "clz" and len(ops) >= 2:
            d = operand_reg(insn, ops[0])
            src = self._reg_value(insn, ops[1])
            return self._cond(insn, f"c->r[{d}] = ::dsr::clz32({src});")
        if name in {"swp", "swpb"} and len(ops) >= 3:
            if ops[0].type != ARM_OP_REG or ops[1].type != ARM_OP_REG or ops[2].type != ARM_OP_MEM:
                return None
            d = operand_reg(insn, ops[0])
            source = operand_reg(insn, ops[1])
            base = reg_index(insn, ops[2].mem.base)
            addr = self._cpp_reg(base, insn)
            width = "rd8" if name == "swpb" else "ldr32"
            store = "wr8" if name == "swpb" else "wr32"
            source_expr = self._cpp_reg(source, insn, "store")
            return self._cond(insn, f"std::uint32_t old = ::dsr::{width}(c, {addr});\n::dsr::{store}(c, {addr}, {source_expr}, {_u32(insn.address)});\nc->r[{d}] = old;")
        if name in {"adr"} and len(ops) >= 2:
            d = operand_reg(insn, ops[0])
            value = int(ops[1].imm) & _U32
            if insn.thumb:
                # Thumb-1 ADR's Capstone immediate is an offset, not an
                # absolute address; its PC base is word-aligned.
                value = (((insn.address + 4) & ~3) + value) & _U32
            return self._cond(insn, f"c->r[{d}] = {_u32(value)};")
        return None

    def _branch(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        if name not in {"b", "bl", "blx", "bx"}:
            return None
        shared = self.dispatch_entries is not None
        target = self._branch_target(insn)
        if name == "b":
            if target is None:
                return None
            target_address = target & ~1
            link = self._manual_link(insn)
            if link is not None:
                if shared and self._inside(target_address):
                    target_expr = _u32(target | (1 if self.thumb else 0))
                    body = f"dispatch_target = {target_expr};\ngoto L_region_dispatch;"
                else:
                    body = self._call_with_link(insn, _u32(target), link)
                return self._cond(insn, body)
            if self._inside(target_address):
                if target_address < insn.address:
                    self.loop_headers.add(target_address)
                if shared:
                    target_expr = _u32(target | (1 if self.thumb else 0))
                    if insn.condition == 14:
                        return f"dispatch_target = {target_expr};\ngoto L_region_dispatch;"
                    next_pc = _u32(insn.next_address | (1 if self.thumb else 0))
                    return (
                        f"if (::dsr::condition_passed(c, {insn.condition}u)) {{\n"
                        f"    dispatch_target = {target_expr};\n"
                        "    goto L_region_dispatch;\n"
                        "}\n"
                        f"dispatch_target = {next_pc};\ngoto L_region_dispatch;"
                    )
                if insn.condition == 14:
                    return f"goto L_{target_address:08x};"
                next_pc = insn.next_address
                return f"if (::dsr::condition_passed(c, {insn.condition}u)) goto L_{target_address:08x};\ngoto L_{next_pc:08x};"
            direct = self._call_direct(insn, target, tail=True)
            if direct is not None:
                return self._cond(insn, direct)
            body = f"::dsr::dsr_call(c, {_u32(target)});\nreturn;"
            self.stats["dsr_call_sites"] += 1
            return self._cond(insn, body)
        if name in {"bl", "blx"} and target is not None:
            if name == "blx" and self.arch == "v4t":
                return self._cond(insn, self._emit_bad(insn, "BLX is unavailable on ARMv4T"))
            ret = insn.next_address
            link = ret | (1 if self.thumb else 0)
            thumb_target = not self.thumb if name == "blx" else self.thumb
            target_expr = _u32(target | (1 if thumb_target else 0))
            if shared and name == "bl" and self._inside(target & ~1):
                body = [
                    f"c->r[14] = {_u32(link)};",
                    f"dispatch_target = {target_expr};",
                    "goto L_region_dispatch;",
                ]
                self.stats["direct_calls"] += 1
                return self._cond(insn, "\n".join(body))
            body = [f"c->r[14] = {_u32(link)};"]
            entry = self._slot_for(target, insn)
            if entry is not None and entry.thumb == self.thumb and name != "blx":
                body.append(self._direct_slot_call(entry))
                self.stats["direct_calls"] += 1
            else:
                body.append(f"::dsr::dsr_call(c, {target_expr});")
                self.stats["dsr_call_sites"] += 1
            body.append(f"DSR_POSTCALL(c, {_u32(link)});")
            return self._cond(insn, "\n".join(body))
        if name in {"bx", "blx"}:
            if not insn.operands or insn.operands[0].type != ARM_OP_REG:
                return None
            reg = operand_reg(insn, insn.operands[0])
            value = self._cpp_reg(reg, insn)
            is_link = name == "blx"
            if is_link and self.arch == "v4t":
                return self._cond(insn, self._emit_bad(insn, "BLX register is unavailable on ARMv4T"))
            if not is_link and (reg == 14 or self._popped_before(insn, reg)):
                if shared:
                    body = self._shared_region_return(value, value)
                    return self._cond(insn, "\n".join(body))
                body = f"DSR_RET(c, {value});\nreturn;"
            elif is_link:
                link = insn.next_address | (1 if self.thumb else 0)
                if shared:
                    body = [
                        f"c->r[14] = {_u32(link)};",
                        f"dispatch_target = {value};",
                        f"if ((dispatch_target & 1u) == {1 if self.thumb else 0}u && "
                        f"(dispatch_target & ~1u) >= {_u32(self.begin)} && "
                        f"(dispatch_target & ~1u) < {_u32(self.end)}) goto L_region_dispatch;",
                        "::dsr::dsr_call(c, dispatch_target);",
                        f"DSR_POSTCALL(c, {_u32(link)});",
                        f"dispatch_target = {_u32(link)};",
                        "goto L_region_dispatch;",
                    ]
                    self.stats["dsr_call_sites"] += 1
                    return self._cond(insn, "\n".join(body))
                body = f"c->r[14] = {_u32(link)};\n::dsr::dsr_call(c, {value});\nDSR_POSTCALL(c, {_u32(link)});"
                self.stats["dsr_call_sites"] += 1
            else:
                body = "\n".join(self._pc_transfer(insn, value))
            return self._cond(insn, body)
        return None

    def _popped_before(self, insn: DecodedInstruction, reg: int) -> bool:
        """True when `bx reg` returns a value just popped off the stack.

        ARMv4T Thumb epilogues cannot pop into pc with interworking and use
        `pop {rX}; bx rX` (optionally with an `add sp` in between)."""
        step = 2 if insn.thumb else 4
        cursor = insn.address
        for _ in range(3):
            cursor -= step
            prev = self.insns.get(cursor)
            if prev is None:
                return False
            name = prev.mnemonic.split(".")[0]
            ops = prev.operands
            if name == "pop" or (name.startswith("ldm") and ops and ops[0].type == ARM_OP_REG
                                 and operand_reg(prev, ops[0]) == 13 and prev.writeback):
                regs = [operand_reg(prev, x) for x in ops if x.type == ARM_OP_REG]
                return reg in regs
            if name == "ldr" and ops and operand_reg(prev, ops[0]) == reg:
                mem = next((x for x in ops if x.type == ARM_OP_MEM), None)
                return mem is not None and mem.mem.base and reg_index(prev, mem.mem.base) == 13 and prev.writeback
            if name == "add" and ops and operand_reg(prev, ops[0]) == 13:
                continue
            return False
        return False

    def _swi(self, insn: DecodedInstruction) -> str | None:
        if insn.mnemonic.split(".")[0] not in {"swi", "svc"}:
            return None
        immediate = int(insn.operands[0].imm) if insn.operands and insn.operands[0].type == ARM_OP_IMM else insn.raw & (0xFF if self.thumb else 0xFFFFFF)
        return self._cond(insn, f"::dsr::dsr_swi(c, {_u32(immediate)}, {_u32(insn.address)}, {'true' if self.thumb else 'false'});")

    def _mrs_msr(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        ops = insn.operands
        if name == "mrs" and len(ops) >= 2:
            d = operand_reg(insn, ops[0])
            spsr = "spsr" in insn.op_str.lower()
            return self._cond(insn, f"c->r[{d}] = ::dsr::dsr_mrs_{'spsr' if spsr else 'cpsr'}(c);")
        if name == "msr":
            spsr = "spsr" in insn.op_str.lower()
            mask = ((insn.raw >> 16) & 15) if not self.thumb else 15
            source = ops[-1] if ops else None
            if source is None:
                return None
            if source.type == ARM_OP_IMM:
                value = _u32(int(source.imm))
            elif source.type == ARM_OP_REG:
                reg = operand_reg(insn, source)
                value = self._cpp_reg(reg, insn)
            else:
                return None
            fn = "dsr_msr_spsr" if spsr else "dsr_msr_cpsr"
            return self._cond(insn, f"::dsr::{fn}(c, {value}, {mask}u);")
        return None

    def _coprocessor(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        unsupported_base = next((base for base in ("cdp", "ldc", "stc") if name.startswith(base)), None)
        if name not in {"mrc", "mcr"} and unsupported_base is None:
            return None
        if unsupported_base is not None:
            return self._cond(insn, self._emit_bad(insn, f"{unsupported_base.upper()} has no runtime coprocessor operation API"))
        raw = insn.raw
        cp = (raw >> 8) & 15
        op1 = (raw >> 21) & 7
        crn = (raw >> 16) & 15
        rd = (raw >> 12) & 15
        crm = raw & 15
        op2 = (raw >> 5) & 7
        if name == "mrc":
            value = f"::dsr::dsr_mrc(c, {cp}u, {op1}u, {crn}u, {crm}u, {op2}u, {_u32(insn.address)})"
            if rd == 15:
                body = f"std::uint32_t cp_value = {value};\nc->n = (cp_value >> 31) & 1u;\nc->z = (cp_value >> 30) & 1u;\nc->cf = (cp_value >> 29) & 1u;\nc->v = (cp_value >> 28) & 1u;"
            else:
                body = f"c->r[{rd}] = {value};"
        else:
            value = self._cpp_reg(rd, insn)
            body = f"::dsr::dsr_mcr(c, {cp}u, {op1}u, {crn}u, {crm}u, {op2}u, {value}, {_u32(insn.address)});"
        return self._cond(insn, body)

    def _misc(self, insn: DecodedInstruction) -> str | None:
        name = insn.mnemonic.split(".")[0]
        if name in {"nop", "pld"}:
            # PLD is a hint and has no architectural state effects.
            return self._cond(insn, "/* hint */")
        if name in {"bkpt", "udf"}:
            return self._cond(insn, self._emit_bad(insn, name.upper()))
        return None

    def _one(self, insn: DecodedInstruction) -> tuple[str, bool]:
        if unsupported_for_arch(insn, self.arch):
            return (
                self._cond(insn, self._emit_bad(insn, f"{insn.mnemonic} is unavailable on {self.arch}")),
                insn.condition == 14,
            )
        for translator in (
            self._branch,
            self._jump_table,
            self._swi,
            self._mrs_msr,
            self._coprocessor,
            self._block_transfer,
            self._memory_instruction,
            self._saturating,
            self._multiply_misc,
            self._mul,
            self._alu,
            self._misc,
        ):
            body = translator(insn)
            if body is not None:
                return body, self._terminator(insn) and insn.condition == 14
        return (
            self._cond(insn, self._emit_bad(insn, "unimplemented instruction decoder case")),
            insn.condition == 14,
        )

    def _cost(self, block: Block) -> tuple[int, int]:
        """Return the CPU-cycle numerator and its denominator's binary shift.

        ARM9 uses a calibrated average of 161/64 cycles per ordinary fetched
        instruction, counting both halves of a long Thumb call. Data/delay
        SWIs charge one base cycle; runtime HLE adds dispatch/handler cycles.
        This is not a cache/wait-state model. ARM7 keeps its integer estimate.
        """
        if self.cpu == "arm9":
            cost = 0
            for insn in block.instructions:
                if self._branch_name(insn) in {"swi", "svc"}:
                    number = (insn.raw & 0xFF) if insn.thumb else ((insn.raw >> 16) & 0xFF)
                    if number in _ARM9_HLE_TIMED_SWIS:
                        cost += 64
                        continue
                cost += (2 if insn.thumb and insn.size == 4 else 1) * 161
            return cost or 161, 6
        cost = len(block.instructions)
        for insn in block.instructions:
            if insn.mnemonic.split(".")[0] in {"ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd", "str", "strb", "strh", "strd", "swp", "swpb"} or insn.mnemonic.startswith(("ldm", "stm", "push", "pop")):
                cost += 1
        return max(1, cost), 0

    def _check_overlaps(self) -> None:
        """Diagnose decoded instructions that start inside another one."""
        ordered = sorted(self.insns.items())
        for (address, insn), (next_address, _) in zip(ordered, ordered[1:]):
            if next_address < insn.next_address:
                self.stats["unsupported_sites"].append({
                    "address": next_address,
                    "module": self.module["name"],
                    "instruction": self.insns[next_address].text,
                    "reason": f"starts inside instruction at 0x{address:08x} (bad branch/jump-table target)",
                })
                self.stats["undefined_sites"] += 1

    def emit(self) -> str:
        self._discover()
        shared = self.dispatch_entries is not None
        if shared:
            self._decode_shared_region()
        self._check_overlaps()
        blocks = self._blocks()
        if not blocks:
            self.stats["unsupported_sites"].append({
                "address": self.entry.address, "module": self.module["name"],
                "instruction": "<decode failure>", "reason": "no instruction decoded",
            })
            self.stats["undefined_sites"] += 1
            return self._emit_empty()
        self.stats["functions"] += len(self.dispatch_entries) if shared else 1
        self.stats["blocks"] += len(blocks)
        self.stats["extra_entries"] += (
            sum(int(entry.extra) for entry in self.dispatch_entries) if shared else int(self.entry.extra)
        )
        if shared:
            code = [
                f"void {self._shared_body_name()}(dsr::Cpu* c, std::uint32_t entry_address) {{",
                f"    DSR_ENTRY(c, {self.entry.module_id}, entry_address);",
                "    if (c->budget <= 0) dsr::dsr_checkpoint(c, entry_address);",
                "    bool dispatch_first_block = true;",
                f"    std::uint32_t dispatch_target = entry_address | {1 if self.thumb else 0}u;",
                "    goto L_region_dispatch;",
                "L_region_dispatch: ;",
                f"    if ((dispatch_target & 1u) != {1 if self.thumb else 0}u) {{",
                "        ::dsr::dsr_call(c, dispatch_target);",
                "        return;",
                "    }",
                "    switch (dispatch_target & ~1u) {",
            ]
            for address in sorted(self.insns):
                code.append(f"    case {_u32(address)}: goto L_{address:08x};")
            code.extend([
                "    default: {",
                f"        std::uint32_t invalid_address = dispatch_target & ~1u;",
                f"        if (invalid_address >= {_u32(self.begin)} && invalid_address < {_u32(self.end)})",
                "            ::dsr::dsr_unresolved_target(c, dispatch_target);",
                "        ::dsr::dsr_call(c, dispatch_target);",
                "        return;",
                "    }",
                "    }",
            ])
        else:
            code = [
                f"static void {self._body_name()}(dsr::Cpu* c) {{",
                f"    DSR_ENTRY(c, {self.entry.module_id}, {_u32(self.entry.address)});",
                f"    goto L_{self.entry.address:08x};",
            ]
        for block in blocks:
            if block.address not in self.insns:
                continue
            code.append(f"L_{block.address:08x}: ;")
            if shared:
                if block.address in self.loop_headers:
                    code.append(
                        f"    if (!dispatch_first_block && c->budget <= 0) "
                        f"dsr::dsr_checkpoint(c, {_u32(block.address)});"
                    )
                code.append("    dispatch_first_block = false;")
            elif block.address in self.loop_headers or block.address == self.entry.address:
                code.append(f"    if (c->budget <= 0) dsr::dsr_checkpoint(c, {_u32(block.address)});")
            cost, shift = self._cost(block)
            if shift:
                code.append(f"    c->cycle_fraction += {cost}u;")
                code.append(f"    c->budget -= c->cycle_fraction >> {shift};")
                code.append(f"    c->cycle_fraction &= {(1 << shift) - 1}u;")
            else:
                code.append(f"    c->budget -= {cost};")
            last_always_exits = False
            for insn in block.instructions:
                code.append("    {")
                code.append(f"        // 0x{insn.address:08x}: {insn.text}")
                self.stats["instructions"] += 1
                if self._branch_name(insn) in {"bl", "blx"}:
                    self.stats["direct_branch_sites"] += 1
                if insn.mnemonic.startswith(("ldr", "str", "ldm", "stm", "push", "pop", "swp")):
                    self.stats["memory_sites"] += 1
                body, exits = self._one(insn)
                code.extend("        " + line if line else "" for line in body.splitlines())
                code.append("    }")
                last_always_exits = exits
            if not last_always_exits:
                next_addr = block.instructions[-1].next_address
                if any(b.address == next_addr for b in blocks):
                    code.append(f"    goto L_{next_addr:08x};")
                else:
                    code.append("    DSR_RET(c, c->r[14]);")
                    code.append("    return;")
        code.append("}")
        if shared:
            for entry in self.dispatch_entries:
                code.extend([
                    f"static void {entry.body_symbol}(dsr::Cpu* c) {{",
                    f"    {self._shared_body_name()}(c, {_u32(entry.address)});",
                    "}",
                ])
        return "\n".join(code)

    def _body_name(self) -> str:
        return self.entry.body_symbol

    def _shared_body_name(self) -> str:
        mode = "thumb" if self.thumb else "arm"
        return f"rc_shared_{_entry_suffix(self.cpu, self.entry.module_id, self.begin)}_{mode}"

    def _emit_empty(self) -> str:
        if self.dispatch_entries is not None:
            code = [
                f"void {self._shared_body_name()}(dsr::Cpu* c, std::uint32_t entry_address) {{",
                f"    DSR_ENTRY(c, {self.entry.module_id}, entry_address);",
                "    dsr::dsr_undefined(c, entry_address, 0u);",
                "}",
            ]
            for entry in self.dispatch_entries:
                code.extend([
                    f"static void {entry.body_symbol}(dsr::Cpu* c) {{",
                    f"    {self._shared_body_name()}(c, {_u32(entry.address)});",
                    "}",
                ])
            return "\n".join(code)
        return "\n".join(
            [
                f"static void {self._body_name()}(dsr::Cpu* c) {{",
                f"    dsr::dsr_undefined(c, {_u32(self.entry.address)}, 0u);",
                "}",
            ]
        )



# Keep emitted source indentation deterministic and inexpensive.
def _indent(text: str, count: int = 4) -> str:
    prefix = " " * count
    return "\n".join(prefix + line if line else line for line in text.splitlines())


def _parse_extra_entries(path: str | Path | None) -> list[tuple[int, int, bool]]:
    if path is None:
        return []
    entries: list[tuple[int, int, bool]] = []
    for line_no, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 3:
            raise ValueError(f"{path}:{line_no}: expected module_id addr thumb")
        try:
            module_text, address_text, thumb_text = fields
            if module_text.startswith(("0x", "-0x")):
                module_id = int(module_text, 0)
            elif module_text.lstrip("-").isdigit():
                module_id = int(module_text, 10)
            else:
                module_id = int(module_text, 16)
            address = int(address_text, 0) if address_text.startswith("0x") else int(address_text, 16)
            thumb_raw = int(thumb_text, 0)
        except ValueError as exc:
            raise ValueError(f"{path}:{line_no}: malformed entry") from exc
        if module_id >= 0x80000000:
            module_id -= 0x100000000
        if thumb_raw not in (0, 1):
            raise ValueError(f"{path}:{line_no}: thumb must be 0 or 1")
        entries.append((module_id, address & ~1, bool(thumb_raw)))
    return entries


def _prepare_entries(manifest: dict[str, Any], extra_path: str | Path | None) -> tuple[list[Entry], dict[int, bytes], dict[tuple[int, int], Entry], dict[tuple[int, int], list[int]]]:
    cpu = manifest["cpu"]
    entries: list[Entry] = []
    images: dict[int, bytes] = {}
    module_by_id = {int(module["module_id"]): module for module in manifest["modules"]}
    function_by_id: dict[int, list[dict[str, Any]]] = {}
    function_starts: dict[int, list[int]] = {}
    calls: dict[tuple[int, int], list[int]] = {}
    call_records: list[tuple[list[int], int]] = []
    module_entries: dict[tuple[int, int], Entry] = {}
    for module in manifest["modules"]:
        module_id = int(module["module_id"])
        images[module_id] = _module_image(module)
        functions = sorted(module.get("functions", []), key=lambda fn: int(fn["addr"]))
        function_by_id[module_id] = functions
        function_starts[module_id] = [int(fn["addr"]) & ~1 for fn in functions]
        for fn in functions:
            addr = int(fn["addr"]) & ~1
            thumb = bool(fn.get("thumb", False))
            name = str(fn.get("name") or f"func_{addr:08x}")
            entry = Entry(module, fn, addr, thumb, name, False, cpu)
            entries.append(entry)
            module_entries.setdefault((module_id, addr), entry)
        for call in module.get("calls", []):
            source = int(call["from"]) & ~1
            target = int(call["to"]) & ~1
            targets = [int(value) for value in call.get("modules", [])]
            calls[(module_id, source)] = targets
            call_records.append((targets, target))

    extras = _parse_extra_entries(extra_path)
    for module in manifest["modules"]:
        module_id = int(module["module_id"])
        for point in module.get("entry_points", []):
            address, thumb = int(point["addr"]), bool(point["thumb"])
            if address % (2 if thumb else 4):
                raise ValueError(f"manifest entry 0x{address:08x} is misaligned in module {module_id}")
            owners = [fn for fn in function_by_id[module_id]
                      if bool(fn.get("thumb", False)) == thumb
                      and int(fn["addr"]) <= address < int(fn["addr"]) + int(fn["size"])]
            if not owners or any(int(low) <= address < int(high)
                                 for low, high in module.get("data_ranges", [])):
                raise ValueError(f"manifest entry 0x{address:08x} is not code in the requested state in module {module_id}")
            extras.append((module_id, address, thumb))
    for target_modules, target in call_records:
        for module_id in target_modules:
            functions = function_by_id.get(module_id, [])
            starts = function_starts.get(module_id, [])
            index = bisect_right(starts, target) - 1
            if index < 0:
                continue
            owner = functions[index]
            start = starts[index]
            if start < target < start + int(owner["size"]):
                extras.append((module_id, target, bool(owner.get("thumb", False))))

    seen_extra = {(entry.module_id, entry.address, entry.thumb) for entry in entries}
    for module_id, address, thumb in extras:
        if (module_id, address, thumb) in seen_extra:
            continue
        seen_extra.add((module_id, address, thumb))
        module = module_by_id.get(module_id)
        if module is None:
            raise ValueError(f"extra entry references unknown module id {module_id}")
        functions = function_by_id.get(module_id, [])
        owners = [fn for fn in functions
                  if bool(fn.get("thumb", False)) == thumb
                  and int(fn["addr"]) <= address < int(fn["addr"]) + int(fn["size"])]
        owner = max(owners, key=lambda fn: int(fn["addr"])) if owners else None
        if owner is None:
            raise ValueError(f"extra entry 0x{address:08x} is not inside a known function in module {module_id}")
        entry = Entry(module, owner, address, thumb, f"extra_{address:08x}", True, cpu)
        entries.append(entry)
        module_entries.setdefault((module_id, address), entry)
    entries.sort(key=lambda entry: (entry.module_id, entry.address, entry.extra))
    return entries, images, module_entries, calls




def _prepare_dispatch_regions(
    manifest: dict[str, Any],
    entries: list[Entry],
    module_entries: dict[tuple[int, int], Entry],
    cpu: str,
) -> tuple[list[DispatchRegion], dict[tuple[int, int, bool], Entry]]:
    mode_entries = {
        (entry.module_id, entry.address, entry.thumb): entry
        for entry in entries
    }
    regions: list[DispatchRegion] = []
    for module in manifest["modules"]:
        module_id = int(module["module_id"])
        for record in module.get("dispatch_regions", []):
            start = int(record["addr"]) & ~1
            size = int(record["size"])
            if size <= 0:
                continue
            end = start + size
            thumb = bool(record["thumb"])
            slots: list[Entry] = []
            for raw_address in record.get("entries", []):
                address = (int(raw_address, 0) if isinstance(raw_address, str) else int(raw_address)) & ~1
                if not start <= address < end:
                    continue
                key = (module_id, address, thumb)
                slot = mode_entries.get(key)
                if slot is None:
                    owners = [
                        candidate for candidate in module.get("functions", [])
                        if bool(candidate.get("thumb", False)) == thumb
                        and int(candidate["addr"]) <= address < int(candidate["addr"]) + int(candidate["size"])
                    ]
                    owner = min(
                        owners,
                        key=lambda candidate: int(candidate["size"]),
                        default={"addr": start, "size": size, "thumb": thumb},
                    )
                    slot = Entry(module, owner, address, thumb, f"dispatch_{address:08x}", True, cpu)
                    mode_entries[key] = slot
                    entries.append(slot)
                module_entries.setdefault((module_id, address), slot)
                slots.append(slot)
            slots = sorted({(slot.module_id, slot.address, slot.thumb): slot for slot in slots}.values(),
                           key=lambda slot: (slot.address, slot.thumb))
            if not slots:
                continue
            data_ranges = []
            for raw_range in record.get("data_ranges", []):
                if len(raw_range) != 2:
                    continue
                low, high = (
                    int(value, 0) if isinstance(value, str) else int(value)
                    for value in raw_range
                )
                low, high = max(start, low), min(end, high)
                if low < high:
                    data_ranges.append((low, high))
            body_function = {
                "addr": start,
                "size": size,
                "thumb": thumb,
                "name": f"dispatch_region_{start:08x}",
            }
            body_entry = Entry(module, body_function, start, thumb, body_function["name"], False, cpu)
            regions.append(DispatchRegion(module, start, size, thumb, slots, sorted(data_ranges), body_entry))
    regions.sort(key=lambda region: (region.module_id, region.address, region.thumb))
    return regions, mode_entries


def _secondary_branch_entries(
    branch_edges: list[tuple[Entry, int]],
    entries: list[Entry],
    module_entries: dict[tuple[int, int], Entry],
    module_mode_entries: dict[tuple[int, int, bool], Entry],
    boundaries: dict[tuple[int, int, bool], set[int]],
    overlaps: dict[tuple[int, int, bool], set[int]],
    cpu: str,
    table_slots: set[tuple[int, int]] = frozenset(),
) -> tuple[list[Entry], set[tuple[int, int, bool, bool]]]:
    """Add entries for direct branches into another function's decoded blocks.

    ``table_slots`` are ARM jump-table slot addresses reached by a computed
    ``add pc, pc, rX, lsl #2``. A slot may sit past an unconditional branch in
    a DSD-split owner, so it need not be a decoded boundary of that owner; any
    word-aligned ARM address is an instruction start.
    """
    functions = [entry for entry in entries if not entry.extra]
    candidates: dict[tuple[int, int, bool], Entry] = {}
    sources: set[tuple[int, int, bool, bool]] = set()
    for source, target in branch_edges:
        address = target & ~1
        key = (source.module_id, address, source.thumb)
        source_key = (source.module_id, source.address, source.thumb, source.extra)
        if key in module_mode_entries:
            continue
        if key in candidates:
            sources.add(source_key)
            continue

        owners = [
            entry for entry in functions
            if entry.module_id == source.module_id and entry.thumb == source.thumb
            and entry.address <= address < entry.end
        ]
        if not owners:
            continue
        if len(owners) > 1:
            # Recursive ARM7 discovery can give two entry roots the same shared
            # tail. These are not ambiguous owners if their same-image, same-mode
            # decoded suffixes and bounds agree. Do not pick an arbitrary range
            # owner: divergent bounds, undecoded data and overlapping instructions
            # still reject the secondary entry.
            suffix = {
                boundary for boundary in boundaries.get(
                    (owners[0].module_id, owners[0].address, owners[0].thumb), set()
                ) if boundary >= address
            }
            if address not in suffix or any(
                owner.end != owners[0].end
                or suffix != {
                    boundary for boundary in boundaries.get(
                        (owner.module_id, owner.address, owner.thumb), set()
                    ) if boundary >= address
                }
                or any(boundary in overlaps.get(
                    (owner.module_id, owner.address, owner.thumb), set()
                ) for boundary in suffix)
                for owner in owners
            ):
                continue
        owner = max(owners, key=lambda candidate: candidate.address)
        if owner.address >= address:
            continue

        owner_key = (owner.module_id, owner.address, owner.thumb)
        decoded = address in boundaries.get(owner_key, set())
        slot = (source.module_id, address) in table_slots and not owner.thumb and address % 4 == 0
        if (not decoded and not slot) or address in overlaps.get(owner_key, set()):
            continue

        candidates[key] = Entry(
            owner.module, owner.function, address, owner.thumb,
            f"extra_{address:08x}", True, cpu,
        )
        sources.add(source_key)
    result = [candidates[key] for key in sorted(candidates)]
    for entry in result:
        module_entries.setdefault((entry.module_id, entry.address), entry)
        module_mode_entries[(entry.module_id, entry.address, entry.thumb)] = entry
    return result, sources


def _disambiguate_entry_symbols(entries: list[Entry]) -> None:
    modes: dict[tuple[int, int], set[bool]] = {}
    for entry in entries:
        modes.setdefault((entry.module_id, entry.address), set()).add(entry.thumb)
    for entry in entries:
        if len(modes[(entry.module_id, entry.address)]) > 1:
            entry._slot_mode = "_thumb" if entry.thumb else "_arm"


def _entry_hash(entry: Entry, image: bytes, base: int) -> int:
    start = entry.address - base
    if start < 0 or start >= len(image):
        data = b""
    else:
        data = image[start : min(start + entry.size, len(image))]
    return _fnv1a64(data)

def _dispatch_region_hash(region: DispatchRegion, image: bytes, base: int) -> int:
    start = max(0, region.address - base)
    end = min(start + region.size, len(image))
    value = 0xCBF29CE484222325
    range_index = 0
    for offset in range(start, end):
        address = base + offset
        while range_index < len(region.data_ranges) and region.data_ranges[range_index][1] <= address:
            range_index += 1
        if range_index < len(region.data_ranges):
            low, high = region.data_ranges[range_index]
            if low <= address < high:
                continue
        value = ((value ^ image[offset]) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def _fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def emit_manifest(manifest: dict[str, Any], output_dir: str | Path, extra_entries: str | Path | None = None, chunk_size: int = 400) -> dict[str, Any]:
    """Emit generated C++ and return deterministic generation statistics."""
    if chunk_size < 1:
        raise ValueError("chunk_size must be positive")
    cpu, arch = manifest["cpu"], manifest["arch"]
    entries, images, module_entries, calls = _prepare_entries(manifest, extra_entries)
    regions, module_mode_entries = _prepare_dispatch_regions(manifest, entries, module_entries, cpu)
    shared_entry_keys = {
        (region.module_id, entry.address, entry.thumb)
        for region in regions for entry in region.entries
    }
    _disambiguate_entry_symbols(entries)
    stats: dict[str, Any] = {
        "cpu": cpu,
        "arch": arch,
        "functions": 0,
        "blocks": 0,
        "instructions": 0,
        "extra_entries": 0,
        "jump_tables": 0,
        "direct_calls": 0,
        "tail_calls": 0,
        "dsr_call_sites": 0,
        "dynamic_exits": 0,
        "direct_branch_sites": 0,
        "memory_sites": 0,
        "undefined_sites": 0,
        "unsupported_sites": [],
    }
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    address_entries: dict[int, list[Entry]] = {}
    for entry in entries:
        address_entries.setdefault(entry.address, []).append(entry)
    decoders: dict[tuple[int, bool], Decoder] = {}

    def decoder_for(entry: Entry) -> Decoder:
        key = (entry.module_id, entry.thumb)
        decoder = decoders.get(key)
        if decoder is None:
            decoder = Decoder(images[entry.module_id], int(entry.module["base"]), arch, entry.thumb)
            decoders[key] = decoder
        return decoder

    stat_keys = tuple(key for key in stats if key not in {"cpu", "arch"})

    def fresh_stats() -> dict[str, Any]:
        return {key: [] if key == "unsupported_sites" else 0 for key in stat_keys}

    def entry_key(entry: Entry) -> tuple[int, int, bool, bool]:
        return (entry.module_id, entry.address, entry.thumb, entry.extra)

    emitted: list[tuple[Entry, str, dict[str, Any]]] = []
    branch_edges: list[tuple[Entry, int]] = []
    table_slots: set[tuple[int, int]] = set()
    boundaries: dict[tuple[int, int, bool], set[int]] = {}
    overlaps: dict[tuple[int, int, bool], set[int]] = {}
    for entry in entries:
        if (entry.module_id, entry.address, entry.thumb) in shared_entry_keys:
            continue
        local_stats = fresh_stats()
        emitter = FunctionEmitter(
            cpu, arch, entry.module, images[entry.module_id], entry, address_entries,
            calls, local_stats, module_entries, decoder_for(entry),
            module_mode_entries=module_mode_entries,
        )
        body = emitter.emit()
        emitted.append((entry, body, local_stats))
        if not entry.extra:
            key = (entry.module_id, entry.address, entry.thumb)
            ordered = sorted(emitter.insns.items())
            boundaries[key] = set(emitter.insns)
            overlaps[key] = {
                next_address
                for (address, insn), (next_address, _) in zip(ordered, ordered[1:])
                if next_address < insn.next_address
            }
        for insn in emitter.insns.values():
            for slot in emitter._external_table_slots(insn):
                branch_edges.append((entry, slot))
                table_slots.add((entry.module_id, slot))
            branch_name = emitter._branch_name(insn)
            if branch_name not in {"b", "bl"}:
                continue
            target = emitter._branch_target(insn)
            if target is not None and (branch_name == "bl" or not emitter._inside(target & ~1)):
                branch_edges.append((entry, target))

    slot_modes = {entry_key(entry): entry._slot_mode for entry in entries}

    secondary, reemit_sources = _secondary_branch_entries(
        branch_edges, entries, module_entries, module_mode_entries, boundaries, overlaps, cpu,
        table_slots,
    )
    for entry in secondary:
        entries.append(entry)
        address_entries.setdefault(entry.address, []).append(entry)
        module_entries.setdefault((entry.module_id, entry.address), entry)
        module_mode_entries[(entry.module_id, entry.address, entry.thumb)] = entry
    _disambiguate_entry_symbols(entries)
    slot_changes = {
        entry_key(entry) for entry in entries
        if entry_key(entry) in slot_modes and entry._slot_mode != slot_modes[entry_key(entry)]
    }
    reemit_sources.update(slot_changes)
    if slot_changes:
        reemit_sources.update(entry_key(entry) for entry, _, _ in emitted)

    for index, (entry, _, _) in enumerate(emitted):
        if entry_key(entry) not in reemit_sources:
            continue
        local_stats = fresh_stats()
        emitter = FunctionEmitter(
            cpu, arch, entry.module, images[entry.module_id], entry, address_entries,
            calls, local_stats, module_entries, decoder_for(entry),
            module_mode_entries=module_mode_entries,
        )
        emitted[index] = (entry, emitter.emit(), local_stats)

    for entry in secondary:
        local_stats = fresh_stats()
        emitter = FunctionEmitter(
            cpu, arch, entry.module, images[entry.module_id], entry, address_entries,
            calls, local_stats, module_entries, decoder_for(entry),
            module_mode_entries=module_mode_entries,
        )
        emitted.append((entry, emitter.emit(), local_stats))
    for region in regions:
        local_stats = fresh_stats()
        emitter = FunctionEmitter(
            cpu, arch, region.module, images[region.module_id], region.body_entry, address_entries,
            calls, local_stats, module_entries, decoder_for(region.body_entry),
            dispatch_entries=region.entries, data_ranges=region.data_ranges,
            module_mode_entries=module_mode_entries,
        )
        body = emitter.emit()
        first_entry = region.entries[0]
        emitted.append((first_entry, body, local_stats))
        for entry in region.entries[1:]:
            emitted.append((entry, "", fresh_stats()))

    emitted.sort(key=lambda item: (item[0].module_id, item[0].address, item[0].thumb, item[0].extra))
    entries = [entry for entry, _, _ in emitted]
    bodies = [body for _, body, _ in emitted]
    for _, _, local_stats in emitted:
        for key in stat_keys:
            if key == "unsupported_sites":
                stats[key].extend(local_stats[key])
            else:
                stats[key] += local_stats[key]

    # Declare every slot before writing chunks so cross-chunk direct calls resolve.
    decls = ["#pragma once", "#include <cstdint>", '#include "dsr/gen.h"', "", "namespace dsr {"]
    for entry in entries:
        decls.append(f"extern GuestFn {entry.slot};")
    for region in regions:
        decls.append(f"extern void {region.body_symbol}(Cpu*, std::uint32_t);")
    decls += ["} // namespace dsr", ""]
    (output / "decls.h").write_text("\n".join(decls), encoding="utf-8")
    entry_positions = {entry_key(entry): index for index, entry in enumerate(entries)}
    shared_ranges = []
    for region in regions:
        positions = sorted(entry_positions[entry_key(entry)] for entry in region.entries)
        if positions:
            shared_ranges.append((positions[0], positions[-1]))
    chunk_ranges: list[tuple[int, int]] = []
    cursor = 0
    while cursor < len(entries):
        end = min(len(entries), cursor + chunk_size)
        while True:
            extended = False
            for low, high in shared_ranges:
                if low < end <= high:
                    end = high + 1
                    extended = True
            if not extended:
                break
        chunk_ranges.append((cursor, end))
        cursor = end
    for stale in output.glob("chunk_*.cpp"):
        stale.unlink()
    for index, (chunk_start, chunk_end) in enumerate(chunk_ranges):
        content = ['#include "decls.h"', "", "namespace dsr {", ""]
        content.extend(bodies[chunk_start:chunk_end])
        content.append("")
        for entry in entries[chunk_start:chunk_end]:
            content.append(f"GuestFn {entry.slot} = &{entry.body_symbol};")
        content += ["", "} // namespace dsr", ""]
        (output / f"chunk_{index:03d}.cpp").write_text("\n".join(content), encoding="utf-8")
    rows = ["#include \"decls.h\"", "", "namespace dsr {", "namespace {", "const FnEntry kEntries[] = {"]
    for entry in entries:
        image = images[entry.module_id]
        digest = _entry_hash(entry, image, int(entry.module["base"]))
        name = entry.name.replace("\\", "\\\\").replace('"', '\\"')
        rows.append(f"    {{{_u32(entry.address)}, {entry.size}u, {entry.module_id}, {1 if entry.thumb else 0}u, 0x{digest:016x}ull, &{entry.slot}, \"{name}\"}},")
    rows += ["};", "} // namespace", f"const FnTable& gen_table_{cpu}() {{", "    static const FnTable table{kEntries, sizeof(kEntries) / sizeof(kEntries[0])};", "    return table;", "}"]
    if regions:
        rows.append("namespace {")
        if any(region.data_ranges for region in regions):
            rows.append("const GuestAddressRange kDispatchRegionDataRanges[] = {")
            for region in regions:
                for start, end in region.data_ranges:
                    rows.append(f"    {{{_u32(start)}, {end - start}u}},")
            rows.append("};")
        rows.append("const DispatchRegionEntry kDispatchRegions[] = {")
        range_index = 0
        for region in regions:
            image = images[region.module_id]
            digest = _dispatch_region_hash(region, image, int(region.module["base"]))
            ranges = (
                f"kDispatchRegionDataRanges + {range_index}"
                if region.data_ranges else "nullptr"
            )
            rows.append(
                f"    {{{_u32(region.address)}, {region.size}u, {region.module_id}, "
                f"{1 if region.thumb else 0}u, 0x{digest:016x}ull, {ranges}, "
                f"{len(region.data_ranges)}u, &{region.body_symbol}}},"
            )
            range_index += len(region.data_ranges)
        rows += ["};", "} // namespace", f"const DispatchRegionTable& gen_dispatch_regions_{cpu}() {{",
                 "    static const DispatchRegionTable table{kDispatchRegions, sizeof(kDispatchRegions) / sizeof(kDispatchRegions[0])};",
                 "    return table;", "}"]
    else:
        rows += [f"const DispatchRegionTable& gen_dispatch_regions_{cpu}() {{",
                 "    static const DispatchRegionTable table{nullptr, 0};",
                 "    return table;", "}"]
    rows += ["} // namespace dsr", ""]
    (output / "fn_table.cpp").write_text("\n".join(rows), encoding="utf-8")
    (output / "stats.json").write_text(json.dumps(stats, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return stats


def emit_file(manifest_path: str | Path, output_dir: str | Path, extra_entries: str | Path | None = None, chunk_size: int = 400) -> dict[str, Any]:
    manifest = _read_json_manifest(Path(manifest_path))
    return emit_manifest(manifest, output_dir, extra_entries, chunk_size)
