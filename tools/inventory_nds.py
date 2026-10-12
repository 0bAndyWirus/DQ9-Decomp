#!/usr/bin/env python3
"""Report Nintendo DS image structure without extracting files or assets."""

from __future__ import annotations

import argparse
import mmap
import struct
import sys
from pathlib import Path
from typing import Any


class NDSFormatError(ValueError):
    """The image contains invalid or out-of-bounds Nintendo DS structures."""


def unpack(image: bytes | mmap.mmap, fmt: str, offset: int) -> tuple[int, ...]:
    return struct.unpack_from(fmt, image, offset)


def checked_region(image_size: int, offset: int, size: int, name: str) -> None:
    if offset < 0 or size < 0 or offset > image_size or size > image_size - offset:
        raise NDSFormatError(f"{name} region is outside the image: offset=0x{offset:X}, size=0x{size:X}")


def inventory(image: bytes | mmap.mmap) -> dict[str, Any]:
    image_size = len(image)
    if image_size < 0x200:
        raise NDSFormatError("image is shorter than the 0x200-byte NDS header")
    header = image[:0x200]

    title = header[0:12].split(b"\0", 1)[0].decode("ascii", errors="replace").strip()
    product_code = header[0x0C:0x10].decode("ascii", errors="replace")
    revision = header[0x1E]

    def section(label: str, field_offset: int, *, required: bool = False) -> tuple[int, int]:
        offset, size = unpack(image, "<II", field_offset)
        if required and (offset == 0 or size == 0):
            raise NDSFormatError(f"{label} region is missing")
        if offset or size:
            checked_region(image_size, offset, size, label)
        return offset, size

    def executable(label: str, field_offset: int) -> tuple[int, int]:
        offset = unpack(image, "<I", field_offset)[0]
        size = unpack(image, "<I", field_offset + 12)[0]
        if offset == 0 or size == 0:
            raise NDSFormatError(f"{label} region is missing")
        checked_region(image_size, offset, size, label)
        return offset, size

    arm9_offset, arm9_size = executable("ARM9", 0x20)
    arm7_offset, arm7_size = executable("ARM7", 0x30)
    fnt_offset, fnt_size = section("FNT", 0x40, required=True)
    fat_offset, fat_size = section("FAT", 0x48, required=True)
    arm9_ovl_offset, arm9_ovl_size = section("ARM9 overlay table", 0x50)
    arm7_ovl_offset, arm7_ovl_size = section("ARM7 overlay table", 0x58)

    if fnt_size < 8:
        raise NDSFormatError("FNT is too small to contain its root directory record")
    if fat_size % 8:
        raise NDSFormatError("FAT size is not a multiple of its 8-byte file records")
    if arm9_ovl_size % 32 or arm7_ovl_size % 32:
        raise NDSFormatError("overlay table size is not a multiple of its 32-byte records")

    file_count = fat_size // 8
    for index in range(file_count):
        start, end = unpack(image, "<II", fat_offset + index * 8)
        if end < start:
            raise NDSFormatError(f"FAT file {index} has an end offset before its start")
        checked_region(image_size, start, end - start, f"FAT file {index}")

    directory_count = unpack(image, "<H", fnt_offset + 6)[0]
    if directory_count == 0 or directory_count * 8 > fnt_size:
        raise NDSFormatError("FNT directory table count is invalid or out of bounds")

    # Each directory record supplies its own first file ID; folder IDs are not
    # assigned by a simple depth-first walk in all retail images.
    visited: set[int] = set()
    file_ids: set[int] = set()
    stack: list[tuple[int, int | None, list[tuple[str, int]], int]] = []

    def enter_directory(directory_id: int, parent_id: int | None) -> None:
        index = directory_id & 0x0FFF
        if directory_id & 0xF000 != 0xF000 or index >= directory_count:
            raise NDSFormatError(f"FNT contains invalid directory ID 0x{directory_id:04X}")
        if directory_id in visited:
            raise NDSFormatError(f"FNT directory 0x{directory_id:04X} is referenced more than once")
        visited.add(directory_id)
        record_offset = fnt_offset + index * 8
        name_table_offset, first_file_id, parent_or_count = unpack(image, "<IHH", record_offset)
        if parent_id is None:
            if directory_id != 0xF000 or parent_or_count != directory_count:
                raise NDSFormatError("FNT root directory record is inconsistent")
        elif parent_or_count != parent_id:
            raise NDSFormatError(f"FNT directory 0x{directory_id:04X} has an inconsistent parent ID")

        name_cursor = fnt_offset + name_table_offset
        fnt_end = fnt_offset + fnt_size
        entries: list[tuple[str, int]] = []
        directory_file_id = first_file_id
        while True:
            if name_cursor >= fnt_end:
                raise NDSFormatError(f"FNT directory 0x{directory_id:04X} has no terminator")
            name_length = image[name_cursor]
            name_cursor += 1
            if name_length == 0:
                break
            is_directory = bool(name_length & 0x80)
            name_length &= 0x7F
            if name_length == 0 or name_cursor + name_length > fnt_end:
                raise NDSFormatError(f"FNT directory 0x{directory_id:04X} has an invalid name entry")
            name_cursor += name_length
            if is_directory:
                if name_cursor + 2 > fnt_end:
                    raise NDSFormatError(f"FNT directory 0x{directory_id:04X} has a truncated child ID")
                child_id = unpack(image, "<H", name_cursor)[0]
                name_cursor += 2
                entries.append(("directory", child_id))
            else:
                entries.append(("file", directory_file_id))
                directory_file_id += 1
        stack.append((directory_id, parent_id, entries, 0))

    enter_directory(0xF000, None)
    while stack:
        directory_id, parent_id, entries, entry_index = stack[-1]
        if entry_index == len(entries):
            stack.pop()
            continue
        kind, child_id = entries[entry_index]
        stack[-1] = (directory_id, parent_id, entries, entry_index + 1)
        if kind == "file":
            if child_id >= file_count or child_id in file_ids:
                raise NDSFormatError(f"FNT references invalid or duplicate FAT file {child_id}")
            file_ids.add(child_id)
        else:
            enter_directory(child_id, directory_id)

    if len(visited) != directory_count:
        raise NDSFormatError(f"FNT declares {directory_count} directories but references {len(visited)}")
    named_file_count = len(file_ids)

    overlay_counts = {}
    for label, offset, size in (
        ("ARM9", arm9_ovl_offset, arm9_ovl_size),
        ("ARM7", arm7_ovl_offset, arm7_ovl_size),
    ):
        count = size // 32
        for index in range(count):
            record_offset = offset + index * 32
            file_id = unpack(image, "<I", record_offset + 0x18)[0]
            if file_id >= file_count:
                raise NDSFormatError(f"{label} overlay {index} references missing FAT file {file_id}")
        overlay_counts[label] = count

    return {
        "image_size": image_size,
        "title": title,
        "product_code": product_code,
        "revision": revision,
        "arm9_size": arm9_size,
        "arm7_size": arm7_size,
        "nitrofs_files": file_count,
        "nitrofs_named_files": named_file_count,
        "nitrofs_unnamed_files": file_count - named_file_count,
        "nitrofs_directories": directory_count,
        "arm9_overlays": overlay_counts["ARM9"],
        "arm7_overlays": overlay_counts["ARM7"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inventory a local NDS image without extracting files or assets.")
    parser.add_argument("image", type=Path, help="path to a locally held .nds file")
    args = parser.parse_args()

    try:
        with args.image.open("rb") as source:
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as image:
                result = inventory(image)
    except (OSError, NDSFormatError, ValueError) as error:
        print(f"Cannot inventory local NDS image: {error}", file=sys.stderr)
        return 2

    print(f"Title: {result['title'] or '(none)'}")
    print(f"Product code: {result['product_code']}")
    print(f"Revision: {result['revision']}")
    print(f"Image size: {result['image_size']} bytes")
    print(f"ARM9 code: {result['arm9_size']} bytes")
    print(f"ARM7 code: {result['arm7_size']} bytes")
    print(f"NitroFS: {result['nitrofs_files']} FAT files ({result['nitrofs_named_files']} named, {result['nitrofs_unnamed_files']} unnamed), {result['nitrofs_directories']} directories")
    print(f"Overlays: ARM9 {result['arm9_overlays']}, ARM7 {result['arm7_overlays']}")
    print("Read-only structural inventory; no files or assets were extracted.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
