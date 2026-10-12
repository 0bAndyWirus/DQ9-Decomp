"""Contract tests for the renderer determinism run matrix (no ROM required)."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import determinism_check


class DeterminismCheckTests(unittest.TestCase):
    def test_complete_matrix_and_fresh_save(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("dq9", "game.nds", "source.sav", "input.txt"):
                (root / name).write_text("source")
            # Invented hashes and a short synthetic frame sequence.
            rows = "".join(f"{frame} 0123456789abcdef fedcba9876543210\n" for frame in range(8))
            (root / "baseline.txt").write_text(rows)
            calls = []

            def run(command, *, env, stdout, stderr):
                calls.append((command, env["DSR_RENDER_THREADS"]))
                save = Path(command[command.index("--save") + 1])
                self.assertEqual(save.read_text(), "source")
                save.write_text("modified by guest")
                Path(command[command.index("--hash-file") + 1]).write_text(rows)
                self.assertIn("--rtc-epoch", command)
                self.assertIn("--rtc-timezone-offset", command)
                self.assertEqual(command[command.index("--frames") + 1], "8")
                return subprocess.CompletedProcess(command, 0)

            with patch.object(determinism_check.shutil, "which", return_value="/usr/bin/taskset"), \
                    patch.object(determinism_check.subprocess, "run", side_effect=run), \
                    patch("builtins.print"):
                status = determinism_check.main([
                    "--binary", str(root / "dq9"), "--rom", str(root / "game.nds"),
                    "--save", str(root / "source.sav"), "--input", str(root / "input.txt"),
                    "--baseline", str(root / "baseline.txt"), "--out", str(root / "out"), "--frames", "8",
                ])
            self.assertEqual(status, 0)
            self.assertEqual([threads for _, threads in calls], ["0"] * 3 + ["1"] * 3 + ["2"] * 3 + ["6"] * 4)
            self.assertEqual(sum(command[0] == "taskset" for command, _ in calls), 1)
            self.assertEqual(calls[-1][0][:3], ["taskset", "-c", "0"])
            self.assertEqual((root / "source.sav").read_text(), "source")

    def test_incomplete_coverage_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "hashes.txt"
            path.write_text("0 0123456789abcdef fedcba9876543210\n")
            with self.assertRaisesRegex(ValueError, "exactly frames"):
                determinism_check.checked_hashes(path, 8)

    def test_missing_prerequisite_fails_before_running(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(determinism_check.subprocess, "run") as run, \
                patch("builtins.print"):
            root = Path(temp)
            status = determinism_check.main([
                "--binary", str(root / "missing"), "--rom", str(root / "synthetic.nds"),
                "--save", str(root / "source.sav"), "--input", str(root / "inputs.txt"),
                "--baseline", str(root / "hashes.txt"), "--frames", "8",
            ])
            self.assertEqual(status, 2)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
