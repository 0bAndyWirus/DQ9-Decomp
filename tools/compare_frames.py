#!/usr/bin/env python3
"""Compare two same-size RGB screen captures (PPM P6 or non-interlaced 8-bit RGB/RGBA PNG).

Prints one summary line per pair: differing pixels, percentage, bounding box and
the largest per-channel difference. Exit status is 0 when identical, 1 otherwise.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class ImageError(ValueError):
    pass


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(data: bytes) -> tuple[int, int, bytes]:
    if not data.startswith(PNG_SIGNATURE):
        raise ImageError("not a PNG file")
    offset = len(PNG_SIGNATURE)
    header: tuple[int, int, int, int, int, int, int] | None = None
    seen_idat = False
    seen_plte = False
    ended_idat = False
    seen_iend = False
    compressed = bytearray()
    first_chunk = True
    while offset < len(data):
        if offset + 12 > len(data):
            raise ImageError("truncated PNG chunk header")
        length, kind = struct.unpack_from(">I4s", data, offset)
        if any(not (65 <= byte <= 90 or 97 <= byte <= 122) for byte in kind) or kind[2] & 0x20:
            raise ImageError("invalid PNG chunk type")
        end = offset + 12 + length
        if end > len(data):
            raise ImageError("truncated PNG chunk")
        body = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != expected_crc:
            raise ImageError(f"bad CRC in PNG {kind.decode('ascii', 'replace')} chunk")
        offset = end
        if first_chunk and kind != b"IHDR":
            raise ImageError("PNG IHDR must be the first chunk")
        first_chunk = False
        if kind == b"IHDR":
            if header is not None or length != 13:
                raise ImageError("PNG must contain exactly one 13-byte IHDR")
            header = struct.unpack(">IIBBBBB", body)
        elif header is None:
            raise ImageError("PNG chunk appears before IHDR")
        elif kind == b"IDAT":
            if ended_idat:
                raise ImageError("PNG IDAT chunks must be consecutive")
            seen_idat = True
            compressed.extend(body)
        elif kind == b"IEND":
            if length != 0 or not seen_idat:
                raise ImageError("invalid PNG IEND chunk or missing IDAT")
            seen_iend = True
            if offset != len(data):
                raise ImageError("trailing data after PNG IEND")
            break
        else:
            if seen_idat:
                ended_idat = True
            if kind == b"PLTE":
                if seen_plte or seen_idat or length == 0 or length > 768 or length % 3:
                    raise ImageError("invalid or misplaced PNG PLTE chunk")
                seen_plte = True
            elif kind[0] & 0x20 == 0:
                raise ImageError(f"unsupported critical PNG chunk {kind.decode('ascii', 'replace')}")
    if header is None:
        raise ImageError("PNG has no IHDR")
    if not seen_iend:
        raise ImageError("PNG has no IEND")
    width, height, depth, color_type, compression, filtering, interlace = header
    if not width or not height or width > 16384 or height > 16384:
        raise ImageError("PNG dimensions must be between 1 and 16384")
    if depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise ImageError("only non-interlaced 8-bit RGB/RGBA PNG is supported")
    if compression != 0 or filtering != 0:
        raise ImageError("unsupported PNG compression or filter method")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    expected_size = height * (stride + 1)
    if expected_size > 64 * 1024 * 1024:
        raise ImageError("PNG decompressed pixel data exceeds 64 MiB limit")
    try:
        decoder = zlib.decompressobj()
        raw = decoder.decompress(bytes(compressed), expected_size + 1)
        if len(raw) > expected_size or decoder.unconsumed_tail:
            raise ImageError("PNG decompressed pixel data exceeds declared size")
        raw += decoder.flush()
    except zlib.error as exc:
        raise ImageError(f"invalid PNG compressed data: {exc}") from exc
    if not decoder.eof or decoder.unused_data:
        raise ImageError("invalid PNG compressed stream")
    if len(raw) != expected_size:
        raise ImageError("PNG pixel data has the wrong length")
    rgb = bytearray(width * height * 3)
    previous = bytearray(stride)
    rgb_offset = 0
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if filter_type == 1:
                line[x] = (line[x] + left) & 255
            elif filter_type == 2:
                line[x] = (line[x] + up) & 255
            elif filter_type == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 255
            elif filter_type == 4:
                line[x] = (line[x] + _paeth(left, up, up_left)) & 255
            elif filter_type != 0:
                raise ImageError(f"unknown PNG filter {filter_type}")
        if channels == 3:
            rgb[rgb_offset:rgb_offset + stride] = line
            rgb_offset += stride
        else:
            for x in range(0, stride, 4):
                rgb[rgb_offset:rgb_offset + 3] = line[x:x + 3]
                rgb_offset += 3
        previous = line
    return width, height, bytes(rgb)


def read_ppm(data: bytes) -> tuple[int, int, bytes]:
    """Parse a binary PPM, consuming exactly one whitespace byte after maxval.

    Raster bytes may themselves be whitespace values, so they must not be
    stripped or split off with bytes.split().
    """
    whitespace = b" \t\r\n\x0b\x0c"
    position = 0
    tokens: list[bytes] = []
    while len(tokens) < 4:
        while position < len(data) and data[position] in whitespace:
            position += 1
        if position < len(data) and data[position] == 0x23 and tokens:  # '#' comment inside header
            while position < len(data) and data[position] != 0x0A:
                position += 1
            continue
        start = position
        while position < len(data) and data[position] not in whitespace:
            position += 1
        if start == position:
            raise ImageError("truncated PPM header")
        tokens.append(data[start:position])
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ImageError("only binary PPM (P6, maxval 255) is supported")
    if position >= len(data) or data[position] not in whitespace:
        raise ImageError("PPM header is not followed by whitespace")
    pixels = data[position + 1 :]
    try:
        width, height = int(tokens[1]), int(tokens[2])
    except ValueError as error:
        raise ImageError("invalid PPM dimensions") from error
    if len(pixels) != width * height * 3:
        raise ImageError("PPM pixel data has the wrong length")
    return width, height, pixels


def read_image(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    return read_png(data) if data.startswith(PNG_SIGNATURE) else read_ppm(data)


def compare(first: tuple[int, int, bytes], second: tuple[int, int, bytes]) -> dict[str, object]:
    if first[:2] != second[:2]:
        raise ImageError(f"size mismatch: {first[0]}x{first[1]} vs {second[0]}x{second[1]}")
    width, height, a = first
    b = second[2]
    different = 0
    max_delta = 0
    min_x, min_y, max_x, max_y = width, height, -1, -1
    for index in range(width * height):
        o = index * 3
        if a[o : o + 3] == b[o : o + 3]:
            continue
        different += 1
        max_delta = max(max_delta, abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
        y, x = divmod(index, width)
        min_x, max_x = min(min_x, x), max(max_x, x)
        min_y, max_y = min(min_y, y), max(max_y, y)
    return {
        "pixels": width * height,
        "different": different,
        "max_delta": max_delta,
        "bbox": None if different == 0 else (min_x, min_y, max_x, max_y),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path)
    parser.add_argument("reference", type=Path)
    args = parser.parse_args(argv)
    try:
        result = compare(read_image(args.native), read_image(args.reference))
    except (ImageError, OSError, zlib.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    percent = 100.0 * result["different"] / result["pixels"]
    print(
        f"{args.native.name} vs {args.reference.name}: {result['different']}/{result['pixels']} pixels differ "
        f"({percent:.2f}%), max channel delta {result['max_delta']}, bbox {result['bbox']}"
    )
    return 0 if result["different"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
