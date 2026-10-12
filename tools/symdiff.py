#!/usr/bin/env python3
"""Compare one function symbol between target and base objects with objdiff.

Project resolution: `--project` (default `extracted/dqix-decomp-eu`) selects the
project directory containing objdiff.json and build/eur/report.json. The executable
is `<project>/objdiff-cli` when executable, otherwise `objdiff-cli` from PATH.

Usage:
    symdiff.py <unit> [<symbol>] [--project PATH] [--context N] [--json]
    symdiff.py --find-unit <symbol-or-address> [--project PATH] [--json]

Exit codes: 0 exact match, 1 mismatch, 2 tool error (including timeout/output limit).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path
from typing import Any

DEFAULT_PROJECT = "extracted/dqix-decomp-eu"
DEFAULT_CONTEXT = 3

DIFF_KINDS_ACTUAL = {
    "DIFF_NONE",
    "DIFF_REPLACED",
    "DIFF_DELETE",
    "DIFF_INSERT",
    "DIFF_ARG_MISMATCH",
    "DIFF_OP_MISMATCH",
}


def _error(message: str) -> int:
    print(f"symdiff: error: {message}", file=sys.stderr)
    return 2


def _load_project(project_dir: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    objdiff_path = project_dir / "objdiff.json"
    report_path = project_dir / "build" / "eur" / "report.json"
    if not objdiff_path.is_file():
        raise FileNotFoundError(f"missing project file: {objdiff_path}")
    if not report_path.is_file():
        raise FileNotFoundError(f"missing report file: {report_path}")
    with open(objdiff_path, "r", encoding="utf-8") as f:
        objdiff = json.load(f)
    with open(report_path, "r", encoding="utf-8") as f:
        report = json.load(f)
    return objdiff, report


def _build_unit_maps(
    objdiff: dict[str, Any], report: dict[str, Any]
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]], dict[str, list[str]]]:
    objdiff_unit_map: dict[str, dict[str, Any]] = {
        u["name"]: u for u in objdiff.get("units", []) if "name" in u
    }
    report_unit_map: dict[str, dict[str, Any]] = {
        u["name"]: u for u in report.get("units", []) if "name" in u
    }
    symbol_to_units: dict[str, list[str]] = defaultdict(list)
    for unit_name, ru in report_unit_map.items():
        if unit_name not in objdiff_unit_map:
            continue
        seen: set[str] = set()
        for fn in ru.get("functions", []):
            name = fn.get("name")
            if name and name not in seen:
                seen.add(name)
                symbol_to_units[name].append(unit_name)
    return objdiff_unit_map, report_unit_map, dict(symbol_to_units)


def _find_objdiff_cli(project_dir: Path) -> str:
    bundled = project_dir / ("objdiff-cli.exe" if os.name == "nt" else "objdiff-cli")
    if bundled.is_file() and os.access(bundled, os.X_OK):
        return str(bundled)
    return "objdiff-cli"


def _run_process(command: list[str], timeout: float, max_output: int) -> tuple[int, str, str]:
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=os.name != "nt")
    assert process.stdout is not None and process.stderr is not None
    outputs = {"stdout": bytearray(), "stderr": bytearray()}
    output_lock = threading.Lock()
    exceeded = threading.Event()

    def drain(stream, key: str) -> None:
        while chunk := stream.read(8192):
            with output_lock:
                remaining = max_output - sum(map(len, outputs.values()))
                outputs[key].extend(chunk[:max(0, remaining)])
                if len(chunk) > remaining:
                    exceeded.set()
                    return

    readers = [
        threading.Thread(target=drain, args=(process.stdout, "stdout"), daemon=True),
        threading.Thread(target=drain, args=(process.stderr, "stderr"), daemon=True),
    ]
    for reader in readers:
        reader.start()
    deadline = time.monotonic() + timeout
    try:
        while process.poll() is None:
            if exceeded.is_set():
                raise RuntimeError(f"objdiff-cli output exceeded {max_output} bytes")
            if time.monotonic() >= deadline:
                raise RuntimeError(f"objdiff-cli timed out after {timeout:g} seconds")
            time.sleep(0.01)
        for reader in readers:
            reader.join(timeout=max(0.0, deadline - time.monotonic()))
        if any(reader.is_alive() for reader in readers):
            raise RuntimeError(f"objdiff-cli output streams did not close within {timeout:g} seconds")
        if exceeded.is_set():
            raise RuntimeError(f"objdiff-cli output exceeded {max_output} bytes")
        return (process.returncode or 0, outputs["stdout"].decode("utf-8", "replace"),
                outputs["stderr"].decode("utf-8", "replace"))
    except RuntimeError:
        try:
            if os.name == "nt":
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                process.kill()
            else:
                os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        raise
    finally:
        for reader in readers:
            reader.join(timeout=1)
        process.stdout.close()
        process.stderr.close()


def _run_objdiff(
    project_dir: Path, unit_name: str, symbol_name: str
) -> dict[str, Any]:
    cli = _find_objdiff_cli(project_dir)
    cmd = [
        cli, "diff", "-p", str(project_dir), "-u", unit_name, symbol_name,
        "-o", "-", "--format", "json",
    ]
    try:
        returncode, stdout, stderr = _run_process(cmd, timeout=60.0, max_output=16 * 1024 * 1024)
    except FileNotFoundError as exc:
        raise RuntimeError(f"objdiff-cli not found: {exc}") from exc
    except OSError as exc:
        raise RuntimeError(f"failed to run objdiff-cli: {exc}") from exc
    if returncode != 0:
        err = stderr.strip() or stdout.strip()
        raise RuntimeError(f"objdiff-cli failed (exit {returncode}): {err}")
    try:
        return json.loads(stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"could not parse objdiff JSON: {exc}") from exc


def _find_symbol_in_side(
    side_data: dict[str, Any], symbol_name: str
) -> list[dict[str, Any]] | None:
    for section in side_data.get("sections", []):
        for sym in section.get("symbols", []):
            if sym.get("symbol", {}).get("name") == symbol_name:
                return sym.get("instructions", [])
    return None


def _extract_rows(
    diff_data: dict[str, Any], symbol_name: str
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    left_rows = _find_symbol_in_side(diff_data.get("left", {}), symbol_name)
    right_rows = _find_symbol_in_side(diff_data.get("right", {}), symbol_name)

    if left_rows is None and right_rows is None:
        raise ValueError(f"symbol '{symbol_name}' not found in diff output")
    if left_rows is None:
        left_rows = []
    if right_rows is None:
        right_rows = []
    return left_rows, right_rows


def _to_int(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            try:
                return int(value, 16)
            except ValueError:
                return None
    return None


def _row_addr(left_row: dict[str, Any] | None, right_row: dict[str, Any] | None) -> int | None:
    if left_row:
        addr = _to_int(left_row.get("instruction", {}).get("address"))
        if addr is not None:
            return addr
    if right_row:
        addr = _to_int(right_row.get("instruction", {}).get("address"))
        if addr is not None:
            return addr
    return None


def _format_addr(addr: int | None) -> str:
    if addr is None:
        return ""
    return f"0x{addr:x}"


def _asm_text(row: dict[str, Any] | None) -> str:
    if row is None:
        return "<missing>"
    ins = row.get("instruction", {})
    formatted = ins.get("formatted")
    if formatted is not None:
        return formatted
    return ins.get("mnemonic", "?")


def _reloc_text(row: dict[str, Any] | None) -> str:
    if row is None:
        return ""
    reloc = row.get("instruction", {}).get("relocation")
    if not reloc:
        return ""
    target = reloc.get("target", {})
    sym = target.get("symbol", {}).get("name")
    if sym:
        addend = target.get("addend", "0")
        try:
            addend_int = int(addend)
        except (TypeError, ValueError):
            addend_int = 0
        if addend_int != 0:
            return f"{sym}+{addend}"
        return sym
    return reloc.get("type_name", "?")


def _row_kind(left_row: dict[str, Any] | None, right_row: dict[str, Any] | None) -> str:
    if left_row is None:
        return "MISSING_LEFT"
    if right_row is None:
        return "MISSING_RIGHT"
    left_kind = left_row.get("diff_kind")
    right_kind = right_row.get("diff_kind")
    if left_kind:
        return left_kind
    if right_kind:
        return right_kind
    return ""


def _is_mismatch(left_row: dict[str, Any] | None, right_row: dict[str, Any] | None) -> bool:
    if left_row is None or right_row is None:
        return True
    if left_row.get("diff_kind") or right_row.get("diff_kind"):
        return True
    return False


def _match_percent(
    report_unit_map: dict[str, dict[str, Any]], unit_name: str, symbol_name: str
) -> float | None:
    ru = report_unit_map.get(unit_name)
    if not ru:
        return None
    for fn in ru.get("functions", []):
        if fn.get("name") == symbol_name:
            pct = fn.get("fuzzy_match_percent")
            if pct is not None:
                return float(pct)
    pct = ru.get("measures", {}).get("fuzzy_match_percent")
    if pct is not None:
        return float(pct)
    return None


def _collect_mismatch_blocks(
    rows: list[tuple[dict[str, Any] | None, dict[str, Any] | None]], context: int
) -> list[tuple[int, int]]:
    mismatch_indices = [i for i, (l, r) in enumerate(rows) if _is_mismatch(l, r)]
    if not mismatch_indices:
        return []
    blocks: list[tuple[int, int]] = []
    for idx in mismatch_indices:
        start = max(0, idx - context)
        end = min(len(rows), idx + context + 1)
        if blocks and start <= blocks[-1][1]:
            blocks[-1] = (blocks[-1][0], max(blocks[-1][1], end))
        else:
            blocks.append((start, end))
    return blocks


def _format_text_report(
    unit_name: str,
    symbol_name: str,
    match_pct: float | None,
    left_count: int,
    right_count: int,
    rows: list[tuple[dict[str, Any] | None, dict[str, Any] | None]],
    context: int,
) -> tuple[list[str], bool]:
    pct_text = f"{match_pct}%" if match_pct is not None else "N/A"
    header = (
        f"unit: {unit_name}  symbol: {symbol_name}  "
        f"match: {pct_text}  instructions: {left_count} / {right_count}"
    )
    lines: list[str] = [header]

    blocks = _collect_mismatch_blocks(rows, context)
    if not blocks:
        return lines, True

    max_addr = 0
    max_left = 0
    max_right = 0
    for start, end in blocks:
        for i in range(start, end):
            l, r = rows[i]
            max_addr = max(max_addr, len(_format_addr(_row_addr(l, r))))
            max_left = max(max_left, len(_asm_text(l)))
            max_right = max(max_right, len(_asm_text(r)))

    first = True
    for start, end in blocks:
        if not first:
            lines.append("...")
        first = False
        for i in range(start, end):
            l, r = rows[i]
            addr = _format_addr(_row_addr(l, r))
            left_asm = _asm_text(l)
            right_asm = _asm_text(r)
            kind = _row_kind(l, r)
            if kind:
                line = (
                    f"{addr:{max_addr}}  {left_asm:{max_left}} | "
                    f"{right_asm:{max_right}}  [{kind}]"
                )
            else:
                line = (
                    f"{addr:{max_addr}}  {left_asm:{max_left}} | "
                    f"{right_asm:{max_right}}"
                )
            lines.append(line)
            if kind == "DIFF_ARG_MISMATCH":
                left_reloc = _reloc_text(l)
                right_reloc = _reloc_text(r)
                if left_reloc or right_reloc:
                    detail = f"reloc: {left_reloc} -> {right_reloc}"
                    lines.append(f"{'':{max_addr}}  {detail}")
    return lines, False


def _format_json_report(
    unit_name: str,
    symbol_name: str,
    match_pct: float | None,
    left_count: int,
    right_count: int,
    rows: list[tuple[dict[str, Any] | None, dict[str, Any] | None]],
    context: int,
) -> tuple[str, bool]:
    blocks = _collect_mismatch_blocks(rows, context)
    mismatches: list[dict[str, Any]] = []
    for start, end in blocks:
        for i in range(start, end):
            l, r = rows[i]
            addr = _row_addr(l, r)
            entry: dict[str, Any] = {
                "index": i,
                "address": _format_addr(addr) if addr is not None else None,
                "left": _asm_text(l),
                "right": _asm_text(r),
                "kind": _row_kind(l, r) or None,
            }
            if _row_kind(l, r) == "DIFF_ARG_MISMATCH":
                entry["left_relocation"] = _reloc_text(l) or None
                entry["right_relocation"] = _reloc_text(r) or None
            mismatches.append(entry)
    report = {
        "unit": unit_name,
        "symbol": symbol_name,
        "match_percent": match_pct,
        "left_count": left_count,
        "right_count": right_count,
        "exact": not mismatches,
        "mismatches": mismatches,
    }
    return json.dumps(report, indent=2), not mismatches


def _find_unit(query: str, project_dir: Path) -> list[str]:
    objdiff, report = _load_project(project_dir)
    _, _, symbol_to_units = _build_unit_maps(objdiff, report)

    if re.fullmatch(r"0x[0-9a-fA-F]+", query) or re.fullmatch(r"[0-9a-fA-F]{4,8}", query):
        addr = query.lower().removeprefix("0x")
        matches = {
            unit
            for sym, units in symbol_to_units.items()
            if addr in sym.lower()
            for unit in units
        }
        return sorted(matches)

    return sorted(symbol_to_units.get(query, []))


def _infer_symbol(unit_name: str, report_unit_map: dict[str, dict[str, Any]]) -> str | None:
    ru = report_unit_map.get(unit_name)
    if not ru:
        return None
    functions = [fn["name"] for fn in ru.get("functions", []) if "name" in fn]
    if len(functions) == 1:
        return functions[0]
    return None


def _resolve_args(args: argparse.Namespace) -> tuple[str, str] | None:
    """Return (unit, symbol) or None if the request is a --find-unit query."""
    if args.find_unit:
        return None

    unit = args.unit_or_symbol
    symbol = args.symbol
    if symbol is None:
        try:
            objdiff, report = _load_project(Path(args.project))
            objdiff_unit_map, report_unit_map, _ = _build_unit_maps(objdiff, report)
        except (OSError, ValueError, KeyError) as exc:
            raise RuntimeError(str(exc)) from exc
        if unit not in objdiff_unit_map:
            raise RuntimeError(f"unit '{unit}' not found in objdiff.json")
        symbol = _infer_symbol(unit, report_unit_map)
        if symbol is None:
            raise RuntimeError(
                f"unit '{unit}' does not contain exactly one function; "
                "pass the symbol name explicitly or use --find-unit"
            )
    return unit, symbol


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare one function symbol using objdiff-cli and report only mismatches.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples (invented symbols and addresses):
  symdiff.py src/Recovered/main/synthetic_helper
  symdiff.py _dsd_gap@main_1 synthetic_initializer --context 5
  symdiff.py --find-unit synthetic_helper
  symdiff.py --find-unit 0x1000
        """.strip(),
    )
    parser.add_argument(
        "unit_or_symbol",
        nargs="?",
        help="Unit name (or symbol for --find-unit mode).",
    )
    parser.add_argument(
        "symbol",
        nargs="?",
        help="Function symbol to diff (optional if the unit has one function).",
    )
    parser.add_argument(
        "-p",
        "--project",
        default=DEFAULT_PROJECT,
        help=f"Path to the objdiff project directory (default: {DEFAULT_PROJECT}).",
    )
    parser.add_argument(
        "--context",
        type=int,
        default=DEFAULT_CONTEXT,
        help=f"Number of context rows around each mismatch (default: {DEFAULT_CONTEXT}).",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output a JSON report instead of plain text.",
    )
    parser.add_argument(
        "--find-unit",
        dest="find_unit",
        action="store_true",
        help="Look up the unit containing SYMBOL (or the address in SYMBOL).",
    )

    args = parser.parse_args(argv)

    if args.find_unit:
        if not args.unit_or_symbol:
            parser.error("--find-unit requires a symbol or address argument")
        try:
            matches = _find_unit(args.unit_or_symbol, Path(args.project))
        except (OSError, ValueError, KeyError) as exc:
            return _error(str(exc))
        if not matches:
            return _error(f"no unit found for '{args.unit_or_symbol}'")
        if args.json:
            print(json.dumps({"matches": matches}, indent=2))
        else:
            for unit in matches:
                print(unit)
        return 0

    if not args.unit_or_symbol:
        parser.error("expected a unit name (or --find-unit <symbol>)")

    try:
        resolved = _resolve_args(args)
    except RuntimeError as exc:
        return _error(str(exc))
    assert resolved is not None
    unit_name, symbol_name = resolved

    project_dir = Path(args.project)
    try:
        objdiff, report = _load_project(project_dir)
        _, report_unit_map, _ = _build_unit_maps(objdiff, report)
    except (OSError, ValueError, KeyError) as exc:
        return _error(str(exc))

    if unit_name not in {u["name"] for u in objdiff.get("units", [])}:
        return _error(f"unit '{unit_name}' not found in objdiff.json")

    try:
        diff_data = _run_objdiff(project_dir, unit_name, symbol_name)
    except RuntimeError as exc:
        return _error(str(exc))

    try:
        left_rows, right_rows = _extract_rows(diff_data, symbol_name)
    except ValueError as exc:
        return _error(str(exc))

    left_count = len(left_rows)
    right_count = len(right_rows)
    max_len = max(left_count, right_count)
    rows: list[tuple[dict[str, Any] | None, dict[str, Any] | None]] = [
        (
            left_rows[i] if i < left_count else None,
            right_rows[i] if i < right_count else None,
        )
        for i in range(max_len)
    ]

    match_pct = _match_percent(report_unit_map, unit_name, symbol_name)

    if args.json:
        output, exact = _format_json_report(
            unit_name, symbol_name, match_pct, left_count, right_count, rows, args.context
        )
    else:
        lines, exact = _format_text_report(
            unit_name, symbol_name, match_pct, left_count, right_count, rows, args.context
        )
        output = "\n".join(lines)

    print(output)
    return 0 if exact else 1


if __name__ == "__main__":
    sys.exit(main())
