#!/usr/bin/env python3
"""Add the `#define` aliases that `port_eu_unit.py` asks for to candidate sources, rebuild, and re-verify.

A compiled candidate can reference a symbol under a name that differs from the one the module's symbols.txt uses
for that address (a helper called `synthetic_helper` that the target names `memset`). `port_eu_unit.py` then exits 1
and lists `#define <object name> <module name>` lines; the integrator refuses such a candidate because the source
does not carry them yet. This tool puts exactly those lines at the very top of the source (so declarations in
headers are renamed too), rebuilds the object with the flags of the workspace's `mwcc` rule, and keeps the change
only when the integrator's own non-mutating preflight now accepts the candidate. Otherwise the original source and
object are restored byte for byte.

  apply_source_defines.py --list manifests.txt [--ok-list ok.txt] [--report report.json] [--project EU]

Idempotent (defines already present are not added again). Holds the integrator's workspace lock for the whole run.
Writes only the candidates' own .cpp/.o files and the optional output files; never config/ or src/Recovered.

Exit status: 0 every listed candidate is now accepted by the preflight, 1 some were not, 2 bad input / lock held.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import integrate_candidates as candidates_tool  # noqa: E402
from tools import refresh_candidates as refresh  # noqa: E402

HEADER = "// Aliases required by tools/port_eu_unit.py: references must use the module's symbol names.\n"


def plan_defines(candidate: candidates_tool.Candidate, project: Path) -> tuple[list[str], list[str]]:
    """(`#define` lines, conflicts) from a non-mutating port plan that tolerates missing defines."""
    command = candidates_tool._port_command(candidate, project, False) + ["--allow-defines"]
    result = subprocess.run(command, cwd=project, text=True, capture_output=True, check=False)
    try:
        plan = json.loads(result.stdout)
    except ValueError as error:
        raise refresh.RefreshError(f"{candidate.manifest.name}: unreadable port plan: "
                                   f"{(result.stderr or result.stdout)[-200:].strip()}") from error
    return list(plan.get("source_defines", [])), list(plan.get("conflicts", []))


def with_defines(text: str, defines: Sequence[str]) -> str:
    """`text` with the missing defines prepended; unchanged when they are all present already."""
    present = {line.strip() for line in text.splitlines()}
    missing = [line for line in defines if line not in present]
    if not missing:
        return text
    return HEADER + "\n".join(missing) + "\n" + text


def write_atomic(path: Path, data: bytes) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(data)
    os.replace(temporary, path)


def process(project: Path, manifest: Path, flags: Sequence[str]) -> dict:
    """Apply, rebuild and verify one candidate; restore it unless the preflight accepts the result."""
    source, obj = refresh.candidate_paths(project, manifest)
    try:
        candidate = candidates_tool.load_candidate(manifest, project)
    except candidates_tool.CandidateError as error:
        return {"status": "not-loadable", "detail": str(error)[-200:]}
    try:
        defines, conflicts = plan_defines(candidate, project)
    except refresh.RefreshError as error:
        return {"status": "no-plan", "detail": str(error)}
    if conflicts:
        return {"status": "conflicts", "detail": "; ".join(conflicts)[:300]}
    if not defines:
        return {"status": "no-defines", "detail": ""}
    original_source, original_object = source.read_bytes(), obj.read_bytes()
    new_text = with_defines(original_source.decode("utf-8"), defines)
    if new_text.encode("utf-8") == original_source:
        return {"status": "already-applied", "detail": ""}
    write_atomic(source, new_text.encode("utf-8"))
    status, detail = refresh.refresh_one(project, manifest, flags, force=True)
    accepted = False
    reason = ""
    if status == "refreshed":
        accepted_list, rejected = refresh.verify(project, [manifest])
        accepted = bool(accepted_list)
        reason = rejected[0][1] if rejected else ""
    if accepted:
        return {"status": "applied", "detail": f"{len(defines)} define(s)", "defines": defines}
    write_atomic(source, original_source)  # restore both files exactly
    write_atomic(obj, original_object)
    os.utime(obj)  # keep the restored object newer than the restored source
    return {"status": "reverted", "detail": (detail or reason)[-300:], "defines": defines}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", type=Path, required=True, help="project-relative manifest paths, one per line")
    parser.add_argument("--project", type=Path, default=refresh.DEFAULT_PROJECT)
    parser.add_argument("--ok-list", type=Path, help="write manifests the preflight accepts afterwards")
    parser.add_argument("--report", type=Path, help="write a JSON report")
    args = parser.parse_args(argv)
    project = args.project.resolve()
    try:
        with refresh._exclusive(project):
            if (project / refresh.JOURNAL).exists():
                raise refresh.RefreshError(f"an integration transaction journal exists ({project / refresh.JOURNAL})")
            manifests = [project / line.strip() for line in args.list.read_text().splitlines() if line.strip()]
            flags = refresh.mwcc_flags((project / "build.ninja").read_text(encoding="utf-8"))
            outcomes = {}
            for manifest in manifests:
                key = manifest.relative_to(project).as_posix()
                outcomes[key] = (process(project, manifest, flags) if manifest.is_file()
                                 else {"status": "no-manifest", "detail": ""})
    except (OSError, refresh.RefreshError) as error:
        print(f"apply_source_defines.py: {error}", file=sys.stderr)
        return 2
    counts: dict[str, int] = {}
    for value in outcomes.values():
        counts[value["status"]] = counts.get(value["status"], 0) + 1
    print(", ".join(f"{name}={count}" for name, count in sorted(counts.items())) or "nothing to do")
    good = {"applied", "already-applied"}
    for key, value in outcomes.items():
        if value["status"] not in good:
            print(f"  {key}: {value['status']} {value['detail'][:140].replace(chr(10), ' ')}")
    if args.ok_list:
        args.ok_list.write_text("".join(f"{k}\n" for k, v in sorted(outcomes.items()) if v["status"] in good),
                                encoding="utf-8")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(outcomes, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if all(v["status"] in good for v in outcomes.values()) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        raise SystemExit(1)
