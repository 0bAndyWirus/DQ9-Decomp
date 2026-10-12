import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import refresh_candidates as refresh

BUILD_NINJA = """rule mwcc
  command = $
      /abs/wibo $
      "./tools/mwccarm/2.0/sp2p2/mwccarm.exe" -O2 -enum int -char signed $
      -proc arm946e -lang=c++ -w off -sym on -str pool,reuse -i include $cc_flags -d $
      $game_version -MD -c $in -o $basedir && $python $
      tools/transform_dep.py $basefile.d $basefile.d
  depfile = $basefile.d

rule lcf
  command = dsd lcf
"""


def _candidate(project: Path, author="A", stem="unit", *, object_newer=True, with_object=True):
    base = project / "src/_candidates" / author
    base.mkdir(parents=True, exist_ok=True)
    manifest = base / f"{stem}.json"
    manifest.write_text("{}")
    source = base / f"{stem}.cpp"
    source.write_text("int f(){return 1;}\n")
    obj = project / "build/scratch" / author / f"{stem}.o"
    if with_object:
        obj.parent.mkdir(parents=True, exist_ok=True)
        obj.write_bytes(b"old")
        stamp = os.stat(source).st_mtime_ns + (10**9 if object_newer else -10**9)
        os.utime(obj, ns=(stamp, stamp))
    return manifest, source, obj


def _compiler(project: Path, content=b"new-object", code=0):
    def run(command, cwd):
        out = Path(cwd) / command[command.index("-o") + 1]
        if code == 0:
            out.write_bytes(content)
        return SimpleNamespace(returncode=code, stdout="", stderr="" if code == 0 else "error: boom")
    return run


class FlagTests(unittest.TestCase):
    def test_flags_come_from_the_ninja_rule_with_variables_resolved_and_io_options_removed(self):
        flags = refresh.mwcc_flags(BUILD_NINJA)
        self.assertEqual(flags[:2], ["/abs/wibo", "./tools/mwccarm/2.0/sp2p2/mwccarm.exe"])
        self.assertIn("-lang=c++", flags)
        self.assertEqual(flags[flags.index("-d") + 1], "eur")
        for unwanted in ("-MD", "-c", "-o", "$in", "$basedir"):
            self.assertNotIn(unwanted, flags)
        self.assertNotIn("transform_dep.py", " ".join(flags))

    def test_a_ninja_file_without_a_usable_rule_is_an_error(self):
        for text in ("rule lcf\n  command = x\n", "rule mwcc\n  command = gcc -c $in\n"):
            with self.subTest(text=text), self.assertRaises(refresh.RefreshError):
                refresh.mwcc_flags(text)

    def test_compile_command_appends_input_and_output(self):
        self.assertEqual(refresh.compile_command(["cc", "-O2"], Path("a.cpp"), Path("a.o")),
                         ["cc", "-O2", "-c", "a.cpp", "-o", "a.o"])


class RefreshTests(unittest.TestCase):
    def test_stale_and_missing_objects_are_rebuilt_and_dated_after_the_source(self):
        for kwargs in ({"object_newer": False}, {"with_object": False}):
            with self.subTest(kwargs=kwargs), tempfile.TemporaryDirectory() as directory:
                project = Path(directory)
                manifest, source, obj = _candidate(project, **kwargs)
                status, _ = refresh.refresh_one(project, manifest, ["cc"], _compiler(project))
                self.assertEqual(status, "refreshed")
                self.assertEqual(obj.read_bytes(), b"new-object")
                self.assertGreaterEqual(obj.stat().st_mtime_ns, source.stat().st_mtime_ns)
                self.assertFalse(refresh.needs_refresh(source, obj))
                self.assertEqual(list(obj.parent.glob(".*")), [])  # no temp files left behind

    def test_a_fresh_object_is_left_alone_unless_forced(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, _, obj = _candidate(project)
            run = _compiler(project)
            self.assertEqual(refresh.refresh_one(project, manifest, ["cc"], run)[0], "fresh")
            self.assertEqual(obj.read_bytes(), b"old")
            self.assertEqual(refresh.refresh_one(project, manifest, ["cc"], run, force=True)[0], "refreshed")

    def test_a_failed_compile_keeps_the_previous_object_and_leaves_no_temp_file(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, _, obj = _candidate(project, object_newer=False)
            status, detail = refresh.refresh_one(project, manifest, ["cc"], _compiler(project, code=1))
            self.assertEqual(status, "compile-failed")
            self.assertIn("boom", detail)
            self.assertEqual(obj.read_bytes(), b"old")
            self.assertEqual(list(obj.parent.glob(".*")), [])

    def test_a_compiler_that_exits_zero_without_output_is_a_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, _, obj = _candidate(project, object_newer=False)

            def silent(command, cwd):
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            self.assertEqual(refresh.refresh_one(project, manifest, ["cc"], silent)[0], "compile-failed")
            self.assertEqual(obj.read_bytes(), b"old")

    def test_a_timeout_is_reported_not_raised(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, _, _ = _candidate(project, object_newer=False)

            def slow(command, cwd):
                raise subprocess.TimeoutExpired(command, 1)

            self.assertEqual(refresh.refresh_one(project, manifest, ["cc"], slow)[0], "compile-failed")


class CommandTests(unittest.TestCase):
    def _project(self, directory: str):
        project = Path(directory)
        (project / "build.ninja").write_text(BUILD_NINJA)
        return project

    def test_refuses_while_the_integration_lock_is_held_even_without_a_journal(self):
        from tools import integrate_batch
        with tempfile.TemporaryDirectory() as directory:
            project = self._project(directory)
            manifest, _, obj = _candidate(project, object_newer=False)
            listing = Path(directory) / "list.txt"
            listing.write_text(f"{manifest.relative_to(project).as_posix()}\n")
            with integrate_batch.WorkspaceLock(project), patch.object(refresh, "_run") as compiler:
                code = refresh.main(["--list", str(listing), "--project", str(project)])
            self.assertEqual(code, 2)
            compiler.assert_not_called()  # an integrator may be about to read these objects
            self.assertEqual(obj.read_bytes(), b"old")

    def test_the_lock_is_held_during_every_compile_and_released_afterwards(self):
        from tools import integrate_batch
        with tempfile.TemporaryDirectory() as directory:
            project = self._project(directory)
            manifest, _, _ = _candidate(project, object_newer=False)
            listing = Path(directory) / "list.txt"
            listing.write_text(f"{manifest.relative_to(project).as_posix()}\n")
            probes = []

            def compiler(command, cwd):
                try:
                    with integrate_batch.WorkspaceLock(project):
                        probes.append("lock was free")
                except integrate_batch.BatchError:
                    probes.append("lock was held")
                return _compiler(project)(command, cwd)

            with patch.object(refresh, "_run", compiler):
                self.assertEqual(refresh.main(["--list", str(listing), "--project", str(project)]), 0)
            self.assertEqual(probes, ["lock was held"])
            with integrate_batch.WorkspaceLock(project):  # released: an integrator can start now
                pass

    def test_refuses_while_a_transaction_journal_exists(self):
        with tempfile.TemporaryDirectory() as directory:
            project = self._project(directory)
            (project / refresh.JOURNAL).parent.mkdir(parents=True)
            (project / refresh.JOURNAL).write_text("{}")
            listing = Path(directory) / "list.txt"
            listing.write_text("src/_candidates/A/unit.json\n")
            self.assertEqual(refresh.main(["--list", str(listing), "--project", str(project)]), 2)

    def test_end_to_end_report_ok_list_and_exit_status_with_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            project = self._project(directory)
            good, _, _ = _candidate(project, stem="good", object_newer=False)
            bad, _, _ = _candidate(project, stem="bad", object_newer=False)
            broken, _, _ = _candidate(project, stem="broken", object_newer=False)
            listing = Path(directory) / "list.txt"
            listing.write_text("".join(f"{m.relative_to(project).as_posix()}\n" for m in (good, bad, broken)))
            ok_list, report = Path(directory) / "ok.txt", Path(directory) / "report.json"

            def run(command, cwd):
                code = 1 if "broken" in " ".join(command) else 0
                return _compiler(project, code=code)(command, cwd)

            rejected = [(bad, "preflight failed: .text: no member matches the target modules")]
            with patch.object(refresh, "_run", run), \
                    patch.object(refresh, "verify", return_value=([good], rejected)):
                code = refresh.main(["--list", str(listing), "--project", str(project), "--verify",
                                     "--ok-list", str(ok_list), "--report", str(report)])
            self.assertEqual(code, 1)  # one did not compile, one did not match
            self.assertEqual(ok_list.read_text().split(), ["src/_candidates/A/good.json"])
            data = json.loads(report.read_text())
            self.assertEqual(data["src/_candidates/A/good.json"]["verified"], True)
            self.assertEqual(data["src/_candidates/A/bad.json"]["verified"], False)
            self.assertIn("no member matches", data["src/_candidates/A/bad.json"]["verify_reason"])
            self.assertEqual(data["src/_candidates/A/broken.json"]["status"], "compile-failed")

    def test_all_good_exits_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            project = self._project(directory)
            manifest, _, _ = _candidate(project, object_newer=False)
            listing = Path(directory) / "list.txt"
            listing.write_text(f"{manifest.relative_to(project).as_posix()}\n")
            with patch.object(refresh, "_run", _compiler(project)):
                self.assertEqual(refresh.main(["--list", str(listing), "--project", str(project)]), 0)


if __name__ == "__main__":
    unittest.main()
