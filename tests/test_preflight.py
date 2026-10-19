import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import preflight  # noqa: E402


def write_image(path: Path, size=0x10000, arm9=(0x4000, 0x4000), arm7=(0x8000, 0x2000), used=0x10000, fix_crc=True):
    data = bytearray(size)
    struct.pack_into("<I", data, 0x20, arm9[0])
    struct.pack_into("<I", data, 0x2C, arm9[1])
    struct.pack_into("<I", data, 0x30, arm7[0])
    struct.pack_into("<I", data, 0x3C, arm7[1])
    struct.pack_into("<I", data, 0x80, used)
    if fix_crc:
        struct.pack_into("<H", data, 0x15E, preflight._crc16(bytes(data[:0x15E])))
    path.write_bytes(data)


class RomImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, **kwargs):
        path = self.dir / "image.nds"
        write_image(path, **kwargs)
        return preflight.check_rom_image(path)

    def test_valid_header(self):
        self.assertEqual(self.check().status, preflight.OK)

    def test_patched_header_is_only_a_warning(self):
        self.assertEqual(self.check(fix_crc=False).status, preflight.WARN)

    def test_truncated_image_names_both_sizes(self):
        result = self.check(size=0x6000)
        self.assertEqual(result.status, preflight.FAIL)
        self.assertIn("truncated (24576 of 65536 bytes)", result.detail)

    def test_payload_outside_the_file_is_rejected(self):
        result = self.check(arm9=(0x7457F159, 0x1234), arm7=(0x99999999, 0x5555), used=0x42, fix_crc=False)
        self.assertEqual(result.status, preflight.FAIL)
        self.assertIn("does not look like", result.detail)

    def test_tiny_and_missing_files(self):
        small = self.dir / "small.nds"
        small.write_bytes(b"\0" * 100)
        self.assertIn("too small", preflight.check_rom_image(small).detail)
        self.assertEqual(preflight.check_rom_image(self.dir / "absent.nds").status, preflight.FAIL)


class EnvironmentTests(unittest.TestCase):
    def test_missing_python_module_names_the_pip_package(self):
        real = preflight.importlib.util.find_spec
        with mock.patch.object(preflight.importlib.util, "find_spec",
                               lambda name, *a: None if name == "yaml" else real(name, *a)):
            result = preflight.check_modules()
        self.assertEqual(result.status, preflight.FAIL)
        self.assertIn("PyYAML", result.detail)

    def test_low_disk_space_fails(self):
        usage = mock.Mock(free=int(1e9))
        with mock.patch.object(preflight.shutil, "disk_usage", return_value=usage):
            self.assertEqual(preflight.check_disk(Path(".")).status, preflight.FAIL)

    def test_report_is_false_only_on_failures(self):
        ok = preflight.Check("a", preflight.OK, "x")
        warn = preflight.Check("b", preflight.WARN, "y")
        fail = preflight.Check("c", preflight.FAIL, "z")
        with mock.patch("builtins.print"):
            self.assertTrue(preflight.report([ok, warn]))
            self.assertFalse(preflight.report([ok, fail]))


if __name__ == "__main__":
    unittest.main()
