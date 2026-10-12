#!/usr/bin/env python3
"""Audit ROM-local code pointers and observed oracle call targets, by module/state.

Reports and augmented manifests contain ROM-derived addresses and stay in build/.
Instruction-PC coverage is reported separately: range coverage is not entry coverage.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from recomp.dsd_manifest import (
    _CoverageIndex, _GapScanner, _effect, _read_code_sections, _load_yaml,
    _RELOC_RE, _module_ids, _number,
)

ENTRY_RE = re.compile(
    r'\{0x([0-9a-fA-F]+)u, \d+u, (-?\d+), ([01])u, '
    r'0x[0-9a-fA-F]+ull, &[A-Za-z0-9_:]+, "'
)
EntryKey = tuple[int, int, bool]


@dataclass
class Module:
    record: dict[str, Any]
    image: bytes
    ranges: list[tuple[int, int]]
    scanner: _GapScanner
    boundaries: dict[tuple[int, bool], dict[int, int]] = field(default_factory=dict)
    owner_cache: dict[tuple[int, bool], list[dict[str, Any]]] = field(default_factory=dict)

    @property
    def name(self) -> str:
        return str(self.record["name"])

    @property
    def module_id(self) -> int:
        return int(self.record["module_id"])

    @property
    def base(self) -> int:
        return int(self.record["base"])

    def contains(self, address: int) -> bool:
        return any(low <= address < high for low, high in self.ranges)

    def data(self, address: int) -> bool:
        return any(int(low) <= address < int(high) for low, high in self.record.get("data_ranges", []))

    def owners(self, address: int, thumb: bool) -> list[dict[str, Any]]:
        key = (address, thumb)
        if key not in self.owner_cache:
            self.owner_cache[key] = [fn for fn in self.record.get("functions", [])
                if bool(fn.get("thumb", False)) == thumb
                and int(fn["addr"]) <= address < int(fn["addr"]) + int(fn["size"])]
        return self.owner_cache[key]

    def fingerprint(self, address: int, length: int) -> bytes:
        offset = address - self.base
        return self.image[offset:offset + length] if offset >= 0 else b""


def generated_entries(lines: Iterable[str]) -> set[EntryKey]:
    return {(int(module), int(address, 16), bool(int(thumb)))
            for line in lines for address, module, thumb in ENTRY_RE.findall(line)}


def _config_records(path: Path) -> list[tuple[str, dict[str, Any]]]:
    config = _load_yaml(path)
    records = [("main", config["main_module"])]
    records += [(str(item["name"]).lower(), item) for item in config.get("autoloads", [])]
    records += [(str(item.get("name", f"ov{item['id']:03d}")), item)
                for item in config.get("overlays", [])]
    return records


def config_ranges(path: Path) -> dict[str, list[tuple[int, int]]]:
    return {name: _read_code_sections(path.parent / str(record.get("delinks", "")))
            for name, record in _config_records(path)}


def config_pointer_modules(path: Path) -> dict[str, dict[int, tuple[int, set[int]]]]:
    """Keep DSD's module provenance for absolute pointers, not only BL relocations."""
    references = {}
    for name, record in _config_records(path):
        relocations = path.parent / str(record.get("relocations", ""))
        if not relocations.is_file():
            continue
        targets = {}
        for line in relocations.read_text().splitlines():
            match = _RELOC_RE.fullmatch(line.strip())
            if match is None or match.group("kind") != "load":
                continue
            ids = _module_ids(match.group("module"))
            if ids is not None:
                targets[_number(match.group("from"))] = (_number(match.group("to")), set(ids))
        references[name] = targets
    return references


def load_modules(manifest: dict[str, Any], ranges: dict[str, list[tuple[int, int]]] | None = None) -> list[Module]:
    result = []
    for record in manifest["modules"]:
        image = Path(record["image"]).read_bytes()
        code = record.get("code_ranges")
        if code is None and ranges is not None:
            code = ranges.get(record["name"])
        if not code:
            # Protected plaintext has no independent DSD delinks file. ARM7's
            # old manifests likewise describe code only via discovered roots.
            code = [(int(fn["addr"]), int(fn["addr"]) + int(fn["size"]))
                    for fn in record.get("functions", [])]
        code = [(int(low), min(int(high), int(record["base"]) + len(image)))
                for low, high in code]
        scanner = _GapScanner(record, image, code, set(), code, set())
        result.append(Module(record, image, code, scanner))
    return result


def matching_code(module: Module, address: int, thumb: bool, sample: bytes) -> bool:
    """Compare live code only; bytes after a short function may be mutable data."""
    owners = module.owners(address, thumb)
    length = min([len(sample)] + [int(fn["addr"]) + int(fn["size"]) - address for fn in owners])
    if length < (2 if thumb else 4):
        return False
    return module.fingerprint(address, length) == sample[:length]


def literal_words(module: Module) -> set[int]:
    """Locate literal storage, not integer values that merely resemble pointers."""
    literals: set[int] = set()
    scanned: set[tuple[int, bool]] = set()
    for fn in module.record.get("functions", []):
        thumb = bool(fn.get("thumb", False))
        start = int(fn["addr"])
        end = min(start + int(fn["size"]), module.base + len(module.image))
        pos = start
        while pos < end:
            if pos in literals:
                pos += 4
                continue
            key = (pos, thumb)
            if key in scanned:
                pos += 2 if thumb else 4
                continue
            scanned.add(key)
            insn = module.scanner._instruction(pos, end, thumb)
            if insn is None:
                pos += 2 if thumb else 4
                continue
            effect = _effect(insn, thumb)
            if effect is not None and effect.literal is not None:
                address, width = effect.literal
                if width == 4 and address % 4 == 0:
                    literals.add(address)
            pos += insn.size
    return literals


def owner_boundaries(module: Module, fn: dict[str, Any]) -> dict[int, int]:
    """Decoded instruction extents, including both paths of local branches."""
    start, thumb = int(fn["addr"]), bool(fn.get("thumb", False))
    key = (start, thumb)
    if key in module.boundaries:
        return module.boundaries[key]
    end = min(start + int(fn["size"]), module.base + len(module.image))
    pending = [start]
    decoded: dict[int, int] = {}
    while pending:
        pos = pending.pop()
        if pos in decoded or not start <= pos < end or module.data(pos):
            continue
        insn = module.scanner._instruction(pos, end, thumb)
        role = _effect(insn, thumb) if insn is not None else None
        if role is None:
            continue
        decoded[pos] = pos + insn.size
        if role.flow in {"branch", "call"} and role.target is not None and start <= role.target < end:
            pending.append(role.target)
        if role.flow in {"branch", "exit"} and not role.conditional:
            continue
        pending.append(pos + insn.size)
    module.boundaries[key] = decoded
    return decoded


def plausible(module: Module, address: int, thumb: bool, *, observed: bool = False) -> tuple[bool, str, int]:
    """Validate a bounded body without decoding data or changing instruction state."""
    unit = 2 if thumb else 4
    if address % unit:
        return False, "misaligned_target", 0
    if not module.contains(address):
        return False, "outside_code", 0
    if module.data(address):
        return False, "target_data", 0
    owners = module.owners(address, thumb)
    other_owners = module.owners(address, not thumb)
    if other_owners and not owners:
        return False, "mode_mismatch", 0
    if len({int(fn["addr"]) + int(fn["size"]) for fn in owners}) > 1:
        return False, "ambiguous_owner", 0
    for fn in owners:
        boundaries = owner_boundaries(module, fn)
        if any(low < address < high for low, high in boundaries.items()):
            return False, "inside_instruction", 0
    section_end = min(high for low, high in module.ranges if low <= address < high)
    end = min([section_end] + [int(fn["addr"]) + int(fn["size"]) for fn in owners])
    first = module.scanner._instruction(address, end, thumb)
    effect = _effect(first, thumb) if first is not None else None
    if effect is None:
        return False, "invalid_instruction", 0
    if not observed and effect.conditional:
        return False, "conditional_start", 0
    if not observed and not module.scanner.looks_like_prologue(address, thumb):
        # A frame-less body is not proven to be a function just because a data
        # integer points into a valid instruction stream. Literal branch thunks
        # are admitted only when their destination is itself a known entry.
        known = {int(fn["addr"]) for fn in module.record.get("functions", [])
                 if bool(fn.get("thumb", False)) == thumb}
        if effect.flow != "branch" or effect.conditional or effect.target not in known:
            return False, "no_entry_prologue_or_thunk", 0
    pending = [address]
    visited: set[int] = set()
    literals: set[int] = set()
    high = address
    exits = 0
    while pending:
        pos = pending.pop()
        if pos in visited:
            continue
        if len(visited) >= 256:
            return False, "body_limit", 0
        if not address <= pos < end or pos % unit or module.data(pos) or pos in literals:
            return False, "branch_into_data_or_outside_owner", 0
        insn = module.scanner._instruction(pos, end, thumb)
        role = _effect(insn, thumb) if insn is not None else None
        if role is None:
            return False, "invalid_body", 0
        visited.add(pos)
        high = max(high, pos + insn.size)
        if role.literal is not None:
            low, width = role.literal
            if low < pos + insn.size and pos < low + width:
                return False, "literal_overlaps_instruction", 0
            literals.update(range(low, low + width, unit))
        if role.flow == "branch" and role.target is not None:
            if address <= role.target < end:
                pending.append(role.target)
            elif module.contains(role.target):
                exits += 1
            else:
                return False, "unknown_tail_target", 0
            if not role.conditional:
                continue
        elif role.flow == "exit":
            exits += 1
            if not role.conditional:
                continue
        pending.append(pos + insn.size)
    if visited & literals:
        return False, "literal_overlaps_instruction", 0
    return (True, "validated", high - address) if exits else (False, "no_exit", 0)


def audit_pointers(modules: list[Module], entries: set[EntryKey], *,
                   pointer_modules: dict[str, dict[int, tuple[int, set[int]]]] | None = None) -> dict[str, Any]:
    counts: Counter[str] = Counter()
    candidates: dict[EntryKey, dict[str, Any]] = {}
    ambiguous: dict[tuple[int, bool], dict[str, Any]] = {}
    low = min((start for module in modules for start, _ in module.ranges), default=0)
    high = max((end for module in modules for _, end in module.ranges), default=0)
    for source in modules:
        literals = literal_words(source)
        for offset in range(0, len(source.image) - 3, 4):
            value = struct.unpack_from("<I", source.image, offset)[0]
            address, thumb = value & ~1, bool(value & 1)
            if not low <= address < high:
                continue
            targets = [module for module in modules if module.contains(address)]
            if not targets:
                continue
            counts["pointer_occurrences"] += 1
            if address % (2 if thumb else 4):
                counts["misaligned_target"] += 1
                continue
            source_address = source.base + offset
            evidence = "literal" if source_address in literals else "data" if source.data(source_address) else "untyped_word"
            reference = (pointer_modules or {}).get(source.name, {}).get(source_address)
            matched_reference = reference is not None and (reference[0] & ~1) == address
            if matched_reference:
                targets = [module for module in targets if module.module_id in reference[1]]
                if not targets:
                    counts["relocation_target_not_code"] += 1
                    continue
            target = next((module for module in targets if module is source), None)
            if target is None and matched_reference and len(targets) == 1:
                target = targets[0]
            resident = [module for module in targets if module.module_id < 0]
            if target is None and len(resident) == 1:
                target = resident[0]
            if target is None:
                # Raw addresses never identify an overlay in a reused region.
                counts["ambiguous_overlay_source"] += 1
                unresolved = ambiguous.setdefault((address, thumb), {
                    "addr": address, "thumb": thumb, "sources": [], "possible_modules": []})
                unresolved["sources"].append({"module": source.name, "addr": source_address,
                                              "evidence": evidence})
                if not unresolved["possible_modules"]:
                    for possible in targets:
                        known = (possible.module_id, address, thumb) in entries
                        valid, reason, size = (True, "existing_entry", 0) if known else plausible(
                            possible, address, thumb)
                        unresolved["possible_modules"].append({
                            "module": possible.name, "module_id": possible.module_id,
                            "already_entry": known, "plausible_missing_entry": valid and not known,
                            "reason": reason, "size": size})
                continue
            key = (target.module_id, address, thumb)
            if key in entries:
                counts["existing_entry"] += 1
                continue
            valid, reason, size = plausible(target, address, thumb)
            if evidence == "untyped_word":
                valid, reason = False, "source_not_data_or_literal"
            counts["accepted" if valid else reason] += 1
            record = candidates.setdefault(key, {"module": target.name, "module_id": target.module_id,
                "addr": address, "thumb": thumb, "accepted": False, "size": size, "sources": []})
            record["accepted"] |= valid
            record["sources"].append({"module": source.name, "addr": source_address,
                                      "evidence": evidence, "reason": reason})
    records = sorted(candidates.values(), key=lambda record: (
        not record["accepted"], not any(s["evidence"] == "literal" for s in record["sources"]),
        -len(record["sources"]), record["module_id"], record["addr"], record["thumb"]))
    unresolved = sorted(ambiguous.values(), key=lambda record: (
        not any(module["plausible_missing_entry"] for module in record["possible_modules"]),
        not any(source["evidence"] == "literal" for source in record["sources"]),
        -len(record["sources"]), record["addr"], record["thumb"]))
    return {"counts": dict(counts), "unique_missing_candidates": len(records),
            "candidates": records, "ambiguous_overlay_candidates": unresolved}


def load_call_targets(lines: Iterable[str]) -> list[dict[str, Any]]:
    targets: dict[tuple[int, int, bool, bytes], dict[str, Any]] = {}
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 6 or fields[0] not in {"0", "1"} or fields[2] not in {"a", "t"}:
            raise ValueError(f"call-target row {number}: expected 'cpu addr a|t bytes32 source frame'")
        cpu, address, thumb = int(fields[0]), int(fields[1], 16), fields[2] == "t"
        sample = bytes.fromhex(fields[3])
        if len(sample) != 32:
            raise ValueError(f"call-target row {number}: fingerprint must be 32 bytes")
        key = (cpu, address, thumb, sample)
        targets.setdefault(key, {"cpu": cpu, "addr": address, "thumb": thumb,
                                "sample": sample, "source": int(fields[4], 16), "frame": int(fields[5])})
    return list(targets.values())


def audit_calls(modules: list[Module], entries: set[EntryKey], calls: list[dict[str, Any]], cpu: int) -> dict[str, Any]:
    records: dict[tuple[int, bool, tuple[int, ...]], dict[str, Any]] = {}
    bios = 0
    for call in calls:
        if call["cpu"] != cpu:
            continue
        address, thumb = call["addr"], call["thumb"]
        if (cpu == 0 and address >= 0xffff0000) or (cpu == 1 and address < 0x4000):
            bios += 1
            continue
        matches = [module for module in modules if module.contains(address)
                   and matching_code(module, address, thumb, call["sample"])]
        key = (address, thumb, tuple(sorted(module.module_id for module in matches)))
        record = records.setdefault(key, {"addr": address, "thumb": thumb,
            "modules": [module.name for module in matches], "module_ids": list(key[2]),
            "source": call["source"], "frame": call["frame"], "covered": bool(matches), "holes": []})
        for module in matches:
            if (module.module_id, address, thumb) in entries:
                continue
            record["covered"] = False
            valid, reason, size = plausible(module, address, thumb, observed=True)
            hole = {"module": module.name, "module_id": module.module_id, "addr": address,
                    "thumb": thumb, "accepted": valid, "reason": reason, "size": size}
            if hole not in record["holes"]:
                record["holes"].append(hole)
    records_list = sorted(records.values(), key=lambda record: (record["addr"], record["thumb"], record["module_ids"]))
    resolved = [record for record in records_list if record["modules"]]
    covered = sum(record["covered"] for record in resolved)
    return {"observed_targets": len(records_list), "resolved_targets": len(resolved),
            "covered_targets": covered, "coverage_percent": 100 * covered / len(records_list) if records_list else None,
            "resolved_coverage_percent": 100 * covered / len(resolved) if resolved else None,
            "unresolved_targets": len(records_list) - len(resolved), "bios_samples_ignored": bios,
            "targets": records_list}


def apply_entries(manifest: dict[str, Any], additions: list[dict[str, Any]]) -> list[EntryKey]:
    modules = {int(module["module_id"]): module for module in manifest["modules"]}
    applied = []
    for record in additions:
        if not record.get("accepted", False):
            continue
        module_id, address, thumb = int(record["module_id"]), int(record["addr"]), bool(record["thumb"])
        module = modules[module_id]
        if any(int(fn["addr"]) == address and bool(fn.get("thumb", False)) == thumb
               for fn in module["functions"]):
            continue
        owners = [fn for fn in module["functions"] if bool(fn.get("thumb", False)) == thumb
                  and int(fn["addr"]) < address < int(fn["addr"]) + int(fn["size"])]
        if owners:
            entry = {"addr": address, "thumb": thumb}
            points = module.setdefault("entry_points", [])
            if entry in points:
                continue
            points.append(entry)
            points.sort(key=lambda point: (int(point["addr"]), bool(point["thumb"])))
        else:
            size = int(record["size"])
            if size <= 0:
                raise ValueError(f"validated entry 0x{address:08x} has no decoded body")
            module["functions"].append({"addr": address, "thumb": thumb, "size": size,
                                        "name": f"audit_{address:08x}"})
            module["functions"].sort(key=lambda fn: (int(fn["addr"]), bool(fn.get("thumb", False))))
        applied.append((module_id, address, thumb))
    return applied


def pc_coverage(modules: list[Module], lines: Iterable[str]) -> dict[str, int]:
    pcs = set()
    for number, line in enumerate(lines, 1):
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 2 or fields[1] not in {"a", "t"}:
            raise ValueError(f"PC row {number}: expected 'addr a|t'")
        pcs.add((int(fields[0], 16), fields[1] == "t"))
    index = _CoverageIndex([module.record for module in modules])
    covered = sum(index.covers(address, thumb) for address, thumb in pcs)
    return {"executed_pcs": len(pcs), "range_covered": covered, "outside_ranges": len(pcs) - covered}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm9-manifest", type=Path, default=ROOT / "build/gen/arm9.manifest.json")
    parser.add_argument("--arm7-manifest", type=Path, default=ROOT / "build/gen/arm7.manifest.json")
    parser.add_argument("--generated-dir", type=Path, default=ROOT / "build/gen")
    parser.add_argument("--arm9-config", type=Path, default=ROOT / "extracted/dqix-decomp-eu/config/eur/arm9/config.yaml")
    parser.add_argument("--call-targets", type=Path, action="append", default=[])
    parser.add_argument("--pcs-arm9", type=Path, action="append", default=[])
    parser.add_argument("--pcs-arm7", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, default=ROOT / "build/static-entry-audit")
    parser.add_argument("--apply-dir", type=Path, help="write augmented manifests to this separate directory")
    args = parser.parse_args(argv)
    report = {}
    calls = [row for path in args.call_targets for row in load_call_targets(path.read_text().splitlines())]
    for cpu, number, manifest_path in (("arm9", 0, args.arm9_manifest), ("arm7", 1, args.arm7_manifest)):
        manifest = json.loads(manifest_path.read_text())
        ranges = config_ranges(args.arm9_config) if cpu == "arm9" else None
        modules = load_modules(manifest, ranges)
        entries = generated_entries((args.generated_dir / cpu / "fn_table.cpp").read_text().splitlines())
        references = config_pointer_modules(args.arm9_config) if cpu == "arm9" else None
        pointers = audit_pointers(modules, entries, pointer_modules=references)
        observed = audit_calls(modules, entries, calls, number)
        additions = [record for record in pointers["candidates"] if record["accepted"]]
        additions += [hole for record in observed["targets"] for hole in record["holes"] if hole["accepted"]]
        applied = apply_entries(manifest, additions) if args.apply_dir is not None else []
        paths = args.pcs_arm9 if cpu == "arm9" else args.pcs_arm7
        pcs = pc_coverage(modules, (line for path in paths for line in path.read_text().splitlines())) if paths else None
        report[cpu] = {"modules": len(modules), "entries_before": len(entries), "pointers": pointers,
                       "calls": observed, "pcs": pcs, "added_entries": applied}
        if args.apply_dir is not None:
            args.apply_dir.mkdir(parents=True, exist_ok=True)
            (args.apply_dir / f"{cpu}.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"{cpu}: {len(entries)} entries; {pointers['counts']}; "
              f"call targets {observed['covered_targets']}/{observed['resolved_targets']} covered, "
              f"{observed['unresolved_targets']} unresolved; added {len(applied)}")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"static entry audit: {error}", file=sys.stderr)
        raise SystemExit(2)
