#!/usr/bin/env python3
"""Per-manifest ledger, retry lists, ETA/watch and candidate health for integration runs.

Read-only: parses the runner's results.json / chunkNNN.log files and inspects the
workspace; it never writes inside the EU workspace and never runs the integrator.

  integration_ledger.py ledger  --run-dir D [--run-dir D2 ...] --out ledger.json
  integration_ledger.py summary --run-dir D [...]
  integration_ledger.py retry   --run-dir D [...] --out-dir DIR
  integration_ledger.py watch   --run-dir D
  integration_ledger.py health  [--manifest-list FILE] [--out report.json]

Run dirs are listed oldest first; a later run overrides an earlier outcome for the same
manifest. Outcome of a chunk is read from its log: integrated units are the chunk's
manifests minus skipped and excluded ones, and only when the chunk exited 0.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "extracted" / "dqix-decomp-eu"
CANDIDATES = "src/_candidates/"
SLOW_SECONDS = 300

_SKIP_RE = re.compile(r"^\s+skipped\s+(\S+\.json):\s+(?:\S+\.json:\s+)?(.*)$")
_EXCLUDED_RE = re.compile(r"^\s{4,}(\S+\.json):\s+(.*)$")
_INTEGRATED_RE = re.compile(r"Integrated:\s+(\d+)\s+unit")
_MATCHED_RE = re.compile(r"Matched:\s+(\d+)\s+functions,\s+(\d+)\s+bytes,\s+(\d+)/(\d+)\s+units")
_FAILURE_RE = re.compile(r"^integrate_batch\.py:\s+(.*)$", re.MULTILINE)
_RUNNER_HEAD_RE = re.compile(r"(\d+) manifests, (\d+) chunks")


def relative_manifest(text: str) -> str:
    """Normalise any absolute/relative manifest path to `src/_candidates/<author>/<name>.json`."""
    index = text.find(CANDIDATES)
    return text[index:] if index >= 0 else text


def categorize(reason: str) -> str:
    """Map a free-text skip/exclusion reason to a stable category."""
    checks = (
        ("newer than its object", "stale-object"),
        ("object does not exist", "missing-object"),
        ("conflicts with different", "header-conflict"),
        ("differs from the one other batch", "header-conflict"),
        ("destination already exists", "destination-exists"),
        ("already delinked", "already-delinked"),
        ("preflight", "preflight-rejected"),
        ("link failure", "link-bisection"),
        ("checksum", "checksum"),
        ("duplicate", "duplicate"),
        ("panic", "report-panic"),
        ("multiply-defined", "multiply-defined"),
        ("Thumb", "thumb-boundary"),
    )
    for needle, category in checks:
        if needle in reason:
            return category
    return "other"


def failure_kind(message: str | None) -> str:
    if not message:
        return "unknown"
    if message.startswith("apply failed for"):
        name = message.split()[3] if len(message.split()) > 3 else "?"
        return f"apply-failed:{name}"
    if message.startswith("retry limit reached"):
        return "retry-limit"
    if message.startswith("could not isolate"):
        return "unisolated"
    return "other"


@dataclass
class ChunkLog:
    integrated_units: int | None = None
    matched: tuple[int, int, int, int] | None = None
    skipped: dict[str, str] = field(default_factory=dict)
    excluded: dict[str, str] = field(default_factory=dict)
    failure: str | None = None


def parse_chunk_log(text: str) -> ChunkLog:
    log = ChunkLog()
    in_excluded = False
    for line in text.splitlines():
        if (match := _INTEGRATED_RE.search(line)):
            log.integrated_units = int(match.group(1))
        if (match := _MATCHED_RE.search(line)):
            log.matched = tuple(int(group) for group in match.groups())  # type: ignore[assignment]
        if line.strip().startswith("Excluded manifests:"):
            in_excluded = True
            continue
        if (match := _SKIP_RE.match(line)):
            log.skipped[relative_manifest(match.group(1))] = match.group(2)
            continue
        if in_excluded and (match := _EXCLUDED_RE.match(line)):
            log.excluded[relative_manifest(match.group(1))] = match.group(2)
            continue
        if in_excluded and line.strip() and not line.startswith(" "):
            in_excluded = False
    if (match := _FAILURE_RE.search(text)):
        log.failure = match.group(1).strip()
    return log


@dataclass
class Run:
    directory: Path
    chunks: list[dict]
    total_chunks: int | None
    total_manifests: int | None


def load_run(directory: Path) -> Run:
    results_path = directory / "results.json"
    chunks = json.loads(results_path.read_text(encoding="utf-8")) if results_path.is_file() else []
    total_chunks = total_manifests = None
    runner_log = directory / "runner.log"
    if runner_log.is_file():
        match = _RUNNER_HEAD_RE.search(runner_log.read_text(encoding="utf-8", errors="replace"))
        if match:
            total_manifests, total_chunks = int(match.group(1)), int(match.group(2))
    return Run(directory, chunks, total_chunks, total_manifests)


def build_ledger(runs: Sequence[Run], project: Path | None = None) -> dict[str, dict]:
    """One entry per manifest: state, category, chunk, reason. Later runs override earlier ones."""
    ledger: dict[str, dict] = {}
    for run in runs:
        for chunk in run.chunks:
            log_path = run.directory / f"chunk{chunk['chunk']:03d}.log"
            log = parse_chunk_log(log_path.read_text(encoding="utf-8", errors="replace")) if log_path.is_file() else ChunkLog()
            ok = chunk["exit"] == 0
            kind = failure_kind(log.failure)
            for manifest in chunk["manifests"]:
                manifest = relative_manifest(manifest)
                entry = {"run": run.directory.name, "chunk": chunk["chunk"], "chunk_exit": chunk["exit"]}
                if manifest in log.skipped:
                    reason = log.skipped[manifest]
                    entry.update(state="skipped", category=categorize(reason), reason=reason)
                elif manifest in log.excluded:
                    reason = log.excluded[manifest]
                    entry.update(state="excluded", category=categorize(reason), reason=reason)
                elif ok:
                    entry.update(state="integrated", category="integrated", reason="")
                else:
                    entry.update(state="chunk-failed", category=f"chunk-failed:{kind.split(':')[0]}", reason=log.failure or "")
                ledger[manifest] = entry
    if project is not None:
        _reconcile_with_workspace(ledger, project)
    return ledger


JOURNAL = "build/scratch/integrate_candidates_rollback.json"


def _recovered_stems(project: Path) -> set[str]:
    """Stems of every manifest under src/Recovered/<module>/ and its `_h<digest>/` variant directories."""
    root = project / "src" / "Recovered"
    stems: set[str] = set()
    if root.is_dir():
        for _directory, _subdirs, files in os.walk(root):
            stems.update(name[:-5] for name in files if name.endswith(".json"))
    return stems


def _reconcile_with_workspace(ledger: dict[str, dict], project: Path) -> None:
    """Cross-check logged outcomes against the workspace without ever inventing a success.

    Logged outcomes are authoritative. Files move *before* checks during an apply and move back on
    rollback, so a missing candidate proves nothing while a transaction journal exists: in that case every
    entry is tagged `fs=in-flight` immediately, without scanning. When the workspace is idle:
    - logged integrated + still on disk, or no copy under src/Recovered -> `integrated-unconfirmed`;
    - logged not-integrated + candidate absent -> `unknown-absent` (never promoted, never retried).
    """
    journal = project / JOURNAL
    if journal.is_file():
        for entry in ledger.values():
            entry["fs"] = "in-flight"
        return
    recovered = _recovered_stems(project)
    on_disk = {manifest: (project / manifest).is_file() for manifest in ledger}
    if journal.is_file():  # an apply started during the scan: the observations cannot be trusted
        for entry in ledger.values():
            entry["fs"] = "in-flight"
        return
    for manifest, entry in ledger.items():
        entry["on_disk"] = on_disk[manifest]
        if entry["state"] == "integrated":
            if on_disk[manifest] or Path(manifest).stem not in recovered:
                entry["state"], entry["category"] = "integrated-unconfirmed", "integrated-unconfirmed"
        elif not on_disk[manifest]:
            entry["state"], entry["category"] = "unknown-absent", "unknown-absent"


def estimate_eta(chunk_seconds: Sequence[float], remaining: int) -> dict[str, float]:
    """ETA in seconds: optimistic (median clean chunk), expected (mixes in slow-chunk rate), pessimistic (mean)."""
    if not chunk_seconds or remaining <= 0:
        return {"optimistic": 0.0, "expected": 0.0, "pessimistic": 0.0}
    clean = [value for value in chunk_seconds if value <= SLOW_SECONDS] or list(chunk_seconds)
    slow = [value for value in chunk_seconds if value > SLOW_SECONDS]
    median_clean = statistics.median(clean)
    rate = len(slow) / len(chunk_seconds)
    expected_each = median_clean * (1 - rate) + (statistics.mean(slow) * rate if slow else 0.0)
    return {
        "optimistic": remaining * median_clean,
        "expected": remaining * expected_each,
        "pessimistic": remaining * statistics.mean(chunk_seconds),
    }


def _hours(seconds: float) -> str:
    return f"{seconds / 3600:.1f} h"


def format_summary(runs: Sequence[Run], ledger: dict[str, dict]) -> str:
    lines: list[str] = []
    for run in runs:
        seconds = [chunk["seconds"] for chunk in run.chunks]
        slow = [value for value in seconds if value > SLOW_SECONDS]
        nonzero = [chunk["chunk"] for chunk in run.chunks if chunk["exit"]]
        total = f"/{run.total_chunks}" if run.total_chunks else ""
        lines.append(f"{run.directory.name}: {len(run.chunks)}{total} chunks, {_hours(sum(seconds))}, "
                     f"{len(slow)} slow (>{SLOW_SECONDS}s) = {_hours(sum(slow))}, non-zero: {nonzero or 'none'}")
        if run.total_chunks and run.chunks:
            eta = estimate_eta(seconds, run.total_chunks - len(run.chunks))
            lines.append("  ETA remaining: " + ", ".join(f"{name} {_hours(value)}" for name, value in eta.items()))
    states = Counter(entry["state"] for entry in ledger.values())
    lines.append("manifests by state: " + ", ".join(f"{name}={count}" for name, count in sorted(states.items())))
    if any(entry.get("fs") == "in-flight" for entry in ledger.values()):
        lines.append("NOTE: an integration transaction is in flight; workspace cross-checks were skipped, logged outcomes only")
    categories = Counter(entry["category"] for entry in ledger.values() if entry["state"] != "integrated")
    for category, count in categories.most_common():
        lines.append(f"  {count:5d}  {category}")
    last = None
    for run in runs:
        for chunk in run.chunks:
            log_path = run.directory / f"chunk{chunk['chunk']:03d}.log"
            if log_path.is_file() and (matched := parse_chunk_log(log_path.read_text(errors="replace")).matched):
                last = matched
    if last:
        lines.append(f"last reported match: {last[0]} functions, {last[1]} bytes, {last[2]}/{last[3]} units")
    return "\n".join(lines)


RETRY_FILES = {
    "retry-stale-object.txt": {"stale-object", "missing-object"},
    "retry-header-conflict.txt": {"header-conflict"},
    "retry-chunk-failed.txt": {"chunk-failed:apply-failed", "chunk-failed:retry-limit", "chunk-failed:unisolated",
                               "chunk-failed:other", "chunk-failed:unknown"},
    "retry-bisection.txt": {"link-bisection", "checksum", "report-panic", "multiply-defined", "preflight-rejected"},
    "retry-other.txt": {"other", "duplicate", "thumb-boundary", "already-delinked", "destination-exists"},
}


def write_retry_lists(ledger: dict[str, dict], out_dir: Path, project: Path | None = None) -> dict[str, int]:
    """Write one manifest list per cause for entries that are still not integrated and still on disk."""
    if any(entry.get("fs") == "in-flight" for entry in ledger.values()):
        raise ValueError("a transaction journal exists (an integration is applying candidates); "
                         "file locations are unreliable, so retry lists are not written until the workspace is idle")
    out_dir.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    pending_all: list[str] = []
    for name, categories in RETRY_FILES.items():
        manifests = sorted(m for m, e in ledger.items()
                           if e["state"] not in ("integrated", "integrated-unconfirmed") and e["category"] in categories
                           and (project is None or (project / m).is_file()))
        (out_dir / name).write_text("".join(f"{m}\n" for m in manifests), encoding="utf-8")
        counts[name] = len(manifests)
        pending_all.extend(manifests)
    (out_dir / "retry-all.txt").write_text("".join(f"{m}\n" for m in sorted(set(pending_all))), encoding="utf-8")
    counts["retry-all.txt"] = len(set(pending_all))
    return counts


def running_processes(names: Iterable[str] = ("integrate_batch.py", "integration-runner")) -> list[tuple[int, str]]:
    found = []
    if os.name == "nt":
        # Win32 has no /proc. CIM exposes command lines without an extra Python
        # dependency; UTF-8 stdout keeps paths independent of the console ACP.
        query = ("[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); "
                 "Get-CimInstance Win32_Process | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress")
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", query],
                                capture_output=True, encoding="utf-8", check=True)
        records = json.loads(result.stdout) if result.stdout.strip() else []
        if isinstance(records, dict):
            records = [records]
        for record in records:
            cmdline = record.get("CommandLine") or ""
            if any(name in cmdline for name in names) and "integration_ledger" not in cmdline:
                found.append((int(record["ProcessId"]), cmdline[:140]))
        return found
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            cmdline = Path(f"/proc/{entry}/cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace").strip()
        except OSError:
            continue
        if any(name in cmdline for name in names) and "integration_ledger" not in cmdline:
            found.append((int(entry), cmdline[:140]))
    return found


def watch(run: Run, now: float | None = None, stuck_minutes: float = 30.0) -> tuple[str, int]:
    """Describe the current chunk; exit status 1 if it looks stuck or nothing is running."""
    now = time.time() if now is None else now
    done = len(run.chunks)
    marker = run.directory / "results.json"
    if not marker.is_file():
        marker = run.directory / "runner.log"
    started = marker.stat().st_mtime if marker.is_file() else now
    age = max(0.0, now - started)
    processes = running_processes()
    seconds = [chunk["seconds"] for chunk in run.chunks]
    typical = statistics.median(seconds) if seconds else 0.0
    lines = [f"{run.directory.name}: chunk {done + 1}{f'/{run.total_chunks}' if run.total_chunks else ''} "
             f"running for {age / 60:.1f} min (median chunk {typical:.0f}s)"]
    lines += [f"  pid {pid}: {cmd}" for pid, cmd in processes] or ["  no integration process found"]
    status = 0
    if not processes:
        status = 1
    elif age > stuck_minutes * 60:
        lines.append(f"  WARNING: current chunk exceeds {stuck_minutes:g} min (bisection or a stuck link)")
        status = 1
    return "\n".join(lines), status


def _is_stale(source: Path, obj: Path) -> str | None:
    if not source.is_file():
        return "missing-source"
    if not obj.is_file():
        return "missing-object"
    if source.stat().st_mtime_ns > obj.stat().st_mtime_ns:
        return "stale-object"
    return None


def health_scan(project: Path, manifests: Sequence[str] | None = None) -> dict:
    """Read-only scan of pending candidates: stale/missing objects and author-local header conflicts."""
    from tools.integrate_candidates import local_headers  # reuse the integrator's include resolution

    if manifests is None:
        manifests = sorted(p.relative_to(project).as_posix() for p in (project / "src/_candidates").glob("*/*.json"))
    problems: dict[str, list[str]] = defaultdict(list)
    with_headers: list[str] = []
    variants: dict[tuple[str, str], dict[str, list[str]]] = defaultdict(lambda: defaultdict(list))
    ok = 0
    against_existing: dict[tuple[str, str], list[str]] = defaultdict(list)
    for manifest in manifests:
        path = project / manifest
        if not path.is_file():
            continue
        author, stem = path.parent.name, path.stem
        source = path.with_suffix(".cpp")
        obj = project / "build" / "scratch" / author / f"{stem}.o"
        problem = _is_stale(source, obj)
        if problem:
            problems[problem].append(manifest)
            continue
        try:
            module = json.loads(path.read_text(encoding="utf-8"))["module"]
        except (OSError, ValueError, KeyError, TypeError):
            problems["bad-manifest"].append(manifest)
            continue
        headers = local_headers(source, project / "src" / "Recovered" / module)
        if headers:
            with_headers.append(manifest)
        for header, destination in headers:
            name = destination.relative_to(project / "src" / "Recovered" / module).as_posix()
            digest = hashlib.sha1(header.read_bytes()).hexdigest()[:10]
            variants[(module, name)][digest].append(manifest)
            if destination.is_file() and destination.read_bytes() != header.read_bytes():
                against_existing[(module, name)].append(manifest)
        ok += 1
    conflicts = []
    for (module, header), by_digest in sorted(variants.items()):
        if len(by_digest) > 1:
            majority = max(by_digest.values(), key=len)
            minority = [m for group in by_digest.values() if group is not majority for m in group]
            conflicts.append({"module": module, "header": header, "variants": {d: len(v) for d, v in by_digest.items()},
                              "minority_manifests": sorted(minority)})
    return {
        "scanned": len(manifests), "healthy": ok,
        "problems": {kind: sorted(items) for kind, items in problems.items()},
        "with_local_headers": len(with_headers),
        "header_conflicts": conflicts,
        "manifests_needing_variant_dir": sum(len(c["minority_manifests"]) for c in conflicts)
        + len({m for items in against_existing.values() for m in items}),
        "conflicts_with_existing_headers": [
            {"module": module, "header": header, "manifests": sorted(items)}
            for (module, header), items in sorted(against_existing.items())],
    }


def _load_runs(directories: Sequence[Path]) -> list[Run]:
    return [load_run(directory) for directory in directories]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("ledger", "summary", "retry", "watch", "health"))
    parser.add_argument("--run-dir", type=Path, action="append", default=[], help="runner directory (repeat, oldest first)")
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--out", type=Path, help="ledger/health JSON output")
    parser.add_argument("--out-dir", type=Path, help="directory for retry lists")
    parser.add_argument("--manifest-list", type=Path, help="health: file with project-relative manifest paths")
    parser.add_argument("--stuck-minutes", type=float, default=30.0)
    args = parser.parse_args(argv)
    try:
        if args.command == "health":
            manifests = None
            if args.manifest_list:
                manifests = [line.strip() for line in args.manifest_list.read_text().splitlines() if line.strip()]
            report = health_scan(args.project, manifests)
            summary = {key: ({kind: len(items) for kind, items in value.items()} if key == "problems"
                             else len(value) if isinstance(value, list) else value)
                       for key, value in report.items()}
            print(json.dumps(summary, indent=1))
            if args.out:
                args.out.parent.mkdir(parents=True, exist_ok=True)
                args.out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
            return 0
        if not args.run_dir:
            parser.error("--run-dir is required")
        runs = _load_runs(args.run_dir)
        if args.command == "watch":
            text, status = watch(runs[-1], stuck_minutes=args.stuck_minutes)
            print(text)
            return status
        ledger = build_ledger(runs, args.project if args.project.is_dir() else None)
        if args.command == "summary":
            print(format_summary(runs, ledger))
        elif args.command == "ledger":
            if not args.out:
                parser.error("--out is required for ledger")
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(ledger, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            print(f"wrote {len(ledger)} manifests to {args.out}")
        else:
            if not args.out_dir:
                parser.error("--out-dir is required for retry")
            for name, count in write_retry_lists(ledger, args.out_dir, args.project if args.project.is_dir() else None).items():
                print(f"{count:5d}  {args.out_dir / name}")
    except (OSError, ValueError) as error:
        print(f"integration_ledger.py: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(ROOT))
    try:
        raise SystemExit(main())
    except BrokenPipeError:  # consumer such as `head` closed the pipe
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        raise SystemExit(1)
