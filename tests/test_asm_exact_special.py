import unittest

from tools.asm_exact_gen import Unsupported, parse_functions
from tools.asm_exact_special import convert_special, generate, render_special

# Invented assembly fixtures exercise generic ARM syntax and inline-data conversion.


INLINE_DATA_ASM = """\
    arm_func_start target
 target: ; 0x08000000
    stmdb sp!, {r4, lr}
    ldr r4, .L_08000018
    bne .L_08000014
.L_08000010: ; synthetic inline table
    .byte 0xaa, 0xbb, 0xcc, 0xdd
.L_08000014:
    ldmia sp!, {r4, pc}
.L_08000018: .word synthetic_data
    arm_func_end target
"""


class SpecialConvertTests(unittest.TestCase):
    def test_preserves_interleaved_byte_and_word_data(self):
        func = parse_functions(INLINE_DATA_ASM)["target"]
        body, symbols = convert_special(func)
        self.assertEqual(
            [
                "stmdb sp!, {r4, lr}",
                "ldr r4, =synthetic_data",
                "bne L_08000014",
                "L_08000010:",
                "dcd 0xddccbbaa",
                "L_08000014:",
                "ldmia sp!, {r4, pc}",
            ],
            body,
        )
        self.assertEqual({"synthetic_data"}, symbols)

    def test_retains_explicit_stack_frame(self):
        body, _ = convert_special(parse_functions(INLINE_DATA_ASM)["target"])
        self.assertEqual("stmdb sp!, {r4, lr}", body[0])
        self.assertEqual("ldmia sp!, {r4, pc}", body[-1])

    def test_render_uses_nofralloc_without_synthesized_wrapper(self):
        func = parse_functions(INLINE_DATA_ASM)["target"]
        body, symbols = convert_special(func)
        source = render_special("target", func, body, symbols, {"synthetic_data": "data"})
        self.assertIn('extern "C" unsigned synthetic_data[];', source)
        self.assertIn('extern "C" asm void target(void)', source)
        self.assertIn("    nofralloc", source)
        self.assertIn("    dcd 0xddccbbaa", source)
        self.assertNotIn("dcb", source)
        self.assertNotIn("__attribute__((naked))", source)

    def test_fixed_offset_branch_is_rejected_with_assembler_reason(self):
        func = parse_functions(
            """\
            arm_func_start f
            f: ; 0x1
                eors r2, r0, r1
                bmi #0x40
                bx lr
            arm_func_end f
            """
        )["f"]
        with self.assertRaisesRegex(Unsupported, "fixed-offset branch.*DCD emits data"):
            convert_special(func)

    def test_branch_to_external_code_label_and_not_inline_data(self):
        external = parse_functions(
            """\
            arm_func_start f
            f: ; 0x1
                bne .L_00000010
                bx lr
            arm_func_end f
            """
        )["f"]
        body, symbols = convert_special(external)
        self.assertEqual(["bne L_00000010", "bx lr"], body)
        self.assertEqual({"L_00000010"}, symbols)
        source = render_special("f", external, body, symbols, {})
        self.assertIn('extern "C" void L_00000010(void);', source)

        data_target = parse_functions(
            """\
            arm_func_start f
            f: ; 0x1
                bne .L_00000004
            .L_00000004: .byte 0x00, 0x00, 0x00, 0x00
                bx lr
            arm_func_end f
            """
        )["f"]
        with self.assertRaisesRegex(Unsupported, "branch to non-code label"):
            convert_special(data_target)

    def test_unsupported_directive_does_not_get_silently_dropped(self):
        func = parse_functions(
            """\
            arm_func_start f
            f: ; 0x1
                .space 4
                bx lr
            arm_func_end f
            """
        )["f"]
        with self.assertRaisesRegex(Unsupported, "unsupported directive.*space"):
            convert_special(func)

    def test_generate_selects_the_named_function(self):
        source = generate(INLINE_DATA_ASM, "target", {})
        self.assertIn("dcd 0xddccbbaa", source)
        with self.assertRaisesRegex(Unsupported, "not in assembly snapshot"):
            generate(INLINE_DATA_ASM, "missing", {})


if __name__ == "__main__":
    unittest.main()
