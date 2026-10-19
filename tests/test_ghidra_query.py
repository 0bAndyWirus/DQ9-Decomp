import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from tools import ghidra_query


class GhidraQueryTests(unittest.TestCase):
    def setUp(self):
        # Invented module metadata in a synthetic non-hardware address range.
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        extract = Path(temp.name) / "extract"
        (extract / "arm9").mkdir(parents=True)
        (extract / "arm9" / "arm9.bin").write_bytes(b"\0" * 16)
        (extract / "arm9" / "arm9.yaml").write_text("base_address: 0x08000000\n", encoding="utf-8")
        # run() prepares a real PyGhidra environment from a local Ghidra install; these tests use
        # a mocked Popen, so keep them independent of whether Ghidra is installed on this machine.
        env_patcher = mock.patch.object(ghidra_query, "pyghidra_environment", return_value={})
        env_patcher.start()
        self.addCleanup(env_patcher.stop)
        for target in ("EXTRACT", "SYMBOLS_ROOT"):
            patcher = mock.patch.object(ghidra_query, target, extract if target == "EXTRACT" else Path(temp.name) / "symbols")
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_build_query_command_passes_json_and_disables_analysis(self):
        command = ghidra_query.build_command(
            "main", "disasm", ['{"addr":"0x08000000","length":16}']
        )
        self.assertIn("-process", command)
        self.assertIn("arm9.bin", command)
        self.assertIn("-noanalysis", command)
        self.assertEqual(command[-2:], ["disasm", '{"addr":"0x08000000","length":16}'])

    @mock.patch("tools.ghidra_query.subprocess.Popen")
    def test_run_parses_json_marker_and_measures_latency(self, popen):
        popen.return_value = mock.Mock(
            returncode=0,
            stdout=io.BytesIO(b'headless log\nGHIDRA_QUERY_JSON:{"items":[1]}\n'),
            wait=mock.Mock(return_value=0),
        )
        result, elapsed = ghidra_query.run("main", "funcs", ['{"limit":1}'])
        self.assertEqual(result["items"], [1])
        self.assertEqual(result["latency_seconds"], round(elapsed, 3))

    @mock.patch("tools.ghidra_query.subprocess.Popen")
    def test_run_reports_invalid_or_missing_json(self, popen):
        popen.return_value = mock.Mock(stdout=io.BytesIO(b"nothing"), wait=mock.Mock(return_value=0))
        with self.assertRaisesRegex(RuntimeError, "did not emit JSON"):
            ghidra_query.run("main", "funcs")
        popen.return_value = mock.Mock(stdout=io.BytesIO(b"GHIDRA_QUERY_JSON:not-json"), wait=mock.Mock(return_value=0))
        with self.assertRaisesRegex(RuntimeError, "invalid JSON"):
            ghidra_query.run("main", "funcs")

    @mock.patch("tools.ghidra_query.MAX_LOG_BYTES", 64)
    @mock.patch("tools.ghidra_query.subprocess.Popen")
    def test_capture_retains_bounded_tail_and_marker(self, popen):
        output = b"x" * 1000 + b'\nGHIDRA_QUERY_JSON:{"ok":true}\n'
        popen.return_value = mock.Mock(stdout=io.BytesIO(output), wait=mock.Mock(return_value=0))
        result, _ = ghidra_query.run("main", "funcs")
        self.assertTrue(result["ok"])
        with mock.patch("tools.ghidra_query.subprocess.Popen") as failed:
            failed.return_value = mock.Mock(stdout=io.BytesIO(b"x" * 1000), wait=mock.Mock(return_value=9))
            with self.assertRaises(RuntimeError) as error:
                ghidra_query.run("main", "funcs")
            self.assertLess(len(str(error.exception)), 200)

    def test_missing_install_reports_checked_path_and_environment_override(self):
        with tempfile.TemporaryDirectory() as directory:
            checked = Path(directory) / "missing" / "support" / "pyghidraRun"
            stderr = io.StringIO()
            with mock.patch("tools.ghidra_query.HEADLESS", checked), mock.patch.dict("os.environ", {"GHIDRA_INSTALL_DIR": directory}):
                with redirect_stderr(stderr):
                    self.assertEqual(ghidra_query.main(["funcs", "main"]), 2)
            self.assertIn(str(checked), stderr.getvalue())
            self.assertIn("GHIDRA_INSTALL_DIR", stderr.getvalue())

    def test_import_incomplete_default_warns_and_strict_mode_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            headless = root / "pyghidraRun"
            image = root / "arm9.bin"
            headless.touch()
            image.touch()
            projects = root / "projects"
            result = {
                "functions_named": 3, "functions_failed": 1, "labels_failed": 0,
                "symbol_lines_skipped": 0, "incomplete": True,
            }
            patches = (
                mock.patch("tools.ghidra_query.HEADLESS", headless),
                mock.patch("tools.ghidra_query.CACHE", root / "cache"),
                mock.patch("tools.ghidra_query.module_spec", return_value={"image": image}),
                mock.patch("tools.ghidra_query.project_paths", return_value=(projects, "main")),
                mock.patch("tools.ghidra_query.run", return_value=(dict(result), 0.1)),
            )
            for patcher in patches:
                patcher.start()
            try:
                stdout, stderr = io.StringIO(), io.StringIO()
                with redirect_stdout(stdout), redirect_stderr(stderr):
                    self.assertEqual(ghidra_query.main(["import", "main"]), 0)
                self.assertTrue(json.loads(stdout.getvalue())["incomplete"])
                self.assertIn("1 failures", stderr.getvalue())
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.assertEqual(ghidra_query.main(["import", "main", "--strict-import"]), 1)
            finally:
                for patcher in reversed(patches):
                    patcher.stop()


if __name__ == "__main__":
    unittest.main()
