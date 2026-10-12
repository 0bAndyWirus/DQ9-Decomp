from pathlib import Path
import struct
import unittest

from tools.small_function_candidates import (
    FunctionPattern,
    candidate_symbol,
    delinks_stanza,
    format_source_unit_path,
    recognize_function,
    render_candidate,
)

# Hand-constructed ARM ISA examples; field offsets and symbols are invented fixtures,
# not an ABI or instruction sequences taken from a title.
BX_LR = 0xE12FFF1E


def arm_words(*words: int) -> bytes:
    return b"".join(struct.pack("<I", word) for word in words)


class SmallFunctionCandidateTests(unittest.TestCase):
    def test_recognizes_and_generates_constant_return(self):
        pattern = recognize_function(arm_words(0xE3A0002A, BX_LR))
        self.assertEqual(pattern, FunctionPattern("constant_return", return_value=42))
        source = render_candidate(pattern, "candidate")
        self.assertIn("unsigned int candidate(void)", source)
        self.assertIn("return 0x0000002au;", source)

    def test_recognizes_mvn_constant_return(self):
        pattern = recognize_function(arm_words(0xE3E00000, BX_LR))
        self.assertEqual(pattern, FunctionPattern("constant_return", return_value=0xFFFFFFFF))

    def test_recognizes_word_and_byte_field_getters(self):
        word = recognize_function(arm_words(0xE590000C, BX_LR))
        self.assertEqual(word, FunctionPattern("field_getter", offset=12, field_type="unsigned int"))
        word_source = render_candidate(word, "word_getter")
        self.assertIn("unsigned char _padding[12];", word_source)
        self.assertIn("return object->field;", word_source)

        byte = recognize_function(arm_words(0xE5D00003, BX_LR))
        self.assertEqual(byte, FunctionPattern("field_getter", offset=3, field_type="unsigned char"))

    def test_recognizes_field_setters(self):
        word = recognize_function(arm_words(0xE5801010, BX_LR))
        self.assertEqual(word, FunctionPattern("field_setter", offset=16, field_type="unsigned int"))
        word_source = render_candidate(word, "word_setter")
        self.assertIn("object->field = value;", word_source)

        byte = recognize_function(arm_words(0xE5C01003, BX_LR))
        self.assertEqual(byte, FunctionPattern("field_setter", offset=3, field_type="unsigned char"))

    def test_recognizes_halfword_accessors_and_rejects_unaligned_access(self):
        getter = recognize_function(arm_words(0xE1D000B2, BX_LR))
        self.assertEqual(getter, FunctionPattern("field_getter", offset=2, field_type="unsigned short"))
        setter = recognize_function(arm_words(0xE1C010B2, BX_LR))
        self.assertEqual(setter, FunctionPattern("field_setter", offset=2, field_type="unsigned short"))
        self.assertIsNone(recognize_function(arm_words(0xE1D000B1, BX_LR)))

    def test_recognizes_argument_forwarding_thunk(self):
        pattern = recognize_function(arm_words(0xE1A00002, BX_LR))
        self.assertEqual(pattern, FunctionPattern("argument_forwarder", argument_index=2))
        source = render_candidate(pattern, "forwarder")
        self.assertIn("unsigned int arg0, unsigned int arg1, unsigned int arg2", source)
        self.assertIn("return arg2;", source)

    def test_recognizes_empty_return_thunk(self):
        pattern = recognize_function(arm_words(BX_LR))
        self.assertEqual(pattern, FunctionPattern("empty_thunk"))
        self.assertIn("void empty(void) {}", render_candidate(pattern, "empty"))

    def test_rejects_unknown_partial_oversized_and_misaligned_functions(self):
        self.assertIsNone(recognize_function(arm_words(0xEAFFFFFE, BX_LR)))
        self.assertIsNone(recognize_function(arm_words(0xE3A00001, BX_LR, BX_LR)))
        self.assertIsNone(recognize_function(b"\x00\x00\x00"))
        self.assertIsNone(recognize_function(arm_words(BX_LR) * 17))

    def test_generated_symbol_must_be_a_c_identifier(self):
        with self.assertRaises(ValueError):
            render_candidate(FunctionPattern("empty_thunk"), "not-a-symbol")

    def test_candidate_symbols_are_deterministic_and_object_specific(self):
        first_path = Path("synthetic/arm9/Sample-State.o")
        second_path = Path("synthetic/overlay/Sample-State.o")
        first = candidate_symbol(first_path, 3)
        self.assertEqual(first, candidate_symbol(first_path, 3))
        self.assertNotEqual(first, candidate_symbol(second_path, 3))
        self.assertTrue(first.startswith("candidate_Sample_State_"))

    def test_dsd_source_path_and_delinks_stanza_are_exact(self):
        symbol = "candidate_Sample_State_0123456789abcdef_0003"
        source_path = format_source_unit_path("src/SmallFunctions/{symbol}.cpp", symbol)
        self.assertEqual(source_path, f"src/SmallFunctions/{symbol}.cpp")
        self.assertEqual(
            delinks_stanza(source_path, 0x08000020, 8),
            chr(10).join((
                f"{source_path}:",
                "    complete",
                "    .text start:0x08000020 end:0x08000028",
                "",
            )),
        )

    def test_dsd_source_path_requires_symbol_template_and_relative_path(self):
        with self.assertRaises(ValueError):
            format_source_unit_path("src/candidate.cpp", "candidate")
        with self.assertRaises(ValueError):
            format_source_unit_path("src/../{symbol}.cpp", "candidate")


if __name__ == "__main__":
    unittest.main()
