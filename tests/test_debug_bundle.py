import contextlib
import io
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from tools import debug_bundle as bundle


def ppm(pixel=b"\x10\x20\x30"):
    return b"P6\n1 1\n255\n" + pixel


class DebugBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.debug = self.root / "debug"
        self.debug.mkdir()
        self.bundle = self.debug / "20261010-120000-issue"
        self.bundle.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def test_index_parsing_and_newest_first_limit(self):
        (self.debug / "index.tsv").write_text(
            "20261009-120000\told\told note\t10\n"
            "bad row\n"
            "20261010-120000\tnew\tnew note\t20\n"
            "20261011\tbroken\tnot a frame\tframe\n")
        rows = bundle.sorted_index(self.debug)
        self.assertEqual([row["directory"] for row in rows], ["new", "old"])
        self.assertEqual(bundle.select_bundle(self.debug, "1"), self.debug / "new")
        status, output = self.run_cli(["--debug-dir", str(self.debug), "list", "--limit", "1"])
        self.assertEqual(status, 0)
        self.assertIn("new", output)
        self.assertNotIn("old", output)
        self.assertEqual(bundle.parse_index(self.root / "absent.tsv"), [])

    def test_default_index_combines_current_and_legacy(self):
        current = self.root / "dq9-remastered" / "debug"
        legacy = self.root / "dq9-decomp" / "debug"
        with patch.object(bundle, "DEFAULT_DEBUG_DIR", current), patch.object(bundle, "LEGACY_DEBUG_DIR", legacy):
            self.assertEqual(bundle.sorted_index(None), [])
            legacy.mkdir(parents=True)
            (legacy / "index.tsv").write_text("20261010\tshared\tlegacy\t20\n")
            current.mkdir(parents=True)
            (current / "index.tsv").write_text("20261011\tshared\tcurrent\t21\n")
            (legacy / "shared").mkdir()
            (current / "shared").mkdir()
            self.assertEqual(bundle.select_bundle(None, "1"), current / "shared")
            self.assertEqual(bundle.select_bundle(None, "2"), legacy / "shared")
            self.assertEqual(bundle.select_bundle(None, "shared"), current / "shared")
            self.assertEqual(bundle.select_bundle(legacy, "1"), legacy / "shared")
            status, output = self.run_cli(["list"])
            self.assertEqual(status, 0)
            self.assertIn(str(current / "shared"), output)
            self.assertIn(str(legacy / "shared"), output)

    def test_default_selection_prefers_current_and_falls_back_to_legacy(self):
        current = self.root / "dq9-remastered" / "debug"
        legacy = self.root / "dq9-decomp" / "debug"
        with patch.object(bundle, "DEFAULT_DEBUG_DIR", current), patch.object(bundle, "LEGACY_DEBUG_DIR", legacy):
            legacy_bundle = legacy / "shared"
            legacy_bundle.mkdir(parents=True)
            marker = legacy_bundle / "marker.txt"
            marker.write_text("legacy")
            (legacy / "index.tsv").write_text("20261010\tshared\tlegacy\t20\n")
            self.assertEqual(bundle.select_bundle(None, "shared"), legacy_bundle)
            self.assertEqual(bundle.select_bundle(None, "1"), legacy_bundle)
            self.assertEqual(bundle.select_bundle(None, "missing"), current / "missing")

            current_bundle = current / "shared"
            current_bundle.mkdir(parents=True)
            (current / "index.tsv").write_text("20261010\tshared\tcurrent\t21\n")
            self.assertEqual(bundle.select_bundle(None, "shared"), current_bundle)
            self.assertEqual(bundle.select_bundle(None, "1"), current_bundle)
            self.assertEqual(bundle.select_bundle(None, "2"), legacy_bundle)
            self.assertEqual(marker.read_text(), "legacy")
            self.assertTrue((legacy / "index.tsv").is_file())

    def test_comment_metadata_and_missing_file(self):
        path = self.bundle / "comment.txt"
        path.write_text("Black screen after title\nSome extra detail\nframe: 123\nticks: 90\n")
        text, metadata = bundle.parse_comment(path)
        self.assertEqual(text, "Black screen after title\nSome extra detail")
        self.assertEqual(metadata, {"frame": "123", "ticks": "90"})
        self.assertEqual(bundle.parse_comment(self.bundle / "missing.txt"), ("", {}))
        path.write_text("audio_debug: solo=0 muted=3,15\nThe song sounds wrong\nMore detail\n"
                        "timestamp: 20261011-121151\nframe: 123\n")
        text, metadata = bundle.parse_comment(path)
        self.assertEqual(text, "The song sounds wrong\nMore detail")
        self.assertEqual(metadata["audio_debug"], "solo=0 muted=3,15")
        self.assertEqual(metadata["frame"], "123")

    def test_replay_script_parses_quoted_paths(self):
        script = self.bundle / "replay.sh"
        script.write_text('#!/bin/sh\nexec "/path with spaces/dq9" --rom "/roms/my game.nds" \\\n  --input-script "input logs/session input.txt" --headless\n')
        self.assertEqual(bundle.parse_replay_command(script), [
            "/path with spaces/dq9", "--rom", "/roms/my game.nds",
            "--input-script", "input logs/session input.txt", "--headless"])
    def test_replay_preserves_caller_supplied_layout_metadata(self):
        script = self.bundle / "replay.sh"
        script.write_text('exec dq9 --rom "/tmp/synthetic.nds" --abi-config "/tmp/private-layout.json" --headless\n')
        self.assertEqual(bundle.parse_replay_command(script), [
            "dq9", "--rom", "/tmp/synthetic.nds", "--abi-config", "/tmp/private-layout.json", "--headless"])

    def test_replay_script_ignores_shell_preamble_and_joins_continuations(self):
        script = self.bundle / "replay.sh"
        script.write_text(
            '#!/bin/sh\n'
            'set -eu\n'
            'cd "$(dirname "$0")"\n'
            'mkdir -p replay_out\n'
            'cp session_start.sav replay_out/replay.sav\n'
            'SAVE_COPY=replay_out/replay.sav\n'
            '# actual game invocation follows\n'
            'dq9 --rom "/rom directory/game image.nds" \\\n'
            '  --input-script "input logs/session input.txt" \\\n'
            '  --save replay_out/replay.sav --headless\n')
        self.assertEqual(bundle.parse_replay_command(script), [
            "dq9", "--rom", "/rom directory/game image.nds",
            "--input-script", "input logs/session input.txt",
            "--save", "replay_out/replay.sav", "--headless"])

    def test_inventory_and_show_are_robust_to_missing_files(self):
        (self.bundle / "comment.txt").write_text("frame: 42\n")
        (self.bundle / "state.txt").write_text("state")
        (self.bundle / "empty").write_bytes(b"")
        (self.bundle / "audio_stats.txt").write_text("rms: 0.1\npeak: 0.5\n")
        entries = bundle.inventory(self.bundle)
        self.assertEqual([(entry["name"], entry["size"]) for entry in entries],
                         [("audio_stats.txt", 19), ("comment.txt", 10), ("empty", 0), ("state.txt", 5)])
        status, output = self.run_cli(["--debug-dir", str(self.debug), "show", self.bundle.name])
        self.assertEqual(status, 0)
        self.assertIn("frame: 42", output)
        self.assertIn("Audio stats:", output)
        self.assertIn("rms: 0.1", output)
        self.assertIn("state.txt (5 bytes)", output)
        self.assertIn("replay.sh: no", output)
        self.assertEqual(bundle.inventory(self.root / "missing"), [])

    def test_export_excludes_dumps_by_default_and_includes_on_opt_in(self):
        for name, data in (("comment.txt", b"frame: 9"), ("vram.bin", b"vram"),
                           ("mainram.bin", b"ram"), ("screenshot_top.png", b"png"),
                           ("game.nds", b"ROM")):
            (self.bundle / name).write_bytes(data)
        default_archive = self.root / "default.tar.gz"
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.assertTrue(bundle.export_bundle(self.bundle, default_archive))
        self.assertIn("omitted", stderr.getvalue())
        with tarfile.open(default_archive, "r:gz") as archive:
            names = archive.getnames()
        self.assertIn(f"{self.bundle.name}/comment.txt", names)
        self.assertIn(f"{self.bundle.name}/screenshot_top.png", names)
        self.assertFalse(any(name.endswith(("vram.bin", "mainram.bin", "game.nds")) for name in names))
        dump_archive = self.root / "dumps.tar.gz"
        bundle.export_bundle(self.bundle, dump_archive, with_dumps=True)
        with tarfile.open(dump_archive, "r:gz") as archive:
            names = archive.getnames()
        self.assertIn(f"{self.bundle.name}/vram.bin", names)
        self.assertIn(f"{self.bundle.name}/mainram.bin", names)
        self.assertFalse(any(name.endswith("game.nds") for name in names))

    def test_replay_updates_recorded_command_runs_and_compares(self):
        (self.bundle / "comment.txt").write_text("frame: 12\n")
        (self.bundle / "session_input.txt").write_text("0\n")
        start_save = self.bundle / "session_start.sav"
        start_save.write_bytes(b"start baseline")
        replay_save = self.bundle / "replay_out" / "replay.sav"
        replay_save.parent.mkdir()
        replay_save.write_bytes(b"replay baseline")
        (self.bundle / "replay.sh").write_text(
            '#!/bin/sh\nset -eu\ncd "$(dirname "$0")"\nmkdir -p replay_out\n'
            'cp session_start.sav replay_out/replay.sav\n'
            'exec dq9 --rom "/recorded/game.nds" --input-script session_input.txt \\\n'
            '  --save replay_out/replay.sav --frames 100 --dump-frames 3 \\\n'
            '  --dump-dir replay_out/screens --dump-audio replay_out/audio.wav --headless\n')
        (self.bundle / "screenshot_top.ppm").write_bytes(ppm())
        out = self.root / "out"
        seen = []

        class Result:
            returncode = 0

        def runner(command):
            seen.append(command)
            save_path = Path(command[command.index("--save") + 1])
            self.assertTrue(save_path.is_relative_to(out))
            self.assertEqual(save_path.read_bytes(), b"start baseline")
            save_path.write_bytes(b"mutated by dq9")
            self.assertEqual((out / "session_start.sav").read_bytes(), b"start baseline")
            screens = out / "screens"
            screens.mkdir(exist_ok=True)
            (screens / "frame_0010_top.ppm").write_bytes(ppm())
            (screens / "frame_0011_top.ppm").write_bytes(ppm())
            (screens / "frame_0012_top.ppm").write_bytes(ppm())
            return Result()

        output = io.StringIO()
        with patch.object(bundle.subprocess, "run", runner), contextlib.redirect_stdout(output):
            status = bundle.replay(self.bundle, frames_before=2, dq9=Path("custom dq9"),
                                   rom=Path("chosen.nds"), out=out)
        self.assertEqual(status, 0)
        command = seen[0]
        self.assertEqual(command[0], "custom dq9")
        self.assertEqual(command[command.index("--rom") + 1], "chosen.nds")
        self.assertEqual(command[command.index("--frames") + 1], "13")
        self.assertEqual(command[command.index("--dump-frames") + 1], "10,11,12")
        self.assertEqual(command[command.index("--dump-dir") + 1], str(out.resolve() / "screens"))
        self.assertEqual(command[command.index("--dump-audio") + 1], str(out.resolve() / "audio.wav"))
        self.assertIn("--dump-audio", command)
        self.assertEqual(replay_save.read_bytes(), b"replay baseline")
        self.assertEqual(start_save.read_bytes(), b"start baseline")
        self.assertIn("MATCH", output.getvalue())
    def test_replay_expands_script_variables_and_seeds_output_paths(self):
        (self.bundle / "comment.txt").write_text("frame: 12\n")
        input_script = self.bundle / "session_input.txt"
        input_script.write_text("0\n")
        start_save = self.bundle / "session_start.sav"
        start_save.write_bytes(b"baseline bytes")
        stale_save = self.bundle / "replay_out" / "replay.sav"
        stale_save.parent.mkdir()
        stale_save.write_bytes(b"stale mutated bytes")
        (self.bundle / "replay.sh").write_text(
            '#!/bin/sh\nset -eu\n'
            'SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"\n'
            'OUT="$SCRIPT_DIR/replay_out"\n'
            'mkdir -p "$OUT"\n'
            'exec dq9 --rom game.nds --input-script "${SCRIPT_DIR}/session_input.txt" \\\n'
            '  --save "${OUT}/replay.sav" --dump-dir "$OUT/captures" \\\n'
            '  --dump-audio "${OUT}/audio_tail.wav" --headless\n')
        (self.bundle / "screenshot_top.ppm").write_bytes(ppm())
        out = self.root / "replay-output"
        out.mkdir()

        class Result:
            returncode = 0

        def runner(command):
            save = Path(command[command.index("--save") + 1])
            self.assertEqual(save, out.resolve() / "replay.sav")
            self.assertEqual(save.read_bytes(), b"baseline bytes")
            self.assertEqual(Path(command[command.index("--input-script") + 1]), input_script.resolve())
            self.assertEqual(Path(command[command.index("--dump-dir") + 1]),
                             out.resolve() / "captures")
            self.assertEqual(Path(command[command.index("--dump-audio") + 1]),
                             out.resolve() / "audio_tail.wav")
            (out / "captures").mkdir(exist_ok=True)
            (out / "captures" / "frame_0012_top.ppm").write_bytes(ppm())
            save.write_bytes(b"mutated by dq9")
            return Result()

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bundle.replay(self.bundle, dq9=Path("dq9"), out=out, runner=runner), 0)
        self.assertEqual(start_save.read_bytes(), b"baseline bytes")
        self.assertEqual(stale_save.read_bytes(), b"stale mutated bytes")

    def test_replay_mismatch_and_missing_screenshot_errors(self):
        (self.bundle / "comment.txt").write_text("frame: 4\n")
        (self.bundle / "replay.sh").write_text("dq9 --rom game.nds --headless\n")
        (self.bundle / "session_input.txt").write_text("0\n")
        out = self.root / "out"
        out.mkdir()
        (self.bundle / "screenshot_bottom.ppm").write_bytes(ppm())

        class Result:
            returncode = 0

        def runner(_command):
            (out / "frame_0004_bottom.ppm").write_bytes(ppm(b"\x00\x00\x00"))
            return Result()

        self.assertEqual(bundle.replay(self.bundle, out=out, runner=runner), 1)
        (self.bundle / "screenshot_bottom.ppm").unlink()
        with self.assertRaisesRegex(ValueError, "no matching"):
            bundle.replay(self.bundle, out=out, runner=runner)
    def test_interactive_quick_reload_uses_recorded_clock_and_fresh_save(self):
        (self.bundle / "comment.txt").write_text(
            "Audio stutter\nframe: 12\nrtc_epoch: 1704067190\nrtc_timezone_offset_minutes: 120\n")
        (self.bundle / "session_input.txt").write_text("0\n")
        start_save = self.bundle / "session_start.sav"
        start_save.write_bytes(b"launch baseline")
        (self.bundle / "replay.sh").write_text(
            '#!/bin/sh\nset -eu\ncd "$(dirname "$0")"\nmkdir -p replay_out\n'
            'cp session_start.sav replay_out/replay.sav\n'
            'exec dq9 --rom game.nds --input-script session_input.txt '
            '--save replay_out/replay.sav --frames 13 --dump-frames 12 '
            '--dump-dir replay_out --dump-audio replay_out/audio.wav --headless\n')
        out = self.root / "interactive-output"
        seen = []

        class Result:
            returncode = 0

        def runner(command):
            seen.append(command)
            save = Path(command[command.index("--save") + 1])
            self.assertEqual(save, out.resolve() / "replay.sav")
            self.assertEqual(save.read_bytes(), b"launch baseline")
            save.write_bytes(b"changed by interactive game")
            self.assertEqual(start_save.read_bytes(), b"launch baseline")
            return Result()

        with contextlib.redirect_stdout(io.StringIO()):
            status, _ = self.run_cli([
                "--debug-dir", str(self.debug), "replay", self.bundle.name,
                "--interactive", "--out", str(out), "--dq9", "custom dq9",
            ], runner=runner)
        self.assertEqual(status, 0)
        command = seen[0]
        self.assertEqual(command[0], "custom dq9")
        self.assertNotIn("--headless", command)
        self.assertNotIn("--frames", command)
        self.assertIn("--debug-tool", command)
        self.assertEqual(command[command.index("--pause-at-frame") + 1], "12")
        self.assertEqual(command[command.index("--rtc-epoch") + 1], "1704067190")
        self.assertEqual(command[command.index("--rtc-timezone-offset") + 1], "120")
        self.assertNotIn("--dump-audio", command)
        self.assertEqual(start_save.read_bytes(), b"launch baseline")


    def run_cli(self, arguments, runner=None):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = bundle.main(arguments, runner=runner)
        return status, output.getvalue()


if __name__ == "__main__":
    unittest.main()
