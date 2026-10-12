#!/usr/bin/env python3
"""Frame regression checks: run native and diff stage checkpoints against oracle captures.

Usage (.venv/bin/python from the repository root):

    .venv/bin/python tools/regress_frames.py reference/desmume-trace/stages.json
    .venv/bin/python tools/regress_frames.py --update-tolerances reference/desmume-trace/stages.json

The JSON stage file requires `native`, `rom`, `input_script`, `oracle_dir`, and a
non-empty `stages` list. Each stage has a unique name, non-negative integer frame,
and non-negative integer top/bottom pixel tolerances. Optional `save` and `dump_dir`
are passed to dq9; `dump_dir`, when configured, retains native captures there.
`--update-tolerances` atomically records observed diffs as new baselines.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare_frames import ImageError, compare, read_image

REQUIRED_KEYS = ("native", "rom", "input_script", "oracle_dir", "stages")


def _load_stages(path: Path) -> dict:
    config = json.loads(path.read_text())
    if not isinstance(config, dict):
        raise ValueError(f"{path}: top-level JSON value must be an object")
    missing = [key for key in REQUIRED_KEYS if key not in config]
    if missing:
        raise ValueError(f"{path}: missing keys {', '.join(missing)}")
    for key in ("native", "rom", "input_script", "oracle_dir"):
        if not isinstance(config[key], str) or not config[key].strip():
            raise ValueError(f"{path}: {key!r} must be a non-empty string")
    for key in ("save", "dump_dir"):
        if key in config and (not isinstance(config[key], str) or not config[key].strip()):
            raise ValueError(f"{path}: {key!r} must be a non-empty string when supplied")
    if not isinstance(config["stages"], list) or not config["stages"]:
        raise ValueError(f"{path}: 'stages' must be a non-empty list")
    sections = []
    names: set[str] = set()
    for index, entry in enumerate(config["stages"]):
        label = f"{path}: stage {index + 1}"
        if not isinstance(entry, dict):
            raise ValueError(f"{label} must be an object")
        missing = [key for key in ("name", "frame", "top_tolerance", "bottom_tolerance") if key not in entry]
        if missing:
            raise ValueError(f"{label} missing keys {', '.join(missing)}")
        name = entry["name"]
        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"{label} name must be a non-empty string")
        if name in names:
            raise ValueError(f"{label}: duplicate stage name {name!r}")
        names.add(name)
        for key in ("frame", "top_tolerance", "bottom_tolerance"):
            value = entry[key]
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise ValueError(f"{label} {key} must be a non-negative integer")
        sections.append(dict(entry))
    config["stages"] = sections
    return config


def _invoke_native(config: dict, dump_dir: Path) -> None:
    command = [
        str(config["native"]),
        "--rom", str(config["rom"]),
        "--frames", str(max(stage["frame"] for stage in config["stages"]) + 1),
        "--headless",
        "--dump-frames", ",".join(str(stage["frame"]) for stage in config["stages"]),
        "--dump-dir", str(dump_dir),
        "--input-script", str(config["input_script"]),
    ]
    if config.get("save"):
        command += ["--save", str(config["save"])]
    result = subprocess.run(command)
    if result.returncode != 0:
        raise RuntimeError(f"native run failed with exit status {result.returncode}: {' '.join(command)}")


def _capture_diffs(config: dict, native_dump_dir: Path) -> dict[str, dict[str, object]]:
    oracle_dir = Path(config["oracle_dir"])
    results = {}
    for stage in config["stages"]:
        frame = stage["frame"]
        stage_diffs = {}
        for screen in ("top", "bottom"):
            native_image = read_image(native_dump_dir / f"frame_{frame:04d}_{screen}.ppm")
            oracle_image = read_image(oracle_dir / f"frame_{frame:04d}_{screen}.png")
            stage_diffs[screen] = compare(native_image, oracle_image)["different"]
        results[stage["name"]] = stage_diffs
    return results


def _resolve_runner(runner) -> object:
    return runner if runner is not None else _invoke_native


def run_regression(config: dict, runner=None) -> dict[str, dict[str, object]]:
    if config.get("dump_dir"):
        dump_dir = Path(config["dump_dir"])
        dump_dir.mkdir(parents=True, exist_ok=True)
        for stage in config["stages"]:
            for screen in ("top", "bottom"):
                (dump_dir / f"frame_{stage['frame']:04d}_{screen}.ppm").unlink(missing_ok=True)
        _resolve_runner(runner)(config, dump_dir)
        return _capture_diffs(config, dump_dir)
    with tempfile.TemporaryDirectory(prefix="dq9-regress-") as directory:
        _resolve_runner(runner)(config, Path(directory))
        return _capture_diffs(config, Path(directory))


def update_tolerances(path: Path, runner=None) -> int:
    config = _load_stages(path)
    results = run_regression(config, runner)
    changes = []
    for stage in config["stages"]:
        diffs = results[stage["name"]]
        old_top, old_bottom = stage["top_tolerance"], stage["bottom_tolerance"]
        stage["top_tolerance"] = diffs["top"]
        stage["bottom_tolerance"] = diffs["bottom"]
        changes.append((stage["name"], old_top, diffs["top"], old_bottom, diffs["bottom"]))
    temporary = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", suffix=".tmp", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(json.dumps(config, indent=2) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    for name, old_top, new_top, old_bottom, new_bottom in changes:
        print(f"{name}: recorded top {new_top}, bottom {new_bottom}; "
              f"changed top tolerance {old_top} -> {new_top}, bottom tolerance {old_bottom} -> {new_bottom}")
    return 0


def main(argv: list[str] | None = None, runner=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stages", type=Path)
    parser.add_argument("--update-tolerances", action="store_true",
                        help="record current diffs as baseline tolerances instead of checking")
    args = parser.parse_args(argv)
    try:
        config = _load_stages(args.stages)
    except (ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    if args.update_tolerances:
        return update_tolerances(args.stages, runner)
    results = run_regression(config, runner)
    header = f"{'stages':20s} {'frames':>8s} {'top diff':>10s} {'bottom diff':>12s} {'result':8s}"
    print(header)
    failures = 0
    for stage in config["stages"]:
        diffs = results[stage["name"]]
        top_failed = diffs["top"] > stage["top_tolerance"]
        bottom_failed = diffs["bottom"] > stage["bottom_tolerance"]
        fails = top_failed or bottom_failed
        failures += int(fails)
        print(
            f"{stage['name']:20s} {stage['frame']:>8d} {diffs['top']:>10d} {diffs['bottom']:>12d} "
            f"{'DIFF' if fails else 'OK':8s}"
        )
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ImageError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
