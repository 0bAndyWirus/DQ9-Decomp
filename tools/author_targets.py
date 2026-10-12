#!/usr/bin/env python3
"""Build author work batches from a fresh decomp queue, excluding work that is already claimed.

After integration the old prepared target lists name gap objects that no longer exist. This reads
`build/decomp-queue.json` (from tools/decomp_queue.py; run tools/queue_freshness.py first), drops

  * functions already covered by a candidate manifest under src/_candidates (pending or waiting for retry) or an
    integrated manifest under src/Recovered (including `_h<digest>/` variant directories),
  * functions excluded by a caller-supplied --skip-list JSON or DQ9_SKIP_LIST,
  * optionally the functions in --parked (a JSON list of {module, addr|address} that earlier authors could not match;
    they are listed separately and only added at the end of the batches with --include-parked),

and groups what is left into batches of --limit targets: modules ordered by their best queue priority, addresses
ascending inside a module, so each author's range is contiguous where possible. The instruction mode (arm/thumb)
comes from the DSD symbols file, since the queue does not carry it.

Writes only the output JSON (default build/author-targets.json). Exit status: 0 written, 1 queue stale/unusable and
--require-fresh given, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.integrate_candidates import SkipList, forbidden_function, load_skip_list  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "extracted" / "dqix-decomp-eu"
DEFAULT_QUEUE = ROOT / "build" / "decomp-queue.json"
DEFAULT_OUTPUT = ROOT / "build" / "author-targets.json"

_SYMBOL_MODE = re.compile(r"^(\S+)\s+kind:function\((\w+),size=0x[0-9a-fA-F]+\).*?\baddr:(0x[0-9a-fA-F]+)")


def is_forbidden(module: str, address: int, name: str, skip_list: SkipList = SkipList()) -> str | None:
    """Reason a function matches the supplied exclusions, shared with the integrator."""
    return forbidden_function(module, address, name, skip_list)


def symbols_path(project: Path, module: str) -> Path:
    arm9 = project / "config" / "eur" / "arm9"
    if module == "main":
        return arm9 / "symbols.txt"
    if module in {"itcm", "dtcm"}:
        return arm9 / module / "symbols.txt"
    return arm9 / "overlays" / module / "symbols.txt"


def load_modes(project: Path, modules: Iterable[str]) -> dict[tuple[str, int], str]:
    """(module, even address) -> `arm` or `thumb` for every function symbol of the given modules."""
    modes: dict[tuple[str, int], str] = {}
    for module in sorted(set(modules)):
        path = symbols_path(project, module)
        if not path.is_file():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            match = _SYMBOL_MODE.match(line)
            if match:
                modes[(module, int(match.group(3), 16) & ~1)] = match.group(2).lower()
    return modes


def claimed_functions(project: Path) -> set[tuple[str, int]]:
    """(module, address) of every function in a candidate or integrated manifest."""
    claimed: set[tuple[str, int]] = set()
    for root in (project / "src" / "_candidates", project / "src" / "Recovered"):
        if not root.is_dir():
            continue
        for manifest in root.rglob("*.json"):
            try:
                data = json.loads(manifest.read_text(encoding="utf-8"))
                module = data["module"]
                for function in data["functions"]:
                    claimed.add((module, int(function["address"], 0) & ~1))
            except (OSError, ValueError, KeyError, TypeError):
                continue
    return claimed


def parked_keys(data: Any) -> set[tuple[str, int]]:
    """(module, even address) of every previously unmatched function in a parked-history document.

    Accepts a list of records or `{"unresolved": [...]}`; a record is either flat (`module` + `addr`/`address`) or
    an attempt history (`{"identity": ..., "attempts": [flat records]}`). Anything else is an error: silently
    reading zero keys from a differently shaped file would put every parked function back into the fresh list.
    """
    if isinstance(data, Mapping):
        data = data.get("unresolved")
    if not isinstance(data, list):
        raise ValueError("parked document must be a list of records or an object with an `unresolved` list")
    keys: set[tuple[str, int]] = set()
    for number, record in enumerate(data):
        found = 0
        flat = record.get("attempts") if isinstance(record, Mapping) and "attempts" in record else [record]
        if not isinstance(flat, list):
            raise ValueError(f"parked record {number}: `attempts` must be a list")
        for item in flat:
            address = item.get("addr", item.get("address")) if isinstance(item, Mapping) else None
            if isinstance(address, str):
                address = int(address, 0)
            if isinstance(item, Mapping) and isinstance(item.get("module"), str) and isinstance(address, int):
                keys.add((item["module"], address & ~1))
                found += 1
        if not found:
            raise ValueError(f"parked record {number} has no module/address: {str(record)[:80]}")
    return keys


JOURNAL = "build/scratch/integrate_candidates_rollback.json"
PASSING_OVERALL = {"PASS", "PASS_WITH_KNOWN_BASELINE_FAIL", "PASS_WITH_CHUNK_FAILURES"}


def gates_problem(final_json: Path) -> str | None:
    """Why an integration run's gates do not allow publishing work lists, or None when they passed."""
    try:
        final = json.loads(final_json.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return f"cannot read {final_json}: {error}"
    overall = final.get("overall") if isinstance(final, dict) else None
    if overall not in PASSING_OVERALL:
        return f"integration gates did not pass ({final_json}: overall={overall!r})"
    return None


def build_targets(entries: Sequence[Mapping[str, Any]], claimed: set[tuple[str, int]],
                  modes: Mapping[tuple[str, int], str], parked: set[tuple[str, int]] = frozenset(),
                  include_parked: bool = False, skip_list: SkipList = SkipList()) -> tuple[list[dict], dict[str, int]]:
    """Filter and order queue entries; returns (targets, counts of what was dropped and why)."""
    counts = {"queue": len(entries), "matched": 0, "claimed": 0, "forbidden": 0, "parked": 0, "no_module": 0}
    fresh: list[tuple[int, dict]] = []
    retry: list[tuple[int, dict]] = []
    for entry in entries:
        if entry.get("fuzzy_match_percent") == 100.0:
            counts["matched"] += 1
            continue
        module, address, name = entry.get("module"), entry.get("address"), entry.get("function_name", "")
        if not isinstance(module, str) or not isinstance(address, int):
            counts["no_module"] += 1
            continue
        key = (module, address & ~1)
        if is_forbidden(module, address, name, skip_list):
            counts["forbidden"] += 1
            continue
        if key in claimed:
            counts["claimed"] += 1
            continue
        target = {"name": name, "module": module, "addr": address, "size": entry.get("size"),
                  "mode": modes.get(key, "arm"), "reportUnit": entry.get("unit"),
                  "gapObject": entry.get("gap_object"), "hot": bool(entry.get("hot_path", {}).get("is_hot")),
                  "difficulty": entry.get("difficulty", {}).get("score"), "targetObject": None, "group": None,
                  "sourcePath": None, "boundaryNote": None}
        rank = entry.get("priority_rank", 10**9)
        if key in parked:
            counts["parked"] += 1
            retry.append((rank, target))
        else:
            fresh.append((rank, target))
    chosen = fresh + (retry if include_parked else [])
    best_rank: dict[str, int] = {}
    for rank, target in chosen:
        best_rank[target["module"]] = min(best_rank.get(target["module"], rank), rank)
    chosen.sort(key=lambda item: (best_rank[item[1]["module"]], item[1]["module"], item[1]["addr"]))
    counts["targets"] = len(chosen)
    return [target for _, target in chosen], counts


def make_batches(targets: Sequence[dict], limit: int, prefix: str, concurrency: int) -> dict:
    if limit < 1:
        raise ValueError("--limit must be positive")
    batches = []
    for index in range(0, len(targets), limit):
        number = index // limit + 1
        batches.append({"batch": number, "author": f"{prefix}{number:04d}", "targets": list(targets[index:index + limit])})
    return {"at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "targetFunctions": len(targets),
            "concurrency": concurrency, "assignmentLimit": limit, "batches": batches}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--queue", type=Path, default=DEFAULT_QUEUE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--skip-list", type=Path,
                        help="private JSON exclusions (default: DQ9_SKIP_LIST; otherwise empty with warning)")
    parser.add_argument("--parked", type=Path,
                        help="JSON of earlier unmatched functions: a list of {module, addr|address} or of "
                             "{identity, attempts: [{module, addr, ...}]}, or {unresolved: [...]} (unrecognized "
                             "shapes are rejected)")
    parser.add_argument("--include-parked", action="store_true", help="append parked functions after the fresh ones")
    parser.add_argument("--limit", type=int, default=20, help="targets per batch (default 20)")
    parser.add_argument("--concurrency", type=int, default=1, help="recorded in the output only")
    parser.add_argument("--author-prefix", default="AuthorNext")
    parser.add_argument("--require-fresh", action="store_true", help="fail (exit 1) when queue_freshness reports stale")
    parser.add_argument("--require-gates", type=Path, metavar="FINAL_JSON",
                        help="fail (exit 1) unless this integration_runner final.json passed its gates")
    args = parser.parse_args(argv)
    try:
        journal = args.project / JOURNAL
        if journal.exists():
            print(f"author_targets.py: an integration transaction journal exists ({journal}); candidates are "
                  "moved while it does, so claimed functions cannot be computed", file=sys.stderr)
            return 1
        if args.require_gates:
            problem = gates_problem(args.require_gates)
            if problem:
                print(f"author_targets.py: {problem}", file=sys.stderr)
                return 1
        if args.require_fresh:
            sys.path.insert(0, str(ROOT))
            from tools import queue_freshness
            if queue_freshness.main(["--queue", str(args.queue), "--project", str(args.project)]) != 0:
                return 1
        queue = json.loads(args.queue.read_text(encoding="utf-8"))
        entries = queue["queue"]
        parked = parked_keys(json.loads(args.parked.read_text(encoding="utf-8"))) if args.parked else set()
        modes = load_modes(args.project, {entry.get("module") for entry in entries if isinstance(entry.get("module"), str)})
        targets, counts = build_targets(entries, claimed_functions(args.project), modes, parked,
                                       args.include_parked, load_skip_list(args.skip_list))
        payload = make_batches(targets, args.limit, args.author_prefix, args.concurrency)
        payload["source"] = {"queue": str(args.queue), "counts": counts}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"author_targets.py: {error}", file=sys.stderr)
        return 2
    print(f"{counts['targets']} targets in {len(payload['batches'])} batch(es) -> {args.output}")
    print("dropped: " + ", ".join(f"{name}={value}" for name, value in counts.items()
                                  if name not in ("queue", "targets") and value))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(2)
