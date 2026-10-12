"""Differential execution tests for emitted ARM/Thumb code.

The guest image and manifest are synthesized here; generated C++ and its
flat-memory host are kept in a temporary directory for the duration of the
suite. Unicorn supplies the architectural reference state.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    import unicorn
    from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN, Uc
    from capstone.arm_const import ARM_OP_IMM
    from unicorn.arm_const import (
        UC_CPU_ARM_946,
        UC_ARM_REG_CPSR,
        UC_ARM_REG_LR,
        UC_ARM_REG_R0,
        UC_ARM_REG_R1,
        UC_ARM_REG_R2,
        UC_ARM_REG_R3,
        UC_ARM_REG_R4,
        UC_ARM_REG_R5,
        UC_ARM_REG_R6,
        UC_ARM_REG_R7,
        UC_ARM_REG_R8,
        UC_ARM_REG_R9,
        UC_ARM_REG_R10,
        UC_ARM_REG_R11,
        UC_ARM_REG_R12,
        UC_ARM_REG_PC,
        UC_ARM_REG_SP,
    )
    from recomp.decoder import Decoder, is_operand_memory, is_operand_register, shift_kind
    from recomp.emitter import emit_manifest
except (ImportError, RuntimeError) as exc:  # pragma: no cover - environment diagnostic
    _IMPORT_ERROR = exc
    unicorn = None
    emit_manifest = None
else:
    _IMPORT_ERROR = None


def _operand_shape(insn) -> tuple:
    """Stable decoded operand/shift signature for a synthetic instruction form."""
    operands = []
    for operand in insn.operands:
        if is_operand_register(operand):
            shift = shift_kind(operand.shift.type) if operand.shift.type else (0, False, False)
            operands.append(("reg", shift))
        elif is_operand_memory(operand):
            mem = operand.mem
            shift = shift_kind(operand.shift.type) if operand.shift.type else (0, False, False)
            operands.append(("mem", bool(mem.base), bool(mem.index), bool(mem.disp),
                             bool(mem.scale), shift, bool(mem.lshift)))
        elif operand.type == ARM_OP_IMM:
            operands.append(("imm",))
        else:
            operands.append((str(operand.type),))
    return tuple(operands)


def _synthetic_instruction_forms() -> dict[tuple, tuple[int, bool, int]]:
    """Decode an invented ISA corpus; no application images or manifests are used."""
    words = [
        _arm_clz(rd=4, rm=3),
        _arm_half(load=True, rd=2, rn=8, offset=12, pre=False, writeback=True),
        _arm_half(load=True, rd=2, rn=8, offset=12, kind="sh", pre=False, writeback=True),
        _arm_half(load=False, rd=2, rn=8, offset=12, kind="sb"),  # LDRD encoding.
        _arm_half(load=False, rd=2, rn=8, offset=12, kind="sh"),  # STRD encoding.
        _arm_half(load=True, rd=2, rn=8, offset=12, kind="sb", writeback=True),
        _arm_half(load=True, rd=2, rn=8, offset=12, kind="sh", writeback=True),
        _arm_half(load=False, rd=2, rn=8, offset=12, pre=False),
        _arm_qop(2, rd=3, rm=4, rn=5),  # QDADD.
        _arm_half_mul("smulxy", rd=4, rm=2, rs=3),  # SMULBB.
        0xE7000010 | (4 << 16) | (5 << 12) | (3 << 8) | 2,  # ARMv6 SMLAD.
        _arm_block(load=True, mode="DB", rn=8, registers=(0, 1, 3), writeback=False) | (1 << 22),
    ]
    for load in (False, True):
        for byte in (False, True):
            words.extend((
                _arm_single(load=load, rd=2, rn=8, offset=12, byte=byte),
                _arm_single(load=load, rd=2, rn=8, offset=12, byte=byte, pre=False),
                _arm_single(load=load, rd=2, rn=8, rm=3, byte=byte, shift=1),
            ))
        words.append(_arm_half(load=load, rd=2, rn=8, offset=12))
        for mode in ("IA", "IB", "DA", "DB"):
            words.append(_arm_block(load=load, mode=mode, rn=8, registers=(0, 1, 3)))
    for signed in (False, True):
        for accumulate in (False, True):
            words.append(_arm_long_mul(4, 5, 2, 3, signed=signed,
                                       accumulate=accumulate, set_flags=False))
    for byte in (False, True):
        words.append(0xE1000090 | (int(byte) << 22) | (8 << 16) | (2 << 12) | 3)  # SWP/SWPB.
    samples = [(False, _arm_word(word)) for word in words]
    samples.extend((
        (True, _thumb_halfword(_thumb_alu(2, rd=3, rs=4))),
        (True, _thumb_halfword(_thumb_mem_reg(4, rd=2, rb=5, ro=3))),
    ))
    forms = {}
    for index, (thumb, code) in enumerate(samples):
        address = CODE_BASE + index * 8
        insn = Decoder(code, address, "v5te", thumb).decode(address)
        if insn is None or insn.mnemonic == "udf":
            raise AssertionError(f"invalid synthetic ISA specimen {index}")
        key = (thumb, insn.mnemonic, _operand_shape(insn), insn.condition,
               insn.writeback, insn.update_flags)
        forms.setdefault(key, (insn.raw, thumb, address))
    return forms





CODE_BASE = 0x08000000  # Synthetic flat-memory code space, not a console load address.
PAGE_SIZE = 1 << 14
DATA_BASE = CODE_BASE + PAGE_SIZE
RETURN_ADDRESS = 0x09000000  # Synthetic return sentinel, separately mapped by Unicorn.
MEMORY_SIZE = 256
WIRE_FORMAT = "<I16I5I256sI12s2I"
WIRE_SIZE = struct.calcsize(WIRE_FORMAT)
ARM_BX_LR = 0xE12FFF1E
THUMB_BX_LR = 0x4770


@dataclass(frozen=True)
class FunctionSpec:
    name: str
    category: str
    thumb: bool
    instruction: bytes
    terminal: bool = False
    base_reg: int = 6
    index_reg: int = -1
    index_values: tuple[int, ...] = ()
    shift_reg: int = -1
    shift_values: tuple[int, ...] = ()
    block_mode: str = "IA"
    block_regs: tuple[int, ...] = ()
    pc_offset: int = -1

    def code(self) -> bytes:
        if self.terminal:
            return self.instruction
        ret = struct.pack("<H", THUMB_BX_LR) if self.thumb else struct.pack("<I", ARM_BX_LR)
        return self.instruction + ret

_PRIORITY_FUZZ_MNEMONICS = frozenset({
    "ldrd", "strd", "ldm", "ldmda", "ldmdb", "ldmib", "stm", "stmda",
    "stmdb", "stmib", "umull", "umlal", "smull", "smlal", "qadd", "qsub",
    "qdadd", "qdsub", "clz", "swp", "swpb",
})


def _synthetic_priority_specs(
    forms: dict[tuple, tuple[int, bool, int]],
) -> tuple[list[FunctionSpec], list[tuple[str, int, str]]]:
    """Build valid specimens, separately reporting known unsupported ISA forms."""
    result = []
    expected_failures = []
    for index, (key, (raw, thumb, address)) in enumerate(sorted(
        forms.items(), key=lambda item: (item[0][0], item[0][1], item[0][3:])
    )):
        mnemonic = key[1]
        if mnemonic in {"ldrht", "ldrsht"} and not (raw & (1 << 24)):
            expected_failures.append((
                mnemonic, address, "emitter does not implement post-indexed halfword T-form loads",
            ))
            continue
        if mnemonic == "ldrd":
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this ARMv5TE LDRD form as invalid",
            ))
            continue
        if mnemonic == "strd":
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this ARMv5TE STRD form as invalid",
            ))
            continue
        if mnemonic == "ldrsb" and key[4]:
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this write-back LDRSB encoding as invalid",
            ))
            continue
        if mnemonic == "ldrsh" and key[4]:
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this write-back LDRSH encoding as invalid",
            ))
            continue
        if mnemonic in {"qadd", "qsub", "qdadd", "qdsub"}:
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this ARMv5TE saturating DSP instruction as invalid",
            ))
            continue
        if mnemonic in {"smlad", "smlsd", "smlald", "smlsld"}:
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this ARMv6 dual-multiply instruction as invalid",
            ))
            continue
        if mnemonic == "strh" and not (raw & (1 << 24)):
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this post-indexed STRH form as invalid",
            ))
            continue
        if mnemonic.startswith("smul") and mnemonic != "smull":
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this ARMv5TE signed halfword multiply as invalid",
            ))
            continue
        if mnemonic in {"swp", "swpb"}:
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this swap instruction as invalid",
            ))
            continue
        if mnemonic == "ldmdb" and raw & (1 << 22):
            expected_failures.append((
                mnemonic, address, "Unicorn ARM946 rejects this LDMDB user-register form as invalid",
            ))
            continue
        memory_transfer = mnemonic.startswith(("ldr", "str")) and mnemonic not in {"ldrd", "strd"}
        block_transfer = mnemonic.startswith(("ldm", "stm"))
        dsp_multiply = mnemonic.startswith(("smul", "smla"))
        if thumb or (mnemonic not in _PRIORITY_FUZZ_MNEMONICS and not memory_transfer and
                     not block_transfer and not dsp_multiply):
            continue

        word = raw
        base_reg = 6
        index_reg = -1
        index_values = ()
        block_regs = ()
        category = "priority_instruction"
        if memory_transfer:
            word = (word & ~((15 << 16) | (15 << 12))) | (6 << 16)
            if (word >> 26) & 3 == 1:  # word/byte single data transfer
                if word & (1 << 25):
                    word = (word & ~15) | 7  # preserve shift kind/amount
                    index_reg = 7
                    index_values = (0, 1, 2, 3, 4, 7, 11, 15)
                else:
                    word = (word & ~0xFFF) | 4
            else:  # halfword/signed transfer
                if word & (1 << 22):
                    word = (word & ~0xF0F) | 4
                else:
                    word = (word & ~15) | 7
                    index_reg = 7
                    index_values = (0, 2, 4, 8)
            category = "priority_memory"
        elif mnemonic in {"ldrd", "strd"}:
            word = (word & ~((15 << 16) | (15 << 12))) | (6 << 16)
            if word & (1 << 22):
                word = (word & ~0xF0F) | 4
            else:
                word = (word & ~15) | 7
                index_reg = 7
                index_values = (0, 2, 4, 8)
            category = "priority_memory"
        elif block_transfer:
            word = (word & ~((15 << 16) | 0xFFFF)) | (6 << 16) | 0x000F
            block_regs = (0, 1, 2, 3)
            category = "priority_block"
        elif mnemonic in {"umull", "umlal", "smull", "smlal"}:
            word = (word & ~((15 << 16) | (15 << 12) | (15 << 8) | 15)) | (
                (2 << 16) | (3 << 12) | (1 << 8)
            )
        elif dsp_multiply:
            word = (word & ~((15 << 16) | (15 << 12) | (15 << 8) | 15)) | (
                (3 << 16) | (2 << 12) | (1 << 8)
            )
        elif mnemonic in {"qadd", "qsub", "qdadd", "qdsub"}:
            word = (word & ~((15 << 16) | (15 << 12) | 15)) | (2 << 16) | 1
        elif mnemonic == "clz":
            word = (word & ~((15 << 12) | 15)) | 1
        elif mnemonic in {"swp", "swpb"}:
            word = (word & ~((15 << 16) | (15 << 12) | (15 << 8) | 15)) | (
                (6 << 16) | (1 << 8) | 2
            )
            category = "priority_memory"

        if index_reg >= 0 and word & (1 << 25):
            amount = (word >> 7) & 31
            shift = (word >> 5) & 3
            if shift == 0:
                index_values = tuple(value for value in index_values if (value << amount) <= 48)
            elif shift == 3 and amount:
                index_values = (0,)
            if not index_values:
                index_values = (0,)
        decoded = Decoder(_arm_word(word), 0, "v5te", False).decode(0)
        if decoded is None or decoded.mnemonic != mnemonic:
            raise AssertionError(
                f"synthetic {mnemonic} form at {address:#010x} synthesized as "
                f"{decoded.mnemonic if decoded else 'no instruction'} ({word:#010x})"
            )
        name = f"synthetic_{mnemonic}_{index:04d}_{address:08x}"
        mode = {(0, 1): "IA", (1, 1): "IB", (0, 0): "DA", (1, 0): "DB"}[
            ((word >> 24) & 1, (word >> 23) & 1)
        ] if category == "priority_block" else "IA"
        result.append(FunctionSpec(
            name, category, False, _arm_word(word),
            base_reg=base_reg, index_reg=index_reg, index_values=index_values,
            block_regs=block_regs, block_mode=mode,
        ))
    return result, expected_failures

@dataclass
class TestState:
    slot: int
    spec: FunctionSpec
    sample: int
    regs: list[int]
    flags: tuple[int, int, int, int, int]
    memory: bytes
    slot_address: int = 0
    table_offset: int = 0
    table_bytes: bytes = bytes(12)
    user_regs: tuple[int, int] = (0x01234560, 0xABCDEF00)


@dataclass
class NativeState:
    regs: tuple[int, ...]
    flags: tuple[int, ...]
    memory: bytes
    transfer_target: int = 0
    user_regs: tuple[int, int] = (0x01234560, 0xABCDEF00)


if _IMPORT_ERROR is None:
    ARM_REGISTERS = (
        UC_ARM_REG_R0,
        UC_ARM_REG_R1,
        UC_ARM_REG_R2,
        UC_ARM_REG_R3,
        UC_ARM_REG_R4,
        UC_ARM_REG_R5,
        UC_ARM_REG_R6,
        UC_ARM_REG_R7,
        UC_ARM_REG_R8,
        UC_ARM_REG_R9,
        UC_ARM_REG_R10,
        UC_ARM_REG_R11,
        UC_ARM_REG_R12,
        UC_ARM_REG_SP,
        UC_ARM_REG_LR,
    )
else:
    ARM_REGISTERS = ()


def _arm_word(word: int) -> bytes:
    return struct.pack("<I", word & 0xFFFFFFFF)


def _thumb_halfword(word: int) -> bytes:
    return struct.pack("<H", word & 0xFFFF)


def _arm_dp(
    opcode: int,
    operand2: int,
    *,
    rd: int = 0,
    rn: int = 2,
    set_flags: bool = True,
    immediate: bool = False,
    condition: int = 14,
) -> int:
    return (
        (condition << 28)
        | (int(immediate) << 25)
        | (opcode << 21)
        | (int(set_flags) << 20)
        | (rn << 16)
        | (rd << 12)
        | operand2
    )


def _arm_branch(source: int, target: int, *, condition: int = 14) -> int:
    displacement = target - (source + 8)
    if displacement % 4:
        raise ValueError("ARM branch target must be word-aligned")
    return (condition << 28) | 0x0A000000 | ((displacement >> 2) & 0x00FFFFFF)


def _thumb_cond_branch(source: int, target: int, condition: int = 0) -> int:
    displacement = target - (source + 4)
    offset = displacement >> 1
    if displacement % 2 or not 0 <= condition <= 13 or not -128 <= offset <= 127:
        raise ValueError("Thumb conditional branch requires a valid condition and halfword target")
    return 0xD000 | (condition << 8) | (offset & 0xFF)


def _arm_mul(rd: int, rm: int, rs: int, *, accumulate: int | None = None, set_flags: bool = False) -> int:
    return (
        0xE0000090
        | (int(accumulate is not None) << 21)
        | (int(set_flags) << 20)
        | (rd << 16)
        | ((0 if accumulate is None else accumulate) << 12)
        | (rs << 8)
        | rm
    )


def _arm_long_mul(
    rd_lo: int,
    rd_hi: int,
    rm: int,
    rs: int,
    *,
    signed: bool,
    accumulate: bool,
    set_flags: bool,
) -> int:
    return (
        0xE0800090
        | (int(signed) << 22)
        | (int(accumulate) << 21)
        | (int(set_flags) << 20)
        | (rd_hi << 16)
        | (rd_lo << 12)
        | (rs << 8)
        | rm
    )


def _arm_half_mul(
    family: str,
    rd: int,
    rm: int,
    rs: int,
    *,
    x_high: bool = False,
    y_high: bool = False,
    accumulate: int | None = None,
    rd_hi: int = 1,
) -> int:
    if family == "smulxy":
        base = 0xE1600080 | (rd << 16)
    elif family == "smlaxy":
        base = 0xE1000080 | (rd << 16) | ((0 if accumulate is None else accumulate) << 12)
    elif family == "smulwy":
        base = 0xE12000A0 | (rd << 16)
    elif family == "smlawy":
        base = 0xE1200080 | (rd << 16) | ((0 if accumulate is None else accumulate) << 12)
    elif family == "smlalxy":
        base = 0xE1400080 | (rd_hi << 16) | (rd << 12)
    else:  # pragma: no cover - internal encoder guard
        raise ValueError(family)
    xy = (int(x_high) << 5) | (int(y_high) << 6)
    if family in {"smulwy", "smlawy"}:
        xy = int(y_high) << 6
    return base | (rs << 8) | xy | rm


def _arm_qop(opcode: int, rd: int = 0, rm: int = 1, rn: int = 2) -> int:
    return 0xE1000050 | (opcode << 21) | (rn << 16) | (rd << 12) | rm


def _arm_clz(rd: int = 0, rm: int = 1) -> int:
    return 0xE16F0F10 | (rd << 12) | rm


def _arm_single(
    *,
    load: bool,
    rd: int = 0,
    rn: int = 6,
    offset: int = 0,
    byte: bool = False,
    pre: bool = True,
    up: bool = True,
    writeback: bool = False,
    rm: int | None = None,
    shift: int = 0,
    shift_type: int = 0,
    condition: int = 14,
) -> int:
    register_offset = rm is not None
    low = ((shift & 31) << 7) | ((shift_type & 3) << 5) | (int(rm) if rm is not None else (offset & 0xFFF))
    return (
        (condition << 28)
        | (1 << 26)
        | (int(register_offset) << 25)
        | (1 << 24 if pre else 0)
        | (int(up) << 23)
        | (int(byte) << 22)
        | (int(writeback) << 21)
        | (int(load) << 20)
        | (rn << 16)
        | (rd << 12)
        | low
    )


def _arm_half(
    *,
    load: bool,
    rd: int = 0,
    rn: int = 6,
    offset: int = 0,
    kind: str = "h",
    pre: bool = True,
    up: bool = True,
    writeback: bool = False,
    rm: int | None = None,
) -> int:
    signed, half = {"h": (0, 1), "sb": (1, 0), "sh": (1, 1)}[kind]
    immediate = rm is None
    low = (((offset >> 4) & 15) << 8) | (int(immediate) << 22)
    low |= (1 << 7) | (signed << 6) | (half << 5) | (1 << 4)
    low |= ((offset & 15) if immediate else int(rm))
    return (
        0xE0000000
        | (1 << 24 if pre else 0)
        | (int(up) << 23)
        | low
        | (int(writeback) << 21)
        | (int(load) << 20)
        | (rn << 16)
        | (rd << 12)
    )


def _arm_block(
    *,
    load: bool,
    mode: str,
    rn: int = 8,
    registers: tuple[int, ...] = (0, 2, 4),
    writeback: bool = True,
) -> int:
    p, u = {"IA": (0, 1), "IB": (1, 1), "DA": (0, 0), "DB": (1, 0)}[mode]
    reglist = sum(1 << reg for reg in registers)
    return (
        0xE8000000
        | (p << 24)
        | (u << 23)
        | (int(writeback) << 21)
        | (int(load) << 20)
        | (rn << 16)
        | reglist
    )


def _thumb_alu(opcode: int, rd: int = 0, rs: int = 1) -> int:
    return 0x4000 | (opcode << 6) | (rs << 3) | rd


def _thumb_imm(opcode: int, rd: int = 0, immediate: int = 0xA5) -> int:
    return (opcode << 11) | (rd << 8) | (immediate & 0xFF)


def _thumb_add_sub(*, subtract: bool, immediate: bool, rd: int = 0, rn: int = 1, value: int = 3) -> int:
    return 0x1800 | (int(immediate) << 10) | (int(subtract) << 9) | ((value & 7) << 6) | (rn << 3) | rd


def _thumb_shift(kind: int, rd: int = 0, rm: int = 1, amount: int = 7) -> int:
    return (kind << 11) | ((amount & 31) << 6) | (rm << 3) | rd


def _thumb_mem_reg(opcode: int, rd: int = 0, rb: int = 6, ro: int = 7) -> int:
    return 0x5000 | (opcode << 9) | (ro << 6) | (rb << 3) | rd


def _thumb_mem_imm(
    *,
    load: bool,
    rd: int = 0,
    rb: int = 6,
    offset: int = 3,
    size: int = 4,
) -> int:
    if size == 2:
        return 0x8000 | (int(load) << 11) | ((offset & 31) << 6) | (rb << 3) | rd
    byte = size == 1
    return (
        0x6000
        | (int(byte) << 12)
        | (int(load) << 11)
        | ((offset & 31) << 6)
        | (rb << 3)
        | rd
    )


def _make_specs() -> list[FunctionSpec]:
    specs: list[FunctionSpec] = []
    dp_ops = {
        "and": 0,
        "eor": 1,
        "sub": 2,
        "rsb": 3,
        "add": 4,
        "adc": 5,
        "sbc": 6,
        "rsc": 7,
        "tst": 8,
        "teq": 9,
        "cmp": 10,
        "cmn": 11,
        "orr": 12,
        "mov": 13,
        "bic": 14,
        "mvn": 15,
    }
    for name, opcode in dp_ops.items():
        tests = opcode in {8, 9, 10, 11}
        rn = 0 if opcode in {13, 15} else 2
        word = _arm_dp(opcode, 3, rd=0, rn=rn, set_flags=True, condition=14)
        specs.append(FunctionSpec(f"arm_dp_{name}_reg", "arm_dp", False, _arm_word(word)))
        if not tests and opcode in {0, 2, 4, 12, 13, 15}:
            unflagged = _arm_dp(opcode, 3, rd=0, rn=rn, set_flags=False)
            specs.append(FunctionSpec(f"arm_dp_{name}_no_flags", "arm_dp", False, _arm_word(unflagged)))
    immediate_cases = (
        ("movs_rotated", 13, (2 << 8) | 0x91, 0, 0, True),
        ("tst_rotated", 8, (4 << 8) | 0x20, 0, 2, True),
        ("adds_immediate", 4, 0x35, 0, 2, True),
        ("ands_immediate", 0, 0xA7, 0, 2, True),
        ("cmp_immediate", 10, 0x63, 0, 2, True),
        ("orr_immediate_no_flags", 12, (3 << 8) | 0xC1, 0, 2, False),
    )
    for name, opcode, op2, rd, rn, set_flags in immediate_cases:
        specs.append(FunctionSpec(f"arm_dp_{name}", "arm_dp", False, _arm_word(_arm_dp(opcode, op2, rd=rd, rn=rn, set_flags=set_flags, immediate=True))))
    for condition, name in ((0, "eq"), (1, "ne"), (2, "cs"), (4, "mi"), (6, "vs")):
        word = _arm_dp(4, 3, rd=0, rn=2, set_flags=True, condition=condition)
        specs.append(FunctionSpec(f"arm_dp_add{ name }", "arm_flags", False, _arm_word(word)))

    shift_types = ((0, "lsl"), (1, "lsr"), (2, "asr"), (3, "ror"))
    for shift_type, shift_name in shift_types:
        for amount in (0, 1, 31, 32):
            op2 = ((amount & 31) << 7) | (shift_type << 5) | 3
            word = _arm_dp(13, op2, rd=0, rn=0, set_flags=True)
            specs.append(FunctionSpec(f"arm_shift_{shift_name}_imm_{amount}", "arm_shifts", False, _arm_word(word)))
        for amount in (0, 1, 31, 32, 33, 255):
            op2 = (4 << 8) | (shift_type << 5) | (1 << 4) | 3
            word = _arm_dp(13, op2, rd=0, rn=0, set_flags=True)
            specs.append(FunctionSpec(
                f"arm_shift_{shift_name}_reg_{amount}",
                "arm_shifts",
                False,
                _arm_word(word),
                shift_reg=4,
                shift_values=(amount,),
            ))

    specs.extend(
        (
            FunctionSpec("arm_mul", "multiply", False, _arm_word(_arm_mul(0, 1, 2))),
            FunctionSpec("arm_muls", "multiply", False, _arm_word(_arm_mul(0, 1, 2, set_flags=True))),
            FunctionSpec("arm_mla", "multiply", False, _arm_word(_arm_mul(0, 1, 2, accumulate=3))),
            FunctionSpec("arm_mlas", "multiply", False, _arm_word(_arm_mul(0, 1, 2, accumulate=3, set_flags=True))),
        )
    )
    for name, signed, accumulate, set_flags in (
        ("umull", False, False, False),
        ("umulls", False, False, True),
        ("umlal", False, True, False),
        ("umlals", False, True, True),
        ("smull", True, False, False),
        ("smulls", True, False, True),
        ("smlal", True, True, False),
        ("smlals", True, True, True),
    ):
        specs.append(FunctionSpec(f"arm_{name}", "multiply", False, _arm_word(_arm_long_mul(0, 1, 2, 3, signed=signed, accumulate=accumulate, set_flags=set_flags))))

    specs.append(FunctionSpec("arm_clz", "clz_q_dsp", False, _arm_word(_arm_clz())))
    for op, name in enumerate(("qadd", "qsub", "qdadd", "qdsub")):
        specs.append(FunctionSpec(f"arm_{name}", "clz_q_dsp", False, _arm_word(_arm_qop(op))))
    for x_high, y_high, suffix in ((False, False, "bb"), (False, True, "bt"), (True, False, "tb"), (True, True, "tt")):
        specs.append(FunctionSpec(f"arm_smul{suffix}", "clz_q_dsp", False, _arm_word(_arm_half_mul("smulxy", 0, 1, 2, x_high=x_high, y_high=y_high))))
        specs.append(FunctionSpec(f"arm_smla{suffix}", "clz_q_dsp", False, _arm_word(_arm_half_mul("smlaxy", 0, 1, 2, x_high=x_high, y_high=y_high, accumulate=3))))
        specs.append(FunctionSpec(f"arm_smlal{suffix}", "clz_q_dsp", False, _arm_word(_arm_half_mul("smlalxy", 0, 2, 3, x_high=x_high, y_high=y_high, rd_hi=1))))
    memory_specs = (
        ("ldr_word_unaligned", _arm_single(load=True, offset=0), -1),
        ("str_word_unaligned", _arm_single(load=False, rd=1, offset=3), -1),
        ("ldr_word_negative", _arm_single(load=True, offset=12, up=False), -1),
        ("ldr_postindex_wb", _arm_single(load=True, offset=12, pre=False), -1),
        ("ldr_preindex_wb", _arm_single(load=True, offset=8, writeback=True), -1),
        ("ldr_reg_shifted", _arm_single(load=True, rm=7, shift=2), 7),
        ("str_reg_negative", _arm_single(load=False, rd=1, rm=7, up=False), 7),
        ("ldrb", _arm_single(load=True, byte=True, offset=7), -1),
        ("strb", _arm_single(load=False, byte=True, rd=1, offset=5), -1),
        ("ldrh_unaligned", _arm_half(load=True, offset=1), -1),
        ("strh_unaligned", _arm_half(load=False, rd=1, offset=3), -1),
        ("ldrsb", _arm_half(load=True, kind="sb", offset=3), -1),
        ("ldrsh_unaligned", _arm_half(load=True, kind="sh", offset=1), -1),
        ("ldrh_reg", _arm_half(load=True, rm=7), 7),
        ("strh_reg", _arm_half(load=False, rd=1, rm=7), 7),
    )
    for name, word, index_reg in memory_specs:
        specs.append(FunctionSpec(f"arm_mem_{name}", "arm_memory", False, _arm_word(word), index_reg=index_reg))
    for name, shift_type, shift, index_values in (
        ("asr16", 2, 16, (0x00010000, 0x00020000, 0xFFFF0000)),
        ("lsr16", 1, 16, (0x00010000, 0x00020000)),
        ("ror8", 3, 8, (0x00000100, 0x00000200)),
    ):
        word = _arm_single(load=True, byte=True, rm=7, shift=shift, shift_type=shift_type)
        specs.append(FunctionSpec(
            f"arm_mem_ldrb_reg_{name}", "arm_memory", False, _arm_word(word),
            index_reg=7, index_values=index_values,
        ))
    conditional_store = _arm_single(load=False, rd=1, byte=True, offset=3, condition=0)
    specs.append(FunctionSpec("arm_mem_streq_byte", "arm_memory", False, _arm_word(conditional_store)))

    for mode in ("IA", "IB", "DA", "DB"):
        regs = (0, 2, 4)
        specs.append(FunctionSpec(
            f"arm_stm{mode.lower()}", "arm_block", False,
            _arm_word(_arm_block(load=False, mode=mode, registers=regs)),
            base_reg=8, block_mode=mode, block_regs=regs,
        ))
        specs.append(FunctionSpec(
            f"arm_ldm{mode.lower()}", "arm_block", False,
            _arm_word(_arm_block(load=True, mode=mode, registers=regs)),
            base_reg=8, block_mode=mode, block_regs=regs,
        ))
    sp_regs = (4, 5, 14)
    specs.append(FunctionSpec(
        "arm_stmdb_sp", "arm_block", False,
        _arm_word(_arm_block(load=False, mode="DB", rn=13, registers=sp_regs)),
        base_reg=13, block_mode="DB", block_regs=sp_regs,
    ))
    pc_regs = (0, 1, 15)
    specs.append(FunctionSpec(
        "arm_ldmia_pc_return", "return_paths", False,
        _arm_word(_arm_block(load=True, mode="IA", registers=pc_regs)),
        terminal=True, base_reg=8, block_mode="IA", block_regs=pc_regs, pc_offset=8,
    ))
    specs.append(FunctionSpec("arm_bx_lr_return", "return_paths", False, _arm_word(ARM_BX_LR), terminal=True))
    specs.append(FunctionSpec("arm_mov_pc_lr_return", "return_paths", False, _arm_word(_arm_dp(13, 14, rd=15, rn=0, set_flags=False)), terminal=True))
    table_count = 4
    table_start = 28
    first_case = table_start + table_count * 4
    fallback = first_case + table_count * 8
    branch_table = [
        _arm_dp(10, 0, rn=2, immediate=True),  # cmp r2, #0
        _arm_branch(4, 16, condition=1),  # bne skip_index_bound
        _arm_dp(10, table_count - 1, rn=0, immediate=True),  # cmp r0, #3
        _arm_branch(12, fallback, condition=12),  # bgt fallback
        _arm_dp(10, 0, rn=0, immediate=True),  # cmp r0, #0
        _arm_dp(4, 2 << 7, rd=15, rn=15, set_flags=False, condition=10),  # addge pc, pc, r0, lsl #2
        _arm_branch(24, fallback),
    ]
    branch_table.extend(
        _arm_branch(table_start + index * 4, first_case + index * 8)
        for index in range(table_count)
    )
    for index in range(table_count):
        branch_table.extend((
            _arm_dp(13, index, rd=1, rn=0, set_flags=False, immediate=True),
            ARM_BX_LR,
        ))
    branch_table.extend((
        _arm_dp(13, 0xFF, rd=1, rn=0, set_flags=False, immediate=True),
        ARM_BX_LR,
    ))
    specs.append(FunctionSpec(
        "arm_pc_branch_table", "computed_jump", False,
        b"".join(_arm_word(word) for word in branch_table),
        terminal=True, index_reg=0,
    ))

    # Synthetic regression: r2 has an unrelated comparison, while r5 is guarded
    # by an upper bound of 9 and rebased by 6. Only indices 0..3 are valid even
    # though six branch slots exist; negative indices take the fallback branch.
    guarded_slots = 6
    guarded_first_slot = 24
    guarded_first_case = guarded_first_slot + guarded_slots * 4
    guarded_fallback = guarded_first_case + guarded_slots * 8
    guarded = [
        _arm_dp(10, 0, rn=2, immediate=True),  # cmp r2, #0 (unrelated)
        _arm_dp(10, 9, rn=5, immediate=True),  # cmp r5, #9
        _arm_branch(8, guarded_fallback, condition=12),  # bgt fallback
        _arm_dp(2, 6, rd=2, rn=5, immediate=True),  # subs r2, r5, #6
        _arm_dp(4, (2 << 7) | 2, rd=15, rn=15, set_flags=False, condition=5),  # addpl pc, pc, r2, lsl #2
        _arm_branch(20, guarded_fallback),  # negative index
    ]
    guarded.extend(
        _arm_branch(guarded_first_slot + index * 4, guarded_first_case + index * 8)
        for index in range(guarded_slots)
    )
    for index in range(guarded_slots):
        guarded.extend((_arm_dp(13, index, rd=1, rn=0, set_flags=False, immediate=True), ARM_BX_LR))
    guarded.extend((_arm_dp(13, 0xFF, rd=1, rn=0, set_flags=False, immediate=True), ARM_BX_LR))
    specs.append(FunctionSpec(
        "arm_pc_branch_table_guarded", "computed_jump", False,
        b"".join(_arm_word(word) for word in guarded),
        terminal=True, index_reg=5,
    ))


    for opcode, name in ((0, "and"), (1, "eor"), (2, "lsl"), (3, "lsr"), (4, "asr"), (5, "adc"), (6, "sbc"), (7, "ror"), (8, "tst"), (10, "cmp"), (11, "cmn"), (12, "orr"), (13, "mul"), (14, "bic"), (15, "mvn")):
        specs.append(FunctionSpec(f"thumb_{name}", "thumb_dp", True, _thumb_halfword(_thumb_alu(opcode))))
    for opcode, name in ((4, "movs"), (5, "cmp"), (6, "adds"), (7, "subs")):
        specs.append(FunctionSpec(f"thumb_{name}_imm", "thumb_dp", True, _thumb_halfword(_thumb_imm(opcode, immediate=0xA5))))
    for kind, name in ((0, "lsl"), (1, "lsr"), (2, "asr")):
        for amount in (0, 1, 31):
            specs.append(FunctionSpec(f"thumb_{name}_imm_{amount}", "thumb_dp", True, _thumb_halfword(_thumb_shift(kind, amount=amount))))
    for subtract in (False, True):
        for immediate in (False, True):
            specs.append(FunctionSpec(
                f"thumb_{'sub' if subtract else 'add'}_{'imm' if immediate else 'reg'}3",
                "thumb_dp", True,
                _thumb_halfword(_thumb_add_sub(subtract=subtract, immediate=immediate)),
            ))

    thumb_memory = (
        ("ldr_word_unaligned", _thumb_mem_imm(load=True, offset=0), 6, -1),
        ("str_word_unaligned", _thumb_mem_imm(load=False, rd=1, offset=1), 6, -1),
        ("ldrb", _thumb_mem_imm(load=True, offset=5, size=1), 6, -1),
        ("strb", _thumb_mem_imm(load=False, rd=1, offset=3, size=1), 6, -1),
        ("ldrh_unaligned", _thumb_mem_imm(load=True, offset=1, size=2), 6, -1),
        ("strh_unaligned", _thumb_mem_imm(load=False, rd=1, offset=3, size=2), 6, -1),
        ("str_reg", _thumb_mem_reg(0, rd=1), 6, 7),
        ("strh_reg", _thumb_mem_reg(1, rd=1), 6, 7),
        ("strb_reg", _thumb_mem_reg(2, rd=1), 6, 7),
        ("ldrsb_reg", _thumb_mem_reg(3), 6, 7),
        ("ldr_reg", _thumb_mem_reg(4), 6, 7),
        ("ldrh_reg", _thumb_mem_reg(5), 6, 7),
        ("ldrb_reg", _thumb_mem_reg(6), 6, 7),
        ("ldrsh_reg", _thumb_mem_reg(7), 6, 7),
    )
    for name, halfword, base_reg, index_reg in thumb_memory:
        specs.append(FunctionSpec(f"thumb_mem_{name}", "thumb_memory", True, _thumb_halfword(halfword), base_reg=base_reg, index_reg=index_reg))

    for load, name in ((False, "stmia"), (True, "ldmia")):
        specs.append(FunctionSpec(
            f"thumb_{name}", "thumb_block", True,
            _thumb_halfword(0xC000 | (int(load) << 11) | (6 << 8) | 0x15),
            base_reg=6, block_regs=(0, 2, 4),
        ))
    specs.extend((
        FunctionSpec("thumb_push", "thumb_block", True, _thumb_halfword(0xB400 | (1 << 8) | 0x03), base_reg=13, block_regs=(0, 1, 14)),
        FunctionSpec("thumb_pop", "thumb_block", True, _thumb_halfword(0xBC00 | 0x03), base_reg=13, block_regs=(0, 1)),
        FunctionSpec("thumb_pop_pc_return", "return_paths", True, _thumb_halfword(0xBD01), terminal=True, base_reg=13, pc_offset=4),
        FunctionSpec("thumb_bx_lr_return", "return_paths", True, _thumb_halfword(THUMB_BX_LR), terminal=True),
        FunctionSpec("thumb_mov_pc_lr_return", "return_paths", True, _thumb_halfword(0x46F7), terminal=True),
    ))
    shared_cmp = _arm_dp(10, 0, rd=0, rn=0, immediate=True)
    shared_add_one = _arm_dp(4, 1, rd=0, rn=0, set_flags=False, immediate=True)
    shared_add_two = _arm_dp(4, 2, rd=0, rn=0, set_flags=False, immediate=True)
    specs.extend((
        FunctionSpec(
            "arm_shared_branch_a", "shared_branch", False,
            b"".join(_arm_word(word) for word in (
                shared_cmp, _arm_branch(4, 48, condition=0),
                shared_cmp, _arm_branch(12, 20, condition=0),
                ARM_BX_LR, shared_add_one, ARM_BX_LR,
            )),
            terminal=True,
        ),
        FunctionSpec(
            "arm_shared_branch_b", "shared_branch", False,
            b"".join(_arm_word(word) for word in (
                shared_cmp, _arm_branch(4, -8, condition=0),
                shared_cmp, _arm_branch(12, 20, condition=0),
                ARM_BX_LR, shared_add_two, ARM_BX_LR,
            )),
            terminal=True,
        ),
    ))
    thumb_cmp = 0x2800  # cmp r0, #0
    thumb_add_one = 0x3001
    thumb_add_two = 0x3002
    specs.extend((
        FunctionSpec(
            "thumb_shared_branch_a", "shared_branch", True,
            b"".join(_thumb_halfword(word) for word in (
                thumb_cmp, _thumb_cond_branch(2, 24),
                thumb_cmp, _thumb_cond_branch(6, 10),
                THUMB_BX_LR, thumb_add_one, THUMB_BX_LR,
            )),
            terminal=True,
        ),
        FunctionSpec(
            "thumb_shared_branch_b", "shared_branch", True,
            b"".join(_thumb_halfword(word) for word in (
                thumb_cmp, _thumb_cond_branch(2, -4),
                thumb_cmp, _thumb_cond_branch(6, 10),
                THUMB_BX_LR, thumb_add_two, THUMB_BX_LR,
            )),
            terminal=True,
        ),
    ))

    specs.extend((
        FunctionSpec(
            "arm_shared_branch_data_source", "shared_branch_reject", False,
            _arm_word(_arm_branch(0, 8)), terminal=True,
        ),
        FunctionSpec(
            "arm_shared_branch_data_owner", "shared_branch_reject", False,
            _arm_word(ARM_BX_LR) + _arm_word(0xDEADBEEF), terminal=True,
        ),
        FunctionSpec(
            "thumb_shared_branch_mid_source", "shared_branch_reject", True,
            _thumb_halfword(0xE000), terminal=True,
        ),
        FunctionSpec(
            "thumb_shared_branch_mid_owner", "shared_branch_reject", True,
            _thumb_halfword(0xF000) + _thumb_halfword(0xF800) + _thumb_halfword(THUMB_BX_LR),
            terminal=True,
        ),
        FunctionSpec(
            "arm_shared_branch_ambiguous_source", "shared_branch_reject", False,
            _arm_word(_arm_branch(0, 16)), terminal=True,
        ),
        FunctionSpec(
            "arm_shared_branch_ambiguous_owner", "shared_branch_reject", False,
            b"".join(_arm_word(word) for word in (
                shared_cmp, _arm_branch(4, 12, condition=0),
                ARM_BX_LR, shared_add_one, ARM_BX_LR,
            )),
            terminal=True,
        ),
    ))


    specs.extend((
        FunctionSpec(
            "arm_manual_link_conditional_call",
            "manual_link",
            False,
            b"".join(_arm_word(word) for word in (
                0xE1A0500E,  # mov r5, lr
                0x228F4008,  # addhs r4, pc, #8 -> target at +0x14
                0x328F4008,  # addlo r4, pc, #8 -> target at +0x18
                0xE28FE008,  # add lr, pc, #8 -> continuation at +0x1c
                0xE12FFF14,  # bx r4
                ARM_BX_LR,
                ARM_BX_LR,
                0xE1A0E005,  # mov lr, r5
                0xE2800001,  # add r0, r0, #1
                ARM_BX_LR,
            )),
            terminal=True,
        ),
        FunctionSpec(
            "thumb_manual_link_pc_call",
            "manual_link",
            True,
            b"".join(_thumb_halfword(word) for word in (
                0x4675,  # mov r5, lr
                0x46FE,  # mov lr, pc; Thumb PC value is address + 4
                0x4718,  # bx r3
                0x46AE,  # mov lr, r5
                0x3001,  # adds r0, #1
                THUMB_BX_LR,
                0x46F7,  # mov pc, lr, same-state Thumb return
            )),
            terminal=True,
        ),
    ))

    specs.extend((
        FunctionSpec(
            "arm_manual_link_conditional_b",
            "manual_link_conditional_b",
            False,
            b"".join(_arm_word(word) for word in (
                0xE92D4000,  # push {lr}
                0xE1A0E00F,  # mov lr, pc -> continuation at +12
                _arm_branch(8, 16, condition=1),  # bne to an indirect-call-like leaf
                0xE8BD8000,  # pop {pc}
                ARM_BX_LR,
            )),
            terminal=True,
        ),
        FunctionSpec(
            # crt0 shape: LR is seeded from a literal with an external return
            # address, then a conditional B selects a stack fix-up before the
            # tail jump. The B is an ordinary local branch, not a call.
            "arm_seeded_lr_local_branch",
            "manual_link_conditional_b",
            False,
            b"".join(_arm_word(word) for word in (
                0xE59FE010,  # ldr lr, [pc, #0x10] -> literal at +0x18
                0xE3100004,  # tst r0, #4
                _arm_branch(8, 16, condition=1),  # bne +16 (sub r0)
                0xEA000000,  # b +20 (bx lr)
                0xE2400004,  # sub r0, r0, #4
                ARM_BX_LR,   # tail jump to the seeded return address
                RETURN_ADDRESS,
            )),
            terminal=True,
        ),
        FunctionSpec(
            "arm_relative_offset_table",
            "relative_table",
            False,
            b"".join(_arm_word(word) for word in (
                _arm_dp(4, 0xC40, rd=1, rn=15, set_flags=False, immediate=True),
                _arm_single(load=True, rd=2, rn=1, rm=0, shift=2),
                _arm_dp(4, 1, rd=15, rn=2, set_flags=False),
                _arm_dp(4, 1, rd=4, rn=4, set_flags=False, immediate=True),
                _arm_branch(16, 24),
                _arm_dp(4, 2, rd=4, rn=4, set_flags=False, immediate=True),
                ARM_BX_LR,
            )),
            terminal=True,
            index_reg=0,
        ),
        FunctionSpec(
            # Mobiclip shape: the indexed load overwrites its own index register
            # (`ldr r0, [r1, r0, lsl #2]; add pc, r0, r1`).
            "arm_relative_offset_table_index_reuse",
            "relative_table",
            False,
            b"".join(_arm_word(word) for word in (
                _arm_dp(4, 0xC40, rd=1, rn=15, set_flags=False, immediate=True),
                _arm_single(load=True, rd=0, rn=1, rm=0, shift=2),
                _arm_dp(4, 1, rd=15, rn=0, set_flags=False),
                _arm_dp(4, 1, rd=4, rn=4, set_flags=False, immediate=True),
                _arm_branch(16, 24),
                _arm_dp(4, 2, rd=4, rn=4, set_flags=False, immediate=True),
                ARM_BX_LR,
            )),
            terminal=True,
            index_reg=0,
        ),
        FunctionSpec(
            "arm_shared_region_interior",
            "shared_region",
            False,
            b"".join(_arm_word(word) for word in (
                0xE92D4000,
                _arm_branch(CODE_BASE + 4, CODE_BASE + 32) | (1 << 24),
                _arm_branch(CODE_BASE + 8, CODE_BASE + 40) | (1 << 24),
                0xE8BD4000,
                _arm_dp(4, 1, rd=0, rn=0, set_flags=False, immediate=True),
                _arm_dp(4, 1, rd=1, rn=1, set_flags=False, immediate=True),
                _arm_dp(4, 1, rd=2, rn=2, set_flags=False, immediate=True),
                ARM_BX_LR,
                _arm_dp(4, 1, rd=3, rn=3, set_flags=False, immediate=True),
                _arm_dp(13, 14, rd=15, rn=0, set_flags=False),
                0xE92D4000,
                0xE8BD8000,
            )),
            terminal=True,
        ),
        FunctionSpec(
            "arm_external_call_to_region_interior",
            "shared_region_external_call",
            False,
            b"",
            terminal=True,
        ),
    ))

    # DSD can split an `add pc, pc, rX, lsl #2` table's slots into separate
    # tiny functions; the computed transfer then lands mid-"function".
    specs.extend((
        FunctionSpec(
            "arm_split_table_owner", "split_table", False,
            b"".join(_arm_word(word) for word in (
                0xE3500002,  # cmp r0, #2
                0x908FF100,  # addls pc, pc, r0, lsl #2 -> slots in the next function
                ARM_BX_LR,   # out-of-range index
            )),
            terminal=True,
        ),
        FunctionSpec(
            "arm_split_table_slots", "split_table", False,
            b"".join(_arm_word(word) for word in (
                _arm_branch(0, 12),  # slot 0: b +12 (ends this DSD "function")
                0xE2811002,          # slot 1: add r1, r1, #2 (never decoded from slot 0)
                ARM_BX_LR,           # slot 2
                0xE3A01001,          # slot 0 target: mov r1, #1
                ARM_BX_LR,
            )),
            terminal=True,
        ),
    ))

    specs.append(FunctionSpec(
        "arm_live_pc_literal", "arm_live_literal", False,
        _arm_word(0xE59F0010) + _arm_word(0xE59FF00C),
    ))
    specs.append(FunctionSpec(
        "thumb_live_literal", "thumb_live_literal", True, _thumb_halfword(0x4802),
    ))
    return specs


def _block_start(mode: str, count: int) -> int:
    return {"IA": 0, "IB": 4, "DA": -4 * count + 4, "DB": -4 * count}[mode]


def _put_u32(memory: bytearray, offset: int, value: int) -> None:
    if 0 <= offset <= len(memory) - 4:
        struct.pack_into("<I", memory, offset, value & 0xFFFFFFFF)


def _arm_condition_passed(condition: int, flags: tuple[int, ...]) -> bool:
    n, z, c, v = flags[:4]
    return (
        condition == 14 or
        condition == 0 and bool(z) or
        condition == 1 and not z or
        condition == 2 and bool(c) or
        condition == 3 and not c or
        condition == 4 and bool(n) or
        condition == 5 and not n or
        condition == 6 and bool(v) or
        condition == 7 and not v or
        condition == 8 and bool(c) and not z or
        condition == 9 and (not c or z) or
        condition == 10 and n == v or
        condition == 11 and n != v or
        condition == 12 and not z and n == v or
        condition == 13 and (z or n != v)
    )
def _arm_shifted_offset(word: int, regs: list[int], carry: int) -> int:
    value = regs[word & 15]
    amount = (word >> 7) & 31
    kind = (word >> 5) & 3
    if kind == 0:
        return (value << amount) & 0xFFFFFFFF
    if kind == 1:
        return value >> (amount or 32)
    if kind == 2:
        signed = value if value < 0x80000000 else value - 0x100000000
        return (signed >> (amount or 32)) & 0xFFFFFFFF
    if amount == 0:
        return ((carry << 31) | (value >> 1)) & 0xFFFFFFFF
    amount %= 32
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF
def _make_state(spec: FunctionSpec, slot: int, sample: int, rng: random.Random) -> TestState:
    regs = [rng.getrandbits(32) for _ in range(16)]
    edge_values = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x55555555, 0xAAAAAAAA, 0x10000)
    if sample < len(edge_values):
        for reg in range(13):
            regs[reg] = edge_values[(sample + 3 * reg) % len(edge_values)]
    regs[14] = RETURN_ADDRESS | int(spec.thumb)
    flags = tuple((sample >> bit) & 1 for bit in range(5))
    memory = bytearray(rng.getrandbits(8) for _ in range(MEMORY_SIZE))

    if spec.shift_reg >= 0:
        regs[spec.shift_reg] = spec.shift_values[sample % len(spec.shift_values)]
    if spec.index_reg >= 0:
        regs[spec.index_reg] = rng.choice((0, 1, 2, 3, 4, 7, 11, 15))
        if spec.index_values:
            regs[spec.index_reg] = spec.index_values[sample % len(spec.index_values)]
    if spec.category in {"arm_memory", "thumb_memory", "priority_memory"}:
        regs[spec.base_reg] = DATA_BASE + 64 + (
            (sample & 3) if spec.category != "priority_memory" else 0
        )
    elif spec.category in {"stress_aligned_memory", "stress_rrx_memory"}:
        regs[spec.base_reg] = DATA_BASE + 96
        if spec.category == "stress_rrx_memory":
            offset = (flags[2] << 31) | (regs[spec.index_reg] >> 1)
            regs[spec.base_reg] = (DATA_BASE + 96 - offset) & 0xFFFFFFFF
    elif spec.category in {"arm_block", "thumb_block", "priority_block"} or spec.pc_offset >= 0:
        stack_base = 160 if spec.base_reg == 13 and spec.name == "thumb_push" else 64 if spec.base_reg == 13 else 96
        increment = 0 if spec.name == "thumb_push" else 4
        regs[spec.base_reg] = DATA_BASE + stack_base + ((sample & 3) * increment)
        if spec.pc_offset >= 0:
            _put_u32(memory, regs[spec.base_reg] - DATA_BASE + spec.pc_offset, RETURN_ADDRESS | int(spec.thumb))
        elif spec.block_regs and 15 in spec.block_regs:
            reg_slot = spec.block_regs.index(15)
            addr_offset = _block_start(spec.block_mode, len(spec.block_regs)) + reg_slot * 4
            _put_u32(memory, regs[spec.base_reg] - DATA_BASE + addr_offset, RETURN_ADDRESS)

    return TestState(slot, spec, sample, regs, flags, bytes(memory))

_RUNNER_SOURCE = r'''#include "decls.h"

#include "code_image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <cstdlib>

namespace {
constexpr std::uint32_t kDataBase = 0x08004000u;  // Synthetic flat-memory data space.
constexpr std::size_t kMemorySize = 256;
std::array<std::uint8_t, dsr::kPageSize> flat_memory{};
std::array<std::uint8_t*, dsr::kPageCount> read_pages{};
std::array<std::uint8_t*, dsr::kPageCount> write_pages{};
std::array<std::uint8_t, dsr::kPageSize> code_memory{};
std::array<std::uint32_t, 2> user_sp_lr{};

bool in_memory(std::uint32_t address, std::size_t width) {
    return address >= kDataBase && static_cast<std::uint64_t>(address) + width <=
        static_cast<std::uint64_t>(kDataBase) + flat_memory.size();
}
std::uint32_t flat_read(std::uint32_t address, unsigned width) {
    if (!in_memory(address, width)) return 0;
    const std::size_t offset = address - kDataBase;
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::uint32_t(flat_memory[offset + i]) << (8 * i);
    return value;
}
void flat_write(std::uint32_t address, std::uint32_t value, unsigned width) {
    if (!in_memory(address, width)) return;
    const std::size_t offset = address - kDataBase;
    for (unsigned i = 0; i < width; ++i) flat_memory[offset + i] = std::uint8_t(value >> (8 * i));
}
std::uint64_t guest_hash(
    dsr::Cpu* cpu, std::uint32_t address, std::uint32_t size,
    const dsr::GuestAddressRange* ranges = nullptr, std::size_t range_count = 0) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    std::size_t range_index = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        const std::uint32_t current = address + i;
        while (range_index < range_count &&
               ranges[range_index].begin + ranges[range_index].size <= current)
            ++range_index;
        if (range_index < range_count && current >= ranges[range_index].begin &&
            current - ranges[range_index].begin < ranges[range_index].size)
            continue;
        const std::uint8_t* page = cpu->page_r[current >> dsr::kPageShift];
        if (page == nullptr) return 0;
        hash ^= page[current & (dsr::kPageSize - 1)];
        hash *= 0x100000001b3ull;
    }
    return hash;
}
struct WireCase {
    std::uint32_t entry_addr;
    std::uint32_t regs[16];
    std::uint32_t flags[5];
    std::uint8_t memory[kMemorySize];
    std::uint32_t table_offset;
    std::uint8_t table_bytes[12];
    std::uint32_t user_regs[2];
};
static_assert(sizeof(WireCase) == 368);
}

namespace dsr {
void dsr_checkpoint(Cpu*, std::uint32_t) {}
void dsr_note_return(Cpu* c, std::uint32_t target) { c->r[15] = target; }
void dsr_check_return(Cpu* c, std::uint32_t expected) {
    if ((c->r[15] & ~1u) != (expected & ~1u)) std::abort();
}
std::uint32_t dsr_user_reg_read(Cpu* c, unsigned reg) {
    return reg == 13 || reg == 14 ? user_sp_lr[reg - 13] : c->r[reg];
}
void dsr_user_reg_write(Cpu* c, unsigned reg, std::uint32_t value) {
    if (reg == 13 || reg == 14) user_sp_lr[reg - 13] = value;
    else c->r[reg] = value;
}
void dsr_call(Cpu* c, std::uint32_t target) {
    const std::uint32_t address = target & ~1u;
    const FnTable& functions = gen_table_arm9();
    const FnEntry* entry = nullptr;
    std::size_t exact_count = 0;
    for (std::size_t i = 0; i < functions.count; ++i) {
        const FnEntry& candidate = functions.entries[i];
        if (candidate.addr != address || candidate.slot == nullptr) continue;
        entry = &candidate;
        ++exact_count;
    }
    if (exact_count > 1) {
        entry = nullptr;
        for (std::size_t i = 0; i < functions.count; ++i) {
            const FnEntry& candidate = functions.entries[i];
            if (candidate.addr == address && candidate.slot != nullptr && candidate.size != 0 &&
                guest_hash(c, candidate.addr, candidate.size) == candidate.hash) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry != nullptr) {
        const std::uint32_t old_t = c->cpsr_ctrl & (1u << 5);
        c->cpsr_ctrl = (c->cpsr_ctrl & ~(1u << 5)) | ((target & 1u) ? (1u << 5) : 0u);
        c->r[15] = target;
        (*entry->slot)(c);
        c->cpsr_ctrl = (c->cpsr_ctrl & ~(1u << 5)) | old_t;
        return;
    }
    const DispatchRegionTable& regions = gen_dispatch_regions_arm9();
    for (std::size_t i = 0; i < regions.count; ++i) {
        const DispatchRegionEntry& region = regions.entries[i];
        if (region.thumb != (target & 1u) || address < region.begin ||
            address - region.begin >= region.size ||
            guest_hash(c, region.begin, region.size, region.data_ranges, region.data_range_count) != region.hash)
            continue;
        const std::uint32_t old_t = c->cpsr_ctrl & (1u << 5);
        c->cpsr_ctrl = (c->cpsr_ctrl & ~(1u << 5)) | ((target & 1u) ? (1u << 5) : 0u);
        c->r[15] = target;
        region.body(c, address);
        c->cpsr_ctrl = (c->cpsr_ctrl & ~(1u << 5)) | old_t;
        return;
    }
    c->r[15] = target;
}
[[noreturn]] void dsr_unresolved_target(Cpu*, std::uint32_t) { std::abort(); }
std::uint32_t slow_read32(Cpu*, std::uint32_t a) { return flat_read(a, 4); }
std::uint32_t slow_read16(Cpu*, std::uint32_t a) { return flat_read(a, 2); }
std::uint32_t slow_read8(Cpu*, std::uint32_t a) { return flat_read(a, 1); }
void slow_write32(Cpu*, std::uint32_t a, std::uint32_t v, std::uint32_t) { flat_write(a, v, 4); }
void slow_write16(Cpu*, std::uint32_t a, std::uint32_t v, std::uint32_t) { flat_write(a, v, 2); }
void slow_write8(Cpu*, std::uint32_t a, std::uint32_t v, std::uint32_t) { flat_write(a, v, 1); }
}

int main() {
    std::uint32_t count = 0;
    if (!std::cin.read(reinterpret_cast<char*>(&count), sizeof(count))) return 2;
    constexpr std::size_t data_page = kDataBase >> dsr::kPageShift;
    // Guest code is readable RAM on the DS: PC-relative literals load live.
    std::memcpy(code_memory.data(), kCodeImage, std::min(sizeof(kCodeImage), code_memory.size()));
    read_pages[kCodeBase >> dsr::kPageShift] = code_memory.data();
    read_pages[data_page] = flat_memory.data();
    write_pages[data_page] = flat_memory.data();
    const dsr::FnTable& table = dsr::gen_table_arm9();
    for (std::uint32_t test = 0; test < count; ++test) {
        WireCase input{};
        if (!std::cin.read(reinterpret_cast<char*>(&input), sizeof(input))) return 3;
        const dsr::FnEntry* entry = nullptr;
        for (std::size_t index = 0; index < table.count; ++index) {
            const dsr::FnEntry& candidate = table.entries[index];
            if (candidate.addr != input.entry_addr) continue;
            if (entry != nullptr) return 4;
            entry = &candidate;
        }
        if (entry == nullptr || entry->slot == nullptr) return 4;

        flat_memory.fill(0);
        std::memcpy(flat_memory.data(), input.memory, sizeof(input.memory));
        if (input.table_offset != 0) {
            if (input.table_offset > flat_memory.size() - sizeof(input.table_bytes)) return 6;
            std::memcpy(flat_memory.data() + input.table_offset, input.table_bytes, sizeof(input.table_bytes));
        }
        dsr::Cpu cpu{};
        std::memcpy(cpu.r, input.regs, sizeof(cpu.r));
        std::copy(std::begin(input.user_regs), std::end(input.user_regs), user_sp_lr.begin());
        cpu.n = input.flags[0];
        cpu.z = input.flags[1];
        cpu.cf = input.flags[2];
        cpu.v = input.flags[3];
        cpu.q = input.flags[4];
        cpu.cpsr_ctrl = dsr::kModeSvc | (entry->thumb ? (1u << 5) : 0u);

        cpu.budget = std::numeric_limits<std::int64_t>::max();
        cpu.id = dsr::CpuId::Arm9;
        cpu.page_r = read_pages.data();
        cpu.page_w = write_pages.data();
        (*entry->slot)(&cpu);
        WireCase output{};
        output.entry_addr = input.entry_addr;

        std::memcpy(output.regs, cpu.r, sizeof(output.regs));
        output.flags[0] = cpu.n;
        output.flags[1] = cpu.z;
        output.flags[2] = cpu.cf;
        output.flags[3] = cpu.v;
        output.flags[4] = cpu.q;
        std::memcpy(output.memory, flat_memory.data(), sizeof(output.memory));
        std::copy(user_sp_lr.begin(), user_sp_lr.end(), std::begin(output.user_regs));
        std::cout.write(reinterpret_cast<const char*>(&output), sizeof(output));
        if (!std::cout) return 5;
    }
    return 0;
}
'''


@unittest.skipIf(_IMPORT_ERROR is not None, f"recompiler differential dependencies unavailable: {_IMPORT_ERROR}")
class RecompilerDifferentialTests(unittest.TestCase):
    states_per_function = 64
    sequence_seed = 0xA946_7E57
    sequence_count = 192

    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("g++ is required for emitted-code differential tests")
        cls._temporary = tempfile.TemporaryDirectory(prefix="recomp-differential-")
        root = Path(cls._temporary.name)
        generated = root / "generated"
        synthetic_forms = _synthetic_instruction_forms()
        base_specs = _make_specs()
        from tests.recomp_tests.sequence_generator import make_specs
        base_specs = base_specs[:-2] + make_specs(cls.sequence_seed, cls.sequence_count) + base_specs[-2:]
        priority_specs, priority_expected_failures = _synthetic_priority_specs(synthetic_forms)
        specs = base_specs[:-2] + priority_specs + base_specs[-2:]
        image = bytearray()
        functions = []
        for spec in specs:
            if spec.category in {"arm_live_literal", "thumb_live_literal"}:
                target_offset = PAGE_SIZE - (24 if spec.category == "arm_live_literal" else 10)
                if len(image) > target_offset:
                    raise AssertionError("live literal fixtures no longer fit at the end of the code page")
                image.extend(bytes(target_offset - len(image)))
            alignment = 4 if not spec.thumb else 2
            while (CODE_BASE + len(image)) % alignment:
                image.append(0)
            address = CODE_BASE + len(image)
            if spec.name == "arm_external_call_to_region_interior":
                region = next(item for item in functions if item["name"] == "arm_shared_region_interior")
                target = region["addr"] + 20
                code = b"".join((
                    _arm_word(_arm_dp(13, 14, rd=12, rn=0, set_flags=False)),
                    _arm_word(_arm_branch(address + 4, target) | (1 << 24)),
                    _arm_word(_arm_dp(4, 1, rd=3, rn=3, immediate=True)),
                    _arm_word(_arm_dp(13, 12, rd=14, rn=0, set_flags=False)),
                    _arm_word(ARM_BX_LR),
                ))
            else:
                code = spec.code()
                if spec.name == "stress_thumb_blx_immediate":
                    target = (address + len(code) + 3) & ~3
                    displacement = target - ((address + 2 + 4) & ~3)
                    suffix = 0xE800 | ((displacement >> 1) & 0x7FF)
                    code = code[:4] + _thumb_halfword(suffix) + code[6:]
            image.extend(code)
            functions.append({"addr": address, "size": len(code), "thumb": spec.thumb, "name": spec.name})
            if spec.category == "thumb_live_literal":
                image.extend(bytes(PAGE_SIZE - len(image)))
                image.extend(struct.pack("<I", RETURN_ADDRESS + 8))
        for relative_table in (
            function for function in functions
            if function["name"].startswith("arm_relative_offset_table")
        ):
            table_address = relative_table["addr"] + 0x4008
            table_offset = table_address - CODE_BASE
            if len(image) < table_offset + 12:
                image.extend(bytes(table_offset + 12 - len(image)))
            table_values = (
                (relative_table["addr"] + 12 - table_address) & 0xFFFFFFFF,
                (relative_table["addr"] + 16 - table_address) & 0xFFFFFFFF,
                0xFFFFFFFF,
            )
            struct.pack_into("<III", image, table_offset, *table_values)
        image_path = root / "synthetic.bin"
        image_path.write_bytes(image)
        manifest = {
            "cpu": "arm9",
            "arch": "v5te",
            "modules": [{
                "name": "synthetic",
                "module_id": -1,
                "base": CODE_BASE,
                "image": str(image_path),
                "functions": functions,
                "labels": [],
                "data_ranges": [],
                "calls": [],
            }],
        }
        ambiguous_owner = next(
            function for function in functions
            if function["name"] == "arm_shared_branch_ambiguous_owner"
        )
        manifest["modules"].append({
            "name": "ambiguous_shared_overlay",
            "module_id": 0,
            "base": CODE_BASE,
            "image": str(image_path),
            "functions": [{
                "addr": ambiguous_owner["addr"] + 8,
                "size": 8,
                "thumb": False,
                "name": "overlapping_window",
            }],
            "labels": [],
            "data_ranges": [],
            "calls": [],
        })

        function_by_name = {function["name"]: function for function in functions}
        # Synthetic overlapping entry roots share an identical decoded tail.
        # Cross-function branches must still reach that tail.
        for owner_name in (
            "arm_shared_branch_a", "arm_shared_branch_b",
            "thumb_shared_branch_a", "thumb_shared_branch_b",
        ):
            owner = function_by_name[owner_name]
            offset = 4 if owner["thumb"] else 8
            manifest["modules"][0]["functions"].append({
                "addr": owner["addr"] + offset,
                "size": owner["size"] - offset,
                "thumb": owner["thumb"],
                "name": owner_name + "_alias",
            })
        # The manual-link fixtures call small interior leaves. Register them
        # explicitly rather than letting the flat host silently skip the call.
        for owner_name, offsets in (
            ("arm_manual_link_conditional_call", (20, 24)),
            ("thumb_manual_link_pc_call", (12,)),
            ("arm_manual_link_conditional_b", (16,)),
        ):
            owner = function_by_name[owner_name]
            for offset in offsets:
                manifest["modules"][0]["functions"].append({
                    "addr": owner["addr"] + offset,
                    "size": 2 if owner["thumb"] else 4,
                    "thumb": owner["thumb"],
                    "name": owner_name + f"_leaf_{offset}",
                })
        interior_region = function_by_name["arm_shared_region_interior"]
        manifest["modules"][0]["dispatch_regions"] = [
            {
                "addr": relative_table["addr"],
                "size": relative_table["size"],
                "thumb": False,
                "entries": [
                    relative_table["addr"],
                    relative_table["addr"] + 12,
                    relative_table["addr"] + 24,
                ],
                "data_ranges": [],
            }
            for relative_table in (
                function_by_name["arm_relative_offset_table"],
                function_by_name["arm_relative_offset_table_index_reuse"],
            )
        ] + [
            {
                "addr": interior_region["addr"],
                "size": interior_region["size"],
                "thumb": False,
                "entries": [
                    interior_region["addr"],
                    interior_region["addr"] + 16,
                    interior_region["addr"] + 32,
                    interior_region["addr"] + 40,
                ],
                "data_ranges": [],
            },
        ]

        stats = emit_manifest(manifest, generated, chunk_size=128)
        unsupported = stats["unsupported_sites"]
        if unsupported:
            raise AssertionError(f"unexpected unsupported sites: {unsupported}")
        runner = root / "flat_runtime.cpp"
        runner.write_text(_RUNNER_SOURCE, encoding="utf-8")
        code_bytes = ", ".join(str(byte) for byte in image[:PAGE_SIZE])
        (root / "code_image.h").write_text(
            "#pragma once\n#include <cstdint>\n"
            f"constexpr std::uint32_t kCodeBase = {CODE_BASE:#x}u;\n"
            f"constexpr std::uint8_t kCodeImage[] = {{{code_bytes}}};\n",
            encoding="utf-8",
        )
        executable = root / "differential-runner"
        command = [
            compiler,
            "-std=c++20",
            "-O2",
            "-DDSR_RET_CHECK",
            "-I",
            str(Path(__file__).resolve().parents[2] / "runtime" / "include"),
            "-I",
            str(generated),
            *[str(path) for path in sorted(generated.glob("chunk_*.cpp"))],
            str(generated / "fn_table.cpp"),
            str(runner),
            "-o",
            str(executable),
        ]
        try:
            subprocess.run(command, check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as exc:
            raise AssertionError(f"generated C++ did not compile:\n{exc.stderr}") from exc
        cls.specs = specs
        cls.functions = functions
        cls.image = bytes(image)
        cls.executable = executable
        cls.stats = stats
        cls.synthetic_forms = synthetic_forms
        cls.priority_expected_failures = priority_expected_failures

    @classmethod
    def tearDownClass(cls) -> None:
        temporary = getattr(cls, "_temporary", None)
        if temporary is not None:
            temporary.cleanup()

    def _new_unicorn(self):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
        uc.ctl_set_cpu_model(UC_CPU_ARM_946)
        uc.mem_map(CODE_BASE, PAGE_SIZE)
        uc.mem_write(CODE_BASE, self.image[:PAGE_SIZE])
        uc.mem_map(DATA_BASE, PAGE_SIZE)
        uc.mem_map(RETURN_ADDRESS, 0x1000)
        returned = {"value": False}

        def stop_on_return(engine, address, _size, _user_data):
            if RETURN_ADDRESS <= (address & ~1) < RETURN_ADDRESS + 0x1000:
                returned["value"] = True
                engine.emu_stop()

        uc.hook_add(UC_HOOK_CODE, stop_on_return)
        return uc, returned

    @staticmethod
    def _run_unicorn(uc, returned, state: TestState) -> NativeState:
        uc.mem_write(DATA_BASE, state.memory)
        if state.table_offset != 0:
            uc.mem_write(DATA_BASE + state.table_offset, state.table_bytes)
        for reg, value in zip(ARM_REGISTERS, state.regs[:15]):
            uc.reg_write(reg, value)
        n, z, carry, overflow, q = state.flags
        cpsr = 0xD3 | (int(state.spec.thumb) << 5)
        cpsr |= n << 31 | z << 30 | carry << 29 | overflow << 28 | q << 27
        uc.reg_write(UC_ARM_REG_CPSR, cpsr)
        uc.reg_write(UC_ARM_REG_CPSR, (cpsr & ~31) | 0x10)
        uc.reg_write(UC_ARM_REG_SP, state.user_regs[0])
        uc.reg_write(UC_ARM_REG_LR, state.user_regs[1])
        uc.reg_write(UC_ARM_REG_CPSR, cpsr)
        returned["value"] = False
        address = state.slot_address
        # Unicorn's ARM946 implementation reads PC+8 even for ARM register-shifted
        # operands and stores. The ARM architecture permits +12 stores and
        # requires +12 for Rn/Rm in register-shifted DP (GBATEK, ARM ALU).
        # Substitute an otherwise-unused r12 containing PC+12 for those
        # focused fixtures; leave all other instructions/reference state alone.
        pc12 = state.spec.name in {
            "stress_arm_pc_shift_reg", "stress_arm_pc_rn_regshift",
            "stress_arm_pc_shift_alias", "stress_arm_store_pc", "stress_arm_stm_pc",
        }
        original = None
        if pc12:
            original = bytes(uc.mem_read(address, 4))
            word = struct.unpack("<I", original)[0]
            if state.spec.name == "stress_arm_store_pc":
                word = (word & ~(15 << 12)) | (12 << 12)
            elif state.spec.name == "stress_arm_stm_pc":
                word = (word & ~(1 << 15)) | (1 << 12)
            elif state.spec.name == "stress_arm_pc_rn_regshift":
                word = (word & ~(15 << 16)) | (12 << 16)
            else:
                word = (word & ~15) | 12
            uc.mem_write(address, _arm_word(word))
            uc.ctl_remove_cache(address, address + 4)
            uc.reg_write(UC_ARM_REG_R12, address + 12)
        try:
            uc.emu_start(
                address | int(state.spec.thumb),
                RETURN_ADDRESS + 0x1000,
                count=16 if state.spec.category in {
                    "computed_jump", "manual_link", "relative_table", "shared_region", "shared_region_external_call"
                } else 8,
            )
        except Exception as exc:
            raise AssertionError(f"Unicorn could not execute {state.spec.name} case {state.sample}") from exc
        finally:
            if original is not None:
                uc.mem_write(address, original)
                uc.ctl_remove_cache(address, address + 4)
                uc.reg_write(UC_ARM_REG_R12, state.regs[12])
        if not returned["value"]:
            raise AssertionError(f"Unicorn did not reach return sentinel for {state.spec.name} case {state.sample}")
        regs = tuple(uc.reg_read(reg) & 0xFFFFFFFF for reg in ARM_REGISTERS)
        final_cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        flags = tuple((final_cpsr >> bit) & 1 for bit in (31, 30, 29, 28, 27))
        uc.reg_write(UC_ARM_REG_CPSR, (final_cpsr & ~31) | 0x10)
        user_regs = (uc.reg_read(UC_ARM_REG_SP), uc.reg_read(UC_ARM_REG_LR))
        uc.reg_write(UC_ARM_REG_CPSR, final_cpsr)
        memory = bytes(uc.mem_read(DATA_BASE, MEMORY_SIZE))
        if (
            state.spec.category in {"arm_memory", "priority_memory"}
            and not state.spec.name.startswith(("synthetic_ldrd", "synthetic_strd", "synthetic_swp"))
            and _arm_condition_passed(
                (struct.unpack("<I", state.spec.instruction)[0] >> 28) & 0xF, state.flags
            )
        ):
            instruction = struct.unpack("<I", state.spec.instruction)[0]
            if instruction & (1 << 26):
                rn = (instruction >> 16) & 15
                rd = (instruction >> 12) & 15
                base = state.regs[rn] if rn != 15 else (state.slot_address + 8)
                if instruction & (1 << 25):
                    offset = _arm_shifted_offset(instruction, state.regs, state.flags[2])
                else:
                    offset = instruction & 0xFFF
                if not instruction & (1 << 24):
                    address = base
                elif instruction & (1 << 23):
                    address = (base + offset) & 0xFFFFFFFF
                else:
                    address = (base - offset) & 0xFFFFFFFF
                if not instruction & (1 << 22):
                    aligned = (address - DATA_BASE) & ~3
                    if instruction & (1 << 20):
                        value = int.from_bytes(state.memory[aligned : aligned + 4], "little")
                        rotation = (address & 3) * 8
                        if rotation:
                            value = ((value >> rotation) | (value << (32 - rotation))) & 0xFFFFFFFF
                        if rd < len(regs):
                            regs = tuple(value if index == rd else current for index, current in enumerate(regs))
                    else:
                        memory = bytearray(state.memory)
                        memory[aligned : aligned + 4] = state.regs[rd].to_bytes(4, "little")
                        memory = bytes(memory)
            if instruction & 0x0E000090 == 0x00000090 and (instruction & 0x60):
                rn = (instruction >> 16) & 15
                rd = (instruction >> 12) & 15
                base = state.regs[rn] if rn != 15 else (state.slot_address + 8)
                if instruction & (1 << 22):
                    offset = ((instruction >> 4) & 0xF0) | (instruction & 0xF)
                else:
                    offset = state.regs[instruction & 15]
                if not instruction & (1 << 24):
                    address = base
                elif instruction & (1 << 23):
                    address = (base + offset) & 0xFFFFFFFF
                else:
                    address = (base - offset) & 0xFFFFFFFF
                signed = bool(instruction & (1 << 6))
                half = bool(instruction & (1 << 5))
                load = bool(instruction & (1 << 20))
                if load and half:
                    aligned = (address - DATA_BASE) & ~1
                    value = int.from_bytes(state.memory[aligned : aligned + 2], "little")
                    if signed and value & 0x8000:
                        value |= 0xFFFF0000
                    if rd < len(regs):
                        regs = tuple(value if index == rd else current for index, current in enumerate(regs))
                elif not load and half and not signed:
                    memory = bytearray(state.memory)
                    aligned = (address - DATA_BASE) & ~1
                    memory[aligned : aligned + 2] = (state.regs[rd] & 0xFFFF).to_bytes(2, "little")
                    memory = bytes(memory)
        if state.spec.category == "thumb_memory":
            instruction = struct.unpack("<H", state.spec.instruction)[0]
            top = instruction >> 12
            rd = instruction & 7
            if top in {6, 7}:
                load = bool(instruction & (1 << 11))
                size = 1 if instruction & (1 << 12) else 4
                base_reg = (instruction >> 3) & 7
                address = (state.regs[base_reg] + (((instruction >> 6) & 31) * size)) & 0xFFFFFFFF
                signed = False
            elif top == 8:
                load = bool(instruction & (1 << 11))
                size = 2
                base_reg = (instruction >> 3) & 7
                address = (state.regs[base_reg] + (((instruction >> 6) & 31) * 2)) & 0xFFFFFFFF
                signed = False
            elif top == 5:
                operation = (instruction >> 9) & 7
                base_reg = (instruction >> 3) & 7
                offset_reg = (instruction >> 6) & 7
                address = (state.regs[base_reg] + state.regs[offset_reg]) & 0xFFFFFFFF
                load = operation >= 3
                size = 2 if operation in {1, 5, 7} else 1 if operation in {2, 3, 6} else 4
                signed = operation in {3, 7}
            else:
                size = 0
            if size == 4:
                aligned = (address - DATA_BASE) & ~3
                if load:
                    value = int.from_bytes(state.memory[aligned : aligned + 4], "little")
                    rotation = (address & 3) * 8
                    if rotation:
                        value = ((value >> rotation) | (value << (32 - rotation))) & 0xFFFFFFFF
                    regs = tuple(value if index == rd else current for index, current in enumerate(regs))
                else:
                    memory = bytearray(state.memory)
                    memory[aligned : aligned + 4] = state.regs[rd].to_bytes(4, "little")
                    memory = bytes(memory)
            elif size == 2:
                aligned = (address - DATA_BASE) & ~1
                if load:
                    value = int.from_bytes(state.memory[aligned : aligned + 2], "little")
                    if signed and value & 0x8000:
                        value |= 0xFFFF0000
                    regs = tuple(value if index == rd else current for index, current in enumerate(regs))
                else:
                    memory = bytearray(state.memory)
                    memory[aligned : aligned + 2] = (state.regs[rd] & 0xFFFF).to_bytes(2, "little")
                    memory = bytes(memory)
        return NativeState(regs, flags, memory, uc.reg_read(UC_ARM_REG_PC), user_regs)

    def _native_batch(self, states: list[TestState]) -> list[NativeState]:
        payload = bytearray(struct.pack("<I", len(states)))
        for state in states:
            payload.extend(struct.pack(
                WIRE_FORMAT, state.slot_address, *state.regs, *state.flags, state.memory,
                state.table_offset, state.table_bytes, *state.user_regs,
            ))

        result = subprocess.run(
            [str(self.executable)],
            input=bytes(payload),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        ).stdout
        self.assertEqual(len(result), len(states) * WIRE_SIZE, "flat-memory runner returned a truncated state batch")
        output = []
        for index in range(len(states)):
            offset = index * WIRE_SIZE
            values = struct.unpack_from("<16I5I", result, offset + 4)
            output.append(NativeState(
                tuple(values[:15]), tuple(values[16:21]),
                result[offset + 88 : offset + 88 + MEMORY_SIZE], values[15],
                struct.unpack_from("<2I", result, offset + WIRE_SIZE - 8),
            ))
        return output

    def test_synthetic_priority_forms_have_synthesized_functions(self):
        self.assertTrue(self.synthetic_forms)
        expected, expected_failures = _synthetic_priority_specs(self.synthetic_forms)
        actual = [spec for spec in self.specs if spec.name.startswith("synthetic_")]
        self.assertEqual([spec.name for spec in actual], [spec.name for spec in expected])
        self.assertTrue(actual, "synthetic corpus yielded no priority specimens")
        self.assertEqual(expected_failures, self.priority_expected_failures)
        self.assertEqual(
            {mnemonic for mnemonic, _, _ in expected_failures},
            {"ldrht", "ldrsht", "ldmdb", "ldrd", "strd", "strh", "ldrsb", "ldrsh", "qdadd", "smlad", "smulbb", "swp", "swpb"},
            f"unexpected synthetic expected-failure forms: {expected_failures}",
        )

    def test_arm_and_thumb_batches_match_unicorn(self):
        seed = 0x2119_5EED
        rng = random.Random(seed)
        addresses = [entry["addr"] for entry in self.functions]
        cases: list[TestState] = []
        categories = set()
        for slot, spec in enumerate(self.specs):
            categories.add(spec.category)
            if spec.category in {
                "arm_live_literal", "thumb_live_literal", "computed_jump", "manual_link",
                "manual_link_conditional_b", "relative_table", "shared_region",
                "shared_region_external_call", "shared_branch", "shared_branch_reject", "split_table",
            }:
                continue

            sample_count = 8 if spec.name.startswith("synthetic_") else self.states_per_function
            for sample in range(sample_count):
                state = _make_state(spec, slot, sample, rng)
                state.slot_address = addresses[slot]
                if spec.category == "stress_exchange":
                    target_name = "stress_" + ("arm" if spec.thumb else "thumb") + "_exchange_leaf"
                    target = next(f for f in self.functions if f["name"] == target_name)
                    state.regs[3] = target["addr"] | int(target["thumb"])
                cases.append(state)
        self.assertGreaterEqual(len(cases), 5000, "the differential batch must exercise thousands of states")
        self.assertTrue({"arm_dp", "arm_shifts", "arm_flags", "multiply", "arm_memory", "arm_block", "clz_q_dsp", "thumb_dp", "thumb_memory", "thumb_block", "return_paths"} <= categories)

        native = self._native_batch(cases)
        uc, returned = self._new_unicorn()
        try:
            for case, actual in zip(cases, native):
                expected = self._run_unicorn(uc, returned, case)
                label = f"{case.spec.name} sample={case.sample} seed=0x{seed:x}"
                if actual.regs != expected.regs:
                    mismatches = [(i, hex(a), hex(e)) for i, (a, e) in enumerate(zip(actual.regs, expected.regs)) if a != e]
                    self.fail(f"{label}: register mismatch (rN, emitted, Unicorn): {mismatches}")
                if actual.flags != expected.flags:
                    self.fail(f"{label}: flags emitted={actual.flags} Unicorn={expected.flags}")
                self.assertEqual(actual.transfer_target & ~1, expected.transfer_target & ~1, label + ": PC")
                self.assertEqual(actual.user_regs, expected.user_regs, label + ": user SP/LR")
                if actual.memory != expected.memory:
                    at = next(i for i, (a, e) in enumerate(zip(actual.memory, expected.memory)) if a != e)
                    self.fail(
                        f"{label}: memory mismatch at 0x{DATA_BASE + at:08x}; "
                        f"emitted={actual.memory[at]:02x} Unicorn={expected.memory[at]:02x}"
                    )
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_manual_link_calls_preserve_conditional_targets_and_thumb_returns(self):
        slots = {
            spec.name: index
            for index, spec in enumerate(self.specs)
            if spec.category == "manual_link"
        }
        cases: list[TestState] = []
        expected_r4: list[int | None] = []
        expected_r0: list[int] = []
        arm_spec = self.specs[slots["arm_manual_link_conditional_call"]]
        for sample, carry in enumerate((0, 1)):
            state = _make_state(arm_spec, slots[arm_spec.name], sample, random.Random(sample))
            state.slot_address = self.functions[slots[arm_spec.name]]["addr"]
            state.flags = (0, 0, carry, 0, 0)
            state.regs[0] = 7
            cases.append(state)
            expected_r4.append(state.slot_address + (0x14 if carry else 0x18))
            expected_r0.append(8)

        thumb_spec = self.specs[slots["thumb_manual_link_pc_call"]]
        thumb_state = _make_state(thumb_spec, slots[thumb_spec.name], 0, random.Random(3))
        thumb_state.slot_address = self.functions[slots[thumb_spec.name]]["addr"]
        thumb_state.regs[0] = 11
        thumb_state.regs[3] = thumb_state.slot_address + 0xD
        cases.append(thumb_state)
        expected_r4.append(None)
        expected_r0.append(12)

        native = self._native_batch(cases)
        uc, returned = self._new_unicorn()
        try:
            for index, (state, actual) in enumerate(zip(cases, native)):
                expected = self._run_unicorn(uc, returned, state)
                label = f"{state.spec.name} sample={state.sample}"
                self.assertEqual(actual.regs, expected.regs, label)
                self.assertEqual(actual.flags, expected.flags, label)
                self.assertEqual(actual.memory, expected.memory, label)
                if expected_r4[index] is not None:
                    self.assertEqual(actual.regs[4], expected_r4[index], label)
                self.assertEqual(actual.regs[0], expected_r0[index], label)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_split_function_jump_table_slots_are_dispatchable(self):
        owner = next(i for i, spec in enumerate(self.specs) if spec.name == "arm_split_table_owner")
        spec = self.specs[owner]
        states = []
        for sample, index in enumerate((0, 1, 2, 3)):
            state = _make_state(spec, owner, sample, random.Random(sample))
            state.slot_address = self.functions[owner]["addr"]
            state.regs[0] = index
            state.regs[1] = 10
            state.regs[14] = RETURN_ADDRESS
            states.append(state)
        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual in zip(states, native):
                expected = self._run_unicorn(uc, returned, state)
                label = f"{spec.name} index={state.regs[0]}"
                self.assertEqual(actual.regs, expected.regs, label)
                self.assertEqual(actual.flags, expected.flags, label)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_manual_lr_conditional_b_calls_and_returns_to_lr(self):
        slots = [
            index for index, spec in enumerate(self.specs)
            if spec.category == "manual_link_conditional_b"
        ]
        self.assertEqual(len(slots), 2)
        states = []
        for slot in slots:
            spec = self.specs[slot]
            for sample, zero in enumerate((0, 1)):
                state = _make_state(spec, slot, sample, random.Random(sample))
                state.slot_address = self.functions[slot]["addr"]
                state.regs[13] = DATA_BASE + 64
                state.regs[14] = RETURN_ADDRESS
                state.regs[0] = 4 if zero == 0 else 8
                state.flags = (0, zero, 0, 0, 0)
                states.append(state)
        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual in zip(states, native):
                expected = self._run_unicorn(uc, returned, state)
                label = f"{state.spec.name} z={state.flags[1]}"
                self.assertEqual(actual.regs, expected.regs, label)
                self.assertEqual(actual.flags, expected.flags, label)
                self.assertEqual(actual.memory, expected.memory, label)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_relative_offset_jump_table_matches_unicorn(self):
        slots = [
            index for index, spec in enumerate(self.specs)
            if spec.category == "relative_table"
        ]
        self.assertEqual(len(slots), 2)
        states = []
        indices = []
        cases = ((0, (12, 16)), (0, (16, 12)), (1, (12, 16)), (2, (12, 16)))
        for slot in slots:
            spec = self.specs[slot]
            for sample, (index, targets) in enumerate(cases):
                state = _make_state(spec, slot, sample, random.Random(sample))
                state.slot_address = self.functions[slot]["addr"]
                table_base = state.slot_address + 0x4008
                state.table_offset = table_base - DATA_BASE
                state.table_bytes = struct.pack(
                    "<III",
                    (state.slot_address + targets[0] - table_base) & 0xFFFFFFFF,
                    (state.slot_address + targets[1] - table_base) & 0xFFFFFFFF,
                    (RETURN_ADDRESS - table_base) & 0xFFFFFFFF,
                )
                state.regs[0] = index
                state.regs[4] = 9
                memory = bytearray(state.memory)
                low = max(0, state.table_offset)
                high = min(MEMORY_SIZE, state.table_offset + len(state.table_bytes))
                if low < high:
                    memory[low:high] = state.table_bytes[low - state.table_offset:high - state.table_offset]
                state.memory = bytes(memory)
                states.append(state)
                indices.append(index)
        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual, index in zip(states, native, indices):
                expected = self._run_unicorn(uc, returned, state)
                label = f"{state.spec.name} index={index}"
                if index == 2:
                    self.assertEqual(actual.transfer_target, RETURN_ADDRESS, label)
                self.assertEqual(actual.regs, expected.regs, label)
                self.assertEqual(actual.flags, expected.flags, label)
                self.assertEqual(actual.memory, expected.memory, label)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_external_call_uses_region_fallback_for_unregistered_interior(self):
        slot = next(
            index for index, spec in enumerate(self.specs)
            if spec.category == "shared_region_external_call"
        )
        state = _make_state(self.specs[slot], slot, 0, random.Random(0))
        state.slot_address = self.functions[slot]["addr"]
        state.regs[0:4] = [10, 20, 30, 40]
        state.regs[14] = RETURN_ADDRESS
        actual = self._native_batch([state])[0]
        uc, returned = self._new_unicorn()
        try:
            expected = self._run_unicorn(uc, returned, state)
            label = "BL into unregistered shared-region instruction"
            self.assertEqual(actual.regs, expected.regs, label)
            self.assertEqual(actual.flags, expected.flags, label)
            self.assertEqual(actual.memory, expected.memory, label)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_shared_region_internal_return_and_interior_entry(self):
        slot = next(
            index for index, spec in enumerate(self.specs)
            if spec.category == "shared_region"
        )
        spec = self.specs[slot]
        start = self.functions[slot]["addr"]
        states = []
        for sample, offset in enumerate((0, 16)):
            state = _make_state(spec, slot, sample, random.Random(sample))
            state.slot_address = start + offset
            state.regs[0:4] = [10, 20, 30, 40]
            state.regs[13] = DATA_BASE + 160
            state.regs[14] = RETURN_ADDRESS
            states.append(state)
        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual in zip(states, native):
                expected = self._run_unicorn(uc, returned, state)
                label = f"{state.spec.name} entry=0x{state.slot_address:08x}"
                self.assertEqual(actual.regs, expected.regs, label)
                self.assertEqual(actual.flags, expected.flags, label)
                self.assertEqual(actual.memory, expected.memory, label)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_arm_pc_branch_table_cases_wrap_and_condition_boundaries(self):
        slot = next(i for i, spec in enumerate(self.specs) if spec.category == "computed_jump")
        spec = self.specs[slot]
        inputs = (
            (0, 0, 0), (1, 0, 1), (2, 0, 2), (3, 0, 3), (4, 0, 0xFF),
            (0x40000000, 1, 0), (0x40000001, 1, 1), (0xFFFFFFFF, 0, 0xFF),
        )
        expected_values = tuple(expected for _, _, expected in inputs)
        states = []
        for sample, (index, skip_bound, _) in enumerate(inputs):
            state = _make_state(spec, slot, sample, random.Random(sample))
            state.slot_address = self.functions[slot]["addr"]
            state.regs[0] = index
            state.regs[2] = skip_bound
            states.append(state)

        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual, expected_value in zip(states, native, expected_values):
                expected = self._run_unicorn(uc, returned, state)
                self.assertEqual(actual.regs, expected.regs)
                self.assertEqual(actual.regs[1], expected_value)
                self.assertEqual(actual.flags, expected.flags)
                self.assertEqual(actual.memory, expected.memory)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()


    def test_guarded_pc_branch_table_after_rebasing_subs_matches_unicorn(self):
        slot = next(i for i, spec in enumerate(self.specs) if spec.name == "arm_pc_branch_table_guarded")
        spec = self.specs[slot]
        # Synthetic r5 values 6..9 reach slots 0..3; larger values fail the guard
        # and values below 6 leave addpl untaken.
        inputs = ((6, 0), (7, 1), (8, 2), (9, 3), (10, 0xFF), (5, 0xFF), (0, 0xFF),
                  (0xFFFFFFFF, 0xFF), (0x7FFFFFFF, 0xFF))
        states = []
        for sample, (value, _) in enumerate(inputs):
            state = _make_state(spec, slot, sample, random.Random(sample))
            state.slot_address = self.functions[slot]["addr"]
            state.regs[5] = value
            states.append(state)
        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual, (_, expected_value) in zip(states, native, inputs):
                expected = self._run_unicorn(uc, returned, state)
                self.assertEqual(actual.regs, expected.regs)
                self.assertEqual(actual.regs[1], expected_value)
                self.assertEqual(actual.flags, expected.flags)
                self.assertEqual(actual.memory, expected.memory)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()
    def test_cross_function_branches_enter_secondary_entries(self):
        slots = {
            spec.name: index
            for index, spec in enumerate(self.specs)
            if spec.category == "shared_branch"
        }
        cases = []
        for name, expected_r0 in (
            ("arm_shared_branch_a", 2), ("arm_shared_branch_b", 1),
            ("thumb_shared_branch_a", 2), ("thumb_shared_branch_b", 1),
        ):
            slot = slots[name]
            spec = self.specs[slot]
            return_address = RETURN_ADDRESS | int(spec.thumb)
            state = TestState(
                slot, spec, 0, [0] * 16, (1, 0, 0, 1, 1),
                bytes(MEMORY_SIZE), self.functions[slot]["addr"],
            )
            state.regs[14] = return_address
            state.regs[15] = return_address
            cases.append((state, expected_r0, return_address))

        native = self._native_batch([state for state, _, _ in cases])
        uc, returned = self._new_unicorn()
        try:
            for (state, expected_r0, return_address), actual in zip(cases, native):
                expected = self._run_unicorn(uc, returned, state)
                self.assertEqual(actual.regs, expected.regs)
                self.assertEqual(actual.flags, expected.flags)
                self.assertEqual(actual.memory, expected.memory)
                self.assertEqual(actual.regs[0], expected_r0)
                self.assertEqual(actual.transfer_target, return_address)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_pc_relative_literal_register_and_pc_loads_read_live_ram(self):
        slot = next(i for i, spec in enumerate(self.specs) if spec.category == "arm_live_literal")
        spec = self.specs[slot]
        targets = (RETURN_ADDRESS, RETURN_ADDRESS + 4)
        states = []
        for sample, target in enumerate(targets):
            state = _make_state(spec, slot, sample, random.Random(sample))
            state.slot_address = self.functions[slot]["addr"]
            memory = bytearray(MEMORY_SIZE)
            _put_u32(memory, 0, target)
            state.memory = bytes(memory)
            states.append(state)

        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual, target in zip(states, native, targets):
                expected = self._run_unicorn(uc, returned, state)
                self.assertEqual(actual.regs, expected.regs)
                self.assertEqual(actual.regs[0], target)
                self.assertEqual(actual.transfer_target, target)
                self.assertEqual(actual.memory, expected.memory)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

    def test_thumb_pc_relative_literal_load_reads_live_ram(self):
        slot = next(i for i, spec in enumerate(self.specs) if spec.category == "thumb_live_literal")
        spec = self.specs[slot]
        values = (0x12345678, 0x87654321)
        states = []
        for sample, value in enumerate(values):
            state = _make_state(spec, slot, sample, random.Random(sample))
            state.slot_address = self.functions[slot]["addr"]
            memory = bytearray(MEMORY_SIZE)
            _put_u32(memory, 0, value)
            state.memory = bytes(memory)
            states.append(state)

        native = self._native_batch(states)
        uc, returned = self._new_unicorn()
        try:
            for state, actual, value in zip(states, native, values):
                expected = self._run_unicorn(uc, returned, state)
                self.assertEqual(actual.regs, expected.regs)
                self.assertEqual(actual.regs[0], value)
                self.assertEqual(actual.memory, expected.memory)
        finally:
            close = getattr(uc, "close", None)
            if close is not None:
                close()

if __name__ == "__main__":
    unittest.main()
