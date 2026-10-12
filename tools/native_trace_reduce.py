#!/usr/bin/env python3
"""Reduce a native dq9 DSR_TRACE stream to unique ARM9 `<module_id> <addr_hex>` entries.

The trace of a long playthrough has hundreds of millions of rows, so stream it
through a FIFO instead of storing it:

    mkfifo build/native-trace/trace.fifo
    .venv/bin/python tools/native_trace_reduce.py build/native-trace/entries_arm9.txt \\
        < build/native-trace/trace.fifo &
    DSR_TRACE_FILE=build/native-trace/trace.fifo DSR_TRACE_LIMIT=0xFFFFFFFFFF \\
        build/game-trace-fast/game/dq9 ... --frames N

Input rows: `frame cpu pc r0 r1 r2 r3 sp lr module_id` (cpu 0 = ARM9; module_id is the
manifest module id of the generated body). Only cpu 0 rows are kept. Also writes
OUT.meta with row count, last frame and unique entry count: check `last_frame`
against the requested run length to detect a truncated trace.
"""
from __future__ import annotations

import sys


def reduce_rows(lines) -> tuple[set[tuple[int, int]], int, int]:
    entries: set[tuple[int, int]] = set()
    rows = 0
    last_frame = -1
    for line in lines:
        parts = line.split()
        if len(parts) != 10:
            raise ValueError(f"expected 10 fields (module-aware trace), got {len(parts)}: {line!r}")
        rows += 1
        last_frame = int(parts[0])
        if parts[1] == "0":
            entries.add((int(parts[9]), int(parts[2], 16)))
    return entries, rows, last_frame


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: native_trace_reduce.py OUT < trace-rows", file=sys.stderr)
        return 2
    entries, rows, last_frame = reduce_rows(sys.stdin)
    with open(argv[0], "w") as out:
        for module_id, address in sorted(entries):
            out.write(f"{module_id} {address:x}\n")
    with open(argv[0] + ".meta", "w") as meta:
        meta.write(f"rows {rows}\nlast_frame {last_frame}\nunique_arm9_entries {len(entries)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
