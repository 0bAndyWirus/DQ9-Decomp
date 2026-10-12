"""Symbol-diff checks using invented names and synthetic instruction records."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import symdiff
from tools.symdiff import main


def _write_project(directory: Path, objdiff: dict, report: dict) -> Path:
    project = directory / "project"
    project.mkdir()
    (project / "objdiff.json").write_text(json.dumps(objdiff), encoding="utf-8")
    build_eur = project / "build" / "eur"
    build_eur.mkdir(parents=True)
    (build_eur / "report.json").write_text(json.dumps(report), encoding="utf-8")
    return project


def _make_instruction(
    address: int | None,
    formatted: str,
    diff_kind: str | None = None,
    relocation: dict | None = None,
) -> dict:
    ins: dict = {
        "address": address,
        "formatted": formatted,
        "mnemonic": formatted.split()[0] if formatted else "nop",
    }
    row: dict = {"instruction": ins, "arg_diff": []}
    if diff_kind:
        row["diff_kind"] = diff_kind
    if relocation:
        ins["relocation"] = relocation
    return row


class SymDiffTests(unittest.TestCase):
    def _run(self, project: Path, args: list[str], diff_json: dict | None = None):
        full_args = ["--project", str(project)] + args
        buf = io.StringIO()
        if diff_json is not None:
            with patch("tools.symdiff._run_process") as mock_run, contextlib.redirect_stdout(buf):
                mock_run.return_value = (0, json.dumps(diff_json), "")
                rc = main(full_args)
            return rc, buf.getvalue()
        with contextlib.redirect_stdout(buf):
            rc = main(full_args)
        return rc, buf.getvalue()

    def _make_exact_diff(self) -> dict:
        ins = _make_instruction(0, "mov r0, #0")
        return {
            "left": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [{"symbol": {"name": "func"}, "instructions": [ins]}],
                    }
                ]
            },
            "right": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [{"symbol": {"name": "func"}, "instructions": [ins]}],
                    }
                ]
            },
        }

    def test_exact_match_returns_zero_and_header(self):
        objdiff = {
            "units": [
                {
                    "name": "src/Exact",
                    "target_path": "t.o",
                    "base_path": "b.o",
                }
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Exact",
                    "measures": {"fuzzy_match_percent": 100.0},
                    "functions": [{"name": "func", "fuzzy_match_percent": 100.0}],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Exact"], self._make_exact_diff())
            self.assertEqual(rc, 0)
            self.assertIn("src/Exact", out)
            self.assertIn("func", out)
            self.assertIn("100.0%", out)
            self.assertIn("instructions: 1 / 1", out)
            self.assertNotIn("[", out)

    def test_mismatch_arg_shows_kind_and_relocation(self):
        objdiff = {
            "units": [
                {
                    "name": "src/Mismatch",
                    "target_path": "t.o",
                    "base_path": "b.o",
                }
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Mismatch",
                    "measures": {"fuzzy_match_percent": 50.0},
                    "functions": [{"name": "func", "fuzzy_match_percent": 50.0}],
                }
            ]
        }
        left_reloc = {"target": {"symbol": {"name": "left_sym"}, "addend": "4"}}
        right_reloc = {"target": {"symbol": {"name": "right_sym"}, "addend": "0"}}
        diff = {
            "left": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [
                            {
                                "symbol": {"name": "func"},
                                "instructions": [
                                    _make_instruction(0, "mov r0, #0"),
                                    _make_instruction(
                                        4,
                                        ".word #0x0",
                                        "DIFF_ARG_MISMATCH",
                                        left_reloc,
                                    ),
                                    _make_instruction(8, "bx lr"),
                                ],
                            }
                        ],
                    }
                ]
            },
            "right": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [
                            {
                                "symbol": {"name": "func"},
                                "instructions": [
                                    _make_instruction(0, "mov r0, #0"),
                                    _make_instruction(
                                        4,
                                        ".word #0x4",
                                        "DIFF_ARG_MISMATCH",
                                        right_reloc,
                                    ),
                                    _make_instruction(8, "bx lr"),
                                ],
                            }
                        ],
                    }
                ]
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Mismatch"], diff)
            self.assertEqual(rc, 1)
            self.assertIn("[DIFF_ARG_MISMATCH]", out)
            self.assertIn("left_sym+4 -> right_sym", out)
            self.assertIn("50.0%", out)

    def test_missing_base_gap_returns_mismatch(self):
        objdiff = {
            "units": [
                {"name": "_dsd_gap@main_1", "target_path": "t.o", "base_path": None}
            ]
        }
        report = {
            "units": [
                {
                    "name": "_dsd_gap@main_1",
                    "measures": {},
                    "functions": [{"name": "__sinit_02000000"}],
                }
            ]
        }
        diff = {
            "left": {
                "sections": [
                    {
                        "name": ".init",
                        "symbols": [
                            {
                                "symbol": {"name": "__sinit_02000000"},
                                "instructions": [
                                    _make_instruction(0, "stmdb sp!, {r3, lr}"),
                                    _make_instruction(4, "bl #0x0"),
                                ],
                            }
                        ],
                    }
                ]
            }
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["_dsd_gap@main_1"], diff)
            self.assertEqual(rc, 1)
            self.assertIn("instructions: 2 / 0", out)
            self.assertIn("<missing>", out)
            self.assertIn("[MISSING_RIGHT]", out)

    def test_missing_unit_returns_two(self):
        objdiff = {"units": []}
        report = {"units": []}
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Missing"])
            self.assertEqual(rc, 2)
            self.assertEqual(out, "")

    def test_missing_symbol_in_diff_returns_two(self):
        objdiff = {
            "units": [
                {"name": "src/Unit", "target_path": "t.o", "base_path": "b.o"}
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Unit",
                    "measures": {},
                    "functions": [{"name": "wanted"}],
                }
            ]
        }
        diff = {
            "left": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [
                            {
                                "symbol": {"name": "other"},
                                "instructions": [_make_instruction(0, "nop")],
                            }
                        ],
                    }
                ]
            },
            "right": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [
                            {
                                "symbol": {"name": "other"},
                                "instructions": [_make_instruction(0, "nop")],
                            }
                        ],
                    }
                ]
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Unit", "wanted"], diff)
            self.assertEqual(rc, 2)
            self.assertEqual(out, "")

    def test_ambiguous_symbol_without_explicit_symbol_returns_two(self):
        objdiff = {
            "units": [
                {"name": "src/Unit", "target_path": "t.o", "base_path": "b.o"}
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Unit",
                    "measures": {},
                    "functions": [{"name": "a"}, {"name": "b"}],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Unit"])
            self.assertEqual(rc, 2)
            self.assertEqual(out, "")

    def test_find_unit_by_symbol(self):
        objdiff = {
            "units": [
                {"name": "src/A", "target_path": "a.o", "base_path": "a_base.o"},
                {"name": "src/B", "target_path": "b.o", "base_path": "b_base.o"},
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/A",
                    "measures": {},
                    "functions": [{"name": "uniqueA"}],
                },
                {
                    "name": "src/B",
                    "measures": {},
                    "functions": [{"name": "shared"}],
                },
            ]
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["--find-unit", "uniqueA"])
            self.assertEqual(rc, 0)
            self.assertEqual(out.strip(), "src/A")

    def test_find_unit_by_address(self):
        objdiff = {
            "units": [
                {"name": "src/Synthetic/synthetic_helper", "target_path": "t.o", "base_path": "b.o"}
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Synthetic/synthetic_helper",
                    "measures": {},
                    "functions": [{"name": "synthetic_helper_08000000"}],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["--find-unit", "0x08000000"])
            self.assertEqual(rc, 0)
            self.assertEqual(out.strip(), "src/Synthetic/synthetic_helper")

    def test_context_window_separates_far_mismatches(self):
        objdiff = {
            "units": [
                {"name": "src/Context", "target_path": "t.o", "base_path": "b.o"}
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Context",
                    "measures": {},
                    "functions": [{"name": "func"}],
                }
            ]
        }
        left = [_make_instruction(i * 4, f"ins{i}") for i in range(20)]
        right = [_make_instruction(i * 4, f"ins{i}") for i in range(20)]
        left[2] = _make_instruction(8, "changed", "DIFF_REPLACED")
        right[2] = _make_instruction(8, "mutated", "DIFF_REPLACED")
        left[18] = _make_instruction(72, "changed2", "DIFF_REPLACED")
        right[18] = _make_instruction(72, "mutated2", "DIFF_REPLACED")
        diff = {
            "left": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [{"symbol": {"name": "func"}, "instructions": left}],
                    }
                ]
            },
            "right": {
                "sections": [
                    {
                        "name": ".text",
                        "symbols": [{"symbol": {"name": "func"}, "instructions": right}],
                    }
                ]
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Context", "--context", "1"], diff)
            self.assertEqual(rc, 1)
            self.assertIn("changed", out)
            self.assertIn("changed2", out)
            self.assertIn("...", out)
            self.assertNotIn("ins10", out)

    def test_json_exact_match(self):
        objdiff = {
            "units": [
                {"name": "src/Json", "target_path": "t.o", "base_path": "b.o"}
            ]
        }
        report = {
            "units": [
                {
                    "name": "src/Json",
                    "measures": {"fuzzy_match_percent": 100.0},
                    "functions": [{"name": "func", "fuzzy_match_percent": 100.0}],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as directory:
            project = _write_project(Path(directory), objdiff, report)
            rc, out = self._run(project, ["src/Json", "--json"], self._make_exact_diff())
            self.assertEqual(rc, 0)
            data = json.loads(out)
            self.assertTrue(data["exact"])
            self.assertEqual(data["left_count"], 1)
            self.assertEqual(data["right_count"], 1)
            self.assertEqual(data["mismatches"], [])



    def test_objdiff_output_is_capped(self):
        with self.assertRaisesRegex(RuntimeError, "output exceeded"):
            symdiff._run_process([sys.executable, "-c", "print('x' * 100000)"], 5, 128)

    def test_objdiff_has_a_timeout(self):
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            symdiff._run_process([sys.executable, "-c", "import time; time.sleep(5)"], 0.05, 1024)
if __name__ == "__main__":
    unittest.main()
