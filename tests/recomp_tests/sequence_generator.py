"""Seeded ARMv5TE/Thumb-1 specimens for native/Unicorn differential execution.

No assembler or external image is needed. Reserved/unpredictable encodings (including
Thumb high-register SUB, which does not exist in Thumb-1) are not generated.
Seed, specimen name and sample identify every failure reproducibly.
"""
from __future__ import annotations

import random

SEQUENCE_SEED = 0xA946_7E57


def make_specs(seed: int = SEQUENCE_SEED, count: int = 192):
    # Imported on invocation, after the harness has defined its encoders.
    from tests.recomp_tests import test_differential as h

    rng = random.Random(seed)
    specs = []

    def arm(name, words, category="stress_sequence", **kwargs):
        specs.append(h.FunctionSpec("stress_arm_" + name, category, False,
                                   b"".join(h._arm_word(w) for w in words), **kwargs))

    def thumb(name, words, category="stress_sequence", **kwargs):
        specs.append(h.FunctionSpec("stress_thumb_" + name, category, True,
                                   b"".join(h._thumb_halfword(w) for w in words), **kwargs))

    # The complete opcode/shifter/S-bit cross product, plus register shifts.
    for opcode in range(16):
        for kind in range(4):
            for flags in (False, True):
                if opcode in (8, 9, 10, 11) and not flags:
                    continue
                amount = rng.choice((0, 1, 7, 16, 31))
                op2 = (amount << 7) | (kind << 5) | 3
                arm(f"dp_{opcode}_{kind}_{int(flags)}", [h._arm_dp(
                    opcode, op2, rn=0 if opcode in (13, 15) else 2,
                    set_flags=flags)])
            op2 = (4 << 8) | (kind << 5) | 0x10 | 3
            arm(f"regshift_{opcode}_{kind}", [h._arm_dp(
                opcode, op2, rn=0 if opcode in (13, 15) else 2)],
                shift_reg=4, shift_values=(0, 1, 31, 32, 33, 63, 64, 255, 256, 257))

    # Sequential flag dependencies and all architectural conditions, including AL.
    for condition in range(15):
        arm(f"condition_{condition}", [
            h._arm_dp(4, 3, rn=2, condition=condition),
            h._arm_dp(5, 1, rn=0, condition=(condition + 1) % 15),
            h._arm_dp(8, 0x81, rn=0, immediate=True),
            h._arm_dp(7, 3, rn=0, condition=condition)])
    for index in range(count):
        words = []
        for _ in range(rng.randint(2, 5)):
            if rng.randrange(4) == 0:
                family = rng.randrange(5)
                if family == 0:
                    word = h._arm_mul(0, 1, 2, accumulate=rng.choice((None, 3)),
                                      set_flags=bool(rng.getrandbits(1)))
                elif family == 1:
                    word = h._arm_long_mul(0, 1, 2, 3, signed=bool(rng.getrandbits(1)),
                        accumulate=bool(rng.getrandbits(1)), set_flags=bool(rng.getrandbits(1)))
                elif family == 2:
                    multiply = rng.choice(("smulxy", "smlaxy", "smulwy", "smlawy", "smlalxy"))
                    word = h._arm_half_mul(multiply, 0, 2, 3,
                        x_high=bool(rng.getrandbits(1)), y_high=bool(rng.getrandbits(1)),
                        accumulate=4, rd_hi=1)
                elif family == 3:
                    word = h._arm_qop(rng.randrange(4))
                else:
                    word = h._arm_clz()
                words.append((word & 0x0FFFFFFF) | (rng.randrange(15) << 28))
                continue
            opcode = rng.randrange(16)
            rd, rn, rm = (rng.randrange(6) for _ in range(3))
            flags = opcode in (8, 9, 10, 11) or bool(rng.getrandbits(1))
            form = rng.randrange(3)
            if form == 0:
                op2 = rng.randrange(4096)
            elif form == 1:
                op2 = (rng.randrange(32) << 7) | (rng.randrange(4) << 5) | rm
            else:
                op2 = (rng.randrange(6) << 8) | (rng.randrange(4) << 5) | 0x10 | rm
            words.append(h._arm_dp(opcode, op2, rd=0 if opcode in (8, 9, 10, 11) else rd,
                                   rn=0 if opcode in (13, 15) else rn, set_flags=flags,
                                   condition=rng.randrange(15), immediate=form == 0))
        arm(f"random_{index:03d}", words)
        halfwords = []
        for _ in range(rng.randint(2, 5)):
            rd, rs = rng.randrange(6), rng.randrange(6)
            form = rng.randrange(4)
            if form == 0:
                halfwords.append(h._thumb_alu(rng.randrange(16), rd, rs))
            elif form == 1:
                halfwords.append(h._thumb_imm(rng.randrange(4, 8), rd, rng.randrange(256)))
            elif form == 2:
                halfwords.append(h._thumb_shift(rng.randrange(3), rd, rs, rng.randrange(32)))
            else:
                halfwords.append(h._thumb_add_sub(subtract=bool(rng.getrandbits(1)),
                    immediate=bool(rng.getrandbits(1)), rd=rd, rn=rs, value=rng.randrange(6)))
        thumb(f"random_{index:03d}", halfwords)

    # Word/byte transfers: both directions, indexing, writeback, and every shift.
    shifts = ((0, 2, (0, 1, 2)), (1, 16, (0, 0x10000, 0x20000)),
              (2, 16, (0, 0x10000, 0xffff0000)), (3, 8, (0, 0x100, 0x200)))
    for load in (False, True):
        for byte in (False, True):
            for pre, wb in ((True, False), (True, True), (False, False), (False, True)):
                for up in (False, True):
                    name = f"mem_{int(load)}_{int(byte)}_{int(pre)}_{int(wb)}_{int(up)}"
                    arm(name, [h._arm_single(load=load, byte=byte, pre=pre,
                        writeback=wb, up=up, offset=8)], "arm_memory")
                    for kind, amount, values in shifts:
                        arm(name + f"_shift{kind}", [h._arm_single(load=load, byte=byte,
                            pre=pre, writeback=wb, up=up, rm=7, shift_type=kind, shift=amount)],
                            "arm_memory", index_reg=7, index_values=values)
    # Choose compensating bases to keep RRX C=0 and C=1 effective addresses
    # in the mapped data page, including 32-bit address wraparound.
    for load in (False, True):
        arm(f"rrx_memory_{int(load)}", [h._arm_single(load=load, byte=True,
            rm=7, shift_type=3, shift=0)], "stress_rrx_memory", index_reg=7,
            index_values=(0, 2, 4, 8))
        for kind in (1, 2):
            arm(f"memory_shift32_{int(load)}_{kind}", [h._arm_single(
                load=load, byte=True, rm=7, shift_type=kind, shift=0)],
                "arm_memory", index_reg=7, index_values=(0, 0x7FFFFFFF, 0xFFFFFFFF))
    for kind in ("h", "sb", "sh"):
        for load in ((False, True) if kind == "h" else (True,)):
            # P=0,W=1 halfword transfers are ARMv6 T forms, not ARMv5TE.
            for pre, wb in ((True, False), (True, True), (False, False)):
                for up in (False, True):
                    for register in (False, True):
                        arm(f"half_{kind}_{int(load)}_{int(pre)}_{int(wb)}_{int(up)}_{int(register)}",
                            [h._arm_half(load=load, kind=kind, pre=pre, writeback=wb,
                                up=up, rm=7 if register else None, offset=4)],
                            "arm_memory", index_reg=7 if register else -1,
                            index_values=(0, 2, 4, 8) if register else ())
    for load in (False, True):
        for pre, wb in ((True, False), (True, True), (False, False)):
            for up in (False, True):
                for register in (False, True):
                    # LDRD/STRD use L=0 and SH=10/11 respectively.
                    word = h._arm_half(load=False, kind="sb" if load else "sh",
                        pre=pre, writeback=wb, up=up, rm=7 if register else None, offset=8)
                    arm(f"double_{int(load)}_{int(pre)}_{int(wb)}_{int(up)}_{int(register)}",
                        [word], "stress_aligned_memory", index_reg=7 if register else -1,
                        index_values=(0, 8, 16) if register else ())
    for byte in (False, True):
        arm(f"swp_{int(byte)}", [0xE1060091 | (int(byte) << 22)], "stress_aligned_memory")
    for index in range(32):
        words = []
        for _ in range(rng.randint(2, 5)):
            load = bool(rng.getrandbits(1))
            pre = bool(rng.getrandbits(1))
            wb = pre and bool(rng.getrandbits(1))
            up = bool(rng.getrandbits(1))
            form = rng.randrange(3)
            if form == 0:
                word = h._arm_single(load=load, byte=bool(rng.getrandbits(1)),
                    rd=rng.randrange(4), pre=pre, writeback=wb, up=up, offset=8)
            elif form == 1:
                kind = rng.choice(("h", "sb", "sh")) if load else "h"
                word = h._arm_half(load=load, kind=kind, rd=rng.randrange(4),
                    pre=pre, writeback=wb, up=up, offset=8)
            else:
                word = h._arm_half(load=False, kind="sb" if load else "sh",
                    pre=pre, writeback=wb, up=up, offset=8)
            words.append((word & 0x0FFFFFFF) | (rng.randrange(15) << 28))
        arm(f"memory_sequence_{index}", words, "stress_aligned_memory")

    for mode in ("IA", "IB", "DA", "DB"):
        for load in (False, True):
            for wb in (False, True):
                for user in (False, True):
                    # Include distinct user SP/LR banks. LDM^ with writeback
                    # is architecturally unpredictable and is excluded.
                    if load and wb and user:
                        continue
                    register_lists = ((0, 2, 4), (0, 2, 8), (8, 9, 10))
                    if user:
                        register_lists += ((0, 13, 14),)
                    for regs in register_lists:
                        if load and wb and 8 in regs:
                            continue  # ARMv5 LDM W with base in list: unpredictable.
                        word = h._arm_block(load=load, mode=mode, writeback=wb,
                                            registers=regs) | (int(user) << 22)
                        arm(f"block_{mode}_{int(load)}_{int(wb)}_{int(user)}_{regs[0]}_{regs[-1]}",
                            [word], "arm_block", base_reg=8, block_mode=mode, block_regs=regs)
    for load in (False, True):
        for base in (0, 6):
            regs = (0, 2, 6)
            thumb(f"block_base_{int(load)}_{base}", [0xC000 | (int(load) << 11) |
                (base << 8) | sum(1 << r for r in regs)], "thumb_block",
                base_reg=base, block_regs=regs)
    for opcode in (2, 3, 4, 7):  # LSL/LSR/ASR/ROR by register.
        thumb(f"regshift_{opcode}", [h._thumb_alu(opcode)],
              shift_reg=1, shift_values=(0, 1, 31, 32, 33, 64, 255, 256, 257))
    for mask in (1, 0x35, 0xFF):
        regs = tuple(reg for reg in range(8) if mask & (1 << reg))
        for link in (False, True):
            thumb(f"push_{mask}_{int(link)}", [0xB400 | (int(link) << 8) | mask],
                  "thumb_block", base_reg=13, block_regs=regs + ((14,) if link else ()))
            thumb(f"pop_{mask}_{int(link)}", [0xBC00 | (int(link) << 8) | mask],
                  "thumb_block", base_reg=13, block_regs=regs + ((15,) if link else ()),
                  pc_offset=4 * len(regs) if link else -1, terminal=link)

    for high in (False, True):
        for family in ("smulwy", "smlawy"):
            arm(f"{family}_{int(high)}", [h._arm_half_mul(family, 0, 1, 2,
                y_high=high, accumulate=3 if family == "smlawy" else None)])
    for opcode in range(4):
        arm(f"sticky_q_{opcode}", [h._arm_qop(opcode), h._arm_dp(4, 3, rn=0)])
    # PC reads use instruction-local pipeline bias, not the entry address.
    for role, word in (
        ("read", h._arm_dp(13, 15, rn=0, set_flags=False)),
        ("shift_imm", h._arm_dp(4, (2 << 7) | 15, rn=2)),
        ("shift_reg", h._arm_dp(4, (4 << 8) | 0x10 | 15, rn=2)),
        ("shift_alias", h._arm_dp(13, (4 << 8) | 0x10 | 15, rn=0)),
        ("rn_regshift", h._arm_dp(4, (4 << 8) | 0x10 | 3, rn=15))):
        arm("pc_" + role, [word], shift_reg=4, shift_values=(0, 1, 31, 32, 33, 255))
    arm("store_pc", [h._arm_single(load=False, rd=15)], "stress_aligned_memory")
    arm("stm_pc", [h._arm_block(load=False, mode="IA", registers=(0, 15))],
        "arm_block", base_reg=8, block_regs=(0, 15))
    for rd in (0, 8, 12):
        for rs in (1, 9, 15):
            thumb(f"hi_add_{rd}_{rs}", [0x4400 | ((rd & 8) << 4) | (rs << 3) | (rd & 7)])
            thumb(f"hi_mov_{rd}_{rs}", [0x4600 | ((rd & 8) << 4) | (rs << 3) | (rd & 7)])
            thumb(f"hi_cmp_{rd}_{rs}", [0x4500 | ((rd & 8) << 4) | (rs << 3) | (rd & 7)])
    for condition in range(14):
        thumb(f"branch_{condition}", [h._thumb_cond_branch(0, 6, condition),
                                      0x3001, 0xE000, 0x3801])
    for immediate in (0, 1, 31, 127, 255):
        thumb(f"adr_{immediate}", [0xA000 | immediate])
        thumb(f"add_sp_{immediate}", [0xA800 | immediate])
    for immediate in (0, 1, 127):
        for subtract in (False, True):
            thumb(f"adjust_sp_{int(subtract)}_{immediate}", [0xB000 |
                (int(subtract) << 7) | immediate])
    thumb("bl_pair", [0x4675, 0xF000, 0xF803, 0x46AE, h.THUMB_BX_LR,
                      0x46C0, 0x3001, h.THUMB_BX_LR], terminal=True)
    arm("bl_local", [0xE1A0500E, 0xEB000001, 0xE1A0E005, h.ARM_BX_LR,
                     0xE2800001, h.ARM_BX_LR], terminal=True)
    arm("blx_reg", [0xE1A0500E, 0xE12FFF33, 0xE1A0E005, h.ARM_BX_LR],
        "stress_exchange", terminal=True)
    thumb("blx_reg", [0x4675, 0x4798, 0x46AE, h.THUMB_BX_LR],
          "stress_exchange", terminal=True)
    arm("bx_reg", [0xE12FFF13], "stress_exchange", terminal=True)
    thumb("bx_reg", [0x4718], "stress_exchange", terminal=True)
    arm("blx_immediate", [0xE1A0500E, 0xFA000001, 0xE1A0E005, h.ARM_BX_LR],
        terminal=True)
    thumb("exchange_leaf", [0x3001])
    thumb("blx_immediate", [0x4675, 0xF000, 0xE800, 0x46AE, h.THUMB_BX_LR],
          terminal=True)
    arm("exchange_leaf", [0xE2800001])
    return specs
