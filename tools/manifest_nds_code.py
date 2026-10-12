#!/usr/bin/env python3
"""Emit metadata and hashes for local NDS executable modules; never extract them."""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import struct
import sys
from pathlib import Path
from typing import Any

if __package__:
    from .inventory_nds import NDSFormatError, inventory, unpack
else:
    from inventory_nds import NDSFormatError, inventory, unpack


def sha256_region(image: bytes | mmap.mmap, offset: int, size: int) -> str:
    view = memoryview(image)
    region = view[offset : offset + size]
    try:
        return hashlib.sha256(region).hexdigest()
    finally:
        region.release()
        view.release()


def build_manifest(image: bytes | mmap.mmap) -> dict[str, Any]:
    image_info = inventory(image)
    modules: list[dict[str, Any]] = []

    for name, cpu, header_offset in (
        ("arm9.main", "ARM9", 0x20),
        ("arm7.main", "ARM7", 0x30),
    ):
        rom_offset, entry_address, load_address, size = unpack(image, "<IIII", header_offset)
        modules.append(
            {
                "module_id": name,
                "cpu": cpu,
                "kind": "main",
                "rom_offset": rom_offset,
                "stored_size": size,
                "entry_address": entry_address,
                "load_address": load_address,
                "ram_size": size,
                "sha256": sha256_region(image, rom_offset, size),
                "decomp_status": "not_started",
            }
        )

    fat_offset, fat_size = unpack(image, "<II", 0x48)
    file_count = fat_size // 8
    for cpu, table_offset_field in (("ARM9", 0x50), ("ARM7", 0x58)):
        table_offset, table_size = unpack(image, "<II", table_offset_field)
        seen_overlay_ids: set[int] = set()
        for table_index in range(table_size // 32):
            record = struct.unpack_from("<8I", image, table_offset + table_index * 32)
            overlay_id, load_address, ram_size, bss_size, init_start, init_end, file_id, metadata_word = record
            if overlay_id in seen_overlay_ids:
                raise NDSFormatError(f"{cpu} overlay table repeats overlay ID {overlay_id}")
            seen_overlay_ids.add(overlay_id)
            if file_id >= file_count:
                raise NDSFormatError(f"{cpu} overlay {overlay_id} references missing FAT file {file_id}")
            file_offset, file_end = unpack(image, "<II", fat_offset + file_id * 8)
            stored_size = file_end - file_offset
            modules.append(
                {
                    "module_id": f"{cpu.lower()}.overlay.{overlay_id:03d}",
                    "cpu": cpu,
                    "kind": "overlay",
                    "overlay_id": overlay_id,
                    "overlay_table_index": table_index,
                    "fat_file_id": file_id,
                    "rom_offset": file_offset,
                    "stored_size": stored_size,
                    "load_address": load_address,
                    "ram_size": ram_size,
                    "bss_size": bss_size,
                    "static_init_start": init_start,
                    "static_init_end": init_end,
                    "overlay_metadata_word": metadata_word,
                    "sha256": sha256_region(image, file_offset, stored_size),
                    "decomp_status": "not_started",
                }
            )

    return {
        "schema_version": 1,
        "image": {
            "product_code": image_info["product_code"],
            "revision": image_info["revision"],
            "size": image_info["image_size"],
        },
        "module_count": len(modules),
        "decompiled_module_count": 0,
        "modules": modules,
        "payload_policy": "Metadata and hashes only; module bytes remain in the user-supplied local image.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="List local NDS executable-module metadata and hashes without extracting module bytes."
    )
    parser.add_argument("image", type=Path, help="path to a locally held .nds file")
    args = parser.parse_args()

    try:
        with args.image.open("rb") as source:
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as image:
                result = build_manifest(image)
    except (OSError, NDSFormatError, ValueError, struct.error) as error:
        print(f"Cannot manifest local NDS executable modules: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
