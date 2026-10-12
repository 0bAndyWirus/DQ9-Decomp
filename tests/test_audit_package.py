import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.audit_package import audit, classify, main


class ClassifyTests(unittest.TestCase):
    def test_rom_images_and_generated_sources_are_forbidden(self):
        for name in ("game.nds", "Rom/DQ9.SRL", "gen/arm9/fn_table.cpp", "arm9.bin", "arm9.manifest.json", "x/y.cpp"):
            self.assertIsNotNone(classify(name, 10), name)

    def test_ordinary_package_files_are_allowed(self):
        for name in ("dq9", "dq9.exe", "SDL2.dll", "README.md", "dq9.sh", "dq9.desktop"):
            self.assertIsNone(classify(name, 10), name)

    def test_cmake_scratch_sources_are_not_mistaken_for_guest_code(self):
        self.assertIsNone(classify("cmake/CMakeFiles/3.25.1/CompilerIdCXX/CMakeCXXCompilerId.cpp", 10))
        self.assertIsNotNone(classify("cmake/src/fn_table.cpp", 10))

    def test_oversized_data_file_is_flagged_but_big_binaries_are_not(self):
        self.assertIn("large", classify("assets.dat", 100 * 1024 * 1024))
        self.assertIsNone(classify("dq9.exe", 100 * 1024 * 1024))


class AuditTests(unittest.TestCase):
    def test_directory_zip_and_tar_are_all_audited(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "pkg"
            package.mkdir()
            (package / "dq9").write_bytes(b"x")
            (package / "baserom.nds").write_bytes(b"x")
            archive_zip = root / "pkg.zip"
            with zipfile.ZipFile(archive_zip, "w") as handle:
                handle.write(package / "dq9", "dq9")
            archive_tar = root / "pkg.tar"
            with tarfile.open(archive_tar, "w") as handle:
                handle.add(package / "baserom.nds", "baserom.nds")
            self.assertEqual(audit(package), (2, [("baserom.nds", "matches forbidden pattern *.nds")]))
            self.assertEqual(audit(archive_zip), (1, []))
            self.assertEqual(audit(archive_tar)[1][0][0], "baserom.nds")

    def test_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clean = root / "clean"
            clean.mkdir()
            (clean / "dq9").write_bytes(b"x")
            dirty = root / "dirty"
            dirty.mkdir()
            (dirty / "a.nds").write_bytes(b"x")
            self.assertEqual(main([str(clean)]), 0)
            self.assertEqual(main([str(dirty)]), 1)
            self.assertEqual(main([str(root / "missing")]), 2)


if __name__ == "__main__":
    unittest.main()
