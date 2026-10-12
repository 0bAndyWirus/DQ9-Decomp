import tempfile
import unittest
from pathlib import Path

from tools.scan_upstream_eu_matches import USA_FLAGS, USA_VERSION, load_eu_modules, summarize_sources

class UpstreamEuScanTests(unittest.TestCase):
    def test_load_addresses_come_from_private_image_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build/eur/build"
            build.mkdir(parents=True)
            # Invented module bases and image contents.
            for stem, base in (("arm9", 0x1000), ("itcm", 0x2000), ("dtcm", 0x3000)):
                (build / f"{stem}.bin").write_bytes(b"synthetic")
                (build / f"{stem}.yaml").write_text(f"base_address: {base}\n")
            overlays = root / "extract/eur/arm9_overlays"
            overlays.mkdir(parents=True)
            (overlays / "overlays.yaml").write_text("overlays:\n  - id: 123\n    base_address: 16384\n")
            (build / "arm9_ov123.bin").write_bytes(b"synthetic overlay")
            modules = load_eu_modules(root)
            self.assertEqual([(module.name, module.base) for module in modules],
                             [("main", 0x1000), ("itcm", 0x2000), ("dtcm", 0x3000), ("ov123", 0x4000)])

    def test_usa_flags_use_expected_regional_define_and_mwcc_options(self):
        self.assertEqual(USA_VERSION, "usa")
        self.assertIn("-proc", USA_FLAGS)
        self.assertIn("arm946e", USA_FLAGS)

    def test_summary_aggregates_compile_failures_and_unique_matches(self):
        rows = [
            {
                "source": "one.cpp",
                "compile_returncode": 0,
                "summary": {
                    "all_functions_unique": True,
                    "function_count": 2,
                    "uniquely_matched_functions": 2,
                    "uniquely_matched_function_bytes": 24,
                },
            },
            {
                "source": "two.cpp",
                "compile_returncode": 1,
                "compile_error": "compiler failed",
            },
            {
                "source": "three.cpp",
                "compile_returncode": 0,
                "scan_error": "invalid ELF",
            },
        ]

        self.assertEqual(
            summarize_sources(rows),
            {
                "source_count": 3,
                "compiled": 2,
                "compile_failed": 1,
                "scan_failed": 1,
                "all_functions_unique_units": 1,
                "function_count": 2,
                "uniquely_matched_functions": 2,
                "uniquely_matched_function_bytes": 24,
            },
        )


if __name__ == "__main__":
    unittest.main()
