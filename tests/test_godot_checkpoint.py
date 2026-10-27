"""ROM-free tests for Godot checkpoint replay."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import checkpoint, godot_checkpoint


class GodotCheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ladder = self.root / "ladder"
        self.godot = self.root / "godot"
        self.rom = self.root / "game.nds"
        self.dq9 = self.root / "dq9"
        self.godot.write_text("engine")
        self.rom.write_text("rom fixture")
        self.dq9.write_text("native fixture")
        self.options = ["--ladder", str(self.ladder), "--godot", str(self.godot),
                        "--rom", str(self.rom), "--dq9", str(self.dq9)]
        self.calls = []
        self.hash_drift = False
        self.save_drift = False
        self.chain_drift = False
        self.unchanged = False
        self.exit_code = 0
        self.patcher = patch.object(godot_checkpoint.subprocess, "run", side_effect=self.fake_run)
        self.mock_run = self.patcher.start()
        self.addCleanup(self.patcher.stop)
        self.make_segment("alpha")

    def make_segment(self, name, extra_args=None):
        seg = self.ladder / name
        seg.mkdir(parents=True)
        (seg / "start.sav").write_bytes(b"start")
        (seg / "input.txt").write_text("input fixture\n")
        (seg / "expected.hashes").write_text("".join(
            f"{frame} 0123456789abcdef fedcba9876543210\n" for frame in range(4)))
        end_save = self.root / "expected.sav"
        end_save.write_bytes(b"end")
        meta = {"schema": 1, "name": name, "frames": 4, "language": "fr",
                "rtc_epoch": 123, "rtc_timezone_offset_minutes": -60,
                "extra_args": extra_args or [],
                "recorded": {"end_save_sha256": checkpoint.sha(end_save)}}
        (seg / "segment.json").write_text(json.dumps(meta))
        return seg

    def fake_run(self, command, *, env):
        self.calls.append((command, env))
        def option(flag):
            return command[command.index(flag) + 1]
        save = Path(option("--save"))
        index = len(self.calls)
        is_native = "--hash-file" in command
        initial = save.read_bytes()
        if index == 1 or not hasattr(self, "chain_mode"):
            self.assertEqual(initial, b"start")
        else:
            self.assertEqual(initial, b"end")
        if not is_native:
            self.assertNotEqual(env["XDG_DATA_HOME"], os.environ.get("XDG_DATA_HOME"))
            self.assertEqual(Path(env["XDG_DATA_HOME"]).parent, save.parent)
        save.write_bytes(initial if self.unchanged else b"drift" if self.save_drift else b"end")
        rows = []
        for frame in range(int(option("--frames"))):
            drift = self.hash_drift or (self.chain_drift and index == 3)
            bottom = "0000000000000001" if drift and frame in (1, 3) else "fedcba9876543210"
            rows.append(f"{frame} 0123456789abcdef {bottom}\n")
        Path(option("--hash-file" if is_native else "--hashes")).write_text("".join(rows))
        return subprocess.CompletedProcess(command, self.exit_code)

    def invoke(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = godot_checkpoint.main(self.options + list(args))
        return status, output.getvalue()

    def test_pass_and_arguments(self):
        status, output = self.invoke("run", "alpha")
        self.assertEqual(status, 0, output)
        self.assertIn("PASS alpha", output)
        self.assertIn("audio is not compared", output)
        self.assertIn("SUMMARY: 1 passed, 0 failed, 0 skipped", output)
        self.assertEqual((self.ladder / "alpha/start.sav").read_bytes(), b"start")
        cmd, _ = self.calls[0]
        self.assertEqual(cmd[:3], ["xvfb-run", "-a", str(self.godot)])
        self.assertEqual(cmd[cmd.index("--script") + 1], "res://tests/checkpoint_run.gd")
        for flag, value in (("--rom", str(self.rom)), ("--input", str(self.ladder / "alpha/input.txt")),
                            ("--frames", "4"), ("--language", "fr"), ("--rtc-epoch", "123"),
                            ("--rtc-timezone-offset", "-60")):
            self.assertEqual(cmd[cmd.index(flag) + 1], value)
        self.assertNotIn("--toggles", cmd)
        self.assertNotIn("--gpu3d", cmd)

    def test_hash_and_save_drift(self):
        self.hash_drift = self.save_drift = True
        status, output = self.invoke("run")
        self.assertEqual(status, 1, output)
        self.assertIn("FAIL alpha", output)
        self.assertIn("first=1 (2 frames differ)", output)
        self.assertIn("save sha differs", output)
        self.assertIn("1 failed", output)

    def test_skip_extra_args(self):
        self.make_segment("beta", ["--backup-type", "eeprom64k"])
        status, output = self.invoke("run")
        self.assertEqual(status, 0, output)
        self.assertIn("SKIP beta: extra_args unsupported in Godot", output)
        self.assertIn("1 passed, 0 failed, 1 skipped", output)
        self.assertEqual(len(self.calls), 1)

    def test_toggles_and_gpu3d(self):
        status, output = self.invoke("run", "alpha", "--toggles", "--gpu3d")
        self.assertEqual(status, 0, output)
        cmd, _ = self.calls[0]
        self.assertGreater(cmd.index("--toggles"), cmd.index("--"))
        self.assertGreater(cmd.index("--gpu3d"), cmd.index("--"))

    def test_process_and_configuration_errors(self):
        self.exit_code = 7
        status, output = self.invoke("run", "alpha")
        self.assertEqual(status, 2)
        self.assertIn("Godot exited 7", output)
        status, output = self.invoke("run", "bad name")
        self.assertEqual(status, 2)
        self.assertIn("invalid segment name", output)

    def test_chain_pass_and_hash_drift(self):
        self.chain_mode = True
        self.make_segment("beta")
        status, output = self.invoke("chain", "alpha", "beta", "--frames-b", "3")
        self.assertEqual(status, 0, output)
        self.assertIn("PASS chain alpha->beta: 3 frames, save identical", output)
        self.assertEqual(len(self.calls), 3)
        self.assertIn("--headless", self.calls[1][0])
        self.assertIn("--input-script", self.calls[1][0])
        self.assertEqual((self.ladder / "alpha/start.sav").read_bytes(), b"start")
        self.calls.clear()
        self.chain_drift = True
        status, output = self.invoke("chain", "alpha", "beta")
        self.assertEqual(status, 1, output)
        self.assertIn("FAIL chain alpha->beta: hashes first=1", output)

    def test_chain_unchanged_save(self):
        self.chain_mode = True
        self.make_segment("beta")
        meta_path = self.ladder / "alpha/segment.json"
        meta = json.loads(meta_path.read_text())
        meta["recorded"]["end_save_sha256"] = checkpoint.sha(self.ladder / "alpha/start.sav")
        meta_path.write_text(json.dumps(meta))
        self.unchanged = True
        status, output = self.invoke("chain", "alpha", "beta")
        self.assertEqual(status, 2, output)
        self.assertIn("unchanged", output)
        self.assertEqual(len(self.calls), 1)

    def test_rom_resolution(self):
        options = ["--ladder", str(self.ladder), "--godot", str(self.godot)]
        with patch.dict(os.environ, {"DQ9_ROM": str(self.rom)}):
            self.assertEqual(godot_checkpoint.main(options + ["run"]), 0)
        self.assertEqual(self.calls[-1][0][self.calls[-1][0].index("--rom") + 1], str(self.rom))
        rom_dir = self.root / "ROM"
        rom_dir.mkdir()
        (rom_dir / "z.nds").write_text("fixture")
        first = rom_dir / "a.nds"
        first.write_text("fixture")
        with patch.dict(os.environ, {"DQ9_ROM": ""}), patch.object(godot_checkpoint, "data_root", return_value=self.root):
            self.assertEqual(godot_checkpoint.main(options + ["run"]), 0)
        self.assertEqual(self.calls[-1][0][self.calls[-1][0].index("--rom") + 1], str(first))


if __name__ == "__main__":
    unittest.main()
