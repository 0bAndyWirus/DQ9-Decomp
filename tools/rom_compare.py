#!/usr/bin/env python3
"""Compare a rebuilt NDS image with the reference image and classify the differences.

Streams both files in chunks, merges differing bytes into ranges and identifies
the standard NDS secure-area CRC16 at 0x6C-0x6D and header CRC16 at 0x15E-0x15F.
It prints offsets and counts only, never file contents.

Exit status: 0 identical; 3 differences only in those two CRC fields;
1 any other difference or a size mismatch; 2 unreadable input.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import BinaryIO, Iterator, Sequence

CHUNK = 4 * 1024 * 1024
MERGE_GAP = 64
# (start, end inclusive, name): standard NDS header checksum fields.
KNOWN_FIELDS = ((0x6C, 0x6D, "secure_area_crc16"), (0x15E, 0x15F, "header_crc16"))
EXIT_IDENTICAL, EXIT_DIFFERENT, EXIT_ERROR, EXIT_KNOWN_ONLY = 0, 1, 2, 3


def differing_offsets(first: BinaryIO, second: BinaryIO, chunk: int = CHUNK) -> Iterator[int]:
    """Yield every offset where the two streams differ (up to the shorter length)."""
    base = 0
    while True:
        a, b = first.read(chunk), second.read(chunk)
        if not a or not b:
            return
        length = min(len(a), len(b))
        if a[:length] != b[:length]:
            for index in range(length):
                if a[index] != b[index]:
                    yield base + index
        base += length


def merge_ranges(offsets: Iterator[int], gap: int = MERGE_GAP) -> list[list[int]]:
    """Group offsets into [start, end, count] ranges; offsets closer than `gap` share a range."""
    ranges: list[list[int]] = []
    for offset in offsets:
        if ranges and offset - ranges[-1][1] <= gap:
            ranges[-1][1] = offset
            ranges[-1][2] += 1
        else:
            ranges.append([offset, offset, 1])
    return ranges


def classify(offsets: Sequence[int]) -> tuple[list[str], list[int]]:
    """Return (names of known fields that differ, offsets outside every known field)."""
    known: set[str] = set()
    unknown: list[int] = []
    for offset in offsets:
        for start, end, name in KNOWN_FIELDS:
            if start <= offset <= end:
                known.add(name)
                break
        else:
            unknown.append(offset)
    return sorted(known), unknown


def compare(reference: Path, rebuilt: Path, max_offsets: int = 100_000) -> dict:
    sizes = (reference.stat().st_size, rebuilt.stat().st_size)
    offsets: list[int] = []
    with reference.open("rb") as first, rebuilt.open("rb") as second:
        for count, offset in enumerate(differing_offsets(first, second)):
            if count >= max_offsets:
                break
            offsets.append(offset)
    known, unknown = classify(offsets)
    truncated = len(offsets) >= max_offsets
    if sizes[0] != sizes[1] or unknown or truncated:
        status = EXIT_DIFFERENT
    elif offsets:
        status = EXIT_KNOWN_ONLY
    else:
        status = EXIT_IDENTICAL
    return {"status": status, "sizes": sizes, "differing_bytes": len(offsets), "truncated": truncated,
            "known_fields": known, "unknown_offsets": unknown, "ranges": merge_ranges(iter(offsets))}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", type=Path)
    parser.add_argument("rebuilt", type=Path)
    args = parser.parse_args(argv)
    try:
        result = compare(args.reference, args.rebuilt)
    except OSError as error:
        print(f"rom_compare.py: {error}", file=sys.stderr)
        return EXIT_ERROR
    print(f"sizes: reference {result['sizes'][0]}, rebuilt {result['sizes'][1]}")
    print(f"differing bytes: {result['differing_bytes']}{'+' if result['truncated'] else ''}")
    for start, end, count in result["ranges"][:20]:
        print(f"  0x{start:x}..0x{end:x} ({count} byte(s))")
    if result["known_fields"]:
        print("known header fields differing: " + ", ".join(result["known_fields"]))
    if result["unknown_offsets"]:
        print(f"{len(result['unknown_offsets'])} byte(s) outside the known header fields")
    return result["status"]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(EXIT_ERROR)
