"""Assembly conversion checks with invented ARM/Thumb source, not disassembly."""

import unittest

from tools.asm_exact_gen import Unsupported, convert, parse_functions, render

# Synthetic labels, addresses, registers and immediates are chosen for these tests.
ARM_SAMPLE = """\
    .text
    arm_func_start synthetic_entry
synthetic_entry: ; 0x08000000
    stmdb sp!, {r7, lr}
    ldr r7, .L_08000018
    cmp r0, #0x9
    beq .L_08000014
    bl synthetic_helper
.L_08000014:
    ldmia sp!, {r7, pc}
.L_08000018: .word synthetic_word
    arm_func_end synthetic_entry
"""


class ParseTests(unittest.TestCase):
    def test_splits_functions_and_modes(self):
        text = ARM_SAMPLE + "    thumb_func_start t\nt: ; 0x08000100\n    bx lr\n    thumb_func_end t\n"
        funcs = parse_functions(text)
        self.assertEqual(0x08000000, funcs["synthetic_entry"].address)
        self.assertEqual(0x08000100, funcs["t"].address)


class ConvertTests(unittest.TestCase):
    def setUp(self):
        self.func = parse_functions(ARM_SAMPLE)["synthetic_entry"]

    def test_naked_form_drops_frame_and_rewrites_pool_and_labels(self):
        body, symbols = convert(self.func)
        self.assertEqual(
            ["ldr r7, =synthetic_word", "cmp r0, #0x9", "beq L_08000014", "bl synthetic_helper", "L_08000014:"], body)
        self.assertEqual({"synthetic_word", "synthetic_helper"}, symbols)

    def test_raw_branch_immediate_uses_instruction_address_and_synthesizes_local_label(self):
        text = """\
    arm_func_start f
f: ; 0x08001000
    mov r2, #0x9
    beq #0x8
    mov r3, #0xb
    bx lr
    arm_func_end f
"""
        body, symbols = convert(parse_functions(text)["f"])
        self.assertEqual(
            ["mov r2, #0x9", "beq L_0800100c", "mov r3, #0xb", "L_0800100c:", "bx lr"], body)
        self.assertEqual(set(), symbols)

    def test_raw_external_branch_uses_symbol_at_instruction_plus_delta(self):
        text = """\
    arm_func_start f
f: ; 0x08002000
    bl #-0x100
    bx lr
    arm_func_end f
"""
        body, symbols = convert(
            parse_functions(text)["f"], branch_symbols={0x08001F00: "synthetic_external"})
        self.assertEqual(["bl synthetic_external", "bx lr"], body)
        self.assertEqual({"synthetic_external"}, symbols)

    def test_raw_external_branch_without_symbol_is_rejected(self):
        text = """\
    arm_func_start f
f: ; 0x08002000
    bl #-0x100
    bx lr
    arm_func_end f
"""
        with self.assertRaisesRegex(Unsupported, "raw branch to unknown target 0x08001f00"):
            convert(parse_functions(text)["f"])

    def test_unknown_local_label_can_resolve_to_external_symbol(self):
        text = """\
    arm_func_start f
f: ; 0x08001000
    b .L_08001008
    bx lr
    arm_func_end f
"""
        body, symbols = convert(parse_functions(text)["f"], branch_symbols={0x08001008: "synthetic_tail"})
        self.assertEqual(["b synthetic_tail", "bx lr"], body)
        self.assertEqual({"synthetic_tail"}, symbols)

    def test_nofralloc_form_keeps_every_instruction(self):
        body, _ = convert(self.func, strip_frame=False)
        self.assertEqual("stmdb sp!, {r7, lr}", body[0])
        self.assertEqual("ldmia sp!, {r7, pc}", body[-1])

    def test_jump_table_comments_are_stripped(self):
        text = """\
    arm_func_start f
f: ; 0x08002000
    addpl pc, pc, r0, lsl #0x2
    b .L_a ; case 0
.L_a:
    bx lr
    arm_func_end f
"""
        body, _ = convert(parse_functions(text)["f"])
        self.assertEqual(["addpl pc, pc, r0, lsl #0x2", "b L_a", "L_a:", "bx lr"], body)

    def test_unreferenced_pool_word_is_rejected(self):
        text = """\
    arm_func_start f
f: ; 0x08002000
    mov r0, #0x0
    bx lr
.L_x: .word 0x55
    arm_func_end f
"""
        with self.assertRaises(Unsupported):
            convert(parse_functions(text)["f"])

    def test_thumb_uses_nonflag_spelling_and_needs_nofralloc(self):
        text = """\
    thumb_func_start t
t: ; 0x08002000
    subs r3, r2, #0x2
    bls .L_b
.L_b:
    bx lr
    thumb_func_end t
"""
        func = parse_functions(text)["t"]
        with self.assertRaises(Unsupported):
            convert(func)
        body, _ = convert(func, strip_frame=False)
        self.assertEqual(["sub r3, r2, #0x2", "bls L_b", "L_b:", "bx lr"], body)

    def test_thumb_conditional_bls_uses_two_byte_pc_advance(self):
        text = """\
    thumb_func_start t
t: ; 0x08001000
    bls #0x4
    mov r0, r0
    bx lr
    thumb_func_end t
"""
        body, _ = convert(parse_functions(text)["t"], strip_frame=False)
        self.assertEqual(["bls L_08001004", "mov r0, r0", "L_08001004:", "bx lr"], body)

    def test_thumb_stack_adjust_uses_two_operand_spelling(self):
        text = """\
    thumb_func_start t
t: ; 0x08001000
    sub sp, sp, #0x20
    bx lr
    thumb_func_end t
"""
        body, _ = convert(parse_functions(text)["t"], strip_frame=False)
        self.assertEqual(["sub sp, #0x20", "bx lr"], body)


class RenderTests(unittest.TestCase):
    def test_nofralloc_thumb_source_has_pragma_and_asm_function(self):
        source = render("t", ["bx lr"], set(), {}, nofralloc=True, thumb=True)
        self.assertTrue(source.startswith("#pragma thumb on"))
        self.assertIn('extern "C" asm void t(void)', source)
        self.assertIn("nofralloc", source)


if __name__ == "__main__":
    unittest.main()
