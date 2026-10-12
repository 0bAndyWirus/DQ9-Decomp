"""Local helper selection and verification-command contract checks."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools import asm_exact_run


class HelperCommandTests(unittest.TestCase):
    def test_explicit_main_helper_contract(self):
        with tempfile.TemporaryDirectory() as temp:
            helper = Path(temp) / "main_verify.py"
            helper.touch()
            with patch.dict(os.environ, {"DQ9_HELPER_MAIN": "/missing/ignored.py"}):
                command, env = asm_exact_run.helper_command(
                    "SampleAuthor", "main", "invented", helper_main=helper)
            self.assertEqual(command, [sys.executable, str(helper.resolve()), "verify", "invented"])
            self.assertEqual(env["DQ_AUTHOR"], "SampleAuthor")

    def test_overlay_and_itcm_helper_contract(self):
        with tempfile.TemporaryDirectory() as temp:
            helper = Path(temp) / "overlay_verify.py"
            helper.touch()
            for module in ("ov001", "itcm"):
                with self.subTest(module=module):
                    command, env = asm_exact_run.helper_command(
                        "SampleAuthor", module, "invented", helper_overlay=helper)
                    self.assertEqual(command, [sys.executable, str(helper.resolve()),
                                               "verify", "invented", "--write"])
                    self.assertEqual(env["MEDB_AUTHOR"], "SampleAuthor")

    def test_environment_supplies_helper_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            helper = Path(temp) / "verify.py"
            helper.touch()
            with patch.dict(os.environ, {"DQ9_HELPER_MAIN": str(helper),
                                         "DQ9_HELPER_OVERLAY": str(helper)}):
                for module in ("main", "ov001"):
                    command, _ = asm_exact_run.helper_command("SampleAuthor", module, "invented")
                    self.assertEqual(command[1], str(helper.resolve()))

    def test_missing_helpers_have_actionable_errors(self):
        with patch.dict(os.environ, {}, clear=True):
            for module, kind in (("main", "main"), ("ov001", "overlay")):
                with self.subTest(module=module), self.assertRaisesRegex(
                        ValueError, f"--helper-{kind} or DQ9_HELPER_{kind.upper()}"):
                    asm_exact_run.helper_command("SampleAuthor", module, "invented")

    def test_helper_must_be_a_file(self):
        with tempfile.TemporaryDirectory() as temp:
            for path in (Path(temp), Path(temp) / "missing.py"):
                with self.subTest(path=path), self.assertRaisesRegex(ValueError, "helper is not a file"):
                    asm_exact_run.helper_command("SampleAuthor", "main", "invented", helper_main=path)

    def test_cli_rejects_missing_helper_before_writing_candidates(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            queue = root / "queue.json"
            queue.write_text(json.dumps({"queue": [
                {"address": 0x08000000, "module": "main", "function_name": "invented", "size": 4}
            ]}))
            with patch.dict(os.environ, {}, clear=True), \
                    patch.object(asm_exact_run, "EU", root / "workspace"), \
                    patch.object(asm_exact_run, "ASM_DIRS", []), \
                    patch.object(asm_exact_run, "load_skip_list", return_value={}), \
                    patch.object(asm_exact_run, "forbidden_function", return_value=False), \
                    patch("sys.stderr", new_callable=io.StringIO) as error, \
                    self.assertRaises(SystemExit) as raised:
                asm_exact_run.main(["--author", "SampleAuthor", "--queue", str(queue)])
            self.assertEqual(raised.exception.code, 2)
            self.assertIn("--helper-main", error.getvalue())
            self.assertFalse((root / "workspace").exists())


if __name__ == "__main__":
    unittest.main()
