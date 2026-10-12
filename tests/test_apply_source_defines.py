import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import apply_source_defines as asd
from tools import refresh_candidates as refresh


def _candidate(project: Path, text="int f(){return func_x();}\n"):
    base = project / "src/_candidates/A"
    base.mkdir(parents=True)
    manifest = base / "unit.json"
    manifest.write_text('{"file":"src/_candidates/A/unit.cpp","object":"build/scratch/A/unit.o","module":"ov017",'
                        '"functions":[{"name":"f","address":"0x1000"}]}')
    source = base / "unit.cpp"
    source.write_text(text)
    obj = project / "build/scratch/A/unit.o"
    obj.parent.mkdir(parents=True)
    obj.write_bytes(b"original-object")
    stamp = source.stat().st_mtime_ns + 10**9
    os.utime(obj, ns=(stamp, stamp))
    return manifest, source, obj


class TextTests(unittest.TestCase):
    def test_defines_go_before_everything_so_header_declarations_are_renamed_too(self):
        text = '#include "x.h"\nint f();\n'
        result = asd.with_defines(text, ["#define func_a memset", "#define func_b other"])
        self.assertTrue(result.startswith(asd.HEADER + "#define func_a memset\n#define func_b other\n#include"))
        self.assertTrue(result.endswith(text))

    def test_it_is_idempotent_and_adds_only_the_missing_lines(self):
        once = asd.with_defines("int f();\n", ["#define a b"])
        self.assertEqual(asd.with_defines(once, ["#define a b"]), once)
        twice = asd.with_defines(once, ["#define a b", "#define c d"])
        self.assertEqual(twice.count("#define a b"), 1)
        self.assertIn("#define c d", twice)


class ProcessTests(unittest.TestCase):
    def _run(self, project, manifest, *, defines, compile_ok=True, accepted=True, conflicts=()):
        def compiler(command, cwd):
            out = Path(cwd) / command[command.index("-o") + 1]
            if compile_ok:
                out.write_bytes(b"rebuilt-object")
            return SimpleNamespace(returncode=0 if compile_ok else 1, stdout="", stderr="" if compile_ok else "error: x")

        verdict = ([manifest], []) if accepted else ([], [(manifest, "preflight failed: still wrong")])
        with patch.object(asd, "plan_defines", return_value=(list(defines), list(conflicts))), \
                patch.object(refresh, "_run", compiler), patch.object(refresh, "verify", return_value=verdict):
            return asd.process(project, manifest, ["cc"])

    def test_accepted_result_keeps_the_new_source_and_object(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, source, obj = _candidate(project)
            outcome = self._run(project, manifest, defines=["#define func_x memset"])
            self.assertEqual(outcome["status"], "applied")
            self.assertIn("#define func_x memset", source.read_text())
            self.assertEqual(obj.read_bytes(), b"rebuilt-object")

    def test_a_rejected_result_restores_source_and_object_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, source, obj = _candidate(project)
            before = source.read_bytes()
            outcome = self._run(project, manifest, defines=["#define func_x memset"], accepted=False)
            self.assertEqual(outcome["status"], "reverted")
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(obj.read_bytes(), b"original-object")
            self.assertFalse(refresh.needs_refresh(source, obj))  # restored object is not stale
            self.assertEqual(list(source.parent.glob(".*")) + list(obj.parent.glob(".*")), [])

    def test_a_compile_failure_restores_everything(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, source, obj = _candidate(project)
            before = source.read_bytes()
            outcome = self._run(project, manifest, defines=["#define func_x memset"], compile_ok=False)
            self.assertEqual(outcome["status"], "reverted")
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(obj.read_bytes(), b"original-object")

    def test_nothing_is_touched_without_defines_or_with_conflicts(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, source, obj = _candidate(project)
            before = source.read_bytes()
            self.assertEqual(self._run(project, manifest, defines=[])["status"], "no-defines")
            self.assertEqual(self._run(project, manifest, defines=["#define a b"], conflicts=["overlap"])["status"],
                             "conflicts")
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(obj.read_bytes(), b"original-object")

    def test_defines_already_in_the_source_are_not_rebuilt(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, source, obj = _candidate(project, text="#define func_x memset\nint f(){return func_x();}\n")
            self.assertEqual(self._run(project, manifest, defines=["#define func_x memset"])["status"],
                             "already-applied")
            self.assertEqual(obj.read_bytes(), b"original-object")


class CommandTests(unittest.TestCase):
    def test_refuses_while_the_integration_lock_is_held(self):
        from tools import integrate_batch
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifest, source, obj = _candidate(project)
            (project / "build.ninja").write_text("rule mwcc\n  command = x\n")
            listing = project / "list.txt"
            listing.write_text("src/_candidates/A/unit.json\n")
            before = source.read_bytes()
            with integrate_batch.WorkspaceLock(project), patch.object(asd, "plan_defines") as plan:
                self.assertEqual(asd.main(["--list", str(listing), "--project", str(project)]), 2)
            plan.assert_not_called()
            self.assertEqual(source.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
