"""ROM-free checkpoint recorder and replay contract tests."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import checkpoint


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("dq9", "game.nds", "start.sav", "input.txt"):
            (self.root / name).write_text("source")
        self.options = ["--ladder", str(self.root / "ladder"), "--dq9", str(self.root / "dq9"),
                        "--rom", str(self.root / "game.nds")]
        self.calls = []
        self.hash_drift = False
        self.save_drift = False
        self.screen_drift = False
        self.ppm = b"P6\n2 2\n255\n" + bytes(12)
        self.patcher = patch.object(checkpoint.subprocess, "run", side_effect=self.fake_run)
        self.mock_run = self.patcher.start()
        self.addCleanup(self.patcher.stop)
        self.data_patcher = patch.object(checkpoint, "data_root", return_value=self.root)
        self.data_patcher.start()
        self.addCleanup(self.data_patcher.stop)

    def fake_run(self, command, *, stdout, stderr):
        self.calls.append(command)
        def option(name):
            return command[command.index(name) + 1]
        save = Path(option("--save"))
        self.assertEqual(save.read_text(), "source")
        save.write_text("different" if self.save_drift and len(self.calls) == 3 else "modified by guest")
        rows = []
        for frame in range(int(option("--frames"))):
            bottom = "0000000000000001" if self.hash_drift and len(self.calls) == 3 and frame == 5 else "fedcba9876543210"
            rows.append(f"{frame} 0123456789abcdef {bottom}\n")
        Path(option("--hash-file")).write_text("".join(rows))
        Path(option("--dump-audio")).write_bytes(b"audio")
        if "--dump-frames" in command:
            directory = Path(option("--dump-dir"))
            frame = int(option("--dump-frames"))
            for screen in ("top", "bottom"):
                image = self.ppm if not self.screen_drift else self.ppm[:-1] + b"\x01"
                (directory / f"frame_{frame:04d}_{screen}.ppm").write_bytes(image)
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            status = checkpoint.main(self.options + list(args))
        return status, out.getvalue()

    def add(self):
        return self.invoke("add", "segment", "--save", str(self.root / "start.sav"),
                           "--input", str(self.root / "input.txt"), "--frames", "10")

    def test_add_and_repeat_start_fresh(self):
        self.assertEqual(self.add()[0], 0)
        status, output = self.invoke("run", "--repeat", "2")
        self.assertEqual(status, 0, output)
        self.assertIn("SUMMARY: 2 passed, 0 failed of 2 runs", output)
        self.assertEqual((self.root / "ladder/segment/start.sav").read_text(), "source")
        for cmd in self.calls:
            for flag, value in (("--language", "en"), ("--rtc-epoch", "0"), ("--rtc-timezone-offset", "0")):
                self.assertEqual(cmd[cmd.index(flag) + 1], value)

    def test_hash_drift(self):
        self.assertEqual(self.add()[0], 0)
        self.hash_drift = True
        status, output = self.invoke("run")
        self.assertEqual(status, 1)
        self.assertIn("first=5", output)

    def test_save_drift(self):
        self.assertEqual(self.add()[0], 0)
        self.save_drift = True
        status, output = self.invoke("run")
        self.assertEqual(status, 1)
        self.assertIn("save sha differs", output)

    def test_record_list_oracle_and_binary_note(self):
        oracle = self.root / "oracle"
        oracle.write_text("".join(f"{n} 0123456789abcdef fedcba9876543210\n" for n in range(5)))
        status, output = self.invoke("add", "oracle", "--save", str(self.root / "start.sav"),
                                     "--input", str(self.root / "input.txt"), "--frames", "10",
                                     "--oracle-hashes", str(oracle), "--note", "example")
        self.assertEqual(status, 0, output)
        status, output = self.invoke("list")
        self.assertEqual(status, 0)
        self.assertIn("oracle=yes  example", output)
        (self.root / "dq9").write_text("changed")
        status, output = self.invoke("run", "oracle", "--oracle", "--repeat", "1")
        self.assertEqual(status, 0, output)
        self.assertIn("NOTE oracle: binary changed", output)
        self.assertIn("ORACLE oracle: identical on 5 common frames", output)
        status, output = self.invoke("record", "--all")
        self.assertEqual(status, 0, output)
        self.assertIn("RECORDED oracle: 10 frames", output)
        self.assertNotIn("NOTE oracle:", self.invoke("run", "oracle", "--repeat", "1")[1])

    def test_failed_process_removes_new_segment_and_keeps_log(self):
        self.mock_run.side_effect = lambda command, **kwargs: subprocess.CompletedProcess(command, 7)
        status, output = self.add()
        self.assertEqual(status, 2)
        self.assertIn("dq9 exited 7", output)
        self.assertFalse((self.root / "ladder/segment").exists())
        self.assertEqual(len(list((self.root / "build/checkpoint-runs").glob("segment-record-*/run.log"))), 1)

    def test_bundle_screens_and_metadata(self):
        bundle = self.root / "bundle"
        bundle.mkdir()
        (bundle / "comment.txt").write_text("frame: 9\nrtc_epoch: 123\nrtc_timezone_offset_minutes: 120\n")
        (bundle / "env.txt").write_text("language: fr\n")
        for name in ("session_start.sav", "session_input.txt"):
            (bundle / name).write_text("source")
        for screen in ("top", "bottom"):
            (bundle / f"screenshot_{screen}.ppm").write_bytes(self.ppm)
        status, output = self.invoke("from-bundle", str(bundle), "match")
        self.assertEqual(status, 0, output)
        meta = json.loads((self.root / "ladder/match/segment.json").read_text())
        self.assertEqual((meta["frames"], meta["language"], meta["rtc_epoch"], meta["rtc_timezone_offset_minutes"]),
                         (10, "fr", 123, 120))
        self.assertEqual(meta["extra_args"], [])
        self.assertIs(meta["bundle_screens_match"], True)
        self.screen_drift = True
        status, output = self.invoke("from-bundle", str(bundle), "different")
        self.assertEqual(status, 1, output)
        meta = json.loads((self.root / "ladder/different/segment.json").read_text())
        self.assertIs(meta["bundle_screens_match"], False)

    def test_invalid_name_and_missing_save(self):
        for name, save in (("a b", self.root / "start.sav"), ("ok", self.root / "missing")):
            status, _ = self.invoke("add", name, "--save", str(save), "--input", str(self.root / "input.txt"), "--frames", "10")
            self.assertEqual(status, 2)
        self.mock_run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
