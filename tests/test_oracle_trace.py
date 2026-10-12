"""Synthetic header and entry fixtures, unrelated to any game image."""

import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.oracle.run_oracle import manifest_entries, parse_snapshots, rom_arm9_entry, symbol_entries


SYNTHETIC_ARM9_ENTRY = 0x08000000
SYNTHETIC_ARM7_ENTRY = 0x09000000


def synthetic_header(entry: int = SYNTHETIC_ARM9_ENTRY) -> bytes:
    header = bytearray(0x40)
    struct.pack_into("<I", header, 0x24, entry)
    return bytes(header)


class OracleTraceEntryTests(unittest.TestCase):
    def test_merges_entries_from_both_cpu_manifests(self):
        with tempfile.TemporaryDirectory() as directory:
            arm9 = Path(directory) / "arm9.json"
            arm7 = Path(directory) / "arm7.json"
            arm9.write_text(json.dumps({"modules": [{"functions": [{"addr": SYNTHETIC_ARM9_ENTRY}]}]}))
            arm7.write_text(json.dumps({"modules": [{"functions": [{"addr": SYNTHETIC_ARM7_ENTRY}]}]}))

            self.assertEqual(manifest_entries(arm9, arm7), {SYNTHETIC_ARM9_ENTRY, SYNTHETIC_ARM7_ENTRY})
            self.assertEqual(symbol_entries(None, arm9, arm7), {SYNTHETIC_ARM9_ENTRY, SYNTHETIC_ARM7_ENTRY})

    def test_missing_manifest_is_optional(self):
        missing = Path("missing-arm9-manifest.json")
        self.assertEqual(manifest_entries(missing), set())



    def test_snapshot_ranges_are_parsed_and_bounded(self):
        self.assertEqual(parse_snapshots("0,3-5/2,4", 6), [0, 3, 4, 5])
        for text in ("", "1,,2", "-1", "4-2", "0-3/0", "6"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_snapshots(text, 6)

    def test_cli_rejects_invalid_snapshot_before_starting_oracle(self):
        import contextlib
        import io
        from tools.oracle import run_oracle

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom, binary = root / "rom.nds", root / "oracle"
            rom.write_bytes(synthetic_header())
            binary.write_bytes(b"not executed")
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    run_oracle.main(["--rom", str(rom), "--frames", "2", "--output", str(root / "out"),
                                     "--binary", str(binary), "--snapshots", "2"])
            self.assertEqual(raised.exception.code, 2)

    def test_cli_rejects_invalid_hash_destination_before_starting_oracle(self):
        import contextlib
        import io
        from tools.oracle import run_oracle

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom, binary, destination = root / "rom.nds", root / "oracle", root / "hashes"
            rom.write_bytes(synthetic_header())
            binary.write_bytes(b"not executed")
            destination.mkdir()
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    run_oracle.main(["--rom", str(rom), "--frames", "2", "--output", str(root / "out"),
                                     "--binary", str(binary), "--snapshots", "0", "--hashes",
                                     str(destination)])
            self.assertEqual(raised.exception.code, 2)

    def test_success_summary_lists_missing_optional_inputs(self):
        import contextlib
        import io
        from types import SimpleNamespace
        from unittest.mock import patch
        from tools.oracle import run_oracle

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom, binary, output = root / "rom.nds", root / "oracle", root / "capture"
            rom.write_bytes(synthetic_header())
            binary.write_bytes(b"not executed")
            args = ["--rom", str(rom), "--frames", "1", "--snapshots", "0", "--output", str(output),
                    "--binary", str(binary), "--symbols", str(root / "missing-symbols"),
                    "--arm9-manifest", str(root / "missing-arm9"),
                    "--arm7-manifest", str(root / "missing-arm7")]
            stdout = io.StringIO()
            with patch("tools.oracle.run_oracle.subprocess.run", return_value=SimpleNamespace(returncode=0)), \
                    contextlib.redirect_stdout(stdout):
                self.assertEqual(run_oracle.main(args), 0)
            self.assertIn("Optional trace inputs omitted: --symbols", stdout.getvalue())
            self.assertIn("--arm9-manifest", stdout.getvalue())
            self.assertIn("--arm7-manifest", stdout.getvalue())
            self.assertEqual((output / "entries.txt").read_text(), f"{SYNTHETIC_ARM9_ENTRY:x}\n")

    def test_reads_arm9_entry_from_synthetic_rom_header(self):
        with tempfile.TemporaryDirectory() as directory:
            rom = Path(directory) / "synthetic.nds"
            rom.write_bytes(synthetic_header(SYNTHETIC_ARM9_ENTRY + 0x40))
            self.assertEqual(rom_arm9_entry(rom), SYNTHETIC_ARM9_ENTRY + 0x40)

    def test_rejects_truncated_entry_field(self):
        with tempfile.TemporaryDirectory() as directory:
            rom = Path(directory) / "synthetic.nds"
            rom.write_bytes(synthetic_header()[:0x27])
            with self.assertRaisesRegex(ValueError, "header is too short"):
                rom_arm9_entry(rom)

    def test_cli_uses_explicit_entry_without_reading_header(self):
        import contextlib
        import io
        from types import SimpleNamespace
        from unittest.mock import patch
        from tools.oracle import run_oracle

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom, binary, output = root / "synthetic.nds", root / "oracle", root / "capture"
            rom.write_bytes(b"synthetic input without a ROM header")
            binary.write_bytes(b"not executed")
            entry = SYNTHETIC_ARM9_ENTRY + 0x80
            args = ["--rom", str(rom), "--entry", hex(entry), "--frames", "1", "--snapshots", "0",
                    "--output", str(output), "--binary", str(binary),
                    "--arm9-manifest", str(root / "missing-arm9"),
                    "--arm7-manifest", str(root / "missing-arm7")]
            with patch("tools.oracle.run_oracle.subprocess.run", return_value=SimpleNamespace(returncode=0)), \
                    patch("tools.oracle.run_oracle.rom_arm9_entry") as read_entry, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_oracle.main(args), 0)
            read_entry.assert_not_called()
            self.assertEqual((output / "entries.txt").read_text(), f"{entry:x}\n")

    def test_cli_rejects_truncated_header_before_starting_oracle(self):
        import contextlib
        import io
        from unittest.mock import patch
        from tools.oracle import run_oracle

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom, binary = root / "synthetic.nds", root / "oracle"
            rom.write_bytes(synthetic_header()[:0x27])
            binary.write_bytes(b"not executed")
            stderr = io.StringIO()
            with patch("tools.oracle.run_oracle.subprocess.run") as run, contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised:
                    run_oracle.main(["--rom", str(rom), "--frames", "1", "--snapshots", "0",
                                     "--output", str(root / "capture"), "--binary", str(binary)])
            self.assertEqual(raised.exception.code, 2)
            self.assertIn("header is too short", stderr.getvalue())
            run.assert_not_called()

    def test_cli_rejects_out_of_range_entry_override(self):
        import contextlib
        import io
        from tools.oracle import run_oracle

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom, binary = root / "synthetic.nds", root / "oracle"
            rom.write_bytes(synthetic_header())
            binary.write_bytes(b"not executed")
            for entry in ("-1", "0x100000000"):
                with self.subTest(entry=entry), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as raised:
                        run_oracle.main(["--rom", str(rom), "--entry", entry, "--frames", "1", "--snapshots", "0",
                                         "--output", str(root / "capture"), "--binary", str(binary)])
                self.assertEqual(raised.exception.code, 2)

if __name__ == "__main__":
    unittest.main()
