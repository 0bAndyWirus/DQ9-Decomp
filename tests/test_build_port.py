"""ROM-free orchestration checks; no compiler, SDK or network is invoked."""
import hashlib
import io
import os
from pathlib import Path
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import build_port


class BuildPortTests(unittest.TestCase):
    def windows_os(self):
        return SimpleNamespace(name="nt", environ=os.environ, pathsep=os.pathsep)

    def test_run_uses_interpreter_tools_and_preserves_environment(self):
        with mock.patch.object(build_port.subprocess, "run") as run:
            build_port.run(sys.executable, "-m", "cmake", "--version", env={"SDL_VIDEODRIVER": "dummy"})
        command = run.call_args.args[0]
        self.assertEqual(command, [sys.executable, "-m", "cmake", "--version"])
        env = run.call_args.kwargs["env"]
        self.assertEqual(env["PATH"].split(os.pathsep)[0], str(Path(sys.executable).parent))
        self.assertEqual(env["SDL_VIDEODRIVER"], "dummy")
        self.assertTrue(run.call_args.kwargs["check"])

    def test_runtime_only_needs_no_rom_or_dsd_and_never_configures_game(self):
        with mock.patch.object(build_port, "run") as run, \
                mock.patch.object(build_port, "windows_cmake_options", return_value=[]) as options, \
                mock.patch.object(build_port, "fetch_dsd") as fetch, \
                mock.patch.object(build_port, "ctest_executable", return_value="selected-ctest"):
            self.assertEqual(build_port.main(["--runtime-only"]), 0)
        fetch.assert_not_called()
        options.assert_called_once()
        configure, build, test = run.call_args_list
        self.assertEqual(configure.args[:3], (sys.executable, "-m", "cmake"))
        self.assertIn(build_port.ROOT / "build/win-zig", configure.args)
        self.assertIn("-UDQ9_GENERATED_DIR", configure.args)
        self.assertFalse(any(str(arg).startswith("-DDQ9_GENERATED_DIR=") for arg in configure.args))
        self.assertIn("-DDQ9_BUILD_SDL=ON", configure.args)
        self.assertIn("-DDQ9_BUILD_GODOT=OFF", configure.args)
        self.assertEqual(build.args, (sys.executable, "-m", "cmake", "--build", build_port.ROOT / "build/win-zig"))
        self.assertEqual(test.args[0], "selected-ctest")
        self.assertEqual(test.kwargs["env"]["SDL_VIDEODRIVER"], "dummy")
        self.assertEqual(test.kwargs["env"]["SDL_AUDIODRIVER"], "dummy")

    def test_rom_is_still_required_for_game_generation(self):
        with self.assertRaises(SystemExit) as error:
            build_port.main([])
        self.assertEqual(error.exception.code, 2)

    def test_existing_verified_dsd_is_reused_and_mismatch_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "dsd-linux-x86_64"
            destination = root / "build/tools" / asset
            destination.parent.mkdir(parents=True)
            destination.write_bytes(b"synthetic test executable")
            digest = hashlib.sha256(destination.read_bytes()).hexdigest()
            with mock.patch.object(build_port, "ROOT", root), \
                    mock.patch.object(build_port.sys, "platform", "linux"), \
                    mock.patch.object(build_port.platform, "machine", return_value="x86_64"), \
                    mock.patch.dict(build_port.DSD_SHA256, {asset: digest}), \
                    mock.patch.object(build_port.urllib.request, "urlretrieve") as download:
                self.assertEqual(build_port.fetch_dsd(), destination)
                download.assert_not_called()
                self.assertTrue(destination.stat().st_mode & 0o111)
                destination.write_bytes(b"tampered test fixture")
                with self.assertRaisesRegex(SystemExit, "SHA-256 mismatch"):
                    build_port.fetch_dsd()
                download.assert_not_called()

    def test_download_is_verified_before_installing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = b"synthetic verified download"
            asset = "dsd-linux-x86_64"
            def download(url, destination):
                Path(destination).write_bytes(data)
            with mock.patch.object(build_port, "ROOT", root), \
                    mock.patch.object(build_port.sys, "platform", "linux"), \
                    mock.patch.object(build_port.platform, "machine", return_value="x86_64"), \
                    mock.patch.dict(build_port.DSD_SHA256, {asset: hashlib.sha256(data).hexdigest()}), \
                    mock.patch.object(build_port.urllib.request, "urlretrieve", side_effect=download):
                self.assertEqual(build_port.fetch_dsd().read_bytes(), data)
            self.assertFalse(list((root / "build/tools").glob("*.download")))

    def test_sdk_archive_refuses_traversal_and_escaping_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "sdk"
            destination.mkdir()
            for name, link in (("../escaped", None), ("sdk-link", "../../escaped")):
                archive = root / "archive.tar.gz"
                with tarfile.open(archive, "w:gz") as source:
                    member = tarfile.TarInfo(name)
                    if link:
                        member.type = tarfile.SYMTYPE
                        member.linkname = link
                        source.addfile(member)
                    else:
                        member.size = 1
                        source.addfile(member, io.BytesIO(b"x"))
                with self.assertRaises(ValueError):
                    build_port.extract_sdk(archive, destination)
                self.assertFalse((root / "escaped").exists())

    def test_windows_sdk_flags_and_zig_install_only_if_missing(self):
        with tempfile.TemporaryDirectory() as directory:
            deps = Path(directory)
            root = deps / f"SDL2-{build_port.SDL_VERSION}/x86_64-w64-mingw32"
            (root / "bin").mkdir(parents=True)
            (root / "bin/SDL2.dll").write_bytes(b"synthetic SDK fixture")
            with mock.patch.object(build_port, "os", self.windows_os()), \
                    mock.patch.object(build_port.importlib, "import_module") as import_zig, \
                    mock.patch.object(build_port, "run") as run, \
                    mock.patch.object(build_port.urllib.request, "urlretrieve") as download:
                options = build_port.windows_cmake_options(deps)
                run.assert_not_called()
                download.assert_not_called()
                self.assertIn(f"-DSDL2_DIR={root / 'lib/cmake/SDL2'}", options)
                self.assertIn(f"-DDSR_SDL2_RUNTIME_DLL={root / 'bin/SDL2.dll'}", options)
                self.assertIn(f"-DPython3_EXECUTABLE={sys.executable}", options)
                import_zig.side_effect = ImportError("synthetic missing module")
                build_port.windows_cmake_options(deps)
                run.assert_called_once_with(sys.executable, "-m", "pip", "install", "ziglang==0.17.0")
                self.assertEqual(build_port.tool_environment()["DQ9_ZIG_PYTHON"], sys.executable)

    def test_launch_hints_do_not_enable_debug_tool(self):
        args = [Path("a path/dq9"), Path("my game.nds"), Path("abi.ini")]
        posix = build_port.launch_hint(*args)
        self.assertNotIn("--debug-tool", posix)
        with mock.patch.object(build_port, "os", self.windows_os()):
            windows = build_port.launch_hint(*args)
        self.assertTrue(windows.startswith('& "a path/dq9"'))
        self.assertIn('"my game.nds"', windows)
        self.assertNotIn("--debug-tool", windows)


if __name__ == "__main__":
    unittest.main()
