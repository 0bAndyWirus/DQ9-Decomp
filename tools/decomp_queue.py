#!/usr/bin/env python3
"""Build a prioritized queue of unmatched ARM9 functions from an Objdiff report."""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import os
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Mapping, Sequence

import yaml

if __package__:
    from .port_eu_unit import GAP_ANCHOR_PREFIX, SymbolEntry, TargetModule, TargetReloc, parse_delinks_header, parse_relocs, parse_symbols
else:
    from port_eu_unit import GAP_ANCHOR_PREFIX, SymbolEntry, TargetModule, TargetReloc, parse_delinks_header, parse_relocs, parse_symbols

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "workspace"
CALL_KINDS = frozenset({"arm_call", "arm_call_thumb", "thumb_call", "thumb_call_arm"})
GAP_UNIT = re.compile(r"^_dsd_gap@(?P<module>.+)_(?P<index>\d+)$")
ADDRESS_SUFFIX = re.compile(r"(?:^|_)(?:0x)?(?P<address>[0-9a-fA-F]{8})$")
OVERLAY_REF = re.compile(r"^overlays?\((?P<ids>[^()]*)\)$")
SDK_PREFIXES = (
    "OS_", "MI_", "GXi", "GX3", "GX", "G2", "G3", "FX_", "MATH_", "FS_", "CARD_", "PXI_", "RTC_",
    "TP_", "PM_", "WM_", "WFS_", "SND_", "DGT_", "CT_", "DSP_", "NNS_", "DC_", "MB_", "WVR_", "MIC_",
)
FLOAT_HELPER = re.compile(
    r"(?i)(?:__aeabi_[fd][A-Za-z0-9_]*|softfp|^_[fd](?:add|sub|mul|div|eq|neq|gr|geq|leq|ls|lt|fixu?|flt|fltu?|2f|2d)(?:_|$)|(?:^|_)(?:float|double)(?:_|$))"
)


def _int_value(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            return int(text, 16 if text.lower().startswith("0x") else 10)
        except ValueError:
            return None
    return None


def _normalize_module_name(value: str) -> str | None:
    value = value.strip()
    if value in {"main", "itcm", "dtcm"}:
        return value
    match = re.fullmatch(r"ov(\d+)", value, flags=re.IGNORECASE)
    if match:
        return f"ov{int(match.group(1)):03d}"
    match = re.fullmatch(r"overlay\((\d+)\)", value, flags=re.IGNORECASE)
    if match:
        return f"ov{int(match.group(1)):03d}"
    return None


def _module_candidates(module_ref: Any) -> tuple[str, ...]:
    if not isinstance(module_ref, str):
        return ()
    value = module_ref.strip()
    direct = _normalize_module_name(value)
    if direct:
        return (direct,)
    match = OVERLAY_REF.fullmatch(value)
    if not match:
        return ()
    module_ids = []
    for item in match["ids"].split(","):
        item = item.strip()
        if not item.isdecimal():
            return ()
        module_ids.append(f"ov{int(item):03d}")
    return tuple(dict.fromkeys(module_ids))


def _gap_module(unit_name: str) -> tuple[str | None, str | None]:
    match = GAP_UNIT.fullmatch(unit_name)
    if match is None:
        return None, None
    modules = _module_candidates(match["module"])
    return (modules[0] if len(modules) == 1 else None), unit_name


def _normalized_address(address: int) -> int:
    return address & ~1


def _report_size(function: Mapping[str, Any]) -> int | None:
    size = _int_value(function.get("size"))
    return max(size, 0) if size is not None else None


def _is_matched(function: Mapping[str, Any]) -> bool:
    value = function.get("fuzzy_match_percent")
    try:
        return value is not None and float(value) == 100.0
    except (TypeError, ValueError):
        return False


def _symbol_sort_key(symbol: SymbolEntry) -> tuple[int, str]:
    return (1 if symbol.local else 0, symbol.name)


def _function_symbols(module: TargetModule) -> dict[int, list[SymbolEntry]]:
    result: dict[int, list[SymbolEntry]] = defaultdict(list)
    for symbol in module.symbols:
        if symbol.category == "function":
            result[_normalized_address(symbol.address)].append(symbol)
    for symbols in result.values():
        symbols.sort(key=_symbol_sort_key)
    return dict(result)


def _resolve_symbol(
    function_name: str,
    module_hint: str | None,
    by_name: Mapping[str, list[tuple[str, int, SymbolEntry]]],
    by_address: Mapping[tuple[str, int], list[SymbolEntry]],
) -> dict[str, Any]:
    matches = list(by_name.get(function_name, ()))
    if module_hint is not None:
        matches = [match for match in matches if match[0] == module_hint]

    if not matches:
        suffix = ADDRESS_SUFFIX.search(function_name)
        if suffix is not None:
            address = int(suffix["address"], 16) & ~1
            if module_hint is not None:
                matches = [
                    (module_hint, address, symbol)
                    for symbol in by_address.get((module_hint, address), ())
                ]
            else:
                matches = [
                    (module, address, symbol)
                    for (module, symbol_address), symbols in by_address.items()
                    if symbol_address == address
                    for symbol in symbols
                ]

    identities = sorted({(module, address) for module, address, _symbol in matches})
    if len(identities) == 1:
        module, address = identities[0]
        same_identity = [symbol for item_module, item_address, symbol in matches
                         if (item_module, item_address) == (module, address)]
        chosen = next((symbol for symbol in same_identity if symbol.name == function_name),
                      min(same_identity, key=_symbol_sort_key))
        return {
            "module": module,
            "module_candidates": [module],
            "address": address,
            "symbol": chosen,
            "symbol_candidates": [
                {"module": module, "name": symbol.name, "address": address}
                for symbol in same_identity
            ],
        }

    if identities:
        return {
            "module": module_hint,
            "module_candidates": sorted({module for module, _address in identities}),
            "address": None,
            "symbol": None,
            "symbol_candidates": [
                {"module": module, "name": symbol.name, "address": address}
                for module, address, symbol in sorted(
                    matches, key=lambda item: (item[0], item[1], item[2].name)
                )
            ],
        }

    candidates = [module_hint] if module_hint else []
    return {
        "module": module_hint,
        "module_candidates": candidates,
        "address": None,
        "symbol": None,
        "symbol_candidates": [],
    }


def _module_name_index(
    modules: Mapping[str, TargetModule],
) -> tuple[dict[str, list[tuple[str, int, SymbolEntry]]], dict[tuple[str, int], list[SymbolEntry]]]:
    by_name: dict[str, list[tuple[str, int, SymbolEntry]]] = defaultdict(list)
    by_address: dict[tuple[str, int], list[SymbolEntry]] = {}
    for module_name, module in modules.items():
        for address, symbols in _function_symbols(module).items():
            by_address[(module_name, address)] = symbols
            for symbol in symbols:
                by_name[symbol.name].append((module_name, address, symbol))
    return dict(by_name), by_address


def _preferred_function(symbols: Sequence[SymbolEntry]) -> SymbolEntry:
    return min(symbols, key=_symbol_sort_key)


def _function_ranges(module: TargetModule) -> tuple[list[int], dict[int, tuple[int, SymbolEntry]]]:
    by_address = _function_symbols(module)
    starts = sorted(by_address)
    ranges: dict[int, tuple[int, SymbolEntry]] = {}
    for index, start in enumerate(starts):
        next_start = starts[index + 1] if index + 1 < len(starts) else None
        sizes = [symbol.size for symbol in by_address[start] if symbol.size is not None and symbol.size > 0]
        if sizes:
            end = start + max(sizes)
            if next_start is not None:
                end = min(end, next_start)
        elif next_start is not None:
            end = next_start
        else:
            end = next((section_end for section_start, section_end in module.sections.values()
                        if section_start <= start < section_end), start + 1)
        ranges[start] = (max(end, start + 1), _preferred_function(by_address[start]))
    return starts, ranges


def _owner_at(
    address: int,
    starts_by_module: Mapping[str, list[int]],
    ranges_by_module: Mapping[str, dict[int, tuple[int, SymbolEntry]]],
    module_name: str,
) -> tuple[int, SymbolEntry] | None:
    starts = starts_by_module[module_name]
    index = bisect.bisect_right(starts, _normalized_address(address)) - 1
    if index < 0:
        return None
    start = starts[index]
    end, symbol = ranges_by_module[module_name][start]
    if _normalized_address(address) < end:
        return start, symbol
    return None


def _relocation_graph(modules: Mapping[str, TargetModule]) -> dict[str, Any]:
    by_address = {name: _function_symbols(module) for name, module in modules.items()}
    starts_by_module: dict[str, list[int]] = {}
    ranges_by_module: dict[str, dict[int, tuple[int, SymbolEntry]]] = {}
    for name, module in modules.items():
        starts, ranges = _function_ranges(module)
        starts_by_module[name] = starts
        ranges_by_module[name] = ranges

    outgoing: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)
    branches_by_function: dict[tuple[str, int], list[TargetReloc]] = defaultdict(list)
    incoming: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)
    loads_by_function: dict[tuple[str, int], list[TargetReloc]] = defaultdict(list)

    for source_module, module in modules.items():
        for relocation in module.relocs.values():
            source_owner = _owner_at(relocation.source, starts_by_module, ranges_by_module, source_module)
            if source_owner is None:
                continue
            source_address, source_symbol = source_owner
            identity = (source_module, source_address)
            if relocation.kind in CALL_KINDS:
                target_address = _normalized_address(relocation.effective_target)
                target_modules = _module_candidates(relocation.module)
                if not target_modules:
                    target_modules = tuple(sorted(
                        candidate_module for candidate_module, symbols in by_address.items()
                        if target_address in symbols
                    ))
                edge = {
                    "source_module": source_module,
                    "source_address": source_address,
                    "source_symbol": source_symbol,
                    "call_site": _normalized_address(relocation.source),
                    "target_address": target_address,
                    "target_modules": target_modules,
                    "module_ref": relocation.module,
                }
                outgoing[identity].append(edge)
                for target_module in target_modules:
                    if target_address in by_address.get(target_module, {}):
                        incoming[(target_module, target_address)].append(edge)
            elif relocation.kind == "load":
                loads_by_function[identity].append(relocation)
            elif relocation.kind in {"arm_branch", "thumb_branch"}:
                branches_by_function[identity].append(relocation)

    return {
        "outgoing": outgoing,
        "incoming": incoming,
        "branches": branches_by_function,
        "loads": loads_by_function,
        "by_address": by_address,
        "starts_by_module": starts_by_module,
        "ranges_by_module": ranges_by_module,
    }


def _target_symbols(edge: Mapping[str, Any], graph: Mapping[str, Any]) -> list[dict[str, Any]]:
    resolved = []
    target_address = edge["target_address"]
    for module_name in edge["target_modules"]:
        symbols = graph["by_address"].get(module_name, {}).get(target_address, ())
        if symbols:
            resolved.append({
                "module": module_name,
                "name": _preferred_function(symbols).name,
                "aliases": sorted({symbol.name for symbol in symbols}),
                "address": target_address,
            })
    return resolved


def _edge_groups(edges: Sequence[Mapping[str, Any]], graph: Mapping[str, Any]) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, tuple[str, ...]], list[Mapping[str, Any]]] = defaultdict(list)
    for edge in edges:
        grouped[(edge["target_address"], tuple(edge["target_modules"]))].append(edge)
    result = []
    for (address, target_modules), calls in sorted(grouped.items()):
        first = calls[0]
        target_edge = {"target_address": address, "target_modules": target_modules}
        symbols = _target_symbols(target_edge, graph)
        result.append({
            "module": target_modules[0] if len(target_modules) == 1 else None,
            "module_candidates": list(target_modules),
            "module_ref": first.get("module_ref"),
            "address": address,
            "address_hex": f"0x{address:08x}",
            "symbol_name": symbols[0]["name"] if len(symbols) == 1 else None,
            "resolved_symbols": symbols,
            "call_count": len(calls),
            "call_sites": sorted({call["call_site"] for call in calls}),
        })
    return result


def _caller_groups(edges: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, int, tuple[str, ...]], list[Mapping[str, Any]]] = defaultdict(list)
    for edge in edges:
        grouped[(edge["source_module"], edge["source_address"], tuple(edge["target_modules"]))].append(edge)
    result = []
    for (module_name, address, target_modules), calls in sorted(grouped.items()):
        symbol = calls[0]["source_symbol"]
        result.append({
            "module": module_name,
            "module_candidates": [module_name],
            "symbol_name": symbol.name,
            "address": address,
            "address_hex": f"0x{address:08x}",
            "target_module_candidates": list(target_modules),
            "call_count": len(calls),
            "call_sites": sorted({call["call_site"] for call in calls}),
        })
    return result


def _hot_path_markers(
    event_catalog: Mapping[str, Any] | None,
    script_table_catalog: Mapping[str, Any] | None,
) -> dict[tuple[str, int], list[dict[str, Any]]]:
    markers: dict[tuple[str, int], dict[tuple[Any, ...], dict[str, Any]]] = defaultdict(dict)
    dispatch = (event_catalog or {}).get("secondary_dispatch_catalog", {})
    if isinstance(dispatch, Mapping):
        for entry in dispatch.get("entries", ()):
            if not isinstance(entry, Mapping) or entry.get("callable_by_vm") is not True:
                continue
            address = _int_value(entry.get("handler_address"))
            if address is None:
                continue
            module_names = _module_candidates(entry.get("module", dispatch.get("module")))
            if not module_names:
                continue
            marker = {
                "kind": "event_secondary_callback",
                "address": _normalized_address(address),
                "callback_id": entry.get("callback_id"),
            }
            key = (marker["kind"], marker["address"], marker["callback_id"])
            for module_name in module_names:
                markers[(module_name, marker["address"])][key] = marker

    review = (script_table_catalog or {}).get("dispatch_review", {})
    story = review.get("scenario_story_handler_disassembly", {}) if isinstance(review, Mapping) else {}
    if isinstance(story, Mapping):
        table_modules = _module_candidates(story.get("table_module"))
        for handler in story.get("handlers", ()):
            if not isinstance(handler, Mapping):
                continue
            address = _int_value(handler.get("address"))
            if address is None:
                continue
            normalized = _normalized_address(address)
            marker = {
                "kind": "scenario_story_handler",
                "address": normalized,
                "opcode": handler.get("opcode"),
            }
            key = (marker["kind"], marker["address"], marker["opcode"])
            for module_name in table_modules:
                markers[(module_name, normalized)][key] = marker

    return {
        identity: sorted(values.values(), key=lambda marker: (marker["kind"], marker["address"], str(marker.get("callback_id", marker.get("opcode", "")))))
        for identity, values in markers.items()
    }


def _sdk_classification(symbol_name: str) -> dict[str, Any]:
    prefix = next((candidate for candidate in SDK_PREFIXES if symbol_name.startswith(candidate)), None)
    return {
        "classified": prefix is not None,
        "classification": "nitrosdk_api_prefix" if prefix else None,
        "prefix": prefix,
        "basis": "symbol-name prefix only",
    }


def _difficulty(
    size: int | None,
    call_count: int,
    float_evidence: Sequence[str],
    rodata_load_count: int,
    branch_count: int,
) -> dict[str, Any]:
    size_score = min(size or 0, 1024) / 64.0
    call_score = min(call_count, 24) / 2.0
    float_detected = bool(float_evidence)
    jump_table_likely = rodata_load_count >= 3 or branch_count >= 4
    float_score = 3.0 if float_detected else 0.0
    jump_score = 4.0 if jump_table_likely else 0.0
    factors = {
        "size": {"bytes": size, "score": round(size_score, 3)},
        "call_count": {"count": call_count, "score": round(call_score, 3)},
        "float_softfp": {
            "detected": float_detected,
            "evidence_symbols": sorted(set(float_evidence)),
            "score": float_score,
        },
        "jump_table_heuristic": {
            "likely": jump_table_likely,
            "rodata_load_references": rodata_load_count,
            "branch_references": branch_count,
            "threshold": "at least 3 rodata loads or 4 branch relocations in the function",
            "score": jump_score,
        },
    }
    score = sum(factor["score"] for factor in factors.values())
    return {"score": round(score, 3), "factors": factors}


def _module_source_counts(
    unit_count: int,
    function_count: int,
    matched_count: int,
    unmatched_count: int,
    report: Mapping[str, Any],
    report_path: str | Path | None,
) -> dict[str, Any]:
    source = {
        "path": str(report_path) if report_path is not None else None,
        "objdiff_version": report.get("version"),
        "unit_count": unit_count,
        "function_count": function_count,
        "matched_function_count": matched_count,
        "unmatched_function_count": unmatched_count,
    }
    return source


def _display_function_ref(module_name: str | None, address: int | None) -> tuple[str, int]:
    return (module_name or "", address if address is not None else 0xFFFFFFFF)


def build_queue(
    report: Mapping[str, Any],
    modules: Mapping[str, TargetModule],
    event_catalog: Mapping[str, Any] | None = None,
    script_table_catalog: Mapping[str, Any] | None = None,
    *,
    report_path: str | Path | None = None,
    event_catalog_path: str | Path | None = None,
    script_table_catalog_path: str | Path | None = None,
) -> dict[str, Any]:
    """Build a deterministic queue from parsed report, DSD modules, and hot-path catalogs."""
    by_name, by_address = _module_name_index(modules)
    graph = _relocation_graph(modules)
    hot_paths = _hot_path_markers(event_catalog, script_table_catalog)
    candidates: list[dict[str, Any]] = []
    total_functions = matched_count = 0

    for unit in report.get("units", ()):
        if not isinstance(unit, Mapping):
            continue
        unit_name = str(unit.get("name", ""))
        unit_module, gap_object = _gap_module(unit_name)
        metadata = unit.get("metadata", {})
        metadata = metadata if isinstance(metadata, Mapping) else {}
        object_identity = metadata.get("source_path") or unit_name
        for function in unit.get("functions", ()):
            if not isinstance(function, Mapping):
                continue
            function_name = str(function.get("name", ""))
            if function_name.startswith(GAP_ANCHOR_PREFIX):
                continue
            total_functions += 1
            if _is_matched(function):
                matched_count += 1
                continue

            resolution = _resolve_symbol(function_name, unit_module, by_name, by_address)
            module_name = resolution["module"]
            address = resolution["address"]
            identity = (module_name, address) if module_name is not None and address is not None else None
            outgoing = graph["outgoing"].get(identity, ()) if identity is not None else ()
            incoming = graph["incoming"].get(identity, ()) if identity is not None else ()
            callees = _edge_groups(outgoing, graph)
            callers = _caller_groups(incoming)
            call_count = len(outgoing)

            resolved_name = resolution["symbol"].name if resolution["symbol"] is not None else function_name
            sdk = _sdk_classification(resolved_name)
            size = _report_size(function)
            float_evidence = []
            for name in [resolved_name, *(symbol["name"] for edge in outgoing for symbol in _target_symbols(edge, graph))]:
                if FLOAT_HELPER.search(name):
                    float_evidence.append(name)

            load_relocations = graph["loads"].get(identity, ()) if identity is not None else ()
            rodata_load_count = 0
            branch_count = 0
            if module_name is not None and module_name in modules:
                module = modules[module_name]
                for relocation in load_relocations:
                    target_modules = _module_candidates(relocation.module)
                    if (not target_modules or module_name in target_modules) and module.section_of(
                        _normalized_address(relocation.effective_target)
                    ) == ".rodata":
                        rodata_load_count += 1
                branch_count = len(graph["branches"].get(identity, ())) if identity is not None else 0

            difficulty = _difficulty(size, call_count, float_evidence, rodata_load_count, branch_count)
            markers = hot_paths.get(identity, ()) if identity is not None else ()
            candidates.append({
                "function_name": function_name,
                "symbol_name": resolution["symbol"].name if resolution["symbol"] is not None else None,
                "symbol_candidates": resolution["symbol_candidates"],
                "address": address,
                "address_hex": f"0x{address:08x}" if address is not None else None,
                "module": module_name,
                "module_candidates": resolution["module_candidates"],
                "size": size,
                "gap_object": gap_object,
                "object_identity": object_identity,
                "unit": unit_name,
                "fuzzy_match_percent": function.get("fuzzy_match_percent"),
                "call_count": call_count,
                "call_graph_resolved": identity is not None,
                "callers": callers,
                "callees": callees,
                "is_leaf": not callees,
                "hot_path": {"is_hot": bool(markers), "markers": list(markers)},
                "sdk_classification": sdk,
                "difficulty": difficulty,
                "priority_keys": {},
            })

    candidates.sort(key=lambda candidate: (
        0 if candidate["hot_path"]["is_hot"] else 1,
        0 if candidate["sdk_classification"]["classified"] else 1,
        0 if candidate["is_leaf"] else 1,
        candidate["size"] if candidate["size"] is not None else math.inf,
        candidate["difficulty"]["score"],
        *_display_function_ref(candidate["module"], candidate["address"]),
        candidate["unit"],
        candidate["function_name"],
    ))

    for rank, candidate in enumerate(candidates, start=1):
        candidate["priority_rank"] = rank
        candidate["priority_keys"] = {
            "hot_path_first": 0 if candidate["hot_path"]["is_hot"] else 1,
            "sdk_first": 0 if candidate["sdk_classification"]["classified"] else 1,
            "leaf_first": 0 if candidate["is_leaf"] else 1,
            "size_bytes_ascending": candidate["size"],
            "difficulty_ascending": candidate["difficulty"]["score"],
        }

    matched_count = min(matched_count, total_functions)
    summary = _module_source_counts(
        len(report.get("units", ())), total_functions, matched_count,
        total_functions - matched_count, report, report_path,
    )
    return {
        "schema": "dq9-decomp/decomp-queue",
        "schema_version": 2,
        "report_source": summary,
        "catalog_sources": {
            "event_scripts": str(event_catalog_path) if event_catalog_path is not None else None,
            "script_tables": str(script_table_catalog_path) if script_table_catalog_path is not None else None,
        },
        "prioritization": {
            "sort_keys": [
                "hot_path (true first)", "sdk_classified (true first)", "leaf (true first)",
                "size_bytes (ascending)", "difficulty.score (ascending)", "module, address, unit, function_name (ascending)",
            ],
            "difficulty_formula": "size points + call-count points + float/softfp points + jump-table heuristic points",
        },
        "queue": candidates,
    }


def load_modules(config_path: Path) -> dict[str, TargetModule]:
    """Load DSD module symbols/ranges/relocations; absent relocation files are empty."""
    config_path = Path(config_path)
    config = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    base_dir = config_path.parent
    entries = [config["main_module"], *config.get("autoloads", ()), *config.get("overlays", ())]
    modules: dict[str, TargetModule] = {}
    for entry in entries:
        name = entry["name"]
        delinks_path = base_dir / entry["delinks"]
        symbols_path = base_dir / entry["symbols"]
        relocs_path = base_dir / entry["relocations"]
        modules[name] = TargetModule(
            name=name,
            data=b"",
            delinks_path=delinks_path,
            symbols_path=symbols_path,
            relocs_path=relocs_path,
            sections=parse_delinks_header(delinks_path),
            symbols=parse_symbols(symbols_path),
            relocs=parse_relocs(relocs_path) if relocs_path.exists() else {},
        )
    return modules


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _path_summary(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path.resolve())


def _fingerprint_file(path: Path, root: Path) -> dict[str, Any]:
    path = path.resolve()
    summary_path = Path(os.path.relpath(path, root.resolve())).as_posix()
    try:
        stat = path.stat()
    except FileNotFoundError:
        return {"path": summary_path, "size": None, "mtime_ns": None, "sha256": None}
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "path": summary_path,
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": digest.hexdigest(),
    }


def _input_paths(report_path: Path, event_path: Path | None, table_path: Path | None, config_path: Path) -> list[Path]:
    config = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    paths = [report_path, config_path]
    paths.extend(path for path in (event_path, table_path) if path is not None)
    for entry in [config["main_module"], *config.get("autoloads", ()), *config.get("overlays", ())]:
        for key in ("delinks", "symbols", "relocations"):
            paths.append(config_path.parent / entry[key])
    unique = {}
    for path in paths:
        resolved = path.resolve()
        unique[str(resolved)] = resolved
    return sorted(unique.values(), key=lambda path: _path_summary(path, ROOT))


def _fingerprint_inputs(report_path: Path, event_path: Path | None, table_path: Path | None,
                        config_path: Path, root: Path) -> list[dict[str, Any]]:
    return [_fingerprint_file(path, root) for path in _input_paths(report_path, event_path, table_path, config_path)]


def _content(item: Any) -> tuple[Any, Any] | None:
    """What identifies an input's content; mtime is excluded because a rewrite with identical bytes changes it."""
    return (item.get("size"), item.get("sha256")) if isinstance(item, Mapping) else None


def _changed_inputs(stored: Any, current: list[dict[str, Any]]) -> list[str]:
    previous = {item.get("path"): item for item in stored if isinstance(item, Mapping)} if isinstance(stored, list) else {}
    now = {item["path"]: item for item in current}
    return sorted(path for path in set(previous) | set(now) if _content(previous.get(path)) != _content(now.get(path)))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT,
                        help="DSD workspace (default: workspace/)")
    parser.add_argument("--config", type=Path, help="DSD ARM9 config YAML (default: <project>/config/eur/arm9/config.yaml)")
    parser.add_argument("--report", type=Path, help="Objdiff v2 report (default: <project>/build/eur/report.json)")
    parser.add_argument("--event-catalog", type=Path, help="optional caller-supplied dispatch catalog JSON")
    parser.add_argument("--script-table-catalog", "--script-tables", dest="script_table_catalog", type=Path,
                        help="optional caller-supplied handler catalog JSON")
    parser.add_argument("--output", type=Path, help="queue JSON output (default: build/decomp-queue.json)")
    parser.add_argument("--check", action="store_true", help="compare current inputs with the existing queue fingerprint without regenerating")
    args = parser.parse_args(argv)

    project = args.project
    config_path = args.config or project / "config" / "eur" / "arm9" / "config.yaml"
    report_path = args.report or project / "build" / "eur" / "report.json"
    event_path = args.event_catalog
    table_path = args.script_table_catalog
    output_path = args.output or ROOT / "build" / "decomp-queue.json"
    if args.check:
        existing = _read_json(output_path)
        try:
            input_paths = _input_paths(report_path, event_path, table_path, config_path)
        except (FileNotFoundError, yaml.YAMLError, KeyError, TypeError) as error:
            missing = Path(getattr(error, "filename", None) or config_path)
            print("Changed queue inputs: " + Path(os.path.relpath(missing, project.resolve())).as_posix())
            return 1
        fingerprints = [_fingerprint_file(path, project) for path in input_paths]
        changed = _changed_inputs(existing.get("inputs", {}).get("files"), fingerprints)
        if changed:
            print("Changed queue inputs: " + ", ".join(changed))
            return 1
        print(f"Queue inputs unchanged: {output_path}")
        return 0

    fingerprints = _fingerprint_inputs(report_path, event_path, table_path, config_path, project)

    report = _read_json(report_path)
    event_catalog = _read_json(event_path) if event_path is not None else None
    table_catalog = _read_json(table_path) if table_path is not None else None
    queue = build_queue(
        report, load_modules(config_path), event_catalog, table_catalog,
        report_path=_path_summary(report_path, project),
        event_catalog_path=_path_summary(event_path, project) if event_path is not None else None,
        script_table_catalog_path=_path_summary(table_path, project) if table_path is not None else None,
    )
    queue["inputs"] = {
        "files": fingerprints,
        "command": [sys.argv[0], *(argv if argv is not None else sys.argv[1:])],
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(queue, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {len(queue['queue'])} candidates to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
