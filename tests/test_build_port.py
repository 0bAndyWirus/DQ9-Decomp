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
                run.assert_called_once_with(sys.executable, "-m", "pip", "install", "ziglang==0.16.0")
                self.assertEqual(build_port.tool_environment()["DQ9_ZIG_PYTHON"], sys.executable)

    def test_sdl_download_rejects_tampering_before_extraction(self):
        with tempfile.TemporaryDirectory() as directory:
            deps = Path(directory)
            def download(url, destination):
                Path(destination).write_bytes(b"tampered SDL SDK")
            with mock.patch.object(build_port, "os", self.windows_os()), \
                    mock.patch.object(build_port.importlib, "import_module"), \
                    mock.patch.object(build_port.urllib.request, "urlretrieve", side_effect=download), \
                    mock.patch.object(build_port, "extract_sdk") as extract:
                with self.assertRaisesRegex(SystemExit, "SHA256 mismatch"):
                    build_port.windows_cmake_options(deps)
                extract.assert_not_called()
            self.assertFalse(list(deps.glob("*.download")))
            self.assertFalse(list(deps.glob("*.tar.gz")))

    def test_sdl_download_uses_pinned_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            deps = Path(directory)
            with mock.patch.object(build_port, "os", self.windows_os()), \
                    mock.patch.object(build_port.importlib, "import_module"), \
                    mock.patch.object(build_port, "download_verified") as download, \
                    mock.patch.object(build_port, "extract_sdk") as extract:
                build_port.windows_cmake_options(deps)
            archive = deps.resolve() / f"SDL2-devel-{build_port.SDL_VERSION}-mingw.tar.gz"
            download.assert_called_once_with(build_port.SDL_URL, archive, "sha256", build_port.SDL_SHA256)
            extract.assert_called_once_with(archive, deps.resolve())

    def test_switching_images_requires_a_clean_workspace(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            workspace, generated = root / "workspace", root / "gen"
            rom_a, rom_b = root / "a.nds", root / "b.nds"
            rom_a.write_bytes(b"image A" * 100)
            rom_b.write_bytes(b"image B" * 100)
            # First build: nothing to compare against yet.
            digest_a = build_port.ensure_workspace_matches_rom(workspace, rom_a, False, generated)
            (workspace / "config/arm9").mkdir(parents=True)
            (workspace / "config/arm9/config.yaml").write_text("x", encoding="utf-8")
            (workspace / build_port.WORKSPACE_MARKER).write_text(digest_a + "\n", encoding="utf-8")
            # Same image again: accepted. Another image: refused with the recovery command.
            self.assertEqual(build_port.ensure_workspace_matches_rom(workspace, rom_a, False, generated), digest_a)
            with self.assertRaises(SystemExit) as refused:
                build_port.ensure_workspace_matches_rom(workspace, rom_b, False, generated)
            self.assertIn("--rebuild-workspace", str(refused.exception))
            self.assertIn("different image", str(refused.exception))
            # A workspace from an older script version has no marker: also refused, never guessed.
            (workspace / build_port.WORKSPACE_MARKER).unlink()
            with self.assertRaises(SystemExit) as unknown:
                build_port.ensure_workspace_matches_rom(workspace, rom_a, False, generated)
            self.assertIn("no record", str(unknown.exception))
            # --rebuild-workspace clears the extraction and the generated code of the old image.
            generated.mkdir()
            (generated / "chunk_999.cpp").write_text("old", encoding="utf-8")
            # A custom workspace without our fingerprint is not provably ours: even --rebuild refuses.
            with self.assertRaises(SystemExit):
                build_port.ensure_workspace_matches_rom(workspace, rom_b, True, generated)
            self.assertTrue((workspace / "config/arm9/config.yaml").is_file())
            (workspace / build_port.WORKSPACE_MARKER).write_text(digest_a + "\n", encoding="utf-8")
            digest_b = build_port.ensure_workspace_matches_rom(workspace, rom_b, True, generated)
            self.assertNotEqual(digest_a, digest_b)
            self.assertFalse((workspace / "config").exists())
            self.assertTrue(generated.exists())  # only this checkout's own build/gen is ever removed

    def test_clearing_refuses_user_directories_and_keeps_foreign_files(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rom = root / "stuff" / "game.nds"
            rom.parent.mkdir()
            rom.write_bytes(b"image")
            generated = root / "gen"
            for dangerous in (build_port.ROOT, build_port.ROOT.parent, Path(build_port.ROOT.anchor), rom.parent):
                with self.assertRaises(SystemExit, msg=str(dangerous)):
                    build_port.clear_workspace(dangerous, rom, generated)
            self.assertTrue(rom.exists() and build_port.ROOT.exists())
            foreign = root / "other-project"
            for part in ("config", "build"):
                (foreign / part).mkdir(parents=True)
                (foreign / part / "keep.txt").write_text("mine", encoding="utf-8")
            with self.assertRaises(SystemExit) as refused:
                build_port.clear_workspace(foreign, rom, generated)
            self.assertIn("empty directory", str(refused.exception))
            self.assertTrue((foreign / "config/keep.txt").is_file() and (foreign / "build/keep.txt").is_file())
            workspace = root / "ws"
            for part in (*build_port.WORKSPACE_PARTS, "notes"):
                (workspace / part).mkdir(parents=True)
            (workspace / build_port.WORKSPACE_MARKER).write_text("x", encoding="utf-8")
            build_port.clear_workspace(workspace, rom, generated)
            self.assertTrue((workspace / "notes").is_dir())  # not tool-owned: left alone
            self.assertFalse(any((workspace / part).exists() for part in build_port.WORKSPACE_PARTS))
            self.assertFalse((workspace / build_port.WORKSPACE_MARKER).exists())

    def test_clearing_never_deletes_an_image_stored_in_the_generated_directory(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            generated = root / "build" / "gen"
            generated.mkdir(parents=True)
            rom = generated / "game.nds"
            rom.write_bytes(b"image")
            workspace = root / "ws"
            (workspace / "config").mkdir(parents=True)
            (workspace / build_port.WORKSPACE_MARKER).write_text("x", encoding="utf-8")
            with mock.patch.object(build_port, "ROOT", root):
                with self.assertRaises(SystemExit) as refused:
                    build_port.clear_workspace(workspace, rom, generated)
            self.assertIn("contains the image", str(refused.exception))
            self.assertTrue(rom.is_file())
            self.assertTrue((workspace / "config").is_dir())  # nothing was deleted before the refusal

    def test_windows_build_retries_once_serially_but_linux_does_not(self):
        failure = build_port.subprocess.CalledProcessError(1, ["cmake"])
        cmake = ["cmake"]
        build_dir = Path("b")  # created before os.name is patched: pathlib picks its flavour at construction
        with mock.patch.object(build_port, "run", side_effect=[failure, None]) as run, \
                mock.patch.object(build_port.os, "name", "nt"):
            build_port.build_with_retry(cmake, build_dir, "dq9")
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[1].args[-2:], ("--parallel", "1"))
        self.assertEqual(run.call_args_list[0].args[:5], ("cmake", "--build", build_dir, "--target", "dq9"))
        with mock.patch.object(build_port, "run", side_effect=[failure, failure]) as run, \
                mock.patch.object(build_port.os, "name", "nt"):
            with self.assertRaises(build_port.subprocess.CalledProcessError):
                build_port.build_with_retry(cmake, build_dir)
        self.assertEqual(run.call_count, 2)  # a persistent error is not retried again
        with mock.patch.object(build_port, "run", side_effect=[failure]) as run, \
                mock.patch.object(build_port.os, "name", "posix"):
            with self.assertRaises(build_port.subprocess.CalledProcessError):
                build_port.build_with_retry(cmake, build_dir)
        self.assertEqual(run.call_count, 1)

    def test_backup_choices_match_the_derivation_tool(self):
        from tools import derive_abi_config
        self.assertEqual(build_port.BACKUP_TYPE_CHOICES, derive_abi_config.BACKUP_TYPES)

    def test_launch_hints_do_not_enable_debug_tool(self):
        args = [Path("a path/dq9"), Path("my game.nds"), Path("abi.ini")]
        posix = build_port.launch_hint(*args)
        self.assertNotIn("--debug-tool", posix)
        with mock.patch.object(build_port, "os", self.windows_os()):
            windows = build_port.launch_hint(*args)
        self.assertTrue(windows.startswith('& "a path/dq9"'))
        self.assertIn('"my game.nds"', windows)
        self.assertNotIn("--debug-tool", windows)

    def test_quiet_run_logs_everything_and_shows_the_tail_and_log_path_on_failure(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            with mock.patch.object(build_port, "ROOT", root):
                log = build_port.configure_log()
                try:
                    build_port.run(sys.executable, "-c", "print('quiet-ok-line')")
                    stderr = io.StringIO()
                    with mock.patch.object(build_port.sys, "stderr", stderr), \
                            self.assertRaises(build_port.subprocess.CalledProcessError) as raised:
                        build_port.run(sys.executable, "-c", "print('boom-detail'); raise SystemExit(3)")
                finally:
                    build_port._LOG = None
            self.assertEqual(raised.exception.returncode, 3)
            text = log.read_text(encoding="utf-8")
            self.assertIn("quiet-ok-line", text)
            self.assertIn("boom-detail", text)
            self.assertIn("boom-detail", stderr.getvalue())
            self.assertIn(str(log), stderr.getvalue())

    def test_quiet_run_prints_only_the_summary_line_when_asked(self):
        with tempfile.TemporaryDirectory() as raw, mock.patch.object(build_port, "ROOT", Path(raw)):
            build_port.configure_log()
            out = io.StringIO()
            try:
                with mock.patch.object(build_port.sys, "stdout", out):
                    build_port.run(sys.executable, "-c", "print('noise'); print('final summary')", summary=True)
            finally:
                build_port._LOG = None
        self.assertEqual(out.getvalue().strip(), "final summary")

    def test_pinned_downloads_are_refused_on_digest_mismatch_and_leave_nothing_behind(self):
        def download(url, destination):
            Path(destination).write_bytes(b"tampered")

        with tempfile.TemporaryDirectory() as raw:
            target = Path(raw) / "tool.zip"
            with mock.patch.object(build_port.urllib.request, "urlretrieve", side_effect=download):
                with self.assertRaises(SystemExit):
                    build_port.download_verified("https://example.invalid/x", target, "sha512", "0" * 128)
            self.assertEqual(list(Path(raw).iterdir()), [])
            good = hashlib.sha512(b"tampered").hexdigest()
            with mock.patch.object(build_port.urllib.request, "urlretrieve", side_effect=download) as fetch:
                build_port.download_verified("https://example.invalid/x", target, "sha512", good)
                build_port.download_verified("https://example.invalid/x", target, "sha512", good)
            self.assertEqual(fetch.call_count, 1)  # a verified file is reused

    def test_zip_extraction_refuses_traversal_and_links(self):
        import zipfile

        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            evil = root / "evil.zip"
            with zipfile.ZipFile(evil, "w") as bundle:
                bundle.writestr("../escape.txt", "x")
            with self.assertRaises(ValueError):
                build_port.extract_zip(evil, root / "out")
            link = root / "link.zip"
            with zipfile.ZipFile(link, "w") as bundle:
                info = zipfile.ZipInfo("shortcut")
                info.external_attr = 0o120777 << 16
                bundle.writestr(info, "target")
            with self.assertRaises(ValueError):
                build_port.extract_zip(link, root / "out")
            self.assertFalse((root / "escape.txt").exists())
            fine = root / "fine.zip"
            with zipfile.ZipFile(fine, "w") as bundle:
                bundle.writestr("pkg/file.txt", "ok")
            build_port.extract_zip(fine, root / "out")
            self.assertEqual((root / "out" / "pkg" / "file.txt").read_text(), "ok")

    def test_godot_build_requires_generated_code_and_game_is_not_needed(self):
        with tempfile.TemporaryDirectory() as raw, mock.patch.object(build_port, "GEN", Path(raw) / "gen"):
            with self.assertRaises(SystemExit) as raised:
                build_port.main(["--godot-only"])
            self.assertIn("Build the game first", str(raised.exception))
            with self.assertRaises(SystemExit):
                build_port.main(["--godot-only", "--runtime-only"])  # conflicting modes are an argparse error

    def test_progress_bar_fills_proportionally_and_never_overflows(self):
        self.assertEqual(build_port.progress_bar(0, 100, 10), "[----------]")
        self.assertEqual(build_port.progress_bar(50, 100, 10), "[#####-----]")
        self.assertEqual(build_port.progress_bar(100, 100, 10), "[##########]")
        self.assertEqual(build_port.progress_bar(150, 100, 10), "[##########]")
        self.assertEqual(build_port.progress_bar(1, 0, 10), "[##########]")

    def test_build_progress_lines_show_a_bar_when_output_is_not_a_console(self):
        script = "print('[1/4] a'); print('[2/4] b'); print('[4/4] c')"
        with tempfile.TemporaryDirectory() as raw, mock.patch.object(build_port, "ROOT", Path(raw)):
            build_port.configure_log()
            out = io.StringIO()
            try:
                with mock.patch.object(build_port.sys, "stdout", out):
                    build_port.run(sys.executable, "-c", script, "--build")
            finally:
                build_port._LOG = None
        self.assertIn("0%", out.getvalue())
        self.assertIn("[##############################] 100%", out.getvalue())


if __name__ == "__main__":
    unittest.main()
