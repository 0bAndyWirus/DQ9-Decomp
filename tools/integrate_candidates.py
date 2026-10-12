#!/usr/bin/env python3
"""Preflight and integrate compiled candidate units into the EU DSD project."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "extracted" / "dqix-decomp-eu"
PORT_SCRIPT = ROOT / "tools" / "port_eu_unit.py"
MODULE_RE = re.compile(r"(?:main|itcm|dtcm|ov\d{3})\Z")


class CandidateError(ValueError):
    """Invalid manifest or a failed integration operation."""


class IncompleteCandidate(CandidateError):
    """Candidate is still being produced or was already integrated."""


@dataclass(frozen=True)
class Candidate:
    manifest: Path
    source: Path
    object: Path
    module: str
    functions: tuple[dict[str, str], ...]
    destination: Path
    manifest_destination: Path
    headers: tuple[tuple[Path, Path], ...] = ()  # (author-local source, destination) copies


def _contained(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False

_LOCAL_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def local_headers(source: Path, destination_dir: Path) -> list[tuple[Path, Path]]:
    """Author-local headers a candidate includes (transitively), with their copy destinations.

    Candidates compile in their author directory, so `#include "x.h"` resolves next to the
    source. Once the .cpp moves to src/Recovered/<module>, those headers must move with it.
    """
    found: dict[Path, Path] = {}
    pending = [source]
    root = source.parent
    while pending:
        current = pending.pop()
        for name in _LOCAL_INCLUDE_RE.findall(current.read_text(encoding="utf-8", errors="replace")):
            header = (current.parent / name).resolve()
            if header in found or not header.is_file() or not _contained(header, root):
                continue
            found[header] = destination_dir / header.relative_to(root.resolve())
            pending.append(header)
    return sorted(found.items())


def _same_bytes(a: Path, b: Path) -> bool:
    return a.read_bytes() == b.read_bytes()


VARIANT_PREFIX = "_h"


def header_variant_digest(headers: Sequence[tuple[Path, Path]], base: Path) -> str:
    """Stable id of a header set: sha1 over (relative name, content sha1), so equal sets share a directory."""
    digest = hashlib.sha1()
    for header, destination in sorted(headers, key=lambda item: item[1].relative_to(base).as_posix()):
        digest.update(destination.relative_to(base).as_posix().encode())
        digest.update(hashlib.sha1(header.read_bytes()).digest())
    return digest.hexdigest()[:10]


def place_candidate(project: Path, source: Path, module: str, *,
                    force_variant: bool) -> tuple[Path, tuple[tuple[Path, Path], ...]]:
    """Choose the directory a candidate moves to and the author-local headers that must be copied there.

    Normally `src/Recovered/<module>/`. When one of its headers differs from a same-named header already in
    that directory (or `force_variant` is set, used for batch-internal conflicts), the candidate goes to
    `src/Recovered/<module>/_h<digest>/` together with its own headers, so both versions coexist and
    `#include "x.h"` still resolves beside the source with no source edit. Returns (destination, headers
    still to be copied); a header already present with identical bytes is not copied again.
    """
    base = project / "src" / "Recovered" / module
    plain = local_headers(source, base)
    conflict = any(destination.is_file() and not _same_bytes(header, destination) for header, destination in plain)
    if not (conflict or force_variant) or not plain:
        directory, wanted = base, plain
    else:
        directory = base / f"{VARIANT_PREFIX}{header_variant_digest(plain, base)}"
        wanted = local_headers(source, directory)
    headers: list[tuple[Path, Path]] = []
    for header, destination in wanted:
        if destination.exists():
            if not _same_bytes(header, destination):  # unreachable for variant dirs (digest keys the content)
                raise CandidateError(f"author-local header {header.name} conflicts with different "
                                     f"{destination.relative_to(project)}")
            continue
        headers.append((header, destination))
    return directory / source.name, tuple(headers)




def load_candidate(manifest: Path, project: Path) -> Candidate:
    manifest = manifest.resolve()
    project = project.resolve()
    candidates_root = project / "src" / "_candidates"
    if not _contained(manifest, candidates_root) or not manifest.is_file():
        raise CandidateError(f"manifest must be an existing file under {candidates_root}: {manifest}")
    author = manifest.parent.name
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CandidateError(f"{manifest}: cannot read JSON manifest: {error}") from error
    if not isinstance(data, dict):
        raise CandidateError(f"{manifest}: manifest must be a JSON object")

    file_value, object_value = data.get("file"), data.get("object")
    module, functions = data.get("module"), data.get("functions")
    if object_value is None:  # older manifests omit it; the object location is fixed by convention anyway
        object_value = f"build/scratch/{author}/{manifest.stem}.o"
    if not isinstance(file_value, str) or not isinstance(object_value, str):
        raise CandidateError(f"{manifest}: file and object must be path strings")
    source = (project / file_value).resolve()
    obj = (project / object_value).resolve()
    expected_source = (candidates_root / author / f"{manifest.stem}.cpp").resolve()
    expected_object = (project / "build" / "scratch" / author / f"{manifest.stem}.o").resolve()
    if source != expected_source or not _contained(source, candidates_root):
        raise CandidateError(f"{manifest}: file must be src/_candidates/{author}/{manifest.stem}.cpp")
    if obj != expected_object or not _contained(obj, project / "build" / "scratch"):
        raise CandidateError(f"{manifest}: object must be build/scratch/{author}/{manifest.stem}.o")
    if not source.is_file():
        raise CandidateError(f"{manifest}: candidate source does not exist: {source}")
    if not obj.is_file():
        raise IncompleteCandidate(f"{manifest}: candidate object does not exist: {obj}")
    if source.stat().st_mtime_ns > obj.stat().st_mtime_ns:
        raise IncompleteCandidate(f"{manifest}: candidate source is newer than its object")
    if not isinstance(module, str) or not MODULE_RE.fullmatch(module):
        raise CandidateError(f"{manifest}: invalid module {module!r}")
    if not isinstance(functions, list) or not functions:
        raise CandidateError(f"{manifest}: functions must be a nonempty list")
    checked_functions: list[dict[str, str]] = []
    for index, function in enumerate(functions):
        # Older manifests name the function under "symbol"; both spell the same thing.
        label = function.get("name", function.get("symbol")) if isinstance(function, dict) else None
        if (not isinstance(function, dict) or not isinstance(label, str)
                or not label.strip() or not isinstance(function.get("address"), str)
                or not re.fullmatch(r"0[xX][0-9a-fA-F]+", function["address"])):
            raise CandidateError(f"{manifest}: function {index} needs a nonempty name and hexadecimal address")
        checked_functions.append({"name": label, "address": function["address"]})
    destination, headers = place_candidate(project, source, module, force_variant=False)
    manifest_destination = destination.with_suffix(".json")
    if destination.exists() or manifest_destination.exists():
        raise IncompleteCandidate(f"{manifest}: destination already exists: {destination}")
    return Candidate(manifest, source, obj, module, tuple(checked_functions), destination,
                     manifest_destination, headers)


def discover_manifests(project: Path) -> list[Path]:
    return sorted((project / "src" / "_candidates").glob("*/*.json"))


_FUNCTION_SYMBOL_RE = re.compile(r"\bkind:function\([^,)]*,size=(0x[0-9a-fA-F]+)\).*?\baddr:(0x[0-9a-fA-F]+)")


def _symbols_path(project: Path, module: str) -> Path:
    arm9 = project / "config" / "eur" / "arm9"
    if module == "main":
        return arm9 / "symbols.txt"
    if module in {"itcm", "dtcm"}:
        return arm9 / module / "symbols.txt"
    return arm9 / "overlays" / module / "symbols.txt"


def function_sizes(project: Path, module: str, cache: dict[str, dict[int, int]]) -> dict[int, int]:
    """Map function address to DSD-declared size for one module."""
    if module not in cache:
        sizes: dict[int, int] = {}
        path = _symbols_path(project, module)
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                match = _FUNCTION_SYMBOL_RE.search(line)
                if match:
                    sizes[int(match.group(2), 16) & ~1] = int(match.group(1), 16)
        cache[module] = sizes
    return cache[module]


def contiguity_error(candidate: Candidate, sizes: dict[int, int]) -> str | None:
    """A candidate becomes one delinked range, so its functions must abut.

    Gaps between functions would be swallowed into the unit (or left as
    unreferenced gap objects that the dead-stripping linker drops), shifting
    every later address. Up to three bytes of alignment padding are allowed.
    """
    addresses = sorted(int(function["address"], 0) & ~1 for function in candidate.functions)
    for current, following in zip(addresses, addresses[1:]):
        size = sizes.get(current)
        if size is None:
            return f"function {current:#x} has no DSD-declared size"
        if not 0 <= following - (current + size) <= 3:
            return f"functions {current:#x} and {following:#x} are not contiguous"
    return None


_DELINK_SECTION_RE = re.compile(r"^\s+\.\S+\s+start:(0x[0-9a-fA-F]+)\s+end:(0x[0-9a-fA-F]+)")


def delinked_ranges(project: Path, module: str, cache: dict[str, list[tuple[int, int]]]) -> list[tuple[int, int]]:
    """Address ranges already owned by a source unit in this module's delinks."""
    if module not in cache:
        ranges: list[tuple[int, int]] = []
        path = _symbols_path(project, module).with_name("delinks.txt")
        if path.is_file():
            in_unit = False  # leading section lines describe the whole module
            for line in path.read_text(encoding="utf-8").splitlines():
                if line and not line[0].isspace():
                    in_unit = line.rstrip().endswith(":")
                    continue
                match = _DELINK_SECTION_RE.match(line)
                if in_unit and match:
                    ranges.append((int(match.group(1), 16), int(match.group(2), 16)))
        cache[module] = ranges
    return cache[module]


def ownership_error(candidate: Candidate, ranges: list[tuple[int, int]]) -> str | None:
    for function in candidate.functions:
        address = int(function["address"], 0) & ~1
        for start, end in ranges:
            if start <= address < end:
                return f"function {address:#x} is already delinked in [{start:#x},{end:#x})"
    return None


def rehome_batch_header_conflicts(candidates: Sequence[Candidate],
                                  project: Path) -> tuple[list[Candidate], list[tuple[Path, str]]]:
    """Give candidates of one batch whose same-named headers differ their own variant directory.

    Two units cannot share `src/Recovered/<module>/x.h` with different contents. The majority content keeps
    the module directory (first on a tie); every other unit moves to `<module>/_h<digest>/` with its headers.
    """
    owners: dict[Path, dict[bytes, list[int]]] = {}
    for index, candidate in enumerate(candidates):
        for header, destination in candidate.headers:
            owners.setdefault(destination, {}).setdefault(header.read_bytes(), []).append(index)
    moved: set[int] = set()
    for by_content in owners.values():
        if len(by_content) > 1:
            keep = max(by_content.values(), key=len)
            for group in by_content.values():
                if group is not keep:
                    moved.update(group)
    result: list[Candidate | None] = list(candidates)
    skipped: list[tuple[Path, str]] = []
    for index in sorted(moved):
        candidate = candidates[index]
        destination, headers = place_candidate(project, candidate.source, candidate.module, force_variant=True)
        manifest_destination = destination.with_suffix(".json")
        if destination.exists() or manifest_destination.exists():
            skipped.append((candidate.manifest, f"destination already exists: {destination}"))
            result[index] = None
            continue
        result[index] = replace(candidate, destination=destination, manifest_destination=manifest_destination,
                                headers=headers)
    return [candidate for candidate in result if candidate is not None], skipped


@dataclass(frozen=True)
class SkipList:
    """Caller-supplied exclusions, shared by selection and target generation."""

    functions: frozenset[tuple[str, int]] = frozenset()
    ranges: tuple[tuple[str, int, int], ...] = ()
    name_prefixes: tuple[str, ...] = ()


def load_skip_list(path: Path | None = None) -> SkipList:
    """Load private exclusion rules from JSON or DQ9_SKIP_LIST; no built-in exclusions."""
    if path is None:
        configured = os.environ.get("DQ9_SKIP_LIST")
        path = Path(configured) if configured else None
    if path is None:
        print("warning: no --skip-list or DQ9_SKIP_LIST supplied; using an empty skip list",
              file=sys.stderr)
        return SkipList()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, list):
            data = {"functions": data}
        if not isinstance(data, dict):
            raise ValueError("expected a JSON object or a list of function records")

        def address(value: object) -> int:
            result = int(value, 0) if isinstance(value, str) else value
            if not isinstance(result, int) or isinstance(result, bool) or result < 0:
                raise ValueError("addresses must be nonnegative integers or integer strings")
            return result

        def records(key: str) -> list:
            value = data.get(key, [])
            if not isinstance(value, list):
                raise ValueError(f"{key} must be a list")
            return value

        def module(record: object) -> str:
            if not isinstance(record, dict) or not isinstance(record.get("module"), str):
                raise ValueError("each function/range record needs a module")
            name = record["module"]
            if not MODULE_RE.fullmatch(name):
                raise ValueError(f"invalid module: {name!r}")
            return name

        functions = frozenset((module(record), address(record["address"]) & ~1)
                              for record in records("functions"))
        ranges = []
        for record in records("ranges"):
            name = module(record)
            start, end = address(record["start"]), address(record["end"])
            if start >= end:
                raise ValueError("range start must precede its exclusive end")
            ranges.append((name, start, end))
        prefixes = records("name_prefixes")
        if any(not isinstance(prefix, str) or not prefix for prefix in prefixes):
            raise ValueError("name_prefixes must contain nonempty strings")
        return SkipList(functions, tuple(ranges), tuple(prefixes))
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise CandidateError(f"cannot load skip list {path}: {error}") from error


def forbidden_function(module: str, address: int, name: str = "",
                       skip_list: SkipList = SkipList()) -> str | None:
    """Why a function matches caller-supplied exclusions, or None."""
    address &= ~1
    if any(name.startswith(prefix) for prefix in skip_list.name_prefixes):
        return "function name matches the supplied skip list"
    if (module, address) in skip_list.functions:
        return "function is on the supplied skip list"
    if any(module == owner and start <= address < end for owner, start, end in skip_list.ranges):
        return "function is in a supplied skip-list range"
    return None


def forbidden_reason(candidate: Candidate, skip_list: SkipList = SkipList()) -> str | None:
    """Skip reason when any function of the candidate is forbidden; the whole unit is then refused."""
    for function in candidate.functions:
        reason = forbidden_function(candidate.module, int(function["address"], 0),
                                    function["name"], skip_list)
        if reason:
            return f"{candidate.manifest.name}: {function['name']} {reason}"
    return None


def select_candidates(manifests: Sequence[Path], project: Path,
                      skip_list: SkipList = SkipList()) -> tuple[list[Candidate], list[tuple[Path, str]]]:
    """Snapshot complete candidates and exclude ambiguous units before preflight."""
    candidates: list[Candidate] = []
    skipped: list[tuple[Path, str]] = []
    for manifest in manifests:
        try:
            loaded = load_candidate(manifest, project)
            reason = forbidden_reason(loaded, skip_list)
            if reason:
                skipped.append((manifest.resolve(), reason))
            else:
                candidates.append(loaded)
        except CandidateError as error:
            skipped.append((manifest.resolve(), str(error)))

    size_cache: dict[str, dict[int, int]] = {}
    delink_cache: dict[str, list[tuple[int, int]]] = {}
    contiguous: list[Candidate] = []
    for candidate in candidates:
        error = contiguity_error(candidate, function_sizes(project, candidate.module, size_cache))
        if error is None:
            error = ownership_error(candidate, delinked_ranges(project, candidate.module, delink_cache))
        if error is None:
            contiguous.append(candidate)
        else:
            skipped.append((candidate.manifest, error))
    candidates, rehome_skipped = rehome_batch_header_conflicts(contiguous, project)
    skipped.extend(rehome_skipped)
    address_owners: dict[tuple[str, int], list[Path]] = {}
    destination_owners: dict[Path, list[Path]] = {}
    for candidate in candidates:
        destination_owners.setdefault(candidate.destination, []).append(candidate.manifest)
        for function in candidate.functions:
            key = candidate.module, int(function["address"], 0)
            address_owners.setdefault(key, []).append(candidate.manifest)

    skip_reasons: dict[Path, set[str]] = {}
    for (module, address), owners in address_owners.items():
        if len(owners) > 1:
            manifests_with_address = set(owners)
            owner_names = ", ".join(sorted(path.name for path in manifests_with_address))
            reason = f"duplicate {module} function {address:#x} also appears in {owner_names}"
            for manifest in manifests_with_address:
                skip_reasons.setdefault(manifest, set()).add(reason)
    for destination, owners in destination_owners.items():
        if len(owners) > 1:
            manifests_for_destination = set(owners)
            owner_names = ", ".join(sorted(path.name for path in manifests_for_destination))
            reason = f"duplicate destination {destination} also appears in {owner_names}"
            for manifest in manifests_for_destination:
                skip_reasons.setdefault(manifest, set()).add(reason)

    eligible: list[Candidate] = []
    for candidate in candidates:
        reasons = skip_reasons.get(candidate.manifest)
        if reasons:
            skipped.append((candidate.manifest, "; ".join(sorted(reasons))))
        else:
            eligible.append(candidate)
    return eligible, skipped


def _port_command(candidate: Candidate, project: Path, apply: bool) -> list[str]:
    address = min(int(function["address"], 0) for function in candidate.functions)
    command = [sys.executable, str(PORT_SCRIPT), str(candidate.object),
               candidate.destination.relative_to(project).as_posix(), "--project", str(project),
               "--module", candidate.module, "--text-address", hex(address)]
    if apply:
        command.append("--apply")
    return command


def preflight(candidates: Sequence[Candidate], project: Path) -> None:
    errors: list[str] = []
    for candidate in candidates:
        try:
            result = subprocess.run(_port_command(candidate, project, False), cwd=project,
                                    text=True, capture_output=True, check=False)
        except OSError as error:
            errors.append(f"{candidate.manifest.name}: could not run port_eu_unit.py: {error}")
            continue
        if result.returncode:
            detail = (result.stderr or result.stdout).strip()
            errors.append(f"{candidate.manifest.name}: preflight failed (exit {result.returncode})"
                          + (f": {detail}" if detail else ""))
            continue
        try:
            plan = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            errors.append(f"{candidate.manifest.name}: preflight returned invalid plan: {error}")
            continue
        if not isinstance(plan, dict):
            errors.append(f"{candidate.manifest.name}: preflight plan must be a JSON object")
            continue
        try:
            text_section = plan.get("sections", {}).get(".text", {})
            text_start = int(text_section["start"], 0)
        except (AttributeError, KeyError, TypeError, ValueError):
            errors.append(f"{candidate.manifest.name}: preflight plan has no valid .text start")
            continue
        expected_start = min(int(function["address"], 0) for function in candidate.functions)
        if text_start != expected_start:
            errors.append(f"{candidate.manifest.name}: planned .text start {text_start:#x} "
                          f"does not match earliest function address {expected_start:#x}")
        conflicts = plan.get("conflicts", [])
        defines = plan.get("source_defines", [])
        if conflicts:
            errors.append(f"{candidate.manifest.name}: conflicts: " + "; ".join(map(str, conflicts)))
        if defines:
            errors.append(f"{candidate.manifest.name}: source defines required: "
                          + "; ".join(map(str, defines)))
    if errors:
        raise CandidateError("preflight rejected candidate set: " + " | ".join(errors))

def preflight_eligible(candidates: Sequence[Candidate], project: Path) -> tuple[list[Candidate], list[tuple[Path, str]]]:
    eligible: list[Candidate] = []
    skipped: list[tuple[Path, str]] = []
    for candidate in candidates:
        try:
            preflight([candidate], project)
        except CandidateError as error:
            skipped.append((candidate.manifest, str(error)))
        else:
            eligible.append(candidate)
    return eligible, skipped


def _backup_config(project: Path) -> tuple[Path, dict[Path, bytes]]:
    config_root = project / "config"
    files = {path: path.read_bytes() for path in config_root.rglob("*") if path.is_file()} if config_root.exists() else {}
    return config_root, files


def _restore_config(config_root: Path, files: dict[Path, bytes]) -> None:
    if config_root.exists():
        for path in sorted((p for p in config_root.rglob("*") if p.is_file()), reverse=True):
            if path not in files:
                path.unlink()
    for path, contents in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)

def _rollback_path(project: Path) -> Path:
    return project / "build" / "scratch" / "integrate_candidates_rollback.json"


def _journal_state(project: Path, backups: dict[Path, bytes],
                   planned: Sequence[tuple[Path, Path]], copied: Sequence[Path] = ()) -> dict:
    return {
        "config": [
            {"path": path.relative_to(project).as_posix(), "contents": base64.b64encode(contents).decode("ascii")}
            for path, contents in backups.items()
        ],
        "copied": [destination.relative_to(project).as_posix() for destination in copied],
        "moved": [
            {"destination": destination.relative_to(project).as_posix(),
             "source": source.relative_to(project).as_posix()}
            for destination, source in planned
        ],
    }


def _write_journal(rollback_file: Path, state: dict) -> None:
    rollback_file.parent.mkdir(parents=True, exist_ok=True)
    temporary = rollback_file.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, rollback_file)


class _Terminated(BaseException):
    """Raised from SIGTERM/SIGHUP while applying so the journal rolls the batch back."""


def apply_candidates(candidates: Sequence[Candidate], project: Path) -> None:
    """Apply candidates under a write-ahead journal.

    The journal (config backup + every planned move) is persisted before the first
    mutation. Any failure or termination signal rolls back immediately; if the
    process is killed outright the journal survives and `--rollback` restores the
    workspace, tolerating moves that never happened.
    """
    rollback_file = _rollback_path(project)
    if rollback_file.exists():
        raise CandidateError(f"pending rollback exists at {rollback_file}; run --rollback before another apply")
    config_root, backups = _backup_config(project)
    planned: list[tuple[Path, Path]] = []
    for candidate in candidates:
        planned.append((candidate.destination, candidate.source))
        planned.append((candidate.manifest_destination, candidate.manifest))
    copied = sorted({destination for candidate in candidates for _, destination in candidate.headers})
    _write_journal(rollback_file, _journal_state(project, backups, planned, copied))

    def _terminate(signum, frame):
        raise _Terminated(f"signal {signum}")

    termination_signals = (signal.SIGTERM,) + ((signal.SIGHUP,) if hasattr(signal, "SIGHUP") else ())
    previous = {sig: signal.signal(sig, _terminate) for sig in termination_signals}
    try:
        for candidate in candidates:
            result = subprocess.run(_port_command(candidate, project, True), cwd=project,
                                    text=True, capture_output=True, check=False)
            if result.returncode:
                detail = (result.stderr or result.stdout).strip()
                raise CandidateError(f"apply failed for {candidate.manifest.name} (exit {result.returncode})"
                                     + (f": {detail}" if detail else ""))
            candidate.destination.parent.mkdir(parents=True, exist_ok=True)
            for header, header_destination in candidate.headers:
                if not header_destination.exists():
                    header_destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(header, header_destination)
            shutil.move(str(candidate.source), str(candidate.destination))
            shutil.move(str(candidate.manifest), str(candidate.manifest_destination))
    except BaseException as error:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        previous = {}
        rollback_candidates(project)
        if isinstance(error, CandidateError):
            raise
        if isinstance(error, _Terminated):
            raise CandidateError(f"integration interrupted ({error}); changes rolled back") from None
        if isinstance(error, Exception):
            raise CandidateError(f"integration failed; changes rolled back: {error}") from error
        raise
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def rollback_candidates(project: Path) -> None:
    rollback_file = _rollback_path(project)
    if not rollback_file.is_file():
        raise CandidateError(f"no saved integration state to roll back: {rollback_file}")
    try:
        state = json.loads(rollback_file.read_text(encoding="utf-8"))
        moved = [(project / item["destination"], project / item["source"]) for item in state["moved"]]
        config = {
            project / item["path"]: base64.b64decode(item["contents"], validate=True)
            for item in state["config"]
        }
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        raise CandidateError(f"cannot read saved integration state: {error}") from error
    config_root = project / "config"
    for path in config:
        if not _contained(path, config_root):
            raise CandidateError("saved config backup path escapes config directory")
    pending: list[tuple[Path, Path]] = []
    for destination, source in reversed(moved):
        if not _contained(destination, project) or not _contained(source, project):
            raise CandidateError("saved rollback paths escape project root")
        if destination.is_file() and not source.exists():
            pending.append((destination, source))
        elif source.is_file() and not destination.exists():
            continue  # this move never happened (interrupted apply)
        else:
            raise CandidateError(f"cannot restore {source}: expected exactly one of {source} / {destination}")
    for destination, source in pending:
        source.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(destination), str(source))
    touched_dirs: set[Path] = {destination.parent for destination, _ in pending}
    for copied in state.get("copied", []):
        target = project / copied
        if not _contained(target, project / "src" / "Recovered"):
            raise CandidateError("saved copied-header path escapes src/Recovered")
        target.unlink(missing_ok=True)
        touched_dirs.add(target.parent)
    for directory in touched_dirs:  # a variant directory created by this apply is removed once empty
        if directory.name.startswith(VARIANT_PREFIX) and directory.is_dir() and not any(directory.iterdir()):
            directory.rmdir()
    _restore_config(config_root, config)
    rollback_file.unlink()


def finalize_candidates(project: Path) -> None:
    rollback_file = _rollback_path(project)
    if not rollback_file.is_file():
        raise CandidateError(f"no saved integration state to commit: {rollback_file}")
    rollback_file.unlink()

def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("manifests", nargs="*", type=Path, help="manifest paths (default: discover all pending candidates)")
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT, help="EU project root")
    parser.add_argument("--skip-list", type=Path,
                        help="private JSON exclusions (default: DQ9_SKIP_LIST; otherwise empty with warning)")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--apply", action="store_true", help="commit config edits and move candidate sources")
    action.add_argument("--dry-run", action="store_true", help="preflight only (the default)")
    action.add_argument("--rollback", action="store_true", help="restore the previous applied batch")
    action.add_argument("--commit", action="store_true", help="discard rollback state after successful checks")
    args = parser.parse_args(argv)
    project = args.project.resolve()
    try:
        if args.rollback or args.commit:
            if args.manifests:
                raise CandidateError("--rollback/--commit cannot be combined with manifest paths")
            if args.rollback:
                rollback_candidates(project)
                print("Rolled back the previous integration batch.")
            else:
                finalize_candidates(project)
                print("Committed the previous integration batch; rollback state discarded.")
            return 0
        if args.manifests:
            manifests = tuple(dict.fromkeys(
                p if p.is_absolute() else project / p for p in args.manifests))
        else:
            manifests = tuple(discover_manifests(project))
        if not manifests:
            raise CandidateError("no candidate manifests found")
        candidates, skipped = select_candidates(manifests, project, load_skip_list(args.skip_list))
        candidates, preflight_skipped = preflight_eligible(candidates, project)
        skipped.extend(preflight_skipped)
        if skipped:
            print(f"Skipped {len(skipped)} candidate unit(s):")
            for manifest, reason in skipped:
                try:
                    display_path = manifest.relative_to(project)
                except ValueError:
                    display_path = manifest
                print(f"  {display_path}: {reason}")
        if not candidates:
            print("No eligible candidate units remain.")
            return 0
        if args.apply:
            apply_candidates(candidates, project)
        function_count = sum(len(candidate.functions) for candidate in candidates)
        action = "Integrated" if args.apply else "Preflighted"
        print(f"{action} {len(candidates)} candidate unit(s), {function_count} function(s):")
        author_counts: dict[str, list[int]] = {}
        for candidate in candidates:
            counts = author_counts.setdefault(candidate.manifest.parent.name, [0, 0])
            counts[0] += 1
            counts[1] += len(candidate.functions)
        for author, (units, functions) in sorted(author_counts.items()):
            print(f"  {author}: {units} unit(s), {functions} function(s)")
        for candidate in candidates:
            print(f"  {candidate.manifest.name}: {candidate.module}, {len(candidate.functions)} function(s)"
                  + (f" -> {candidate.destination.relative_to(project)}" if args.apply else ""))
    except (CandidateError, OSError) as error:
        print(f"integrate_candidates.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
