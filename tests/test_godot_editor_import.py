"""Cold-cache editor diagnosis and strict, non-editor extension teardown probes.

Requires a built native extension. GODOT selects the editor executable; absent
Godot/extension prerequisites produce a clear unittest skip, never a fake pass.
Godot 4.5's upstream EditorHelp cleanup race is pinned separately: no launcher or
runtime probe accepts an unsuccessful exit status.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def known_editor_doc_crash(result, version):
    if not (sys.platform == "linux" and version.startswith("4.5.stable.official.")):
        return False
    output = result.stdout + result.stderr
    # These three addresses identify HashMap<ClassDoc>, DocTools::generate and
    # EditorHelp::_gen_extensions_docs in the official Linux x86_64 4.5 editor.
    # Do not accept an arbitrary SIGABRT, an extension backtrace or import error.
    return (result.returncode == -6
            and "handle_crash: Program crashed with signal 11" in output
            and "Engine version: Godot Engine v4.5.stable.official" in output
            and all(re.search(r"\[0x" + address + r"\]", output)
                    for address in ("196cb78", "196e8fd", "1975035")))


class GodotEditorImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        default = Path.home() / "Applications/godot/Godot_v4.5-stable_linux.x86_64"
        requested = os.environ.get("GODOT")
        cls.engine = shutil.which(requested) if requested else (
            str(default) if default.is_file() else shutil.which("godot"))
        if not cls.engine:
            raise unittest.SkipTest("Godot is not installed; set GODOT to a Godot 4.5 executable")
        cls.project = Path(os.environ.get("DQ9_GODOT_PROJECT", ROOT / "godot"))
        extension = "dq9_godot.windows.template_release.x86_64.dll" if os.name == "nt" else "libdq9_godot.so"
        if not (cls.project / "bin" / extension).is_file():
            raise unittest.SkipTest("native Godot extension is not built; build dq9_godot first")
        version = subprocess.run([cls.engine, "--version"], capture_output=True, text=True,
                                 check=True, timeout=30)
        cls.version = version.stdout.strip()

    def fresh_project(self, directory):
        project = Path(directory) / "godot"
        shutil.copytree(self.project, project, ignore=shutil.ignore_patterns(".godot"))
        return project

    def run_engine(self, project, *args):
        result = subprocess.run([self.engine, "--headless", "--path", str(project), *args],
                                capture_output=True, text=True, timeout=180)
        return result

    def assert_clean_runtime(self, result):
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertNotIn("ERROR:", output, output)
        self.assertNotIn("instance binding callbacks", output, output)

    def import_project(self, project):
        result = self.run_engine(project, "--editor", "--import", "--quit")
        if result.returncode != 0:
            self.assertTrue(
                known_editor_doc_crash(result, self.version),
                f"Cold editor import exited {result.returncode} with Godot {self.version}\n"
                + result.stdout + result.stderr)
        cache = project / ".godot/extension_list.cfg"
        self.assertTrue(cache.is_file(), result.stdout + result.stderr)
        self.assertIn("res://dq9.gdextension", cache.read_text())
        return result

    def test_cold_runtime_lifecycle_without_editor_import(self):
        with tempfile.TemporaryDirectory(prefix="dq9-runtime-lifecycle-") as directory:
            project = self.fresh_project(directory)
            self.assertFalse((project / ".godot").exists())
            lifecycle = self.run_engine(project, "--script", "res://tests/test_extension_lifecycle.gd")
            self.assert_clean_runtime(lifecycle)
            self.assertIn("PASS: DqMachine registered; extension lifecycle", lifecycle.stdout)

    def test_five_cold_import_caches_and_strict_runtime_teardown(self):
        for attempt in range(5):
            with self.subTest(attempt=attempt), tempfile.TemporaryDirectory(prefix="dq9-editor-import-") as directory:
                project = self.fresh_project(directory)
                self.import_project(project)
                self.assert_clean_runtime(self.run_engine(project, "--quit-after", "5"))
                lifecycle = self.run_engine(project, "--script", "res://tests/test_extension_lifecycle.gd")
                self.assert_clean_runtime(lifecycle)
                self.assertIn("PASS: DqMachine registered; extension lifecycle", lifecycle.stdout)
                # Warm editor caches must exit cleanly as well. Only the separately
                # identified cold-import upstream bug is accepted above.
                self.assert_clean_runtime(self.run_engine(project, "--editor", "--quit-after", "5"))
                self.assert_clean_runtime(self.run_engine(project, "--editor", "--import", "--quit"))

    def test_known_cold_import_upstream_signature(self):
        version = "4.5.stable.official.test"
        output = ("handle_crash: Program crashed with signal 11\n"
                  "Engine version: Godot Engine v4.5.stable.official\n"
                  "[0x196cb78] [0x196e8fd] [0x1975035]\n")
        def result(returncode, text):
            return subprocess.CompletedProcess([], returncode, stdout=text, stderr="")

        if sys.platform != "linux":
            self.assertFalse(known_editor_doc_crash(result(-6, output), version))
            return
        self.assertTrue(known_editor_doc_crash(result(-6, output), version))
        self.assertFalse(known_editor_doc_crash(result(0, output), version))
        self.assertFalse(known_editor_doc_crash(result(-6, output.replace("[0x196e8fd]", "")), version))
        self.assertFalse(known_editor_doc_crash(result(-6, output), "4.6.stable.official.test"))
        self.assertFalse(known_editor_doc_crash(result(-6, "font worker [0x4759ab8]"), version))


if __name__ == "__main__":
    unittest.main()
