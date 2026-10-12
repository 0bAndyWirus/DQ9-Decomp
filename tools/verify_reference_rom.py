#!/usr/bin/env python3
"""Check a locally available image against a caller-supplied fingerprint profile."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Sequence


CHUNK_SIZE = 4 * 1024 * 1024


@dataclass(frozen=True)
class Fingerprint:
    size: int
    product_code: str | None
    revision: int | None
    sha1: str
    md5: str
    crc32: int


def fingerprint(stream: BinaryIO) -> Fingerprint:
    """Read a seekable local image to hash it and inspect only its NDS header."""
    stream.seek(0)
    header = stream.read(0x1F)
    product_code = None
    revision = None
    if len(header) >= 0x1F:
        try:
            product_code = header[0x0C:0x10].decode("ascii")
        except UnicodeDecodeError:
            pass
        revision = header[0x1E]

    sha1 = hashlib.sha1()
    md5 = hashlib.md5(usedforsecurity=False)
    crc32 = 0
    size = 0
    stream.seek(0)
    while chunk := stream.read(CHUNK_SIZE):
        size += len(chunk)
        sha1.update(chunk)
        md5.update(chunk)
        crc32 = zlib.crc32(chunk, crc32)

    return Fingerprint(size, product_code, revision, sha1.hexdigest(), md5.hexdigest(), crc32)


def load_profile(path: Path) -> Fingerprint:
    """Read a private JSON profile; no compatible-image fingerprints are embedded."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("fingerprint profile must be a JSON object")
    size, revision = data["size"], data["revision"]
    code, sha1, md5 = data["product_code"], data["sha1"], data["md5"]
    crc32 = int(data["crc32"], 0) if isinstance(data["crc32"], str) else data["crc32"]
    if not isinstance(size, int) or isinstance(size, bool) or size < 0:
        raise ValueError("profile size must be a nonnegative integer")
    if not isinstance(revision, int) or isinstance(revision, bool) or not 0 <= revision <= 255:
        raise ValueError("profile revision must be an integer in 0..255")
    if not isinstance(code, str) or len(code) != 4 or not code.isascii():
        raise ValueError("profile product_code must be four ASCII characters")
    for name, value, length in (("sha1", sha1, 40), ("md5", md5, 32)):
        if not isinstance(value, str) or len(value) != length or any(c not in "0123456789abcdefABCDEF" for c in value):
            raise ValueError(f"profile {name} must be a {length}-character hexadecimal digest")
    if not isinstance(crc32, int) or isinstance(crc32, bool) or not 0 <= crc32 <= 0xFFFFFFFF:
        raise ValueError("profile crc32 must be an unsigned 32-bit integer")
    return Fingerprint(size, code, revision, sha1.lower(), md5.lower(), crc32)


def mismatches(value: Fingerprint, expected: Fingerprint) -> tuple[str, ...]:
    fields = (
        ("file size", "size"), ("product code", "product_code"), ("revision", "revision"),
        ("SHA-1", "sha1"), ("MD5", "md5"), ("CRC-32", "crc32"),
    )
    return tuple(name for name, field in fields if getattr(value, field) != getattr(expected, field))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, help="already locally available compatible image (or DQ9_ROM)")
    parser.add_argument("--profile", required=True, type=Path, help="private JSON fingerprint profile")
    args = parser.parse_args(argv)
    image_path = args.rom or (Path(os.environ["DQ9_ROM"]) if os.environ.get("DQ9_ROM") else None)
    if image_path is None:
        parser.error("select an already locally available compatible image with --rom or DQ9_ROM")

    try:
        expected = load_profile(args.profile)
        with image_path.open("rb") as image:
            result = fingerprint(image)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Cannot read image/profile: {error}", file=sys.stderr)
        return 2

    print(f"Size: {result.size} bytes")
    print(f"Product code: {result.product_code or 'unreadable'}")
    print(f"Revision: {result.revision if result.revision is not None else 'unreadable'}")
    print(f"SHA-1: {result.sha1}")
    print(f"MD5: {result.md5}")
    print(f"CRC-32: {result.crc32:08X}")

    differences = mismatches(result, expected)
    if differences:
        print("Fingerprint does not match the supplied profile.", file=sys.stderr)
        print("Mismatched fields: " + ", ".join(differences), file=sys.stderr)
        return 1

    print("Fingerprint matches the supplied profile.")
    print("This verifies identity fingerprints only; it does not extract or upload data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
