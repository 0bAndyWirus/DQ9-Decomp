#!/usr/bin/env python3
"""Generate and verify asm-exact candidates for a shard of the unmatched queue.

For each target of the shard this writes src/_candidates/<AUTHOR>/<name>.cpp with
tools/asm_exact_gen.py and invokes a caller-supplied Python verification helper.
Select scripts with --helper-main / DQ9_HELPER_MAIN and --helper-overlay /
DQ9_HELPER_OVERLAY. CLI paths override environment paths; there are no defaults.

Command contract (working directory: the EU workspace):
- main: Python HELPER verify STEM, with DQ_AUTHOR set to --author.
- overlays and itcm: Python HELPER verify STEM --write, with MEDB_AUTHOR set.
Helpers must exit 0 only for a 100% relocation-aware, contiguous, correctly
placed match and write src/_candidates/<AUTHOR>/<STEM>.json on success.
Only successful sources and manifests are kept; failures are deleted and logged.
Nothing is integrated here (tools/integrate_batch.py does that).

    .venv/bin/python tools/asm_exact_run.py --author AsmExact01 --shard 0 --shards 12 \
        --helper-main /local/main_verify.py --helper-overlay /local/overlay_verify.py

Output: build/asm-exact/<AUTHOR>.tsv with `name module status detail`.
Safe to re-run: names already verified (manifest present) are skipped.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import asm_exact_gen as gen  # noqa: E402
from integrate_candidates import forbidden_function, load_skip_list  # noqa: E402

EU = ROOT / "extracted" / "dqix-decomp-eu"
ASM_DIRS = [EU / "build" / "asm-all"]  # `dsd dis -c config/eur/arm9/config.yaml -a build/asm-all` (run in the EU workspace)
SYMBOLS = EU / "config" / "eur" / "arm9"


def symbol_files() -> list[Path]:
    return [SYMBOLS / "symbols.txt"] + sorted((SYMBOLS / "itcm").glob("symbols.txt")) + sorted(
        (SYMBOLS / "overlays").glob("*/symbols.txt"))


def helper_command(author: str, module: str, stem: str, *,
                   helper_main: Path | None = None,
                   helper_overlay: Path | None = None) -> tuple[list[str], dict[str, str]]:
    env = dict(os.environ)
    kind = "main" if module == "main" else "overlay"
    helper = helper_main if kind == "main" else helper_overlay
    variable = f"DQ9_HELPER_{kind.upper()}"
    selected = helper if helper is not None else env.get(variable)
    if not selected:
        raise ValueError(f"missing {kind} helper: supply --helper-{kind} or {variable}")
    path = Path(selected).expanduser().resolve()
    if not path.is_file():
        raise ValueError(f"{kind} helper is not a file: {path}")
    if kind == "main":
        env["DQ_AUTHOR"] = author
        return [sys.executable, str(path), "verify", stem], env
    env["MEDB_AUTHOR"] = author
    return [sys.executable, str(path), "verify", stem, "--write"], env


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--author", required=True)
    parser.add_argument("--shard", type=int, default=0)
    parser.add_argument("--shards", type=int, default=1)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--helper-main", type=Path,
                        help="main-module Python helper (or DQ9_HELPER_MAIN)")
    parser.add_argument("--helper-overlay", type=Path,
                        help="overlay/itcm Python helper (or DQ9_HELPER_OVERLAY)")
    parser.add_argument("--queue", type=Path, default=ROOT / "build" / "decomp-queue.json")
    parser.add_argument("--skip-list", type=Path,
                        help="private JSON exclusions (default: DQ9_SKIP_LIST; otherwise empty with warning)")
    parser.add_argument("--target-list", type=Path,
                        help="optional newline-separated function names to limit this run")
    args = parser.parse_args(argv)

    queue = json.loads(args.queue.read_text())["queue"]
    skip_list = load_skip_list(args.skip_list)
    targets = [e for e in queue if e["address"] is not None and e["module"] is not None
               and not forbidden_function(e["module"], e["address"], e["function_name"], skip_list)]
    if args.target_list:
        target_list = args.target_list if args.target_list.is_absolute() else ROOT / args.target_list
        selected = {line.strip() for line in target_list.read_text().splitlines() if line.strip()}
        targets = [entry for entry in targets if entry["function_name"] in selected]
    targets = sorted(targets, key=lambda e: (e["module"], e["address"]))

    functions: dict[str, gen.Function] = {}
    for directory in ASM_DIRS:
        for path in sorted(directory.glob("*.s")):
            functions.update(gen.parse_functions(path.read_text()))

    groups: list[list[dict]] = []
    for entry in targets:
        func = functions.get(entry["function_name"])
        if func is not None and func.mode == "thumb" and groups:
            previous = groups[-1][-1]
            previous_func = functions.get(previous["function_name"])
            gap = entry["address"] - (previous["address"] + previous["size"])
            adjacent = (
                previous_func is not None
                and previous_func.mode == "thumb"
                and previous["module"] == entry["module"]
                and previous.get("gap_object") == entry.get("gap_object")
                and 0 <= gap <= 3
            )
            if adjacent:
                groups[-1].append(entry)
                continue
        groups.append([entry])
    groups = groups[args.shard::args.shards]
    if args.limit:
        limited: list[list[dict]] = []
        remaining = args.limit
        for group in groups:
            if remaining <= 0:
                break
            limited.append(group)
            remaining -= len(group)
        groups = limited

    try:
        for module in {group[0]["module"] for group in groups}:
            helper_command(args.author, module, "", helper_main=args.helper_main,
                           helper_overlay=args.helper_overlay)
    except ValueError as error:
        parser.error(str(error))

    symbol_paths = symbol_files()
    kinds = gen.load_kinds(symbol_paths)
    address_symbols = gen.load_symbol_addresses(symbol_paths)

    def symbols_for(entry: dict) -> dict[int, str]:
        result = dict(address_symbols)
        for callee in entry.get("callees", []):
            address = callee.get("address")
            if isinstance(address, str):
                address = int(address, 0)
            if address is None:
                continue
            resolved = callee.get("resolved_symbols", [])
            symbol = next((item for item in resolved
                           if item.get("name") and item.get("address", address) == address), None)
            if symbol is not None:
                result[address] = symbol["name"]
            elif callee.get("symbol_name") and callee.get("module") is not None:
                result[address] = callee["symbol_name"]
        return result

    out_dir = EU / "src" / "_candidates" / args.author
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = ROOT / "build" / "asm-exact" / f"{args.author}.tsv"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    with log_path.open("a") as log:
        for group in groups:
            names = [entry["function_name"] for entry in group]
            module = group[0]["module"]
            stem = names[0] if len(names) == 1 else f"{names[0]}_thumb_run"
            source = out_dir / f"{stem}.cpp"
            manifest = out_dir / f"{stem}.json"
            if manifest.exists() or any((out_dir / f"{name}.json").exists() for name in names):
                counts["already"] = counts.get("already", 0) + len(group)
                continue

            outcome = ""
            functions_in_group = [functions.get(name) for name in names]
            if any(func is None for func in functions_in_group):
                outcome = "no_asm\tnot in asm snapshots"
            else:
                nofralloc_options = (True,) if len(group) > 1 or functions_in_group[0].mode == "thumb" else (False, True)
                for nofralloc in nofralloc_options:
                    try:
                        rendered = []
                        for entry, name, func in zip(group, names, functions_in_group):
                            body, symbols = gen.convert(
                                func, strip_frame=not nofralloc, branch_symbols=symbols_for(entry))
                            rendered.append(gen.render(
                                name, body, symbols, kinds, nofralloc=nofralloc, thumb=func.mode == "thumb"))
                    except gen.Unsupported as error:
                        outcome = f"unsupported\t{error}"
                        continue
                    source.write_text("\n".join(rendered))
                    command, env = helper_command(
                        args.author, module, stem, helper_main=args.helper_main,
                        helper_overlay=args.helper_overlay)
                    result = subprocess.run(command, cwd=EU, env=env, capture_output=True, text=True, timeout=600)
                    if result.returncode == 0 and manifest.exists():
                        outcome = "verified\tnofralloc" if nofralloc else "verified"
                        break
                    tail = (result.stdout + result.stderr).strip().splitlines()
                    outcome = "failed\t" + (tail[-1] if tail else f"exit {result.returncode}")[:200]
            status, _, detail = outcome.partition("\t")
            if status != "verified":
                source.unlink(missing_ok=True)
            for name in names:
                counts[status] = counts.get(status, 0) + 1
                log.write(f"{name}\t{module}\t{status}\t{detail}\n")
            log.flush()
    print(json.dumps(counts, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
