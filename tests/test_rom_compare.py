import io
import tempfile
import unittest
from pathlib import Path

from tools.rom_compare import (
    EXIT_DIFFERENT,
    EXIT_ERROR,
    EXIT_IDENTICAL,
    EXIT_KNOWN_ONLY,
    classify,
    compare,
    differing_offsets,
    main,
    merge_ranges,
)

SIZE = 0x400


def _write(directory: Path, name: str, patches: dict[int, int] | None = None, size: int = SIZE) -> Path:
    data = bytearray(range(256)) * (size // 256)
    for offset, value in (patches or {}).items():
        data[offset] = value
    path = directory / name
    path.write_bytes(bytes(data))
    return path


class OffsetTests(unittest.TestCase):
    def test_differences_are_found_across_chunk_boundaries(self):
        first = io.BytesIO(bytes(40))
        second = io.BytesIO(bytes([0] * 7 + [1] + [0] * 16 + [2] + [0] * 15))
        self.assertEqual(list(differing_offsets(first, second, chunk=8)), [7, 24])

    def test_merge_groups_close_offsets_and_counts_bytes(self):
        self.assertEqual(merge_ranges(iter([10, 11, 200, 201, 202, 900]), gap=64), [[10, 11, 2], [200, 202, 3], [900, 900, 1]])

    def test_classify_separates_known_header_fields_from_everything_else(self):
        self.assertEqual(classify([0x6C, 0x6D, 0x15E, 0x15F]), (["header_crc16", "secure_area_crc16"], []))
        self.assertEqual(classify([0x6C, 0x70]), (["secure_area_crc16"], [0x70]))


class CompareTests(unittest.TestCase):
    def test_exit_codes_for_identical_known_only_other_and_size_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = _write(root, "ref.nds")
            self.assertEqual(compare(reference, _write(root, "same.nds"))["status"], EXIT_IDENTICAL)
            known = _write(root, "known.nds", {0x6C: 9, 0x6D: 9, 0x15E: 9, 0x15F: 9})
            result = compare(reference, known)
            self.assertEqual(result["status"], EXIT_KNOWN_ONLY)
            self.assertEqual(result["differing_bytes"], 4)
            self.assertEqual(result["known_fields"], ["header_crc16", "secure_area_crc16"])
            self.assertEqual(compare(reference, _write(root, "code.nds", {0x6C: 9, 0x200: 9}))["status"], EXIT_DIFFERENT)
            self.assertEqual(compare(reference, _write(root, "short.nds", size=SIZE // 2))["status"], EXIT_DIFFERENT)

    def test_main_returns_the_status_and_two_for_unreadable_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = _write(root, "ref.nds")
            self.assertEqual(main([str(reference), str(_write(root, "k.nds", {0x15E: 1}))]), EXIT_KNOWN_ONLY)
            self.assertEqual(main([str(reference), str(root / "missing.nds")]), EXIT_ERROR)


if __name__ == "__main__":
    unittest.main()
