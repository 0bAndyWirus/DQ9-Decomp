import random
import unittest

from tools.check_coverage import CoverageError, _function_index, _owners, check_coverage, format_report, parse_pcs


class CheckCoverageTests(unittest.TestCase):
    def test_matches_function_ranges_and_instruction_modes(self):
        manifest = {
            "cpu": "arm7",
            "modules": [
                {
                    "name": "synthetic",
                    "module_id": -4,
                    "base": 0x1000,
                    "functions": [
                        {"addr": 0x1000, "size": 8, "thumb": False},
                        {"addr": 0x1010, "size": 8, "thumb": True},
                    ],
                }
            ],
        }
        pcs = parse_pcs(["1000 a", "1004 a", "1008 a", "1010 t", "1014 a", "1020 t"])

        result = check_coverage(manifest, pcs)

        self.assertEqual(result["rows"], 6)
        self.assertEqual(result["covered"], 3)
        self.assertEqual(result["uncovered"], [(0x1008, False), (0x1020, True)])
        self.assertEqual(result["mode_mismatches"][0]["addr"], 0x1014)
        report = format_report(result)
        self.assertIn("outside functions: 2; mode mismatches: 1", report)
        self.assertIn("0x00001008..0x00001008 (ARM, 1 PCs)", report)

    def test_deduplicates_executed_rows_and_rejects_malformed_modes(self):
        manifest = {
            "cpu": "arm7",
            "modules": [
                {"name": "synthetic", "module_id": -4, "base": 0x2000, "functions": [
                    {"addr": 0x2000, "size": 4, "thumb": False}
                ]}
            ],
        }
        pcs = parse_pcs(["2000 a", "2000 a"])
        result = check_coverage(manifest, pcs)

        self.assertEqual(result["rows"], 2)
        self.assertEqual(result["unique_pcs"], 1)
        with self.assertRaises(CoverageError):
            parse_pcs(["2000 x"])

    def test_interval_lookup_matches_brute_force_for_random_overlapping_ranges(self):
        rng = random.Random(90124)
        modules = []
        for module_id in range(5):
            functions = [{"addr": rng.randint(-100, 1000), "size": rng.randint(1, 120),
                          "thumb": bool(rng.getrandbits(1))}
                         for _ in range(80)]
            modules.append({"name": f"module-{module_id}", "module_id": module_id,
                            "base": rng.randint(-200, 200), "functions": functions})
        index = _function_index({"modules": modules})
        addresses = list(range(-120, 1150, 7))
        addresses.extend(fn["addr"] + delta for module in modules for fn in module["functions"]
                         for delta in (0, fn["size"] - 1, fn["size"]))
        for address in addresses:
            expected = [entry for interval in index.values() for entry in interval.entries
                        if entry[0] <= address < entry[1]]
            self.assertEqual(_owners(index, address), expected, f"address {address:#x}")


if __name__ == "__main__":
    unittest.main()
