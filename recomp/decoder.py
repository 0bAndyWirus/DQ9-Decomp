"""ARM/Thumb instruction decoding used by the recompiler.

Capstone is used for instruction boundaries, aliases and readable disassembly;
execution semantics are emitted by :mod:`recomp.emitter` and the runtime helpers.
"""

from __future__ import annotations

from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any

try:
    from capstone import (  # type: ignore[import-not-found]
        CS_ARCH_ARM,
        CS_MODE_ARM,
        CS_MODE_LITTLE_ENDIAN,
        CS_MODE_THUMB,
        Cs,
    )
except ImportError as exc:  # pragma: no cover - environment diagnostic
    raise RuntimeError("recomp requires the Python 'capstone' package") from exc

from capstone.arm_const import (  # type: ignore[import-not-found]
    ARM_CC_AL,
    ARM_CC_INVALID,
    ARM_OP_IMM,
    ARM_OP_MEM,
    ARM_OP_REG,
    ARM_SFT_ASR,
    ARM_SFT_ASR_REG,
    ARM_SFT_LSL,
    ARM_SFT_LSL_REG,
    ARM_SFT_LSR,
    ARM_SFT_LSR_REG,
    ARM_SFT_ROR,
    ARM_SFT_ROR_REG,
    ARM_SFT_RRX,
)


_CONDITION_SUFFIXES = {
    0: ("eq",),
    1: ("ne",),
    2: ("hs", "cs"),
    3: ("lo", "cc"),
    4: ("mi",),
    5: ("pl",),
    6: ("vs",),
    7: ("vc",),
    8: ("hi",),
    9: ("ls",),
    10: ("ge",),
    11: ("lt",),
    12: ("gt",),
    13: ("le",),
    15: ("nv",),
}


def _canonical_mnemonic(mnemonic: str, condition_code: int) -> str:
    condition = condition_code - 1
    for suffix in _CONDITION_SUFFIXES.get(condition, ()):
        for ending in (suffix, "." + suffix):
            if mnemonic.endswith(ending) and len(mnemonic) > len(ending):
                return mnemonic[: -len(ending)]
    return mnemonic


@dataclass(frozen=True)
class DecodedInstruction:
    """One decoded instruction with the Capstone detail object retained."""

    address: int
    size: int
    raw: int
    mnemonic: str
    op_str: str
    text: str
    cc: int
    update_flags: bool
    writeback: bool
    thumb: bool
    detail: Any

    @property
    def next_address(self) -> int:
        return self.address + self.size

    @property
    def condition(self) -> int:
        """Return the architectural four-bit condition field (AL=14)."""
        if self.cc in (ARM_CC_INVALID, ARM_CC_AL):
            return 14
        return self.cc - 1

    @property
    def operands(self) -> list[Any]:
        return self.detail.operands


class Decoder:
    """Incremental Capstone decoder for one immutable guest module image."""

    def __init__(self, image: bytes, base: int, arch: str, thumb: bool):
        self.image = image
        self.base = base & 0xFFFFFFFF
        self.arch = arch
        self.thumb = bool(thumb)
        mode = CS_MODE_THUMB if self.thumb else CS_MODE_ARM
        self._cs = Cs(CS_ARCH_ARM, mode | CS_MODE_LITTLE_ENDIAN)
        self._cs.detail = True

    def decode(self, address: int) -> DecodedInstruction | None:
        """Decode one instruction at *address*, or return None at image bounds."""
        offset = address - self.base
        if offset < 0 or offset >= len(self.image):
            return None
        data = self.image[offset : offset + (4 if not self.thumb else 4)]
        if len(data) < (2 if self.thumb else 4):
            return None
        decoded = next(self._cs.disasm(data, address, count=1), None)
        if decoded is not None and self.thumb and decoded.size == 4:
            # ARMv4T/v5T Thumb has no 32-bit encodings except the BL/BLX
            # prefix+suffix pair; anything else is Thumb-2 (data, not code).
            first = int.from_bytes(data[:2], "little")
            second = int.from_bytes(data[2:4], "little")
            is_pair = (first >> 11) == 0b11110 and (second >> 11) in (0b11111, 0b11101)
            if not is_pair or (self.arch == "v4t" and (second >> 11) == 0b11101):
                decoded = None
        if decoded is None:
            size = 2 if self.thumb else 4
            raw = int.from_bytes(data[:size], "little")
            detail = SimpleNamespace(operands=[], reg_name=lambda _reg: "", regs_access=lambda: ([], []))
            return DecodedInstruction(
                address=address,
                size=size,
                raw=raw,
                mnemonic="udf",
                op_str="",
                text=f".word 0x{raw:0{size * 2}x}",
                cc=ARM_CC_AL,
                update_flags=False,
                writeback=False,
                thumb=self.thumb,
                detail=detail,
            )
        raw = int.from_bytes(data[: decoded.size], "little")
        return DecodedInstruction(
            address=address,
            size=decoded.size,
            raw=raw,
            mnemonic=_canonical_mnemonic(decoded.mnemonic.lower(), decoded.cc),
            op_str=decoded.op_str,
            text=f"{decoded.mnemonic} {decoded.op_str}".rstrip(),
            cc=decoded.cc,
            update_flags=bool(decoded.update_flags),
            writeback=bool(decoded.writeback),
            thumb=self.thumb,
            detail=decoded,
        )


def reg_index(insn: DecodedInstruction, reg_id: int) -> int:
    """Map Capstone ARM aliases (sb/sl/fp/ip/sp/lr/pc) to r0-r15."""
    name = insn.detail.reg_name(reg_id).lower()
    aliases = {
        "sb": 9,
        "sl": 10,
        "fp": 11,
        "ip": 12,
        "sp": 13,
        "lr": 14,
        "pc": 15,
    }
    if name in aliases:
        return aliases[name]
    if name.startswith("r") and name[1:].isdigit():
        value = int(name[1:])
        if 0 <= value <= 15:
            return value
    raise ValueError(f"unrecognized ARM register {name!r} at 0x{insn.address:08x}")


def reg_name(insn: DecodedInstruction, reg_id: int) -> str:
    return f"r{reg_index(insn, reg_id)}"


def operand_reg(insn: DecodedInstruction, operand: Any) -> int:
    if operand.type != ARM_OP_REG:
        raise ValueError(f"expected register operand at 0x{insn.address:08x}")
    return reg_index(insn, operand.reg)


def operand_immediate(operand: Any) -> int:
    if operand.type != ARM_OP_IMM:
        raise ValueError("expected immediate operand")
    return int(operand.imm) & 0xFFFFFFFF


def is_operand_register(operand: Any) -> bool:
    return operand.type == ARM_OP_REG


def is_operand_immediate(operand: Any) -> bool:
    return operand.type == ARM_OP_IMM


def is_operand_memory(operand: Any) -> bool:
    return operand.type == ARM_OP_MEM


def shift_kind(shift_type: int) -> tuple[int, bool, bool]:
    """Normalize Capstone's shift to (LSL/LSR/ASR/ROR, by-register, is-RRX)."""
    mapping = {
        ARM_SFT_LSL: (0, False, False),
        ARM_SFT_LSL_REG: (0, True, False),
        ARM_SFT_LSR: (1, False, False),
        ARM_SFT_LSR_REG: (1, True, False),
        ARM_SFT_ASR: (2, False, False),
        ARM_SFT_ASR_REG: (2, True, False),
        ARM_SFT_ROR: (3, False, False),
        ARM_SFT_ROR_REG: (3, True, False),
        ARM_SFT_RRX: (3, False, True),
    }
    return mapping.get(shift_type, (0, False, False))


def unsupported_for_arch(insn: DecodedInstruction, arch: str) -> bool:
    """Whether a decoded opcode is outside ARMv4T (but valid in ARMv5TE)."""
    if arch != "v4t":
        return False
    mnemonic = insn.mnemonic.split(".")[0]
    if mnemonic in {"blx", "bxj", "bkpt", "clz", "pld", "qadd", "qsub", "qdadd", "qdsub", "ldrd", "strd"}:
        return True
    # SMULL/SMLAL (signed long multiply) are ARMv4 (ARM7TDMI "M"); the
    # halfword/word DSP forms SMULxy/SMULWy/SMLAxy/SMLAWy/SMLALxy are v5TE.
    if mnemonic in {"smull", "smlal"}:
        return False
    if mnemonic.startswith(("smla", "smul")):
        return True
    return False
