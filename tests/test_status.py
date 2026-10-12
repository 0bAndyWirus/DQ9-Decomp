import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import status


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.workspace = self.root / "workspace"
        self.report_path = self.workspace / "build" / "eur" / "report.json"
        self.report_path.parent.mkdir(parents=True)
        self.lock = self.workspace / "build" / "scratch" / "integrate.lock"
        config = self.workspace / "config" / "eur" / "arm9" / "config.yaml"
        config.parent.mkdir(parents=True)
        config.write_text("main_module: {name: synthetic-main}\n"
                          "overlays: [{name: synthetic-one}, {name: synthetic-two}]\n")
        self.patches = [mock.patch.object(status, "WORKSPACE", self.workspace),
                        mock.patch.object(status, "LOCK", self.lock)]
        for patch in self.patches:
            patch.start()
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for patch in reversed(self.patches):
            patch.stop()
        self.temp.cleanup()

    def write_report(self):
        self.report_path.write_text(json.dumps({"measures": {
            "matched_functions": 1, "total_functions": 2, "matched_code": 4,
            "total_code": 8, "complete_units": 1, "total_units": 2}}))

    def run_status(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = status.main(list(args))
        return code, output.getvalue()

    def test_missing_and_malformed_report_fail(self):
        code, output = self.run_status()
        self.assertNotEqual(code, 0)
        self.assertIn("report:", output)
        self.write_report()
        self.report_path.write_text("{")
        code, output = self.run_status("--json")
        self.assertNotEqual(code, 0)
        self.assertEqual(json.loads(output)["status"], 1)


    def test_report_excludes_generated_gap_anchors(self):
        self.report_path.write_text(json.dumps({
            "measures": {
                "matched_functions": 1, "total_functions": 3, "matched_code": 4,
                "total_code": 112, "complete_units": 1, "total_units": 2,
            },
            "units": [{
                "functions": [
                    {"name": "matched", "size": "4", "fuzzy_match_percent": 100.0},
                    {"name": "unmatched", "size": "12", "fuzzy_match_percent": 80.0},
                    {"name": "__dsd_gap_anchor_main_1000", "size": "96", "fuzzy_match_percent": None},
                ],
            }],
        }))
        _line, values = status.report_data()
        self.assertEqual(values["matched_functions"], 1)
        self.assertEqual(values["total_functions"], 2)
        self.assertEqual(values["matched_code"], 4)
        self.assertEqual(values["total_code"], 16)

    @mock.patch.object(status.subprocess, "run")
    def test_module_success_count_and_subprocess_failures(self, run):
        self.write_report()
        run.return_value = mock.Mock(stdout="".join(f"synthetic-module{i}: OK\n" for i in range(3)),
                                     stderr="", returncode=0)
        code, output = self.run_status("--modules")
        self.assertEqual(code, 0)
        self.assertIn("modules: 3 OK", output)
        run.return_value = mock.Mock(stdout="module: OK\n", stderr="", returncode=0)
        code, output = self.run_status("--modules")
        self.assertNotEqual(code, 0)
        self.assertIn("modules: 1 OK", output)
        run.return_value = mock.Mock(stdout="".join(f"synthetic-module{i}: OK\n" for i in range(3)),
                                     stderr="", returncode=1)
        self.assertNotEqual(self.run_status("--modules")[0], 0)
        run.side_effect = OSError("ninja missing")
        code, output = self.run_status("--modules")
        self.assertNotEqual(code, 0)
        self.assertIn("failed to run ninja", output)

    def test_running_lock_is_documented_skip_and_does_not_claim_success(self):
        self.write_report()
        self.lock.parent.mkdir(parents=True, exist_ok=True)
        with self.lock.open("a") as held:
            status.lock_file(held)
            code, output = self.run_status("--modules")
        self.assertEqual(code, 0)
        self.assertIn("modules: skipped, integration running", output)
        self.assertNotIn("modules: 3 OK", output)

    @mock.patch.object(status.subprocess, "run")
    def test_explicit_module_count_overrides_project_config(self, run):
        self.write_report()
        run.return_value = mock.Mock(stdout="".join(f"synthetic-module{i}: OK\n" for i in range(5)),
                                     stderr="", returncode=0)
        code, output = self.run_status("--modules", "--expected-modules", "5", "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output)["modules"]["expected"], 5)
        self.assertEqual(json.loads(output)["modules"]["count"], 5)
        self.assertNotEqual(self.run_status("--modules", "--expected-modules", "2")[0], 0)
        self.assertNotEqual(self.run_status("--modules", "--expected-modules", "0")[0], 0)

    @mock.patch.object(status.subprocess, "run")
    def test_explicit_project_controls_all_workspace_reads(self, run):
        self.write_report()
        run.return_value = mock.Mock(stdout="synthetic-main: OK\nsynthetic-one: OK\nsynthetic-two: OK\n",
                                     stderr="", returncode=0)
        with mock.patch.object(status, "WORKSPACE", self.root / "unused"):
            code, output = self.run_status("--modules", "--project", str(self.workspace), "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output)["modules"]["expected"], 3)
        self.assertEqual(run.call_args.kwargs["cwd"], self.workspace)

    def test_module_config_is_not_needed_unless_modules_are_requested(self):
        self.write_report()
        config = self.workspace / "config" / "eur" / "arm9" / "config.yaml"
        config.unlink()
        self.assertEqual(self.run_status()[0], 0)
        code, output = self.run_status("--modules", "--json")
        self.assertNotEqual(code, 0)
        self.assertIn("configuration error", json.loads(output)["modules"]["line"])

    def test_candidates_summary_json_and_all(self):
        self.write_report()
        candidates = self.workspace / "src" / "_candidates"
        for i in range(12):
            author = candidates / f"author{i:02}"
            author.mkdir(parents=True)
            (author / "candidate.json").write_text("{}")
        code, output = self.run_status("--json")
        self.assertEqual(code, 0)
        data = json.loads(output)
        self.assertEqual(data["candidates"], 12)
        self.assertEqual(data["candidate_authors"], 12)
        self.assertEqual(len(data["top_candidate_authors"]), 10)
        self.assertNotIn("candidate_counts", data)
        code, output = self.run_status()
        self.assertEqual(code, 0)
        self.assertIn("+2 authors", output)
        code, output = self.run_status("--all")
        self.assertEqual(code, 0)
        self.assertIn("author11=1", output)
        code, output = self.run_status("--all", "--json")
        self.assertEqual(code, 0)
        self.assertIn("author11", json.loads(output)["candidate_counts"])

    def test_broken_pipe_is_handled(self):
        self.write_report()
        class Broken:
            def write(self, value):
                raise BrokenPipeError
            def flush(self):
                raise BrokenPipeError
        with mock.patch.object(status.sys, "stdout", Broken()):
            self.assertEqual(status.main([]), 0)


if __name__ == "__main__":
    unittest.main()
