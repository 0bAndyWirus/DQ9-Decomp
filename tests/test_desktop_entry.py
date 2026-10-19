import tempfile
import unittest
from pathlib import Path

from tools import install_desktop_entry as entry


class DesktopEntryTests(unittest.TestCase):
    def test_exec_quoting_escapes_the_reserved_characters(self):
        self.assertEqual(entry.quote_exec_argument('/a b/"x"$y`z\\w'), '"/a b/\\"x\\"\\$y\\`z\\\\w"')
        self.assertEqual(entry.quote_exec_argument("/100%/dq9"), '"/100%%/dq9"')  # % starts a field code

    def test_entry_starts_the_launcher_and_keeps_spaces_in_paths(self):
        text = entry.desktop_entry(Path("/opt/my games/dq9"), Path("/opt/my games/abi.ini"))
        self.assertIn('Exec="/opt/my games/dq9" --launcher --abi-config "/opt/my games/abi.ini"', text)
        self.assertIn("Terminal=false", text)
        self.assertTrue(text.startswith("[Desktop Entry]\n"))

    def test_install_and_uninstall_round_trip(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            exe = root / "dq9"
            exe.write_text("#!/bin/sh\n", encoding="utf-8")
            apps = root / "apps"
            self.assertEqual(entry.main(["--exe", str(exe), "--abi-config", str(root / "none.ini"), "--dir", str(apps)]), 0)
            written = (apps / entry.ENTRY_NAME).read_text(encoding="utf-8")
            self.assertIn("--launcher", written)
            self.assertNotIn("--abi-config", written)  # absent abi.ini is not referenced
            self.assertEqual(entry.main(["--dir", str(apps), "--uninstall"]), 0)
            self.assertFalse((apps / entry.ENTRY_NAME).exists())

    def test_missing_executable_is_reported(self):
        with tempfile.TemporaryDirectory() as raw:
            self.assertEqual(entry.main(["--exe", str(Path(raw) / "missing"), "--dir", raw]), 1)


if __name__ == "__main__":
    unittest.main()
