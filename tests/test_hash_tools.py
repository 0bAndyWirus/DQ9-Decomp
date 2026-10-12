import contextlib
import io
import tempfile
from pathlib import Path
import unittest

from tools import align_hashes, compare_hashes


def _digest(frame_content: int) -> str:
    return f"{frame_content:016x}"


def _write(path: Path, contents: list[tuple[int, int]]) -> Path:
    path.write_text("".join(f"{i} {_digest(t)} {_digest(b)}\n" for i, (t, b) in enumerate(contents)))
    return path


class CompareHashesTests(unittest.TestCase):
    def _run(self, native: list[tuple[int, int]], oracle: list[tuple[int, int]], *extra: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = ["compare_hashes", str(_write(root / "n.txt", native)), str(_write(root / "o.txt", oracle)), *extra]
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                old = compare_hashes.sys.argv
                compare_hashes.sys.argv = args
                try:
                    code = compare_hashes.main()
                finally:
                    compare_hashes.sys.argv = old
        return code, out.getvalue()

    def test_identical_runs_report_no_divergence(self) -> None:
        frames = [(i, i + 1) for i in range(10)]
        code, text = self._run(frames, frames)
        self.assertEqual(code, 0)
        self.assertIn("top: all frames identical", text)
        self.assertIn("bottom: all frames identical", text)

    def test_first_mismatch_and_ranges_are_reported_per_screen(self) -> None:
        oracle = [(i, i) for i in range(12)]
        native = list(oracle)
        native[3] = (99, 3)       # top differs at 3
        native[4] = (99, 4)       # ... and 4
        native[9] = (9, 99)       # bottom differs at 9
        code, text = self._run(native, oracle)
        self.assertEqual(code, 1)
        self.assertIn("top: first mismatch at frame 3; 2 mismatching frames in 1 range(s)", text)
        self.assertIn("  3..4", text)
        self.assertIn("bottom: first mismatch at frame 9; 1 mismatching frames in 1 range(s)", text)
        self.assertIn("first frame where either screen differs: 3", text)

    def test_first_option_ignores_earlier_frames(self) -> None:
        oracle = [(i, i) for i in range(6)]
        native = list(oracle)
        native[1] = (50, 1)
        code, text = self._run(native, oracle, "--first", "2")
        self.assertEqual(code, 0)
        self.assertIn("compared 4 frames (2..5)", text)


    def test_malformed_and_duplicate_rows_are_input_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            native, oracle = Path(directory) / "native", Path(directory) / "oracle"
            native.write_text("0 1 2\n0 3 4\n")
            oracle.write_text("0 1 2\n")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                self.assertEqual(compare_hashes.main([str(native), str(oracle)]), 2)
            self.assertIn("duplicate frame 0", errors.getvalue())
            native.write_text("oops\n")
            with contextlib.redirect_stderr(errors := io.StringIO()):
                self.assertEqual(compare_hashes.main([str(native), str(oracle)]), 2)
            self.assertIn("expected 3 fields", errors.getvalue())

    def test_asymmetric_coverage_is_not_silently_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            native, oracle = Path(directory) / "native", Path(directory) / "oracle"
            native.write_text("0 1 2\n1 3 4\n")
            oracle.write_text("0 1 2\n")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                self.assertEqual(compare_hashes.main([str(native), str(oracle)]), 1)
            self.assertIn("frames only in native: 1 in 1 range(s): 1", errors.getvalue())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(compare_hashes.main([str(native), str(oracle), "--allow-partial"]), 0)

    def test_a_large_coverage_gap_is_reported_as_ranges_not_one_number_per_frame(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            native, oracle = Path(directory) / "native", Path(directory) / "oracle"
            native.write_text("".join(f"{frame} 1 2\n" for frame in range(20_000)))
            oracle.write_text("".join(f"{frame} 1 2\n" for frame in range(5_000)))
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                self.assertEqual(compare_hashes.main([str(native), str(oracle)]), 1)
            message = errors.getvalue().strip()
            self.assertIn("frames only in native: 15000 in 1 range(s): 5000..19999", message)
            self.assertLess(len(message), 200)

    def test_unreadable_hash_file_is_an_input_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            oracle = Path(directory) / "oracle"
            oracle.write_text("0 1 2\n")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(compare_hashes.main([str(Path(directory) / "missing"), str(oracle)]), 2)


class AlignHashesTests(unittest.TestCase):
    @staticmethod
    def _rows(contents: list[tuple[int, int]]) -> list[tuple[str, str]]:
        return [(_digest(t), _digest(b)) for t, b in contents]

    def test_constant_lag_is_found(self) -> None:
        oracle_content = [(i % 7 + 100 * (i // 7), i % 5 + 100 * (i // 5)) for i in range(120)]
        lag = 9
        native_content = [(0, 0)] * lag + oracle_content
        segments = align_hashes.align(self._rows(native_content), self._rows(oracle_content), 20, 3.0, 0, 119)
        self.assertEqual(segments, [(0, 119, lag)])

    def test_offset_change_is_tracked_across_a_stall(self) -> None:
        oracle_content = [(i, i) for i in range(100)]
        # Native repeats frame 50 for ten extra frames (a longer load): lag 0 before, 10 after.
        native_content = oracle_content[:50] + [oracle_content[50]] * 10 + oracle_content[50:]
        segments = align_hashes.align(self._rows(native_content), self._rows(oracle_content), 20, 3.0, 0, 99)
        self.assertEqual(segments[0][2], 0)
        self.assertEqual(segments[-1][2], 10)
        self.assertEqual(segments[0][0], 0)
        self.assertEqual(segments[-1][1], 99)

    def test_content_that_never_matches_is_not_hidden(self) -> None:
        oracle_content = [(i, i) for i in range(30)]
        native_content = [(1000 + i, i) for i in range(30)]  # top screen always differs
        rows_native, rows_oracle = self._rows(native_content), self._rows(oracle_content)
        segments = align_hashes.align(rows_native, rows_oracle, 5, 3.0, 0, 29)
        first, last, offset = segments[0]
        matches = sum(rows_native[f + offset][0] == rows_oracle[f][0] for f in range(first, last + 1))
        self.assertEqual(matches, 0)


    def test_hash_file_gaps_are_rejected_without_compacting_frame_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gap.txt"
            path.write_text("1 1 2\n2 3 4\n")
            with self.assertRaisesRegex(ValueError, "contiguous starting at 0"):
                align_hashes.read_hashes(path)

    def test_align_rejects_empty_and_invalid_ranges_or_costs(self) -> None:
        rows = self._rows([(1, 2)])
        with self.assertRaisesRegex(ValueError, "must not be empty"):
            align_hashes.align([], rows, 0, 1.0, 0, 0)
        for args in ((rows, rows, -1, 1.0, 0, 0), (rows, rows, 0, 0.0, 0, 0),
                     (rows, rows, 0, 1.0, 1, 0), (rows, rows, 0, 1.0, 0, 1)):
            with self.assertRaises(ValueError):
                align_hashes.align(*args)

if __name__ == "__main__":
    unittest.main()
