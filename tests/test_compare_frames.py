import contextlib
import io
import struct
import tempfile
from pathlib import Path
import unittest
import zlib

from tools.compare_frames import ImageError, compare, read_png, read_ppm


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def _png(width: int, height: int, rows: list[bytes], filter_type: int = 0, channels: int = 3) -> bytes:
    stride = width * channels
    raw = bytearray()
    previous = bytes(stride)
    for row in rows:
        raw.append(filter_type)
        for x, value in enumerate(row):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            if filter_type == 1:
                raw.append((value - left) & 255)
            elif filter_type == 2:
                raw.append((value - up) & 255)
            else:
                raw.append(value)
        previous = row
    color_type = 2 if channels == 3 else 6
    return (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(bytes(raw)))
        + _chunk(b"IEND", b"")
    )


class CompareFramesTests(unittest.TestCase):
    rows = [bytes([1, 2, 3, 40, 50, 60]), bytes([7, 8, 9, 100, 110, 120])]

    def test_png_filters_decode_to_the_same_pixels(self):
        expected = b"".join(self.rows)
        for filter_type in (0, 1, 2):
            width, height, pixels = read_png(_png(2, 2, self.rows, filter_type))
            self.assertEqual((width, height, pixels), (2, 2, expected))

    def test_rgba_alpha_is_dropped(self):
        rows = [bytes([1, 2, 3, 255, 4, 5, 6, 0])]
        self.assertEqual(read_png(_png(2, 1, rows, channels=4))[2], bytes([1, 2, 3, 4, 5, 6]))

    def test_ppm_and_png_compare_with_bounding_box(self):
        png = read_png(_png(2, 2, self.rows))
        ppm_data = b"P6\n2 2\n255\n" + b"".join(self.rows[:1]) + bytes([7, 8, 9, 100, 110, 125])
        result = compare(read_ppm(ppm_data), png)
        self.assertEqual(result["different"], 1)
        self.assertEqual(result["max_delta"], 5)
        self.assertEqual(result["bbox"], (1, 1, 1, 1))

    def test_ppm_raster_beginning_with_whitespace_bytes_is_preserved(self):
        for first in (b"\n\x00\x00", b" \x01\x02", b"\t\r\x0b"):
            width, height, pixels = read_ppm(b"P6\n1 1\n255\n" + first)
            self.assertEqual((width, height, pixels), (1, 1, first))

    def test_cli_reports_identical_and_different_files(self):
        from tools.compare_frames import main

        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "a.png"
            ppm = Path(directory) / "b.ppm"
            png.write_bytes(_png(2, 2, self.rows))
            ppm.write_bytes(b"P6\n2 2\n255\n" + b"".join(self.rows))
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(main([str(ppm), str(png)]), 0)
            self.assertIn("0/4 pixels differ", out.getvalue())
            ppm.write_bytes(b"P6\n2 2\n255\n" + b"\0" * 12)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(ppm), str(png)]), 1)

    def test_size_mismatch_and_bad_data_are_errors(self):
        with self.assertRaises(ImageError):
            compare((1, 1, b"\0\0\0"), (2, 1, b"\0" * 6))
        with self.assertRaises(ImageError):
            read_ppm(b"P6\n2 2\n255\nshort")



    def test_png_rejects_oversized_dimensions_before_decompression(self):
        ihdr = struct.pack(">IIBBBBB", 0xFFFFFFFF, 2, 8, 2, 0, 0, 0)
        data = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(b"")) + _chunk(b"IEND", b"")
        with self.assertRaisesRegex(ImageError, "dimensions"):
            read_png(data)

    def test_png_decompression_is_bounded_to_declared_scanline_size(self):
        ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
        bomb = zlib.compress(b"\0" + b"X" * 100_000)
        data = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", bomb) + _chunk(b"IEND", b"")
        with self.assertRaisesRegex(ImageError, "exceeds declared size"):
            read_png(data)

    def test_png_rejects_bad_chunk_crc_order_length_and_format(self):
        ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
        good = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(b"\0abc")) + _chunk(b"IEND", b"")
        bad_crc = bytearray(good)
        bad_crc[29] ^= 1
        for data, message in (
            (bytes(bad_crc), "CRC"),
            (b"\x89PNG\r\n\x1a\n" + _chunk(b"IDAT", b"") + _chunk(b"IHDR", ihdr), "first chunk"),
            (good[:-12], "IEND"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ImageError, message):
                read_png(data)
        unsupported = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 4, 2, 0, 0, 0)) + _chunk(b"IDAT", b"") + _chunk(b"IEND", b"")
        with self.assertRaisesRegex(ImageError, "8-bit RGB/RGBA"):
            read_png(unsupported)

    def test_cli_returns_two_for_invalid_png(self):
        from tools.compare_frames import main

        with tempfile.TemporaryDirectory() as directory:
            bad, good = Path(directory) / "bad.png", Path(directory) / "good.png"
            bad.write_bytes(b"\x89PNG\r\n\x1a\nnot-a-chunk")
            good.write_bytes(_png(2, 2, self.rows))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main([str(bad), str(good)]), 2)
if __name__ == "__main__":
    unittest.main()
