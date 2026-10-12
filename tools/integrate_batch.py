#!/usr/bin/env python3
"""Integrate candidate manifest batches and commit only after module/report checks."""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import integrate_candidates as candidates_tool  # noqa: E402
from tools import port_eu_unit  # noqa: E402
from tools.host_tools import lock_file  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = candidates_tool.DEFAULT_PROJECT
SYMBOL_RE = re.compile(r"Multiply-defined symbols?:?\s*([^\r\n]+)", re.IGNORECASE)
PANIC_TEXT = "objdiff-core/src/arch/arm.rs:130"


class BatchError(RuntimeError):
    pass


def _expand_manifests(values: Sequence[str], project: Path) -> list[Path]:
    paths: list[Path] = []
    for value in values:
        pattern = str(Path(value) if Path(value).is_absolute() else project / value)
        matches = sorted(glob.glob(pattern, recursive=True))
        paths.extend(Path(match) for match in matches if Path(match).is_file())
    return list(dict.fromkeys(path.resolve() for path in paths))


def _run(command: Sequence[str], project: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(command), cwd=project, text=True, capture_output=True, check=False)


def _configure_command(project: Path, dsd: Path, wibo: Path) -> list[str]:
    return [sys.executable, str(project / "tools" / "configure.py"), "eur",
            "--dsd", str(dsd), "-w", str(wibo)]


def _stage(project: Path, dsd: Path, wibo: Path, stage: str) -> subprocess.CompletedProcess[str]:
    if stage == "configure":
        return _run(_configure_command(project, dsd, wibo), project)
    if stage == "check_modules":
        return _run(["ninja", "-j", "8", stage], project)
    return _run(["ninja", stage], project)




def _apply_for_probe(items: Sequence[candidates_tool.Candidate], project: Path) -> bool:
    """Apply a probe subset; False when it cannot be applied (the subset is then inconclusive).

    A subset can fail to apply even though the full batch applied (a candidate's placement can depend on
    a neighbour that is not in the subset). That says nothing about the failure being bisected, so it must
    neither blame the subset nor abort the whole batch. `apply_candidates` has already rolled back.
    """
    try:
        candidates_tool.apply_candidates(items, project)
    except candidates_tool.CandidateError:
        return False
    return True


def _probe_link(items: Sequence[candidates_tool.Candidate], project: Path, dsd: Path, wibo: Path) -> bool:
    """Apply a subset and return whether configure/link fails; always restore it."""
    if not items or not _apply_for_probe(items, project):
        return False
    try:
        configured = _stage(project, dsd, wibo, "configure")
        if configured.returncode:
            return False
        return _stage(project, dsd, wibo, "build/eur/arm9.o").returncode != 0
    finally:
        candidates_tool.rollback_candidates(project)
        _stage(project, dsd, wibo, "configure")  # derived build graph must match restored config


MAX_PAIR_PROBES = 16  # cross-partition pair relinks _bisect_link may spend on one retry (sets of up to 8)


def _bisect_link(items: Sequence[candidates_tool.Candidate], project: Path, dsd: Path,
                 wibo: Path) -> list[candidates_tool.Candidate]:
    """Find one failing manifest, or the first interacting pair, with reversible probes."""
    remaining = list(items)
    while len(remaining) > 1:
        middle = len(remaining) // 2
        left, right = remaining[:middle], remaining[middle:]
        left_fails = _probe_link(left, project, dsd, wibo)
        right_fails = _probe_link(right, project, dsd, wibo)
        if left_fails:
            remaining = left
        elif right_fails:
            remaining = right
        else:
            # Combination-only errors need both members, found by testing cross-partition pairs. The scan is
            # quadratic (each pair is a full relink), so it is limited to small sets; a larger set is reported
            # as not isolated and the caller re-runs it as halves, which is far cheaper than the full scan.
            if len(left) * len(right) > MAX_PAIR_PROBES:
                return []
            for first in left:
                for second in right:
                    pair = [first, second]
                    if _probe_link(pair, project, dsd, wibo):
                        return pair
            return []
    return remaining


def _detail(result: subprocess.CompletedProcess[str]) -> str:
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    return output or f"exit {result.returncode}"


def _run_checked(project: Path, dsd: Path, wibo: Path, stage: str) -> subprocess.CompletedProcess[str]:
    result = _stage(project, dsd, wibo, stage)
    if result.returncode:
        raise BatchError(f"{stage} failed (exit {result.returncode}): {_detail(result)}")
    return result


def _fresh_report(project: Path, report_path: Path) -> dict:
    """Regenerate the objdiff report and return it.

    Freshness comes from generation, not from timestamps: the old artifact is removed first, so any
    report.json present after a successful `ninja report` was written by that command. Comparing the
    file's mtime with `time.time_ns()` is wrong because file timestamps come from a coarse kernel clock
    (or a coarse-granularity filesystem) and can be lower than the wall clock read just before the run,
    or equal to the previous report's mtime, which would reject a valid new report.
    """
    report = report_path
    report.unlink(missing_ok=True)
    result = _run(["ninja", "report"], project)
    if result.returncode:
        raise BatchError(f"report failed (exit {result.returncode}): {_detail(result)}")
    try:
        payload = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BatchError(f"report did not produce a readable report.json: {error}") from error
    if not isinstance(payload, dict) or not isinstance(payload.get("measures"), dict):
        raise BatchError("report.json has no measures object")
    return payload


def _metrics(report: dict) -> tuple[int, int, int, int]:
    values = report["measures"]
    try:
        return (int(values["matched_functions"]), int(values["matched_code"]),
                int(values["complete_units"]), int(values["total_units"]))
    except (KeyError, TypeError, ValueError) as error:
        raise BatchError(f"report measures are incomplete: {error}") from error


def _symbols(output: str) -> list[str]:
    found: list[str] = []
    for line in output.splitlines():
        if "multiply-defined" not in line.lower():
            continue
        match = SYMBOL_RE.search(line)
        text = match.group(1).strip() if match else line.strip()
        text = re.sub(r"\s+", " ", text)
        if text and text not in found:
            found.append(text)
    return found


def _offending_by_symbol(candidates: Sequence[candidates_tool.Candidate], diagnostics: Sequence[str]) -> list[candidates_tool.Candidate]:
    matches = []
    for candidate in candidates:
        names = {str(item.get("name", "")) for item in candidate.functions}
        if any(any(name and re.search(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])", diagnostic)
                   for name in names) or candidate.manifest.stem in diagnostic
               for diagnostic in diagnostics):
            matches.append(candidate)
    return matches
def _offending_by_manifest(candidates: Sequence[candidates_tool.Candidate],
                           diagnostic: str) -> list[candidates_tool.Candidate]:
    """Attribute port/preflight failures to the manifest explicitly named in them."""
    named = re.search(r"(?:apply failed for\s+|preflight rejected candidate set:\s*)([^:\s]+)", diagnostic)
    if named is None:
        return []
    manifest_name = Path(named.group(1)).name
    return [candidate for candidate in candidates if candidate.manifest.name == manifest_name]




def _probe_report(items: Sequence[candidates_tool.Candidate], project: Path, report_path: Path,
                  dsd: Path, wibo: Path) -> bool:
    """Run a reversible configure/report probe; return True only for a command panic."""
    if not _apply_for_probe(items, project):
        return False
    try:
        configured = _stage(project, dsd, wibo, "configure")
        if configured.returncode:
            return False
        # `ninja report` alone reuses stale delink/base objects, so a probe could pass or
        # fail for the wrong candidate; rebuild the modules first (outcome is irrelevant here).
        _run(["ninja", "-j", "8", "check_modules"], project)
        result = _run(["ninja", "report"], project)
        output = result.stdout + "\n" + result.stderr
        return result.returncode != 0 and PANIC_TEXT in output
    finally:
        candidates_tool.rollback_candidates(project)
        _stage(project, dsd, wibo, "configure")  # derived build graph must match restored config


def _require_report_baseline(project: Path, dsd: Path, wibo: Path) -> None:
    """Regenerate and check the report after rollback before attributing failures."""
    configured = _stage(project, dsd, wibo, "configure")
    if configured.returncode:
        raise BatchError(
            "objdiff report baseline configure failed before candidate bisection "
            f"(exit {configured.returncode}): {_detail(configured)}"
        )
    # After a rollback the delink objects are stale until the module build has
    # regenerated them; a report run before that panics in objdiff (arm.rs:130).
    _run(["ninja", "-j", "8", "check_modules"], project)  # outcome irrelevant; only refreshes delinks
    result = _run(["ninja", "report"], project)
    output = result.stdout + "\n" + result.stderr
    if result.returncode or PANIC_TEXT in output:
        result = _run(["ninja", "report"], project)
        output = result.stdout + "\n" + result.stderr
    if result.returncode or PANIC_TEXT in output:
        raise BatchError(
            "objdiff report baseline failed before candidate bisection "
            f"(exit {result.returncode}): {_detail(result)}"
        )


def _bisect_panic(items: Sequence[candidates_tool.Candidate], project: Path,
                  report_path: Path, dsd: Path, wibo: Path) -> list[candidates_tool.Candidate]:
    _require_report_baseline(project, dsd, wibo)
    remaining = list(items)
    while len(remaining) > 1:
        middle = len(remaining) // 2
        left, right = remaining[:middle], remaining[middle:]
        left_panics = _probe_report(left, project, report_path, dsd, wibo)
        right_panics = _probe_report(right, project, report_path, dsd, wibo)
        if left_panics:
            remaining = left
        elif right_panics:
            remaining = right
        else:
            for first in left:
                for second in right:
                    pair = [first, second]
                    if _probe_report(pair, project, report_path, dsd, wibo):
                        return pair
            return []
    return remaining


THUMB_GAP_REASON = "Thumb unit ends on a halfword (leaves a 2-byte gap that makes objdiff panic)"


def _planned_thumb_unit_end(candidate: candidates_tool.Candidate, project: Path) -> int | None:
    """Return the aligned unit end, or None when a halfword end cannot safely be padded."""
    addresses = [int(function["address"], 0) for function in candidate.functions]
    if not addresses or not all(address & 1 for address in addresses):
        return max((address & ~1 for address in addresses), default=0)
    sizes = candidates_tool.function_sizes(project, candidate.module, {})
    ends = []
    for address in addresses:
        start = address & ~1
        size = sizes.get(start)
        if size is None:
            return None
        ends.append(start + size)
    unit_end = max(ends)
    if unit_end % 4 != 2:
        return unit_end
    padded_end = unit_end + 2
    # The padding interval must not be owned by a DSD-sized function.
    for start, size in sizes.items():
        if start < padded_end and start + size > unit_end:
            return None
    if not _can_pad(candidate, project, min(address & ~1 for address in addresses), unit_end):
        return None
    return padded_end


def _can_pad(candidate: candidates_tool.Candidate, project: Path, start: int, unit_end: int) -> bool:
    """Ask the applier's own rule (port_eu_unit.pad_text_end): other symbols, non-zero bytes or the section end refuse."""
    try:
        module = port_eu_unit.load_project(project, "eur")[candidate.module]
    except (OSError, KeyError, ValueError):
        return False
    group = port_eu_unit.Group(".text", [], unit_end - start, candidate.module, start)
    return port_eu_unit.pad_text_end(group, module)


ABUTS_DATA_REASON = ("unit ends right before a data/bss symbol (the leftover gap starts with data, which makes "
                     "objdiff report panic; project rule 3)")
_DATA_SYMBOL_RE = re.compile(r"^\S+\s+kind:(?:data|bss)\b.*?\baddr:(0x[0-9a-fA-F]+)", re.MULTILINE)


def _abuts_data_symbol(candidate: candidates_tool.Candidate, project: Path) -> bool:
    """True when the first byte after the candidate's last function is a data or bss symbol."""
    sizes = candidates_tool.function_sizes(project, candidate.module, {})
    ends = []
    for function in candidate.functions:
        start = int(function["address"], 0) & ~1
        if start not in sizes:
            return False                  # contiguity/size problems are reported by the integrator itself
        ends.append(start + sizes[start])
    path = candidates_tool._symbols_path(project, candidate.module)
    if not ends or not path.is_file():
        return False
    unit_end = max(ends)
    text_end = None
    delinks = path.with_name("delinks.txt")
    if delinks.is_file():
        for line in delinks.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line[0].isspace():
                break                     # the header ends at the first unit
            header = re.match(r"\s*\.text\s+start:0x[0-9a-fA-F]+\s+end:(0x[0-9a-fA-F]+)", line)
            if header:
                text_end = int(header.group(1), 16)
    if text_end is None or unit_end >= text_end:
        return False                      # the unit closes .text; no gap object follows it
    return any(int(match.group(1), 16) in (unit_end, (unit_end + 3) & ~3)
               for match in _DATA_SYMBOL_RE.finditer(path.read_text(encoding="utf-8")))


_CHECK_FAILED = re.compile(r"Check\s+(?:overlay\s+(?P<overlay>\d+)|(?P<name>ARM9\s+main|ITCM|DTCM))\s*:\s*checksum failed",
                           re.IGNORECASE)


def _failed_checksum_modules(diagnostic: str) -> set[str]:
    """Modules named by DSD's `Check overlay 27: checksum failed` / `Check ARM9 main: ...` / `Check ITCM: ...` lines."""
    failed: set[str] = set()
    for match in _CHECK_FAILED.finditer(diagnostic):
        if match.group("overlay") is not None:
            failed.add(f"ov{int(match.group('overlay')):03d}")
        else:
            failed.add("main" if match.group("name").upper().startswith("ARM9") else match.group("name").lower())
    return failed


def _checksum_modules(diagnostic: str, candidates: Sequence[candidates_tool.Candidate]) -> set[str]:
    """Failed modules that hold at least one candidate of this batch.

    DSD prints overlays as `overlay N`, never as `ovNNN`; matching only the latter (as this function once did)
    identified no module in a real run, so every checksum failure ended as "could not isolate". A line that
    does contain a candidate module's own name is still honoured.
    """
    modules = {candidate.module for candidate in candidates}
    failures = _failed_checksum_modules(diagnostic) & modules
    for line in diagnostic.splitlines():
        if "checksum failed" not in line.lower():
            continue
        for module in modules:
            if re.search(rf"(?<![A-Za-z0-9_]){re.escape(module)}(?![A-Za-z0-9_])", line, re.IGNORECASE):
                failures.add(module)
    return failures


def _probe_modules(items: Sequence[candidates_tool.Candidate], project: Path,
                   dsd: Path, wibo: Path) -> bool:
    """Run reversible configure/module checks for a candidate subset (False if it cannot be evaluated)."""
    if not _apply_for_probe(items, project):
        return False
    try:
        configured = _stage(project, dsd, wibo, "configure")
        if configured.returncode:
            return False
        return _stage(project, dsd, wibo, "check_modules").returncode != 0
    finally:
        candidates_tool.rollback_candidates(project)
        _stage(project, dsd, wibo, "configure")  # derived build graph must match restored config


def _bisect_modules(items: Sequence[candidates_tool.Candidate], project: Path,
                    dsd: Path, wibo: Path) -> list[candidates_tool.Candidate]:
    remaining = list(items)
    if not remaining:
        return []
    while len(remaining) > 1:
        middle = len(remaining) // 2
        left, right = remaining[:middle], remaining[middle:]
        left_fails = _probe_modules(left, project, dsd, wibo)
        right_fails = _probe_modules(right, project, dsd, wibo)
        if left_fails:
            remaining = left
        elif right_fails:
            remaining = right
        else:
            return []
    return remaining

def _print_summary(integrated: Sequence[candidates_tool.Candidate], skipped: Sequence[tuple[Path, str]],
                   excluded: Sequence[tuple[Path, str]], report: dict | None) -> None:
    unit_count = len(integrated)
    function_count = sum(len(candidate.functions) for candidate in integrated)
    skipped_functions = 0
    for path, _ in skipped:
        try:
            skipped_functions += len(json.loads(path.read_text(encoding="utf-8"))["functions"])
        except (OSError, json.JSONDecodeError, KeyError, TypeError):
            continue
    print("Batch integration summary:")
    print(f"  Integrated: {unit_count} unit(s), {function_count} function(s)")
    print(f"  Skipped: {len(skipped)} unit(s), {skipped_functions} function(s)")
    if report is None:
        print("  Report metrics: unavailable")
    else:
        functions, byte_count, complete, total = _metrics(report)
        print(f"  Matched: {functions} functions, {byte_count} bytes, {complete}/{total} units")
    print(f"  Excluded manifests: {len(excluded)}")
    for path, reason in excluded:
        print(f"    {path}: {reason}")
    for path, reason in skipped:
        print(f"    skipped {path}: {reason}")


class WorkspaceLock:
    """Exclusive advisory lock so two integrations never mutate one workspace at once."""

    def __init__(self, project: Path) -> None:
        self.path = project / "build" / "scratch" / "integrate.lock"
        self._handle = None

    def __enter__(self) -> "WorkspaceLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._handle = open(self.path, "a+")
        try:
            lock_file(self._handle)
        except OSError as error:
            self._handle.close()
            raise BatchError(f"another integration holds {self.path}; wait for it to finish") from error
        self._handle.seek(0)
        self._handle.truncate()
        self._handle.write(str(os.getpid()))
        self._handle.flush()
        return self

    def __exit__(self, *exc) -> None:
        if self._handle is not None:
            try:
                lock_file(self._handle, unlock=True)
            finally:
                self._handle.close()


def _snapshot_workspace(project: Path, keep: int = 3) -> Path:
    """Tarball config/ and src/ (not build/) so any bad state is one extract away."""
    import tarfile
    directory = project / "build" / "scratch" / "snapshots"
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / time.strftime("pre-batch-%Y%m%d-%H%M%S.tar.gz")
    with tarfile.open(target, "w:gz") as archive:
        for name in ("config", "src"):
            if (project / name).exists():
                archive.add(project / name, arcname=name)
    for old in sorted(directory.glob("pre-batch-*.tar.gz"))[:-keep]:
        old.unlink()
    return target


def _require_consistent_workspace(project: Path) -> None:
    """Refuse to start when config references sources that do not exist (half-applied batch)."""
    journal = candidates_tool._rollback_path(project)
    if journal.exists():
        raise BatchError(f"pending integration journal {journal}; run integrate_candidates.py --rollback first")
    plan = _run(["ninja", "-n", "check_modules"], project)
    missing = sorted({match for match in re.findall(r"-c (src/Recovered/\S+\.cpp)", plan.stdout)
                      if not (project / match).is_file()})
    if missing:
        raise BatchError(f"workspace inconsistent: {len(missing)} configured source(s) missing, e.g. {missing[0]}; "
                         "complete or roll back the interrupted apply before integrating")


def run_batch(manifest_patterns: Sequence[str], project: Path, dsd: Path, wibo: Path,
              *, dry_run: bool = False, max_retries: int = 3,
              report_path: Path | None = None, skip_list: candidates_tool.SkipList = candidates_tool.SkipList()) -> int:
    if dry_run:
        return _run_batch_locked(manifest_patterns, project, dsd, wibo, dry_run=True,
                                 max_retries=max_retries, report_path=report_path, skip_list=skip_list)
    with WorkspaceLock(project):
        _require_consistent_workspace(project)
        print(f"snapshot: {_snapshot_workspace(project)}")
        try:
            return _run_batch_locked(manifest_patterns, project, dsd, wibo, dry_run=False,
                                     max_retries=max_retries, report_path=report_path, skip_list=skip_list)
        finally:
            # A failed or rolled-back run leaves build.ninja and build/eur/build/arm9.bin (which the
            # port's recompiler manifest reads) describing the abandoned state; regenerate both.
            _stage(project, dsd, wibo, "configure")
            _run(["ninja", "-j", "8"], project)  # exit code irrelevant: check_symbols may fail upstream


def _run_batch_locked(manifest_patterns: Sequence[str], project: Path, dsd: Path, wibo: Path,
                      *, dry_run: bool = False, max_retries: int = 3,
                      report_path: Path | None = None, skip_list: candidates_tool.SkipList = candidates_tool.SkipList()) -> int:
    if report_path is None:
        report_path = project / "build" / "eur" / "report.json"
    manifests = _expand_manifests(manifest_patterns, project)
    if not manifests:
        raise BatchError("no candidate manifests matched")
    chosen, skipped = candidates_tool.select_candidates(manifests, project, skip_list)
    boundary_eligible = []
    for candidate in chosen:
        if _planned_thumb_unit_end(candidate, project) is None:
            skipped.append((candidate.manifest, THUMB_GAP_REASON))
        elif _abuts_data_symbol(candidate, project):
            skipped.append((candidate.manifest, ABUTS_DATA_REASON))
        else:
            boundary_eligible.append(candidate)
    chosen, preflight_skipped = candidates_tool.preflight_eligible(boundary_eligible, project)
    skipped.extend(preflight_skipped)
    if dry_run:
        print(f"Dry run: preflighted {len(chosen)} candidate unit(s); no files changed.")
        for path, reason in skipped:
            print(f"  skipped {path}: {reason}")
        return 0
    active = list(chosen)
    excluded: list[tuple[Path, str]] = []
    for attempt in range(max_retries + 1):
        if not active:
            break
        applied = False
        report = None
        try:
            candidates_tool.apply_candidates(active, project)
            applied = True
            _run_checked(project, dsd, wibo, "configure")
            _run_checked(project, dsd, wibo, "build/eur/arm9.o")
            _run_checked(project, dsd, wibo, "check_modules")
            report = _fresh_report(project, report_path)
            metrics = _metrics(report)
            if metrics[3] <= 0 or metrics[2] > metrics[3]:
                raise BatchError("report unit measures are invalid")
            candidates_tool.finalize_candidates(project)
            _print_summary(active, skipped, excluded, report)
            return 0
        except (BatchError, candidates_tool.CandidateError, OSError) as error:
            details = str(error)
            if applied:
                try:
                    candidates_tool.rollback_candidates(project)
                except (candidates_tool.CandidateError, OSError) as rollback_error:
                    raise BatchError(f"rollback failed after: {details}") from rollback_error
            candidates_to_remove: list[candidates_tool.Candidate] = []
            reason = details
            if "check_modules failed" in details:
                if "checksum failed" not in details.lower():
                    _print_summary([], skipped, excluded, None)
                    raise BatchError(
                        f"final module check failed without a recognized checksum failure; "
                        f"candidates rolled back: {details}"
                    ) from error
                failed_modules = _checksum_modules(details, active)
                matching = [item for item in active if item.module in failed_modules]
                main_candidates = [item for item in active if item.module == "main"]
                candidates_to_remove = _bisect_modules(matching, project, dsd, wibo)
                if not candidates_to_remove:
                    candidates_to_remove = _bisect_modules(main_candidates, project, dsd, wibo)
                if not candidates_to_remove and matching:
                    candidates_to_remove = matching
                if not candidates_to_remove:
                    _print_summary([], skipped, excluded, None)
                    raise BatchError(
                        "could not isolate checksum failure to candidate modules "
                        f"{sorted(failed_modules) or ['(module not identified)']}; "
                        f"candidates rolled back: {details}"
                    ) from error
                reason = "module checksum failure isolated by check_modules"
            else:
                named_offenders = _offending_by_manifest(active, details)
                if named_offenders:
                    candidates_to_remove = named_offenders
                elif PANIC_TEXT in details:
                    try:
                        candidates_to_remove = _bisect_panic(
                            active, project, report_path, dsd, wibo)
                    except BatchError:
                        _print_summary([], skipped, excluded, None)
                        raise
                    reason = "objdiff report panic isolated by manifest bisection"
                else:
                    diagnostics = _symbols(details)
                    if diagnostics:
                        candidates_to_remove = _offending_by_symbol(active, diagnostics)
                        reason = "multiply-defined: " + "; ".join(diagnostics)
                    elif "build/eur/arm9.o failed" in details:
                        candidates_to_remove = _bisect_link(active, project, dsd, wibo)
                        reason = "arm9 link failure isolated by manifest bisection"
            if not candidates_to_remove:
                if PANIC_TEXT in details:
                    _print_summary([], skipped, excluded, None)
                    raise BatchError(
                        f"could not isolate objdiff report panic to a candidate: {details}"
                    ) from error
                if "build/eur/arm9.o failed" in details:
                    _print_summary([], skipped, excluded, None)
                    raise BatchError(f"could not isolate arm9 link failure: {details}") from error
                candidates_to_remove = active[:1] if len(active) == 1 else active[-1:]
            for item in candidates_to_remove:
                excluded.append((item.manifest, reason))
            active = [item for item in active if item not in candidates_to_remove]
            if attempt >= max_retries:
                _print_summary([], skipped, excluded, None)
                raise BatchError(f"retry limit reached: {details}") from error
    if excluded and not active:
        _print_summary([], skipped, excluded, None)
        raise BatchError("retry limit reached: no candidate units remain after exclusions")
    _print_summary([], skipped, excluded, None)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("manifests", nargs="+", help="candidate manifest paths or glob patterns")
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--skip-list", type=Path,
                        help="private JSON exclusions (default: DQ9_SKIP_LIST; otherwise empty with warning)")
    parser.add_argument("--dsd", type=Path, default=None, help="default: <project>/dsd-0.12.1-linux-x86_64")
    parser.add_argument("-w", "--wibo", type=Path, default=None, help="default: <project>/wibo")
    parser.add_argument("--max-retries", type=int, default=3, help="maximum rollback/exclusion retries")
    parser.add_argument("--report", type=Path, help="objdiff report path (default: <project>/build/eur/report.json)")
    parser.add_argument("--dry-run", action="store_true", help="preflight only; do not mutate the workspace")
    parser.add_argument("--chunk", type=int, default=None,
                        help="integrate N manifests per run (each chunk is snapshotted and verified; stops at first failure)")
    args = parser.parse_args(argv)
    try:
        if args.max_retries < 0:
            raise BatchError("--max-retries must be nonnegative")
        if args.chunk is not None and args.chunk < 1:
            raise BatchError("--chunk must be a positive integer")
        project = args.project.resolve()
        dsd = (args.dsd or project / "dsd-0.12.1-linux-x86_64").resolve()
        wibo = (args.wibo or project / "wibo").resolve()
        skip_list = candidates_tool.load_skip_list(args.skip_list)
        if args.chunk and not args.dry_run:
            manifests = [str(path) for path in _expand_manifests(args.manifests, project)]
            for start in range(0, len(manifests), args.chunk):
                print(f"chunk {start // args.chunk + 1}: {len(manifests[start:start + args.chunk])} manifest(s)")
                code = run_batch(manifests[start:start + args.chunk], project, dsd, wibo,
                                 max_retries=args.max_retries, report_path=args.report, skip_list=skip_list)
                if code:
                    return code
            return 0
        return run_batch(args.manifests, project, dsd, wibo,
                         dry_run=args.dry_run, max_retries=args.max_retries, report_path=args.report,
                         skip_list=skip_list)
    except (BatchError, candidates_tool.CandidateError, OSError) as error:
        print(f"integrate_batch.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
