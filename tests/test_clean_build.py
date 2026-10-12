import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import clean_build


class CleanBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "build").mkdir()
        self.patch = mock.patch.object(clean_build, "ROOT", self.root)
        self.patch.start()
        self.addCleanup(self.cleanup)

    def cleanup(self):
        self.patch.stop()
        self.temp.cleanup()
    def test_negative_days_rejected_by_cli_and_function(self):
        with self.assertRaises(SystemExit) as error:
            clean_build.main(["--days", "-0.1"])
        self.assertEqual(error.exception.code, 2)
        with self.assertRaises(SystemExit) as error:
            clean_build.main(["--days", "nan"])
        self.assertEqual(error.exception.code, 2)
        with self.assertRaises(ValueError):
            clean_build.candidates(-1)

    def test_explicit_protected_names_are_never_candidates(self):
        names = {"author-batches", "eur", "scratch", "gen", "game", "game-fast",
                 "windows", "deck", "freerun", "parity", "mrc", "oracle"}
        for name in names:
            (self.root / "build" / name).mkdir()
        with mock.patch.object(clean_build, "JUNK_PATTERNS", [r".*"]):
            self.assertEqual(clean_build.candidates(0), [])

    def test_outside_symlink_is_refused(self):
        target = self.root / "outside.log"
        target.write_text("safe")
        link = self.root / "build" / "escape.log"
        link.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "outside build directory"):
            clean_build.candidates(0)

    def test_candidate_remains_inside_build(self):
        candidate = self.root / "build" / "probe.log"
        candidate.write_text("old")
        self.assertEqual(clean_build.candidates(0), [(candidate, 3)])

    def test_build_directory_symlink_to_extracted_data_is_refused(self):
        build = self.root / "build"
        build.rmdir()
        extracted = self.root / "extracted"
        extracted.mkdir()
        (extracted / "unsafe.log").write_text("must remain untouched")
        build.symlink_to(extracted, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "refusing build directory"):
            clean_build.candidates(0)


if __name__ == "__main__":
    unittest.main()
