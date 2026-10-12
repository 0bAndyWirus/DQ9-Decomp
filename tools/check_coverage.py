#!/usr/bin/env python3
"""Compare executed guest PCs and instruction modes with a recompiler manifest."""

from __future__ import annotations

import argparse
import bisect
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


@dataclass
class IntervalIndex:
    entries: list[tuple[int, int, bool, str]]
    starts: list[int]
    prefix_max_end: list[int]


class CoverageError(ValueError):
    """Malformed manifest or executed-PC coverage input."""


def parse_pcs(lines: Iterable[str]) -> list[tuple[int, bool]]:
    pcs: list[tuple[int, bool]] = []
    for line_number, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 2 or fields[1].lower() not in {"a", "t"}:
            raise CoverageError(f"PC line {line_number}: expected '<hex-address> a|t'")
        try:
            address = int(fields[0], 16)
        except ValueError as error:
            raise CoverageError(f"PC line {line_number}: invalid hexadecimal address {fields[0]!r}") from error
        pcs.append((address, fields[1].lower() == "t"))
    return pcs


def _function_index(manifest: dict[str, Any]) -> dict[int, IntervalIndex]:
    modules = manifest.get("modules")
    if not isinstance(modules, list):
        raise CoverageError("manifest must contain a modules list")
    entries_by_module: dict[int, list[tuple[int, int, bool, str]]] = {}
    for module in modules:
        if not isinstance(module, dict):
            raise CoverageError("manifest modules must be objects")
        name = str(module.get("name", "<unnamed>"))
        try:
            module_id = int(module["module_id"])
            functions = module.get("functions", [])
            if not isinstance(functions, list):
                raise TypeError("functions is not a list")
            entries = entries_by_module.setdefault(module_id, [])
            for function in functions:
                address = int(function["addr"])
                size = int(function["size"])
                thumb = bool(function.get("thumb", False))
                if size > 0:
                    entries.append((address, address + size, thumb, name))
        except (KeyError, TypeError, ValueError) as error:
            raise CoverageError(f"invalid module {name!r}: {error}") from error
    indexed = {}
    for module_id, entries in entries_by_module.items():
        entries.sort()
        maximum = entries[0][1]
        prefix_max_end = []
        for _, end, _, _ in entries:
            maximum = max(maximum, end)
            prefix_max_end.append(maximum)
        indexed[module_id] = IntervalIndex(entries, [entry[0] for entry in entries], prefix_max_end)
    return indexed


def _owners(indexed: dict[int, IntervalIndex], address: int) -> list[tuple[int, int, bool, str]]:
    owners = []
    for interval in indexed.values():
        stop = bisect.bisect_right(interval.starts, address)
        start = bisect.bisect_right(interval.prefix_max_end, address, 0, stop)
        owners.extend(entry for entry in interval.entries[start:stop] if address < entry[1])
    return owners


def _groups(pcs: list[tuple[int, bool]]) -> list[dict[str, Any]]:
    if not pcs:
        return []
    ordered = sorted(set(pcs))
    groups: list[dict[str, Any]] = []
    start, thumb = ordered[0]
    previous = start
    count = 1
    for address, mode in ordered[1:]:
        step = 2 if thumb else 4
        if mode == thumb and address == previous + step:
            previous = address
            count += 1
            continue
        groups.append({"start": start, "end": previous, "thumb": thumb, "count": count})
        start = previous = address
        thumb = mode
        count = 1
    groups.append({"start": start, "end": previous, "thumb": thumb, "count": count})
    return groups


def check_coverage(manifest: dict[str, Any], pcs: Iterable[tuple[int, bool]]) -> dict[str, Any]:
    indexed = _function_index(manifest)
    records = list(pcs)
    uncovered: list[tuple[int, bool]] = []
    mismatches: list[dict[str, Any]] = []
    for address, thumb in sorted(set(records)):
        owners = _owners(indexed, address)
        if not owners:
            uncovered.append((address, thumb))
        elif not any(owner[2] == thumb for owner in owners):
            mismatches.append(
                {
                    "addr": address,
                    "executed_thumb": thumb,
                    "functions": [
                        {"module": owner[3], "start": owner[0], "end": owner[1], "thumb": owner[2]}
                        for owner in owners
                    ],
                }
            )
    return {
        "rows": len(records),
        "unique_pcs": len(set(records)),
        "covered": len(set(records)) - len(uncovered) - len(mismatches),
        "uncovered": uncovered,
        "uncovered_groups": _groups(uncovered),
        "mode_mismatches": mismatches,
    }


def format_report(result: dict[str, Any]) -> str:
    lines = [
        "Executed ARM7 PC coverage report",
        "=================================",
        f"Rows: {result['rows']}; unique PCs: {result['unique_pcs']}; covered: {result['covered']}; "
        f"outside functions: {len(result['uncovered'])}; mode mismatches: {len(result['mode_mismatches'])}",
        "",
        "Outside discovered functions (inclusive address ranges):",
    ]
    for group in result["uncovered_groups"]:
        mode = "Thumb" if group["thumb"] else "ARM"
        lines.append(
            f"  0x{group['start']:08X}..0x{group['end']:08X} ({mode}, {group['count']} PCs)"
        )
    if not result["uncovered_groups"]:
        lines.append("  none")
    lines.extend(["", "Mode mismatches:"])
    for mismatch in result["mode_mismatches"]:
        executed = "Thumb" if mismatch["executed_thumb"] else "ARM"
        owners = ", ".join(
            f"{owner['module']}:{'Thumb' if owner['thumb'] else 'ARM'}@0x{owner['start']:08X}"
            for owner in mismatch["functions"]
        )
        lines.append(f"  0x{mismatch['addr']:08X}: executed {executed}, discovered {owners}")
    if not result["mode_mismatches"]:
        lines.append("  none")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check executed ARM7 PCs against discovered functions and modes")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--pcs", required=True, type=Path, help="text rows of '<hex-address> a|t'")
    parser.add_argument("--out", type=Path, help="optional report path (use ignored build/gen)")
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        pcs = parse_pcs(args.pcs.read_text(encoding="utf-8").splitlines())
        result = check_coverage(manifest, pcs)
        report = format_report(result)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(report, encoding="utf-8")
    except (OSError, json.JSONDecodeError, CoverageError) as error:
        print(f"coverage check failed: {error}", file=sys.stderr)
        return 2
    print(report, end="")
    return 1 if result["uncovered"] or result["mode_mismatches"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
