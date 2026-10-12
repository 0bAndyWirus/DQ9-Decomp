import tempfile
import unittest
from pathlib import Path

from tools.force_active_lcf import gap_first_symbols, gap_text_active_symbols, insert_force_active, main

# Synthetic linker layout and symbols, unrelated to any title's module layout.
LCF = "MEMORY {\n    IMAGE : ORIGIN = 0x08000000 > build/image.bin\n}\n\nKEEP_SECTION {\n    .init\n}\n\nSECTIONS {\n    .text : {}\n}\n"


class InsertTests(unittest.TestCase):
    def test_block_goes_right_before_sections_and_keeps_everything_else(self):
        result = insert_force_active(LCF, ["func_a", "func_b"])
        self.assertEqual(result.count("FORCE_ACTIVE {"), 1)
        self.assertLess(result.index("FORCE_ACTIVE {"), result.index("SECTIONS {"))
        self.assertIn("FORCE_ACTIVE {\n    func_a,\n    func_b\n}\n\nSECTIONS {", result)
        self.assertEqual(result.replace(result[result.index("FORCE_ACTIVE {"):result.index("SECTIONS {")], ""), LCF)

    def test_long_lists_are_accepted_in_the_lcf(self):
        names = [f"synthetic_function_{index:08x}" for index in range(200)]
        result = insert_force_active(LCF, names)
        self.assertTrue(all(name in result for name in names))

    def test_no_names_leaves_the_lcf_untouched(self):
        self.assertEqual(insert_force_active(LCF, []), LCF)

    def test_bad_input_is_rejected(self):
        with self.assertRaises(ValueError):
            insert_force_active("MEMORY {}\n", ["a"])                     # no SECTIONS block
        with self.assertRaises(ValueError):
            insert_force_active("FORCE_ACTIVE {\n x\n}\n" + LCF, ["a"])   # would define the block twice
        with self.assertRaises(ValueError):
            insert_force_active(LCF, ["ok", "bad name; rm -rf"])          # not a symbol name


class CommandTests(unittest.TestCase):
    def test_writes_sorted_deduplicated_names_atomically_and_reports_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "in.lcf").write_text(LCF)
            (root / "names.txt").write_text("func_b\n\nfunc_a\nfunc_b\n")
            self.assertEqual(main(["x", str(root / "in.lcf"), str(root / "names.txt"), str(root / "out.lcf")]), 0)
            out = (root / "out.lcf").read_text()
            self.assertIn("    func_a,\n    func_b\n}", out)
            self.assertEqual(out.count("func_b"), 1)
            self.assertEqual(list(root.glob("*.tmp")), [])
            self.assertEqual(main(["x", str(root / "missing.lcf"), str(root / "names.txt"), str(root / "o2.lcf")]), 1)
            self.assertFalse((root / "o2.lcf").exists())
            self.assertEqual(main(["x"]), 2)


class GapTests(unittest.TestCase):
    FUNCTIONS = [("a", 0x100), ("b", 0x140), ("c", 0x181), ("d", 0x200), ("e", 0x240)]

    def test_each_gap_between_units_contributes_its_lowest_function(self):
        # units cover 0x100-0x140 (a) and 0x180-0x200 (c); gaps: 0x140-0x180 (b), 0x200-0x300 (d, e)
        self.assertEqual(gap_first_symbols((0x100, 0x300), [(0x100, 0x140), (0x180, 0x200)], self.FUNCTIONS),
                         {"b", "d"})

    def test_a_gap_without_symbols_and_a_fully_covered_text_add_nothing(self):
        self.assertEqual(gap_first_symbols((0x100, 0x300), [(0x100, 0x300)], self.FUNCTIONS), set())
        self.assertEqual(gap_first_symbols((0x100, 0x140), [], [("a", 0x100)]), {"a"})
        self.assertEqual(gap_first_symbols((0x100, 0x200), [(0x100, 0x1f0)], [("x", 0x1f4 + 0x100)]), set())

    def test_thumb_addresses_are_matched_on_the_even_address(self):
        self.assertEqual(gap_first_symbols((0x100, 0x200), [(0x100, 0x180)], [("t", 0x181), ("u", 0x1c1)]),
                         {"t"})

    def test_global_label_keeps_a_gap_without_function_symbols(self):
        anchor = "__dsd_gap_anchor_main_00000180"
        self.assertEqual(
            gap_first_symbols((0x100, 0x200), [(0x100, 0x180)], [(anchor, 0x180)]),
            {anchor},
        )

    def test_config_scan_includes_a_generated_label_for_a_symbol_less_remainder(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            delinks = root / "delinks.txt"
            delinks.write_text(
                "    .text start:0x1000 end:0x1100 kind:code align:4\n\n"
                "src/Recovered/main/unit.cpp:\n"
                "    complete\n"
                "    .text start:0x1040 end:0x1050\n",
                encoding="utf-8",
            )
            (root / "symbols.txt").write_text(
                "unit kind:function(arm,size=0x10) addr:0x1040\n"
                "__dsd_gap_anchor_main_00001050 kind:label(arm) addr:0x1050\n",
                encoding="utf-8",
            )
            self.assertEqual(
                gap_text_active_symbols(delinks),
                {"__dsd_gap_anchor_main_00001050"},
            )


if __name__ == "__main__":
    unittest.main()
