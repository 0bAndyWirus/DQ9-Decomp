#!/usr/bin/env python3
"""Propose EU function addresses from raw USA images or compiled ELF objects."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

if __package__:
    from .find_eu_matches import Module as MatchModule
    from .find_eu_matches import find_pattern_matches, scan_object
else:
    from find_eu_matches import Module as MatchModule
    from find_eu_matches import find_pattern_matches, scan_object


_ADDRESS = r"0[xX][0-9a-fA-F]+"
_SYMBOL_LINE = re.compile(
    rf"^(?P<name>\S+)\s+kind:(?P<kind>\S+)\s+addr:(?P<address>{_ADDRESS})(?:\s+.*)?$"
)
_FUNCTION_KIND = re.compile(
    r"function\((?P<mode>[^,()]+)(?:,size=(?P<size>0[xX][0-9a-fA-F]+))?\)"
)
_RELOCATION_LINE = re.compile(
    rf"^from:(?P<address>{_ADDRESS})\s+kind:(?P<kind>\S+)(?:\s+.*)?$"
)
_RELOCATION_WIDTHS = {
    "arm_call": 4,
    "thumb_call": 4,
    "thumb_branch": 2,
    "load": 4,
}


@dataclass(frozen=True)
class Symbol:
    name: str
    kind: str
    address: int
    category: str
    mode: str | None = None
    size: int | None = None


@dataclass(frozen=True)
class Relocation:
    address: int
    kind: str


@dataclass(frozen=True)
class ImageModule:
    name: str
    base: int
    path: Path
    data: bytes


@dataclass(frozen=True)
class SourceModule:
    image: ImageModule
    symbols: tuple[Symbol, ...]
    relocations: tuple[Relocation, ...]


def _address(value: str) -> int:
    return int(value, 0)


def parse_symbols(text: str, source: str = "symbols.txt") -> list[Symbol]:
    """Parse DSD symbols.txt lines, retaining non-function symbols as well."""
    symbols = []
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        match = _SYMBOL_LINE.fullmatch(line)
        if match is None:
            raise ValueError(f"{source}:{line_number}: malformed DSD symbol")

        name = match.group("name")
        kind = match.group("kind")
        address = _address(match.group("address"))
        function_match = _FUNCTION_KIND.fullmatch(kind)
        if function_match is not None:
            size_text = function_match.group("size")
            symbols.append(
                Symbol(
                    name=name,
                    kind=kind,
                    address=address,
                    category="function",
                    mode=function_match.group("mode"),
                    size=_address(size_text) if size_text is not None else None,
                )
            )
        elif kind.startswith("function"):
            symbols.append(Symbol(name, kind, address, "function"))
        elif kind.startswith(("data(", "bss", "rodata")):
            symbols.append(Symbol(name, kind, address, "data"))
        else:
            symbols.append(Symbol(name, kind, address, "unknown"))
    return symbols


def parse_relocations(text: str, source: str = "relocs.txt") -> list[Relocation]:
    """Parse DSD relocation sources; other record fields are intentionally ignored."""
    relocations = []
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        match = _RELOCATION_LINE.fullmatch(line)
        if match is None:
            raise ValueError(f"{source}:{line_number}: malformed DSD relocation")
        relocations.append(Relocation(_address(match.group("address")), match.group("kind")))
    return relocations


def relocation_mask(relocations: Sequence[Relocation], start: int, size: int) -> set[int]:
    """Return function-relative bytes occupied by DSD relocation patches."""
    masked: set[int] = set()
    for relocation in relocations:
        relative = relocation.address - start
        if 0 <= relative < size:
            width = _RELOCATION_WIDTHS.get(relocation.kind, 4)
            masked.update(range(relative, min(relative + width, size)))
    return masked


def _hex(address: int) -> str:
    return f"0x{address:08x}"


def _result_for_symbol(
    source: SourceModule,
    symbol: Symbol,
    eu_modules: Sequence[ImageModule],
) -> dict[str, object]:
    result: dict[str, object] = {
        "usa_module": source.image.name,
        "symbol": symbol.name,
        "kind": symbol.kind,
        "usa_address": _hex(symbol.address),
        "size": symbol.size,
        "status": "unknown",
        "reason": "unsupported_symbol_kind",
        "matches": [],
    }
    if symbol.category == "data":
        result["status"] = "data"
        result["reason"] = "data_symbol_not_proposed"
        return result
    if symbol.category != "function":
        return result
    if symbol.mode not in ("arm", "thumb"):
        result["reason"] = "unsupported_function_mode"
        return result
    if symbol.size is None or symbol.size <= 0:
        result["reason"] = "function_size_missing_or_zero"
        return result

    code_address = symbol.address & ~1 if symbol.mode == "thumb" else symbol.address
    offset = code_address - source.image.base
    if offset < 0 or offset + symbol.size > len(source.image.data):
        result["reason"] = "function_outside_usa_image"
        return result

    pattern = source.image.data[offset : offset + symbol.size]
    masked = relocation_mask(source.relocations, code_address, symbol.size)
    result["masked_bytes"] = len(masked)
    if len(masked) == symbol.size:
        result["reason"] = "all_function_bytes_relocated"
        return result

    matches = []
    for module in eu_modules:
        for match_offset in find_pattern_matches(module.data, pattern, masked):
            matches.append(
                {
                    "module": module.name,
                    "address": _hex(module.base + match_offset),
                }
            )
    result["matches"] = matches
    if not matches:
        result["reason"] = "no_eu_match"
    elif len(matches) == 1:
        result["status"] = "proposed"
        result["reason"] = None
        result["eu_module"] = matches[0]["module"]
        result["eu_address"] = matches[0]["address"]
    else:
        result["status"] = "ambiguous"
        result["reason"] = "multiple_eu_matches"
    return result


def propose_symbols(
    usa_modules: Sequence[SourceModule], eu_modules: Sequence[ImageModule]
) -> dict[str, object]:
    """Return all symbol outcomes and only the uniquely located function proposals."""
    symbol_results = [
        _result_for_symbol(source, symbol, eu_modules)
        for source in usa_modules
        for symbol in source.symbols
    ]
    proposals = [
        {
            "usa_module": result["usa_module"],
            "symbol": result["symbol"],
            "usa_address": result["usa_address"],
            "size": result["size"],
            "eu_module": result["eu_module"],
            "eu_address": result["eu_address"],
        }
        for result in symbol_results
        if result["status"] == "proposed"
    ]
    counts = {status: 0 for status in ("proposed", "ambiguous", "unknown", "data")}
    for result in symbol_results:
        counts[result["status"]] += 1
    return {
        "proposals": proposals,
        "symbols": symbol_results,
        "summary": counts,
    }


def propose_object(
    object_path: Path, eu_modules: Sequence[ImageModule]
) -> dict[str, object]:
    """Match STT_FUNC signatures from an ELF object using its section relocations."""
    matcher_modules = [
        MatchModule(module.name, module.base, module.path, module.data)
        for module in eu_modules
    ]
    scan = scan_object(object_path, matcher_modules)
    symbol_results = []
    proposals = []
    for function in scan["functions"]:
        size = function["size"]
        masked_bytes = function["relocated_bytes_masked"]
        matches = []
        if masked_bytes < size:
            matches = [
                {"module": hit["module"], "address": _hex(hit["address"])}
                for hit in function["hits"]
            ]
        result: dict[str, object] = {
            "usa_object": str(object_path),
            "symbol": function["name"],
            "kind": "STT_FUNC",
            "usa_section": function["section"],
            "usa_offset": function["offset"],
            "size": size,
            "masked_bytes": masked_bytes,
            "matches": matches,
            "status": "unknown",
            "reason": "all_function_bytes_relocated"
            if masked_bytes >= size
            else "no_eu_match",
        }
        if len(matches) == 1:
            result["status"] = "proposed"
            result["reason"] = None
            result["eu_module"] = matches[0]["module"]
            result["eu_address"] = matches[0]["address"]
            proposals.append(
                {
                    "usa_object": str(object_path),
                    "symbol": function["name"],
                    "usa_section": function["section"],
                    "usa_offset": function["offset"],
                    "size": size,
                    "eu_module": matches[0]["module"],
                    "eu_address": matches[0]["address"],
                }
            )
        elif len(matches) > 1:
            result["status"] = "ambiguous"
            result["reason"] = "multiple_eu_matches"
        symbol_results.append(result)

    counts = {status: 0 for status in ("proposed", "ambiguous", "unknown", "data")}
    for result in symbol_results:
        counts[result["status"]] += 1
    return {"proposals": proposals, "symbols": symbol_results, "summary": counts}


def _load_image_module(name: str, base_text: str, path_text: str) -> ImageModule:
    try:
        base = int(base_text, 0)
    except ValueError as error:
        raise ValueError(f"module {name!r} has an invalid base address {base_text!r}") from error
    path = Path(path_text)
    return ImageModule(name, base, path, path.read_bytes())


def _load_source_module(values: list[str]) -> SourceModule:
    name, base_text, image_text, symbols_text, relocations_text = values
    image = _load_image_module(name, base_text, image_text)
    symbols_path = Path(symbols_text)
    symbols = parse_symbols(symbols_path.read_text(encoding="utf-8"), str(symbols_path))
    if relocations_text == "-":
        relocations = []
    else:
        relocations_path = Path(relocations_text)
        relocations = parse_relocations(
            relocations_path.read_text(encoding="utf-8"), str(relocations_path)
        )
    return SourceModule(image, tuple(symbols), tuple(relocations))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Propose EU addresses from DSD USA function symbols or compiled USA ELF "
            "objects when relocation-masked bytes have one unique EU match."
        ),
        epilog=(
            "Repeat --usa-module for raw USA image/symbol/relocation sets, or "
            "--usa-object for compiled ELF objects. Repeat --eu-module for every "
            "EU image. Use '-' for a USA module without relocs.txt."
        ),
    )
    parser.add_argument(
        "--usa-module",
        action="append",
        nargs=5,
        metavar=("NAME", "BASE", "IMAGE", "SYMBOLS", "RELOCS"),
        help="USA raw module image, load base, DSD symbols.txt, and relocs.txt (or -)",
    )
    parser.add_argument(
        "--usa-object",
        action="append",
        type=Path,
        metavar="OBJECT",
        help="compiled USA ELF object; repeat for every object to scan",
    )
    parser.add_argument(
        "--eu-module",
        action="append",
        nargs=3,
        metavar=("NAME", "BASE", "IMAGE"),
        required=True,
        help="EU raw module image and its load base; repeat for every module to search",
    )
    args = parser.parse_args(argv)
    usa_module_args = args.usa_module or []
    usa_object_paths = args.usa_object or []
    if not usa_module_args and not usa_object_paths:
        parser.error("provide at least one --usa-module or --usa-object")

    try:
        usa_modules = [_load_source_module(values) for values in usa_module_args]
        eu_modules = [
            _load_image_module(name, base, path) for name, base, path in args.eu_module
        ]
        names = [module.image.name for module in usa_modules]
        if len(names) != len(set(names)):
            raise ValueError("USA module names must be unique")
        names = [module.name for module in eu_modules]
        if len(names) != len(set(names)):
            raise ValueError("EU module names must be unique")

        result = propose_symbols(usa_modules, eu_modules)
        all_symbols = list(result["symbols"])
        all_proposals = list(result["proposals"])
        for object_path in usa_object_paths:
            object_result = propose_object(object_path, eu_modules)
            all_symbols.extend(object_result["symbols"])
            all_proposals.extend(object_result["proposals"])
        counts = {status: 0 for status in ("proposed", "ambiguous", "unknown", "data")}
        for symbol_result in all_symbols:
            counts[symbol_result["status"]] += 1
        result = {
            "proposals": all_proposals,
            "symbols": all_symbols,
            "summary": counts,
        }
    except (OSError, ValueError, struct.error) as error:
        print(f"propose_eu_symbols.py: {error}", file=sys.stderr)
        return 2

    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
