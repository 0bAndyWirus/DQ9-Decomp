import json
import os
import subprocess
import sys
import shutil
import zipfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ("tools/build_windows.sh", "tools/build_deck.sh", "tools/build_both.sh")


class BuildScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.tmp = Path(self.temp.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.marker = self.tmp / "docker-called"
        docker = self.bin / "docker"
        docker.write_text(f"#!/bin/sh\necho called >> '{self.marker}'\nexit 73\n")
        docker.chmod(0o755)
        self.env = {**os.environ, "PATH": f"{self.bin}:{os.environ['PATH']}"}
        for name in ("DQ9_GEN", "DQ9_ROM", "DQ9_SAVE", "JOBS"):
            self.env.pop(name, None)
        self.gen = self.tmp / "gen"
        for cpu in ("arm9", "arm7"):
            directory = self.gen / cpu
            directory.mkdir(parents=True)
            (directory / "fn_table.cpp").write_text("// synthetic\n")

    def tearDown(self):
        self.temp.cleanup()

    def run_script(self, script, *args):
        return subprocess.run(["bash", str(ROOT / script), *map(str, args)], cwd=ROOT,
                              env=self.env, text=True, capture_output=True, check=False)

    def test_jobs_must_be_positive_integer_before_docker(self):
        for script in SCRIPTS:
            with self.subTest(script=script):
                result = self.run_script(script, "--jobs", "0")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("positive integer", result.stderr)
                self.assertFalse(self.marker.exists())

    def test_smoke_rom_is_checked_before_docker(self):
        for options in (("--smoke", self.tmp / "missing.nds"),
                        ("--smoke", "--rom", self.tmp / "missing.nds")):
            with self.subTest(options=options):
                result = self.run_script("tools/build_deck.sh", "--gen", self.gen, *options)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("smoke ROM not found", result.stderr)
                self.assertFalse(self.marker.exists())

    def test_smoke_uses_rom_environment_before_docker(self):
        self.env["DQ9_ROM"] = str(self.tmp / "missing.nds")
        result = self.run_script("tools/build_deck.sh", "--gen", self.gen, "--smoke")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("smoke ROM not found", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_smoke_requires_an_explicit_image_selection(self):
        result = self.run_script("tools/build_deck.sh", "--gen", self.gen, "--smoke")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--rom PATH or DQ9_ROM", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_generated_directory_must_be_supplied_before_build(self):
        for script in SCRIPTS:
            with self.subTest(script=script):
                result = self.run_script(script)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("--gen DIR or DQ9_GEN", result.stderr)
                self.assertFalse(self.marker.exists())

    def test_missing_generated_tables_are_refused_before_build(self):
        incomplete = self.tmp / "incomplete code"
        (incomplete / "arm9").mkdir(parents=True)
        (incomplete / "arm9/fn_table.cpp").write_text("// test source\n")
        for script in SCRIPTS:
            with self.subTest(script=script):
                result = self.run_script(script, "--gen", incomplete)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("arm7/fn_table.cpp", result.stderr)
                self.assertFalse(self.marker.exists())

    def test_deck_accepts_supplied_sources_without_manifests_or_timestamp_rules(self):
        for cpu in ("arm9", "arm7"):
            os.utime(self.gen / cpu / "fn_table.cpp", (1, 1))
        result = self.run_script("tools/build_deck.sh", "--gen", self.gen)
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertTrue(self.marker.exists())
        self.assertIn(str(self.gen), result.stdout)

    def test_deck_generated_directory_environment_and_explicit_override(self):
        self.env["DQ9_GEN"] = str(self.gen)
        result = self.run_script("tools/build_deck.sh")
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertTrue(self.marker.exists())
        self.env["DQ9_GEN"] = str(self.tmp / "missing code")
        result = self.run_script("tools/build_deck.sh", "--gen", self.gen)
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertIn(str(self.gen), result.stdout)

    def test_unused_smoke_image_does_not_change_build(self):
        self.env["DQ9_ROM"] = str(self.tmp / "missing.nds")
        result = self.run_script("tools/build_deck.sh", "--gen", self.gen)
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertTrue(self.marker.exists())

    def test_packaging_scripts_propagate_failure_and_audit_packages(self):
        deck = (ROOT / "tools/build_deck.sh").read_text()
        package = (ROOT / "tools/package_windows.sh").read_text()
        self.assertIn('container_status" -ne 0', deck)
        self.assertIn('native_status" -ne 0', deck)
        self.assertIn('tools/audit_package.py" "${audit_paths[@]}"', deck)
        self.assertIn("'audit_package.py'", package)
        self.assertIn("check=True", package)
        self.assertIn("dq9-decomp-win64", package)

    def test_windows_rejects_retired_container_smoke_option(self):
        result = self.run_script("tools/build_windows.sh", "--smoke", "missing.nds")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown argument", result.stderr)
        self.assertFalse(self.marker.exists())

    def _windows_fixture(self):
        root = self.tmp / "Windows build with spaces"
        (root / "tools").mkdir(parents=True)
        shutil.copy2(ROOT / "tools/build_windows.sh", root / "tools/build_windows.sh")
        binaries = root / ".venv/bin"
        binaries.mkdir(parents=True)
        python = binaries / "python"
        python.write_text("#!/bin/sh\nexit 0\n")
        python.chmod(0o755)
        cmake = binaries / "cmake"
        cmake.write_text(
            f'#!/bin/sh\nprintf "%s\\n" "$@" >> "{root / "cmake-args"}"\n'
            'exit "${FAKE_CMAKE_STATUS:-0}"\n')
        cmake.chmod(0o755)
        generated = root / "gen"
        for cpu in ("arm9", "arm7"):
            directory = generated / cpu
            directory.mkdir(parents=True)
            table = directory / "fn_table.cpp"
            table.write_text("// test source\n")
            os.utime(table, (1, 1))
        sdk = root / "SDL SDK"
        (sdk / "bin").mkdir(parents=True)
        (sdk / "bin/SDL2.dll").write_bytes(b"test SDK fixture")
        return root, generated, sdk

    def test_windows_sdl_uses_shared_verified_download_and_safe_extraction(self):
        script = (ROOT / "tools/build_windows.sh").read_text()
        self.assertIn("download_verified(SDL_URL, archive, \"sha256\", SDL_SHA256)", script)
        self.assertIn("extract_sdk(archive, deps)", script)
        self.assertIn('PYTHONPATH="$ROOT${PYTHONPATH:+:$PYTHONPATH}"', script)
        self.assertNotIn('tar -xzf "$archive"', script)
        self.assertNotIn('curl -fL', script)

    def test_windows_zig_configuration_preserves_paths_with_spaces(self):
        root, generated, sdk = self._windows_fixture()
        result = subprocess.run(
            ["bash", str(root / "tools/build_windows.sh"), "--gen", str(generated),
             "--sdl-root", str(sdk), "--no-ccache", "--jobs", "3"],
            env={**self.env, "DQ9_ZIG_PYTHON": str(root / ".venv/bin/python")},
            text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = (root / "cmake-args").read_text().splitlines()
        self.assertIn(f"-DDQ9_GENERATED_DIR={generated}", arguments)
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={root}/cmake/toolchains/x86_64-windows-zig.cmake", arguments)
        self.assertIn(f"-DDSR_SDL2_RUNTIME_DLL={sdk}/bin/SDL2.dll", arguments)
        self.assertIn("-DDQ9_BUILD_GODOT=OFF", arguments)
        self.assertEqual(arguments[-4:], ["--build", str(root / "build/win-sdl"), "-j", "3"])
        self.assertFalse(self.marker.exists())

    def test_windows_propagates_cmake_failure(self):
        root, generated, sdk = self._windows_fixture()
        result = subprocess.run(
            ["bash", str(root / "tools/build_windows.sh"), "--gen", str(generated),
             "--sdl-root", str(sdk), "--no-ccache"],
            env={**self.env, "DQ9_ZIG_PYTHON": str(root / ".venv/bin/python"),
                 "FAKE_CMAKE_STATUS": "73"},
            text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 73, result.stderr)

    def test_windows_generated_directory_environment_and_explicit_override(self):
        root, generated, sdk = self._windows_fixture()
        environment = {**self.env, "DQ9_GEN": str(generated),
                       "DQ9_ZIG_PYTHON": str(root / ".venv/bin/python")}
        command = ["bash", str(root / "tools/build_windows.sh"),
                   "--sdl-root", str(sdk), "--no-ccache"]
        result = subprocess.run(command, env=environment, text=True, capture_output=True,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"-DDQ9_GENERATED_DIR={generated}",
                      (root / "cmake-args").read_text().splitlines())
        environment["DQ9_GEN"] = str(self.tmp / "missing code")
        result = subprocess.run(command + ["--gen", str(generated)], env=environment,
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_combined_build_requires_and_forwards_supplied_sources(self):
        root, generated, _ = self._windows_fixture()
        shutil.copy2(ROOT / "tools/build_both.sh", root / "tools/build_both.sh")
        bindings = root / "Godot bindings"
        bindings.mkdir()
        (bindings / "CMakeLists.txt").touch()
        result = subprocess.run(
            ["bash", str(root / "tools/build_both.sh"), "--gen", str(generated),
             "--jobs", "3"],
            env={**self.env, "GODOT_CPP_DIR": str(bindings),
                 "DQ9_GEN": str(self.tmp / "unselected code")},
            text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = (root / "cmake-args").read_text().splitlines()
        self.assertEqual(arguments.count(f"-DDQ9_GENERATED_DIR={generated}"), 2)
        self.assertIn(f"-DGODOT_CPP_DIR={bindings}", arguments)
        self.assertEqual(arguments[-4:], ["--build", str(root / "build/godot-ext"), "-j", "3"])
        self.assertIn("--menu", result.stdout)

    def test_combined_godot_test_requires_caller_replay_choices_before_build(self):
        root, generated, _ = self._windows_fixture()
        shutil.copy2(ROOT / "tools/build_both.sh", root / "tools/build_both.sh")
        result = subprocess.run(
            ["bash", str(root / "tools/build_both.sh"), "--godot-only",
             "--gen", str(generated), "--test"],
            env=self.env, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--checkpoints", result.stderr)
        self.assertFalse((root / "cmake-args").exists())

    def test_combined_build_accepts_generated_directory_environment(self):
        root, generated, _ = self._windows_fixture()
        shutil.copy2(ROOT / "tools/build_both.sh", root / "tools/build_both.sh")
        result = subprocess.run(
            ["bash", str(root / "tools/build_both.sh"), "--sdl-only", "--jobs", "2"],
            env={**self.env, "DQ9_GEN": str(generated)}, text=True,
            capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = (root / "cmake-args").read_text().splitlines()
        self.assertEqual(arguments.count(f"-DDQ9_GENERATED_DIR={generated}"), 1)
        self.assertEqual(arguments[-4:], ["--build", str(root / "build/game-ui"), "-j", "2"])

    def test_combined_godot_test_forwards_explicit_replay_choices_and_failure(self):
        root, generated, _ = self._windows_fixture()
        shutil.copy2(ROOT / "tools/build_both.sh", root / "tools/build_both.sh")
        smoke = root / "tools/godot_smoke.sh"
        smoke.write_text(
            f'#!/bin/sh\nprintf "%s\\n" "$@" > "{root / "smoke-args"}"\nexit 74\n')
        smoke.chmod(0o755)
        bindings = root / "Godot bindings"
        bindings.mkdir()
        (bindings / "CMakeLists.txt").touch()
        choices = ["--rom", str(root / "selected image.nds"),
                   "--save", str(root / "source save.sav"),
                   "--input", str(root / "caller inputs.txt"),
                   "--frames", "12", "--checkpoints", "2,8"]
        result = subprocess.run(
            ["bash", str(root / "tools/build_both.sh"), "--godot-only",
             "--gen", str(generated), "--test", *choices],
            env={**self.env, "GODOT_CPP_DIR": str(bindings),
                 "DQ9_ROM": str(root / "unselected image.nds")},
            text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 74, result.stderr)
        self.assertEqual((root / "smoke-args").read_text().splitlines(), choices)

    def _recording_executable(self, path, log, role, status_key):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f"#!{sys.executable}\nimport json, os, sys\nfrom pathlib import Path\n"
            f"with Path({str(log)!r}).open('a') as output:\n"
            f"    output.write(json.dumps({{'tool': {role!r}, 'args': sys.argv[1:], "
            "'cwd': os.getcwd()}) + '\\n')\n"
            f"if {role!r} != 'docker' and '--hash-file' in sys.argv:\n"
            "    Path(sys.argv[sys.argv.index('--hash-file') + 1]).write_text('hash fixture\\n')\n"
            f"if {role!r} != 'docker' and '--save' in sys.argv:\n"
            "    Path(sys.argv[sys.argv.index('--save') + 1]).write_bytes(b'updated save fixture')\n"
            f"if {role!r} == 'docker':\n"
            "    status = int(os.environ.get('FIXTURE_CONTAINER_STATUS', '0')) "
            "if sys.argv[1] == 'run' and 'fixture-runtime' in sys.argv else 0\n"
            "else:\n"
            f"    status = int(os.environ.get({status_key!r}, '0'))\n"
            "sys.exit(status)\n")
        path.chmod(0o755)
        return path

    def _deck_fixture(self):
        root, generated, _ = self._windows_fixture()
        shutil.copy2(ROOT / "tools/build_deck.sh", root / "tools/build_deck.sh")
        bundle = root / "build/deck/dq9-deck"
        bundle.mkdir(parents=True)
        log = root / "invocations.jsonl"
        self._recording_executable(root / ".venv/bin/docker", log, "docker", "")
        self._recording_executable(bundle / "dq9.sh", log, "native", "FIXTURE_NATIVE_STATUS")
        rom = root / "selected image with spaces.nds"
        rom.touch()
        environment = {**self.env, "PATH": f"{root / '.venv/bin'}:{self.env['PATH']}",
                       "DQ9_DECK_SDK_IMAGE": "fixture-sdk",
                       "DQ9_DECK_RUNTIME_IMAGE": "fixture-runtime", "TMPDIR": str(root)}
        return root, generated, rom, log, environment

    def test_deck_smoke_forwards_selected_image_and_preserves_spaces(self):
        root, generated, rom, log, environment = self._deck_fixture()
        environment["DQ9_ROM"] = str(root / "unselected image.nds")
        result = subprocess.run(
            ["bash", str(root / "tools/build_deck.sh"), "--gen", str(generated),
             "--smoke", "--rom", str(rom)], env=environment,
            text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        runtime = next(call["args"] for call in calls
                       if call["tool"] == "docker" and call["args"][0] == "run"
                       and "fixture-runtime" in call["args"])
        self.assertIn(f"{rom}:/rom.nds:ro", runtime)
        self.assertIn(f"{root}/build/deck/dq9-deck:/opt/dq9-deck:ro", runtime)
        self.assertEqual(runtime[-7:],
                         ["--rom", "/rom.nds", "--save", "/tmp/smoke.sav",
                          "--headless", "--frames", "5"])
        native = next(call["args"] for call in calls if call["tool"] == "native")
        self.assertEqual(native[native.index("--rom") + 1], str(rom))
        self.assertEqual(native[-3:], ["--headless", "--frames", "5"])
        self.assertNotIn(environment["DQ9_ROM"], native)

    def test_deck_smoke_propagates_container_and_native_failures(self):
        root, generated, rom, log, environment = self._deck_fixture()
        environment["DQ9_ROM"] = str(rom)
        for container_status, native_status, expected in ((73, 0, 73), (0, 74, 74), (73, 74, 73)):
            with self.subTest(container=container_status, native=native_status):
                environment["FIXTURE_CONTAINER_STATUS"] = str(container_status)
                environment["FIXTURE_NATIVE_STATUS"] = str(native_status)
                result = subprocess.run(
                    ["bash", str(root / "tools/build_deck.sh"),
                     "--gen", str(generated), "--smoke"], env=environment,
                    text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertIn(f"container exit status: {container_status}", result.stdout)
                self.assertIn(f"native exit status: {native_status}", result.stdout)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(sum(call["tool"] == "native" for call in calls), 3)

    def _render_gate_fixture(self):
        root, generated, _ = self._windows_fixture()
        shutil.copy2(ROOT / "tools/render_gate.sh", root / "tools/render_gate.sh")
        ctest = root / ".venv/bin/ctest"
        ctest.write_text("#!/bin/sh\nexit 0\n")
        ctest.chmod(0o755)
        build = root / "build/game-ui"
        log = root / "invocations.jsonl"
        self._recording_executable(build / "game/dq9", log, "native", "FIXTURE_NATIVE_STATUS")
        (build / "CMakeCache.txt").write_text(f"DQ9_GENERATED_DIR:PATH={generated}\n")
        rom = root / "selected image.nds"
        save = root / "source save.sav"
        inputs = root / "caller inputs.txt"
        baseline = root / "caller baseline.txt"
        rom.touch()
        save.write_bytes(b"source save fixture")
        inputs.write_text("0 0 -1 -1\n")
        baseline.write_text("hash fixture\n")
        environment = {**self.env, "DQ9_ROM": str(rom), "DQ9_SAVE": str(save),
                       "TMPDIR": str(root)}
        options = [str(build), "--input", str(inputs), "--frames", "10",
                   "--baseline", str(baseline), "--long-frames", "20",
                   "--long-baseline", str(baseline)]
        return root, rom, save, inputs, log, environment, options

    def test_render_gate_uses_caller_replay_lengths_and_preserves_source_save(self):
        root, rom, save, inputs, log, environment, options = self._render_gate_fixture()
        result = subprocess.run(["sh", str(root / "tools/render_gate.sh"), *options],
                                env=environment, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual([call["args"][call["args"].index("--frames") + 1] for call in calls],
                         ["10", "20"])
        for call in calls:
            arguments = call["args"]
            self.assertEqual(arguments[arguments.index("--rom") + 1], str(rom))
            self.assertEqual(arguments[arguments.index("--input-script") + 1], str(inputs))
            self.assertNotEqual(arguments[arguments.index("--save") + 1], str(save))
            self.assertEqual(call["cwd"], str(root))
        self.assertEqual(save.read_bytes(), b"source save fixture")
        self.assertIn("GATE OK", result.stdout)

    def test_render_gate_quick_skips_only_the_supplied_longer_comparison(self):
        root, _, _, _, log, environment, options = self._render_gate_fixture()
        result = subprocess.run(["sh", str(root / "tools/render_gate.sh"), *options, "--quick"],
                                env=environment, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["args"][calls[0]["args"].index("--frames") + 1], "10")

    def test_render_gate_requires_caller_replay_configuration_before_build(self):
        result = self.run_script("tools/render_gate.sh")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--rom/DQ9_ROM", result.stderr)
        self.assertIn("--frames and --baseline", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_windows_package_whitelist_excludes_untracked_user_data(self):
        root = self.tmp / "package source"
        for directory in ("tools", "packaging/windows", "docs", "godot/scripts"):
            (root / directory).mkdir(parents=True)
        for path in ("tools/package_windows.sh", "tools/audit_package.py",
                     "tools/run_game.bat", "tools/run_game.ps1",
                     "tools/run_godot.bat", "tools/run_godot.ps1",
                     "packaging/windows/README-WINDOWS.txt", "docs/BUILDING_WINDOWS.md"):
            shutil.copy2(ROOT / path, root / path)
        (root / ".venv/bin").mkdir(parents=True)
        (root / ".venv/bin/python").symlink_to(sys.executable)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        (root / "godot/project.godot").write_text("[application]\n")
        (root / "godot/dq9.gdextension").write_text("[libraries]\n")
        (root / "godot/scripts/main.gd").write_text("extends Node\n")
        subprocess.run(["git", "-C", str(root), "add", "godot"], check=True)
        # Even tracked image/save fixtures are excluded; extra resources aren't copied.
        (root / "godot/scripts/excluded.nds").write_bytes(b"image test fixture")
        (root / "godot/scripts/excluded.sav").write_bytes(b"save test fixture")
        subprocess.run(["git", "-C", str(root), "add", "godot/scripts/excluded.nds",
                        "godot/scripts/excluded.sav"], check=True)
        (root / "godot/scripts/untracked.gd").write_text("extends Node\n")
        (root / "godot/bin").mkdir()
        for name in ("dq9_godot.windows.template_release.x86_64.dll", "dsr_session.dll"):
            (root / "godot/bin" / name).write_bytes(b"binary test fixture")
        bindings = self.tmp / "bindings"
        bindings.mkdir()
        (bindings / "LICENSE.md").write_text("license fixture\n")
        sdk = self.tmp / "SDK/x64"
        (sdk / "bin").mkdir(parents=True)
        (sdk.parent / "LICENSE.txt").write_text("license fixture\n")
        (sdk / "bin/SDL2.dll").write_bytes(b"binary test fixture")
        build = root / "build/win-sdl"
        (build / "game").mkdir(parents=True)
        for name in ("dq9.exe", "SDL2.dll"):
            (build / "game" / name).write_bytes(b"binary test fixture")
        (build / "CMakeCache.txt").write_text(f"DSR_SDL2_RUNTIME_DLL:FILEPATH={sdk}/bin/SDL2.dll\n")
        result = subprocess.run(["bash", str(root / "tools/package_windows.sh")],
                                env={**self.env, "GODOT_CPP_DIR": str(bindings)},
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(root / "build/dist/dq9-decomp-win64.zip") as archive:
            names = archive.namelist()
        self.assertIn("dq9-decomp-win64/godot/scripts/main.gd", names)
        self.assertIn("dq9-decomp-win64/dq9.exe", names)
        self.assertFalse(any("excluded" in path or "untracked" in path for path in names), names)
        user_save = root / "build/dist/dq9-decomp-win64/dq9.sav"
        user_save.write_bytes(b"keep this user save")
        result = subprocess.run(["bash", str(root / "tools/package_windows.sh")],
                                env={**self.env, "GODOT_CPP_DIR": str(bindings)},
                                text=True, capture_output=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("user/unknown files", result.stderr)
        self.assertEqual(user_save.read_bytes(), b"keep this user save")
