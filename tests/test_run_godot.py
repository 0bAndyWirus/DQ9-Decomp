"""Launch-wrapper argument and menu tests; no ROM data or engine required."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RunGodotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tools").mkdir()
        (self.root / "godot/bin").mkdir(parents=True)
        (self.root / "godot/bin/libdq9_godot.so").touch()
        self.wrapper = self.root / "tools/run_godot.sh"
        shutil.copy(ROOT / "tools/run_godot.sh", self.wrapper)
        self.engine = self.root / "engine"
        self.engine.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@"\nprintf "cwd=%s\\n" "$PWD"\n'
            'exit "${FIXTURE_EXIT_CODE:-0}"\n')
        self.engine.chmod(0o755)
        self.env = dict(os.environ, GODOT=str(self.engine),
                        HOME=str(self.root / "home"),
                        DQ9_SAVE=str(self.root / "saves/qa.sav"))
        self.env.pop("DQ9_ROM", None)

    def run_wrapper(self, *args):
        return subprocess.run(["sh", str(self.wrapper), *map(str, args)],
                              env=self.env, cwd="/", text=True,
                              capture_output=True)

    def test_menu_without_local_rom(self):
        result = self.run_wrapper("--menu", "fr")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("--autostart", result.stdout.splitlines())
        self.assertIn("fr", result.stdout.splitlines())

    def test_autostart_requires_caller_image_selection(self):
        (self.root / "unselected game.nds").touch()
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 1)
        self.assertIn("--rom PATH or DQ9_ROM", result.stderr)
        self.assertIn("--menu", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_rom_environment_preserves_spaces(self):
        rom = self.root / "selected image with spaces.nds"
        rom.touch()
        self.env["DQ9_ROM"] = str(rom)
        result = self.run_wrapper("de")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertIn(str(rom), arguments)
        self.assertEqual(arguments.count("--rom"), 1)
        self.assertIn("--autostart", arguments)
        self.assertIn("de", arguments)
        self.assertIn("cwd=/", arguments)

    def test_explicit_rom_overrides_environment_and_forwards_options(self):
        rom = self.root / "chosen image with spaces.nds"
        rom.touch()
        self.env["DQ9_ROM"] = str(self.root / "unselected image.nds")
        options = ["--rom", str(rom), "--language", "it", "--app-output",
                   str(self.root / "output with spaces")]
        result = self.run_wrapper(*options)
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertEqual(arguments[-7:-1], options)
        self.assertEqual(arguments.count("--rom"), 1)
        self.assertNotIn(self.env["DQ9_ROM"], arguments)

    def test_explicit_save_creates_selected_parent_without_duplicate_arguments(self):
        save = self.root / "chosen saves/game save.sav"
        result = self.run_wrapper("--menu", "fr", "--save", save)
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertEqual(arguments.count("--save"), 1)
        self.assertIn(str(save), arguments)
        self.assertTrue(save.parent.is_dir())
        self.assertFalse(Path(self.env["DQ9_SAVE"]).parent.exists())

    def test_menu_forwards_image_and_user_options_without_autostart(self):
        rom = self.root / "chosen image.nds"
        rom.touch()
        result = self.run_wrapper("--menu", "es", "--rom", rom, "--app-scale", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertNotIn("--autostart", arguments)
        self.assertEqual(arguments[-5:-1], ["--rom", str(rom), "--app-scale", "2"])
        self.assertIn("es", arguments)

    def test_default_save_uses_product_directory(self):
        self.env.pop("DQ9_SAVE")
        result = self.run_wrapper("--menu")
        self.assertEqual(result.returncode, 0, result.stderr)
        save = self.root / "home/.local/share/dq9-decomp/godot.sav"
        self.assertIn(str(save), result.stdout.splitlines())
        self.assertTrue(save.parent.is_dir())

    def test_godot_can_be_selected_from_path(self):
        binaries = self.root / "bin"
        binaries.mkdir()
        shutil.copy2(self.engine, binaries / "godot")
        self.env.pop("GODOT")
        self.env["PATH"] = f"{binaries}:{self.env['PATH']}"
        result = self.run_wrapper("--menu")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--path", result.stdout.splitlines())

    def test_missing_option_value_is_reported_before_launch(self):
        for option in ("--rom", "--save"):
            with self.subTest(option=option):
                result = self.run_wrapper("--menu", option)
                self.assertEqual(result.returncode, 2)
                self.assertIn("requires a value", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_engine_exit_status_is_preserved(self):
        self.env["FIXTURE_EXIT_CODE"] = "73"
        result = self.run_wrapper("--menu")
        self.assertEqual(result.returncode, 73)


class RunSdlTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tools").mkdir()
        self.wrapper = self.root / "tools/run_sdl.sh"
        shutil.copy2(ROOT / "tools/run_sdl.sh", self.wrapper)
        binary = self.root / "build/game-ui/game/dq9"
        binary.parent.mkdir(parents=True)
        binary.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@"\nprintf "cwd=%s\\n" "$PWD"\n'
            'exit "${FIXTURE_EXIT_CODE:-0}"\n')
        binary.chmod(0o755)
        self.env = dict(os.environ, HOME=str(self.root / "home"),
                        DQ9_SAVE=str(self.root / "saves/qa.sav"))
        self.env.pop("DQ9_ROM", None)

    def run_wrapper(self, *args):
        return subprocess.run(["sh", str(self.wrapper), *map(str, args)], env=self.env,
                              cwd="/", text=True, capture_output=True, check=False)

    def test_menu_without_selected_image_forwards_options(self):
        result = self.run_wrapper("--menu", "--scale", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertIn("--launcher", arguments)
        self.assertNotIn("--rom", arguments)
        self.assertEqual(arguments[-3:-1], ["--scale", "2"])
        self.assertIn(f"cwd={self.root}", arguments)

    def test_direct_launch_requires_caller_image_selection(self):
        (self.root / "unselected game.nds").touch()
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 1)
        self.assertIn("--rom PATH or DQ9_ROM", result.stderr)
        self.assertIn("--menu", result.stderr)

    def test_rom_environment_preserves_spaces_and_language(self):
        rom = self.root / "selected image with spaces.nds"
        rom.touch()
        self.env["DQ9_ROM"] = str(rom)
        result = self.run_wrapper("de")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertIn(str(rom), arguments)
        self.assertEqual(arguments.count("--rom"), 1)
        self.assertIn("--debug-tool", arguments)
        self.assertIn("de", arguments)
        self.assertIn(f"cwd={self.root}", arguments)

    def test_explicit_rom_and_save_override_environment_and_forward_options(self):
        rom = self.root / "chosen image with spaces.nds"
        rom.touch()
        save = self.root / "chosen saves/game.sav"
        self.env["DQ9_ROM"] = str(self.root / "unselected image.nds")
        options = ["--rom", str(rom), "--save", str(save), "--scale", "2"]
        result = self.run_wrapper(*options)
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertEqual(arguments[-7:-1], options)
        self.assertEqual(arguments.count("--rom"), 1)
        self.assertEqual(arguments.count("--save"), 1)
        self.assertNotIn(self.env["DQ9_ROM"], arguments)
        self.assertNotIn(self.env["DQ9_SAVE"], arguments)
        self.assertTrue(save.parent.is_dir())
        self.assertFalse(Path(self.env["DQ9_SAVE"]).parent.exists())

    def test_menu_accepts_environment_image_without_direct_start(self):
        self.env["DQ9_ROM"] = str(self.root / "selected image.nds")
        result = self.run_wrapper("--menu")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = result.stdout.splitlines()
        self.assertIn("--launcher", arguments)
        self.assertIn(self.env["DQ9_ROM"], arguments)
        self.assertNotIn("--debug-tool", arguments)

    def test_default_save_uses_product_directory(self):
        self.env.pop("DQ9_SAVE")
        result = self.run_wrapper("--rom", self.root / "selected image.nds")
        self.assertEqual(result.returncode, 0, result.stderr)
        save = self.root / "home/.local/share/dq9-decomp/dq9.sav"
        self.assertIn(str(save), result.stdout.splitlines())
        self.assertTrue(save.parent.is_dir())

    def test_missing_option_value_is_reported_before_launch(self):
        result = self.run_wrapper("--rom")
        self.assertEqual(result.returncode, 2)
        self.assertIn("requires a value", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_native_exit_status_is_preserved(self):
        self.env["FIXTURE_EXIT_CODE"] = "73"
        result = self.run_wrapper("--menu")
        self.assertEqual(result.returncode, 73)


class WindowsLauncherSourceTests(unittest.TestCase):
    def test_powershell_uses_explicit_image_selection_and_forwards_original_options(self):
        for name in ("run_game", "run_godot"):
            with self.subTest(name=name):
                source = (ROOT / "tools" / f"{name}.ps1").read_text()
                self.assertIn("$env:DQ9_ROM", source)
                self.assertIn("@('--rom', '--save')", source)
                self.assertIn("$commandArgs += $launchArgs", source)
                self.assertIn("--rom PATH or DQ9_ROM", source)
                self.assertNotIn("Get-ChildItem", source)
                self.assertIn("Push-Location $root", source)
                self.assertIn("Pop-Location", source)
                self.assertIn("exit $code", source)
                batch = (ROOT / "tools" / f"{name}.bat").read_text()
                self.assertIn(f'"%~dp0{name}.ps1" %*', batch)
                self.assertIn("exit /b %errorlevel%", batch)


class GodotSmokeScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tools").mkdir()
        (self.root / "godot").mkdir()
        (self.root / ".venv/bin").mkdir(parents=True)
        (self.root / "bin").mkdir()
        self.wrapper = self.root / "tools/godot_smoke.sh"
        shutil.copy2(ROOT / "tools/godot_smoke.sh", self.wrapper)
        self.log = self.root / "invocations.jsonl"
        self.engine = self._executable(self.root / "bin/godot", "godot", "FIXTURE_GODOT_STATUS")
        self.native = self._executable(self.root / "bin/native", "native", "FIXTURE_NATIVE_STATUS")
        self._executable(self.root / ".venv/bin/python", "checker", "FIXTURE_CHECKER_STATUS")
        display = self.root / "bin/xvfb-run"
        display.write_text(
            '#!/bin/sh\n[ "${1:-}" != "-a" ] || shift\n'
            'if [ "${1:-}" = "-s" ]; then shift 2; fi\nexec "$@"\n')
        display.chmod(0o755)
        self.rom = self.root / "selected image.nds"
        self.save = self.root / "source save.sav"
        self.inputs = self.root / "caller inputs.txt"
        self.rom.touch()
        self.save.write_bytes(b"save fixture")
        self.inputs.write_text("0 0 -1 -1\n")
        self.env = dict(os.environ, GODOT=str(self.engine), DQ9_NATIVE=str(self.native),
                        PATH=f"{self.root / 'bin'}:{os.environ['PATH']}",
                        TMPDIR=str(self.root))
        for name in ("DQ9_ROM", "DQ9_SAVE", "DQ9_REVIEW_OUTPUT"):
            self.env.pop(name, None)

    def _executable(self, path, role, status_key):
        path.write_text(
            f"#!{sys.executable}\nimport json, os, sys\nfrom pathlib import Path\n"
            f"with Path({str(self.log)!r}).open('a') as output:\n"
            f"    output.write(json.dumps({{'tool': {role!r}, 'args': sys.argv[1:], "
            "'cwd': os.getcwd()}) + '\\n')\n"
            "if 'res://tests/smoke.gd' in sys.argv:\n"
            "    status = int(os.environ.get('FIXTURE_SMOKE_STATUS', '0'))\n"
            "else:\n"
            f"    status = int(os.environ.get({status_key!r}, '0'))\n"
            "sys.exit(status)\n")
        path.chmod(0o755)
        return path

    def run_wrapper(self, *args):
        return subprocess.run(["bash", str(self.wrapper), *map(str, args)],
                              env=self.env, cwd=self.root, text=True,
                              capture_output=True, check=False)

    def options(self):
        return ["--rom", str(self.rom), "--save", str(self.save),
                "--input", str(self.inputs), "--frames", "12", "--checkpoints", "2,8"]

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_replay_paths_frames_and_checkpoints_reach_both_gpu_tests(self):
        result = self.run_wrapper(*self.options())
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        scene = next(call["args"] for call in calls
                     if "res://tests/test_gpu_scene.gd" in call["args"])
        scene_args = scene[scene.index("--") + 1:]
        self.assertEqual(len(scene_args), 6)
        self.assertEqual(scene_args[0], str(self.rom))
        self.assertEqual(scene_args[2], str(self.inputs))
        self.assertEqual(scene_args[4:], ["12", "2,8"])
        self.assertNotEqual(scene_args[1], str(self.save))
        visible = next(call["args"] for call in calls
                       if "res://tests/test_gpu_visible.gd" in call["args"])
        self.assertEqual(visible[visible.index("--app-checkpoints") + 1], "2,8")
        self.assertEqual(visible[visible.index("--app-input") + 1], str(self.inputs))
        self.assertEqual(visible[visible.index("--rom") + 1], str(self.rom))
        self.assertNotEqual(visible[visible.index("--save") + 1], scene_args[1])
        native = next(call["args"] for call in calls if call["tool"] == "native")
        self.assertEqual(native[native.index("--frames") + 1], "12")
        self.assertEqual(native[native.index("--input-script") + 1], str(self.inputs))
        self.assertNotEqual(native[native.index("--save") + 1], str(self.save))
        checker = next(call["args"] for call in calls if call["tool"] == "checker")
        self.assertEqual(checker[-2:], ["--frames", "12"])
        self.assertEqual(self.save.read_bytes(), b"save fixture")

    def test_rom_and_save_environment_and_positional_compatibility(self):
        self.env["DQ9_ROM"] = str(self.rom)
        self.env["DQ9_SAVE"] = str(self.save)
        result = self.run_wrapper("--input", self.inputs, "--frames", "12",
                                  "--checkpoints", "2,8")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_wrapper(self.rom, self.save, self.inputs, "12",
                                  "--checkpoints", "2,8")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_or_out_of_range_checkpoints_are_rejected_before_launch(self):
        for extra in ((), ("--checkpoints", "12"), ("--checkpoints", "0,2")):
            with self.subTest(extra=extra):
                result = self.run_wrapper(*self.options()[:-2], *extra)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("checkpoint", result.stderr.lower())
                self.assertFalse(self.log.exists())

    def test_missing_caller_paths_are_rejected_before_launch(self):
        result = self.run_wrapper("--checkpoints", "2")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--rom/DQ9_ROM", result.stderr)
        self.assertFalse(self.log.exists())

    def test_checker_failure_takes_precedence_over_smoke_failure(self):
        self.env["FIXTURE_SMOKE_STATUS"] = "74"
        self.env["FIXTURE_CHECKER_STATUS"] = "73"
        result = self.run_wrapper(*self.options())
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertTrue(any(call["tool"] == "checker" for call in self.calls()))

    def test_smoke_failure_is_propagated_when_checker_succeeds(self):
        self.env["FIXTURE_SMOKE_STATUS"] = "74"
        result = self.run_wrapper(*self.options())
        self.assertEqual(result.returncode, 74, result.stderr)

    def test_native_failure_stops_before_hash_checker(self):
        self.env["FIXTURE_NATIVE_STATUS"] = "73"
        result = self.run_wrapper(*self.options())
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Native DQ9-Decomp failed", result.stderr)
        self.assertFalse(any(call["tool"] == "checker" for call in self.calls()))


if __name__ == "__main__":
    unittest.main()
