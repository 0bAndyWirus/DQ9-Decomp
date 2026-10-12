import tempfile
import unittest
from pathlib import Path

from tools.validate_hashes import coverage_difference, main, missing_ranges, parse_hash_file


class ParseHashFileTests(unittest.TestCase):
    def test_valid_rows_are_parsed_and_lowercased(self):
        rows, problems = parse_hash_file("0 AABB ccdd\n1 0011 2233\n")
        self.assertEqual(problems, [])
        self.assertEqual(rows[0], ("aabb", "ccdd"))

    def test_each_kind_of_malformed_row_is_reported_with_its_line_number(self):
        text = "0 aa bb\nnot-a-row\nx aa bb\n3 zz bb\n4 aa bb\n4 cc dd\n"
        rows, problems = parse_hash_file(text)
        joined = "\n".join(problems)
        self.assertIn("line 2: expected 3 fields", joined)
        self.assertIn("line 3: frame index", joined)
        self.assertIn("line 4: digests must be", joined)
        self.assertIn("line 6: duplicate frame 4", joined)
        self.assertEqual(sorted(rows), [0, 4])

    def test_missing_ranges_only_between_present_frames(self):
        self.assertEqual(missing_ranges([1, 2, 5, 6, 9]), [(3, 4), (7, 8)])
        self.assertEqual(missing_ranges([7]), [])

    def test_coverage_difference_reports_each_side(self):
        first = {1: ("a", "b"), 2: ("a", "b")}
        second = {2: ("a", "b"), 3: ("a", "b")}
        self.assertEqual(coverage_difference(first, second), {"only_first": [1], "only_second": [3]})


class CommandTests(unittest.TestCase):
    def _write(self, directory: str, name: str, text: str) -> Path:
        path = Path(directory) / name
        path.write_text(text)
        return path

    def test_gap_inside_a_file_fails_unless_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._write(directory, "a.txt", "0 aa bb\n2 aa bb\n")
            self.assertEqual(main([str(path)]), 1)
            self.assertEqual(main([str(path), "--allow-gaps"]), 0)

    def test_two_files_with_different_coverage_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            first = self._write(directory, "a.txt", "0 aa bb\n1 aa bb\n")
            second = self._write(directory, "b.txt", "1 aa bb\n2 aa bb\n")
            same = self._write(directory, "c.txt", "0 aa bb\n1 cc dd\n")
            self.assertEqual(main([str(first), str(second)]), 1)
            self.assertEqual(main([str(first), str(same)]), 0)

    def test_unreadable_file_is_exit_two(self):
        self.assertEqual(main(["/nonexistent/hashes.txt"]), 2)


if __name__ == "__main__":
    unittest.main()
