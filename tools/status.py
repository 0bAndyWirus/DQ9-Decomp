#!/usr/bin/env python3
"""One-screen project status: decomp report totals, module checksums, pending state.

Reads only; `--modules` runs `ninja check_modules` (needs the project workspace to be idle:
take no integration lock while it runs). Exit 0 means the report is valid and any
requested module check passed; a module check skipped because integration holds the
lock is a documented non-failure and never prints a module-success line.
`--json` selects machine-readable output; `--all` lists every candidate author.
The expected module count comes from the project configuration or --expected-modules.
"""

from __future__ import annotations

import argparse
import io
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import yaml

if __package__:
    from .port_eu_unit import GAP_ANCHOR_PREFIX
    from .host_tools import lock_file
else:
    from port_eu_unit import GAP_ANCHOR_PREFIX
    from host_tools import lock_file


def _gap_anchor_adjustments(report: dict) -> tuple[int, int, int, int]:
    count = size = matched_count = matched_size = 0
    for unit in report.get("units", ()):
        if not isinstance(unit, dict):
            continue
        for function in unit.get("functions", ()):
            if not isinstance(function, dict) or not str(function.get("name", "")).startswith(GAP_ANCHOR_PREFIX):
                continue
            raw_size = function.get("size")
            size_value = int(raw_size, 0) if isinstance(raw_size, str) else int(raw_size)
            if size_value < 0:
                raise ValueError("gap-anchor function size must be nonnegative")
            count += 1
            size += size_value
            fuzzy = function.get("fuzzy_match_percent")
            if fuzzy is not None and float(fuzzy) == 100.0:
                matched_count += 1
                matched_size += size_value
    return count, size, matched_count, matched_size



ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT / "workspace"


def expected_module_count(project: Path) -> int:
    """Count the modules declared by the project's ARM9 configuration."""
    try:
        config = yaml.safe_load((project / "config" / "eur" / "arm9" / "config.yaml").read_text())
    except yaml.YAMLError as error:
        raise ValueError(f"malformed project module config: {error}") from error
    if not isinstance(config, dict) or not isinstance(config.get("main_module"), dict):
        raise ValueError("project config must declare main_module")
    entries = [config["main_module"]]
    for key in ("autoloads", "overlays"):
        modules = config.get(key, [])
        if not isinstance(modules, list) or any(not isinstance(module, dict) for module in modules):
            raise ValueError(f"project config {key} must be a module list")
        entries.extend(modules)
    return len(entries)


def report_data(project: Path | None = None) -> tuple[str, dict[str, int | float]]:
    path = (project or WORKSPACE) / "build" / "eur" / "report.json"
    if not path.is_file():
        raise ValueError(f"report missing: {path}")
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
        measures = report["measures"]
        anchor_count, anchor_size, matched_anchor_count, matched_anchor_size = _gap_anchor_adjustments(report)
        funcs = int(measures["matched_functions"]) - matched_anchor_count
        total = int(measures["total_functions"]) - anchor_count
        code = int(measures["matched_code"]) - matched_anchor_size
        total_code = int(measures["total_code"]) - anchor_size
        complete_units, total_units = int(measures["complete_units"]), int(measures["total_units"])
        if total <= 0 or total_code <= 0 or total_units < 0:
            raise ValueError("report totals must be positive")
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        raise ValueError(f"malformed report {path}: {error}") from error
    age = (time.time() - path.stat().st_mtime) / 60
    line = (f"report: {funcs}/{total} functions ({funcs / total:.1%}), {code}/{total_code} bytes "
            f"({code / total_code:.2%}), units {complete_units}/{total_units} [{age:.0f} min old]")
    return line, {"matched_functions": funcs, "total_functions": total, "matched_code": code,
                  "total_code": total_code, "complete_units": complete_units, "total_units": total_units,
                  "age_minutes": age}


def report_line() -> str:
    return report_data()[0]


LOCK = WORKSPACE / "build" / "scratch" / "integrate.lock"


def integration_running(project: Path | None = None) -> bool:
    """True when another process holds the integration lock (not merely the file)."""
    lock = project / "build" / "scratch" / "integrate.lock" if project is not None else LOCK
    if not lock.exists():
        return False
    with open(lock, "a") as handle:
        try:
            lock_file(handle)
        except OSError:
            return True
        lock_file(handle, unlock=True)
    return False


def pending_lines(all_authors: bool = False, project: Path | None = None) -> tuple[list[str], dict[str, object]]:
    lines = []
    running = integration_running(project)
    if running:
        lines.append("integration RUNNING (lock held)")
    if ((project or WORKSPACE) / "build" / "scratch" / "integrate_candidates_rollback.json").exists():
        lines.append("journal present: active run owns it" if running
                     else "PENDING journal with no active run: integrate_candidates.py --rollback")
    candidates = (project or WORKSPACE) / "src" / "_candidates"
    counts = {}
    if candidates.is_dir():
        counts = {d.name: len(list(d.glob("*.json"))) for d in sorted(candidates.iterdir()) if d.is_dir()}
    waiting = {name: n for name, n in counts.items() if n}
    total = sum(waiting.values())
    authors = sorted(waiting.items(), key=lambda item: (-item[1], item[0]))
    if waiting:
        if all_authors:
            detail = ", ".join(f"{name}={n}" for name, n in sorted(waiting.items()))
            lines.append(f"candidates waiting: {total} manifests, {len(waiting)} authors: {detail}")
        else:
            top = ", ".join(f"{name}={n}" for name, n in authors[:10])
            suffix = f", +{len(authors) - 10} authors" if len(authors) > 10 else ""
            lines.append(f"candidates waiting: {total} manifests, {len(waiting)} authors; top: {top}{suffix}")
    data: dict[str, object] = {"integration_running": running, "candidates": total,
                               "candidate_authors": len(waiting),
                               "top_candidate_authors": dict(authors[:10])}
    if all_authors:
        data["candidate_counts"] = waiting
    return lines, data


def modules_result(expected_modules: int | None = None, project: Path | None = None) -> tuple[str, int | None, int | None]:
    workspace = project or WORKSPACE
    lock = project / "build" / "scratch" / "integrate.lock" if project is not None else LOCK
    lock.parent.mkdir(parents=True, exist_ok=True)
    with open(lock, "a") as handle:
        try:
            lock_file(handle)
        except OSError:
            return "modules: skipped, integration running", None, None
        if expected_modules is None:
            expected_modules = expected_module_count(workspace)
        if type(expected_modules) is not int or expected_modules < 1:
            raise ValueError("expected modules must be a positive integer")
        try:
            result = subprocess.run(["ninja", "check_modules"], cwd=workspace, text=True,
                                    capture_output=True, check=False)
        except (OSError, subprocess.SubprocessError) as error:
            return f"modules: failed to run ninja check_modules: {error}", None, 127
    ok = len(re.findall(r": OK", result.stdout + result.stderr))
    line = f"modules: {ok} OK" + ("" if ok == expected_modules else f"  <-- EXPECTED {expected_modules}")
    return line, ok, result.returncode


def modules_line() -> str:
    return modules_result()[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--modules", action="store_true", help="also run ninja check_modules (slow, workspace must be idle)")
    parser.add_argument("--all", action="store_true", help="list all candidate authors instead of the top ten")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--project", type=Path, help="configured project workspace (default: workspace/)")
    parser.add_argument("--expected-modules", type=int, help="default: count modules in the project ARM9 config")
    args = parser.parse_args(argv)
    project = args.project.resolve() if args.project is not None else None
    status = 0
    output: dict[str, object] = {}
    lines = []
    try:
        line, report = report_data(project)
        output["report"] = report
        lines.append(line)
    except (OSError, ValueError) as error:
        status = 1
        output["report_error"] = str(error)
        lines.append(f"report: error: {error}")
    if args.modules:
        try:
            expected = args.expected_modules if args.expected_modules is not None else expected_module_count(
                project or WORKSPACE)
            line, count, returncode = modules_result(expected, project)
        except (OSError, ValueError, yaml.YAMLError) as error:
            expected, count, returncode = args.expected_modules, None, 2
            line = f"modules: configuration error: {error}"
        output["modules"] = {"line": line, "count": count, "expected": expected, "returncode": returncode}
        lines.append(line)
        if line.startswith("modules: skipped"):
            pass  # Documented non-failure: integration owns the workspace.
        elif count != expected or returncode != 0:
            status = 1
    pending, pending_data = pending_lines(args.all, project)
    output.update(pending_data)
    lines.extend(pending)
    try:
        print(json.dumps({"status": status, **output}, sort_keys=True) if args.json else "\n".join(lines))
    except BrokenPipeError:
        sys.stdout = io.StringIO()
    return status


if __name__ == "__main__":
    sys.exit(main())


