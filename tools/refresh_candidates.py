#!/usr/bin/env python3
"""Recompile candidates whose object is stale or missing, then classify them with the integrator's preflight.

A candidate is skipped by the integrator when its `.cpp` is newer than its object ("candidate source is newer
than its object") or the object does not exist. This tool rebuilds those objects with the exact compile command of
the workspace's `mwcc` ninja rule (read from build.ninja, so it cannot drift from the real build), writing each
object atomically (a failed compile never destroys the previous object), and with --verify runs the integrator's
own non-mutating preflight (`port_eu_unit.py` plan against the current config) so a recompiled source whose bytes no
longer match the module is reported instead of being discovered later by a module checksum.

  refresh_candidates.py --list stale.txt [--verify] [--ok-list ok.txt] [--project EU]

The list holds project-relative manifest paths, one per line. Only objects under build/scratch/<author>/ and the
optional report files are written; config/ and src/ are never touched. It holds the integrator's workspace lock
(build/scratch/integrate.lock) for the whole run and refuses to start while another integration holds it or while a
transaction journal exists (an apply is in flight, or an interrupted apply needs --rollback).

Exit status: 0 every candidate refreshed (and verified when asked), 1 some failed to compile or verify, 2 bad input,
lock held or journal present.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "extracted" / "dqix-decomp-eu"
JOURNAL = "build/scratch/integrate_candidates_rollback.json"
COMPILE_TIMEOUT = 300


class RefreshError(RuntimeError):
    pass


def mwcc_flags(build_ninja_text: str) -> list[str]:
    """Compiler flags of the `rule mwcc` command, without input/output/dependency options.

    Returns the argument vector up to and including the compiler executable followed by its flags, with
    `$cc_flags` and `$game_version` resolved the way the build edges for `src/Recovered` resolve them.
    """
    match = re.search(r"^rule mwcc\n\s+command = (.*?)(?=^\S|\Z)", build_ninja_text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RefreshError("build.ninja has no `rule mwcc`")
    command = re.sub(r"\s*\$\n\s*", " ", match.group(1))
    command = command.split("&&")[0]  # drop the dependency-file post-processing step
    command = command.replace("$cc_flags", "-lang=c++").replace("$game_version", "eur")
    tokens = shlex.split(command)
    for position, token in enumerate(tokens):
        if token.endswith("mwccarm.exe"):
            break
    else:
        raise RefreshError("`rule mwcc` does not invoke mwccarm.exe")
    prefix, flags = tokens[:position + 1], tokens[position + 1:]
    if "-c" not in flags:
        raise RefreshError("`rule mwcc` has no -c input option")
    flags = flags[:flags.index("-c")]
    flags = [flag for flag in flags if flag != "-MD"]
    unresolved = [flag for flag in prefix + flags if "$" in flag]
    if unresolved:
        raise RefreshError(f"unresolved variables in the mwcc command: {unresolved}")
    return prefix + flags


def compile_command(flags: Sequence[str], source: Path, obj: Path) -> list[str]:
    return [*flags, "-c", str(source), "-o", str(obj)]


Runner = Callable[[Sequence[str], Path], "subprocess.CompletedProcess[str]"]


def _run(command: Sequence[str], cwd: Path) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(list(command), cwd=cwd, text=True, capture_output=True, check=False,
                          timeout=COMPILE_TIMEOUT)


def candidate_paths(project: Path, manifest: Path) -> tuple[Path, Path]:
    """(source, object) of a manifest; both are fixed by the manifest's location, never by its contents."""
    return manifest.with_suffix(".cpp"), project / "build" / "scratch" / manifest.parent.name / f"{manifest.stem}.o"


def needs_refresh(source: Path, obj: Path) -> bool:
    return not obj.is_file() or source.stat().st_mtime_ns > obj.stat().st_mtime_ns


def refresh_one(project: Path, manifest: Path, flags: Sequence[str], run: Runner | None = None,
                force: bool = False) -> tuple[str, str]:
    """Recompile one candidate. Returns (status, detail): `fresh`, `refreshed` or `compile-failed` / `no-source`."""
    run = run or _run
    source, obj = candidate_paths(project, manifest)
    if not source.is_file():
        return "no-source", f"{source} does not exist"
    if not force and not needs_refresh(source, obj):
        return "fresh", ""
    obj.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary_name = tempfile.mkstemp(prefix=f".{obj.stem}.", suffix=".o", dir=obj.parent)
    os.close(handle)
    temporary = Path(temporary_name)
    try:
        result = run(compile_command(flags, source.relative_to(project), temporary.relative_to(project)), project)
        output = ((result.stdout or "") + (result.stderr or "")).strip()
        if result.returncode != 0 or temporary.stat().st_size == 0:
            return "compile-failed", output[-400:] or f"compiler exit {result.returncode}"
        os.replace(temporary, obj)  # the new object is dated after the source it was built from
        return "refreshed", output[-200:]
    except subprocess.TimeoutExpired:
        return "compile-failed", f"compiler timed out after {COMPILE_TIMEOUT} s"
    finally:
        temporary.unlink(missing_ok=True)


def verify(project: Path, manifests: Sequence[Path], skip_list_path: Path | None = None) -> tuple[list[Path], list[tuple[Path, str]]]:
    """Run the integrator's non-mutating selection + preflight; returns (accepted, [(manifest, reason)])."""
    sys.path.insert(0, str(ROOT))
    from tools import integrate_candidates as candidates_tool

    chosen, skipped = candidates_tool.select_candidates(manifests, project,
                                                       candidates_tool.load_skip_list(skip_list_path))
    eligible, preflight_skipped = candidates_tool.preflight_eligible(chosen, project)
    return [c.manifest for c in eligible], [*skipped, *preflight_skipped]


def _exclusive(project: Path):
    """The integrator's own workspace lock, held for the whole compile/verify run.

    A journal check alone races an integrator that holds the lock before it writes its journal (or starts
    after the check), which would let objects be replaced while an apply is reading them.
    """
    sys.path.insert(0, str(ROOT))
    from tools import integrate_batch

    class _Lock:
        def __enter__(self):
            self.inner = integrate_batch.WorkspaceLock(project)
            try:
                self.inner.__enter__()
            except integrate_batch.BatchError as error:
                raise RefreshError(str(error)) from error
            return self

        def __exit__(self, *exc):
            self.inner.__exit__(*exc)

    return _Lock()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", type=Path, required=True, help="file with project-relative manifest paths")
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--verify", action="store_true", help="classify the refreshed candidates with the integrator preflight")
    parser.add_argument("--skip-list", type=Path,
                        help="private JSON exclusions for --verify (default: DQ9_SKIP_LIST; otherwise empty with warning)")
    parser.add_argument("--force", action="store_true", help="recompile even when the object is newer than the source")
    parser.add_argument("--ok-list", type=Path, help="write the manifests that passed (needs --verify)")
    parser.add_argument("--report", type=Path, help="write a JSON report")
    args = parser.parse_args(argv)
    if args.ok_list and not args.verify:
        parser.error("--ok-list needs --verify")
    project = args.project.resolve()
    try:
        with _exclusive(project):
            return _refresh_all(args, project)
    except (RefreshError, OSError, ValueError) as error:
        print(f"refresh_candidates.py: {error}", file=sys.stderr)
        return 2


def _refresh_all(args: argparse.Namespace, project: Path) -> int:
    try:
        if (project / JOURNAL).exists():
            raise RefreshError(f"an integration transaction journal exists ({project / JOURNAL}); not refreshing objects")
        manifests = [project / line.strip() for line in args.list.read_text().splitlines() if line.strip()]
        flags = mwcc_flags((project / "build.ninja").read_text(encoding="utf-8"))
    except (OSError, RefreshError) as error:
        print(f"refresh_candidates.py: {error}", file=sys.stderr)
        return 2
    outcomes: dict[str, dict] = {}
    for manifest in manifests:
        key = manifest.relative_to(project).as_posix()
        if not manifest.is_file():
            outcomes[key] = {"status": "no-manifest", "detail": ""}
            continue
        status, detail = refresh_one(project, manifest, flags, force=args.force)
        outcomes[key] = {"status": status, "detail": detail}
    usable = [project / key for key, value in outcomes.items() if value["status"] in ("fresh", "refreshed")]
    if args.verify:
        accepted, rejected = verify(project, usable, args.skip_list)
        accepted_keys = {path.relative_to(project).as_posix() for path in accepted}
        for key in (p.relative_to(project).as_posix() for p in usable):
            outcomes[key]["verified"] = key in accepted_keys
        for path, reason in rejected:
            try:
                key = path.relative_to(project).as_posix()
            except ValueError:
                key = str(path)
            outcomes.setdefault(key, {"status": "rejected", "detail": ""})["verify_reason"] = reason
        if args.ok_list:
            args.ok_list.write_text("".join(f"{key}\n" for key in sorted(accepted_keys)), encoding="utf-8")
    counts: dict[str, int] = {}
    for value in outcomes.values():
        counts[value["status"]] = counts.get(value["status"], 0) + 1
    print(", ".join(f"{name}={count}" for name, count in sorted(counts.items())) or "nothing to do")
    if args.verify:
        print(f"verified (integrator preflight accepts): {sum(1 for v in outcomes.values() if v.get('verified'))}"
              f" of {len(usable)}")
    for key, value in outcomes.items():
        if value["status"] in ("compile-failed", "no-source", "no-manifest") or value.get("verified") is False:
            print(f"  {key}: {value['status']}{' / ' + value['verify_reason'][:100] if value.get('verify_reason') else ''}"
                  f"{' ' + value['detail'][:100].replace(chr(10), ' ') if value['detail'] else ''}")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(outcomes, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    failed = any(v["status"] not in ("fresh", "refreshed") or v.get("verified") is False for v in outcomes.values())
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        raise SystemExit(1)
