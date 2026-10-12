#!/usr/bin/env python3
"""Generate conservative C++ candidates for small ARM functions and exact-match them."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Sequence

if __package__:
    from .find_eu_matches import Function, Module, find_pattern_matches, parse_elf_object
else:
    from find_eu_matches import Function, Module, find_pattern_matches, parse_elf_object

MAX_FUNCTION_SIZE = 0x40
BX_LR = 0xE12FFF1E
MOV_PC_LR = 0xE1A0F00E
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILER_ROOT = ROOT / "extracted" / "dqix-decomp-eu"


@dataclass(frozen=True)
class FunctionPattern:
    """A recognized, relocation-free ARM function shape."""

    kind: str
    return_value: int | None = None
    offset: int | None = None
    field_type: str | None = None
    argument_index: int | None = None


def _word(code: bytes, offset: int = 0) -> int:
    return struct.unpack_from("<I", code, offset)[0]


def _is_return(instruction: int) -> bool:
    return instruction in (BX_LR, MOV_PC_LR)


def _rotate_right(value: int, amount: int) -> int:
    amount &= 31
    value &= 0xFFFFFFFF
    if amount == 0:
        return value
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF


def _decode_constant_return(instruction: int) -> int | None:
    if instruction >> 28 != 0xE or (instruction >> 26) & 0x3 != 0:
        return None
    if not (instruction & (1 << 25)) or instruction & (1 << 20):
        return None
    if (instruction >> 12) & 0xF != 0 or (instruction >> 16) & 0xF != 0:
        return None
    opcode = (instruction >> 21) & 0xF
    if opcode not in (0xD, 0xF):  # MOV or MVN, with no flag update
        return None
    immediate = instruction & 0xFF
    rotated = _rotate_right(immediate, ((instruction >> 8) & 0xF) * 2)
    return rotated if opcode == 0xD else (~rotated & 0xFFFFFFFF)


def _decode_argument_forwarder(instruction: int) -> int | None:
    if instruction >> 28 != 0xE or (instruction >> 26) & 0x3 != 0:
        return None
    if instruction & (1 << 25) or instruction & (1 << 20):
        return None
    if (instruction >> 21) & 0xF != 0xD:
        return None
    if (instruction >> 16) & 0xF != 0 or (instruction >> 12) & 0xF != 0:
        return None
    if (instruction >> 4) & 1 or (instruction >> 5) & 0xFF != 0:
        return None
    source_register = instruction & 0xF
    return source_register if 1 <= source_register <= 3 else None


def _decode_single_transfer(instruction: int) -> tuple[str, int] | None:
    """Decode immediate ARM single/extra load-store at [r0, #positive_imm]."""
    if instruction >> 28 != 0xE or (instruction >> 26) & 0x3 != 0x1:
        return None
    if instruction & (1 << 25):  # register offset
        return None
    if not instruction & (1 << 24) or not instruction & (1 << 23):  # P=1, U=1
        return None
    if instruction & (1 << 21) or (instruction >> 16) & 0xF != 0:
        return None
    load = bool(instruction & (1 << 20))
    data_register = (instruction >> 12) & 0xF
    if data_register != (0 if load else 1):
        return None
    byte = bool(instruction & (1 << 22))
    offset = instruction & 0xFFF
    return ("unsigned char" if byte else "unsigned int", offset)


def _decode_extra_transfer(instruction: int) -> tuple[str, int, bool] | None:
    """Decode immediate LDRH/LDRSB/LDRSH/STRH forms with base r0 and data r0/r1."""
    if instruction >> 28 != 0xE or (instruction >> 25) & 0x7 != 0:
        return None
    if not instruction & (1 << 24) or not instruction & (1 << 23):  # P=1, U=1
        return None
    if not instruction & (1 << 22) or instruction & (1 << 21):  # immediate, W=0
        return None
    if ((instruction >> 7) & 1) != 1 or ((instruction >> 4) & 1) != 1:
        return None
    if (instruction >> 16) & 0xF != 0:
        return None
    data_register = (instruction >> 12) & 0xF
    load = bool(instruction & (1 << 20))
    signed = bool(instruction & (1 << 6))
    half = bool(instruction & (1 << 5))
    offset = (((instruction >> 8) & 0xF) << 4) | (instruction & 0xF)
    if load:
        if data_register != 0:
            return None
        if signed and half:
            return "short", offset, True
        if signed and not half:
            return "signed char", offset, True
        if not signed and half:
            return "unsigned short", offset, True
        return None
    if data_register != 1 or signed or not half:
        return None
    return "unsigned short", offset, False


def _decode_field_transfer(instruction: int) -> tuple[str, int, bool] | None:
    single = _decode_single_transfer(instruction)
    if single is not None:
        field_type, offset = single
        return field_type, offset, bool(instruction & (1 << 20))
    extra = _decode_extra_transfer(instruction)
    if extra is not None:
        field_type, offset, load = extra
        return field_type, offset, load
    return None


def recognize_function(code: bytes) -> FunctionPattern | None:
    """Recognize only complete, tiny A32 functions with no control-flow ambiguity."""
    if not code or len(code) > MAX_FUNCTION_SIZE or len(code) % 4:
        return None
    if len(code) == 4 and _is_return(_word(code)):
        return FunctionPattern("empty_thunk")
    if len(code) != 8 or not _is_return(_word(code, 4)):
        return None

    instruction = _word(code)
    value = _decode_constant_return(instruction)
    if value is not None:
        return FunctionPattern("constant_return", return_value=value)

    forwarded_argument = _decode_argument_forwarder(instruction)
    if forwarded_argument is not None:
        return FunctionPattern("argument_forwarder", argument_index=forwarded_argument)

    transfer = _decode_field_transfer(instruction)
    if transfer is None:
        return None
    field_type, offset, load = transfer
    width = 1 if field_type in ("unsigned char", "signed char") else 2 if "short" in field_type else 4
    if offset % width:
        return None
    if load:
        return FunctionPattern("field_getter", offset=offset, field_type=field_type)
    return FunctionPattern("field_setter", offset=offset, field_type=field_type)


def render_candidate(pattern: FunctionPattern, name: str) -> str:
    """Render a standalone MWCC C++ translation unit for one recognized pattern."""
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise ValueError(f"invalid generated function name {name!r}")
    if pattern.kind == "empty_thunk":
        return f'extern "C" void {name}(void) {{}}\n'
    if pattern.kind == "constant_return" and pattern.return_value is not None:
        return (
            f'extern "C" unsigned int {name}(void) {{\n'
            f"    return 0x{pattern.return_value:08x}u;\n"
            "}\n"
        )
    if pattern.kind == "argument_forwarder" and pattern.argument_index is not None:
        parameters = [f"unsigned int arg{index}" for index in range(pattern.argument_index + 1)]
        return (
            f'extern "C" unsigned int {name}({", ".join(parameters)}) {{\n'
            f"    return arg{pattern.argument_index};\n"
            "}\n"
        )
    if pattern.kind in ("field_getter", "field_setter") and pattern.offset is not None and pattern.field_type:
        object_type = f"{name}_object"
        padding = f"    unsigned char _padding[{pattern.offset}];\n" if pattern.offset else ""
        object_declaration = (
            f"struct {object_type} {{\n"
            f"{padding}"
            f"    {pattern.field_type} field;\n"
            "};\n"
        )
        if pattern.kind == "field_getter":
            function = (
                f'extern "C" {pattern.field_type} {name}(const struct {object_type} *object) {{\n'
                "    return object->field;\n"
                "}\n"
            )
        else:
            function = (
                f'extern "C" void {name}(struct {object_type} *object, {pattern.field_type} value) {{\n'
                "    object->field = value;\n"
                "}\n"
            )
        return object_declaration + function
    raise ValueError(f"unsupported candidate pattern: {pattern!r}")


def _module(value: Sequence[str]) -> Module:
    name, base, raw_path = value
    path = Path(raw_path)
    return Module(name, int(base, 0), path, path.read_bytes())


def _function_bytes(function: Function) -> bytes:
    return function.section.data[function.offset : function.offset + function.size]


def _exact_hits(code: bytes, modules: Sequence[Module]) -> list[dict[str, Any]]:
    hits: list[dict[str, Any]] = []
    for module in modules:
        for offset in find_pattern_matches(module.data, code):
            address = module.base + offset
            if address % 4 == 0:
                hits.append({
                    "module": module.name,
                    "path": str(module.path),
                    "offset": offset,
                    "address": address,
                })
    return hits


def _safe_component(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_.")
    return sanitized or "candidate"


def _safe_identifier(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9_]+", "_", value).strip("_")
    if not sanitized:
        return "object"
    if sanitized[0].isdigit():
        return f"_{sanitized}"
    return sanitized


def candidate_symbol(object_path: Path, index: int) -> str:
    """Give each candidate a deterministic object-specific C symbol."""
    stem = _safe_identifier(object_path.stem)[:32] or "object"
    identity = hashlib.sha256(str(object_path.resolve()).encode("utf-8")).hexdigest()[:16]
    return f"candidate_{stem}_{identity}_{index:04d}"


def format_source_unit_path(template: str, symbol: str) -> str:
    """Expand a DSD source path template and keep it safe for a delinks stanza."""
    if "{symbol}" not in template:
        raise ValueError("source-unit path template must contain {symbol}")
    value = template.format(symbol=symbol)
    if any(character in value for character in (":", chr(10), chr(13))):
        raise ValueError("source-unit path cannot contain colon or newline characters")
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise ValueError("source-unit path must be a non-empty relative path without '..'")
    return path.as_posix()


def delinks_stanza(source_unit_path: str, address: int, size: int) -> str:
    """Format the exact DSD source-unit block used by the existing delinks files."""
    if size <= 0:
        raise ValueError("delinks range size must be positive")
    return chr(10).join((
        f"{source_unit_path}:",
        "    complete",
        f"    .text start:0x{address:08x} end:0x{address + size:08x}",
        "",
    ))


def _save_artifact(
    artifact_root: Path,
    candidate_name: str,
    pattern: FunctionPattern,
    original_function: Function,
    input_object_path: Path,
    source: str,
    compiled_object_path: Path,
    module: Module,
    offset: int,
    code: bytes,
    source_unit_path: str | None = None,
) -> Path:
    artifact_name = (
        f"{candidate_name}_{_safe_component(module.name)}_0x{module.base + offset:08x}"
    )
    destination = artifact_root / artifact_name
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "candidate.cpp").write_text(source, encoding="utf-8")
    (destination / "replacement.bin").write_bytes(code)
    shutil.copyfile(compiled_object_path, destination / "candidate.o")
    metadata = {
        "schema_version": 1,
        "decision": "unique_exact_byte_match",
        "pattern": pattern.kind,
        "input_object": str(input_object_path),
        "input_section": original_function.section.name,
        "input_symbol": original_function.name,
        "input_size": original_function.size,
        "input_offset": original_function.offset,
        "pattern_parameters": {
            "return_value": pattern.return_value,
            "offset": pattern.offset,
            "field_type": pattern.field_type,
            "argument_index": pattern.argument_index,
        },
        "generated_symbol": candidate_name,
        "source_unit_path": source_unit_path,
        "target": {
            "module": module.name,
            "raw_image_sha256": hashlib.sha256(module.data).hexdigest(),
            "raw_image": str(module.path),
            "load_address": module.base,
            "offset": offset,
            "address": module.base + offset,
            "size": len(code),
        },
        "original_bytes": code.hex(),
        "replacement_bytes": code.hex(),
        "replacement_file": "replacement.bin",
        "sha256": hashlib.sha256(code).hexdigest(),
        "relocation_policy": "candidate function must have no relocation sites; raw bytes compared without masking",
    }
    (destination / "patch.json").write_text(json.dumps(metadata, indent=2) + chr(10), encoding="utf-8")
    if source_unit_path is not None:
        stanza = delinks_stanza(source_unit_path, module.base + offset, len(code))
        integration = {
            "schema_version": 1,
            "source_unit_path": source_unit_path,
            "candidate_symbol": candidate_name,
            "module": module.name,
            "address": module.base + offset,
            "size": len(code),
            "delinks_stanza": stanza,
        }
        (destination / "delinks.txt").write_text(stanza, encoding="utf-8")
        (destination / "integration.json").write_text(
            json.dumps(integration, indent=2) + chr(10), encoding="utf-8",
        )
    return destination


def _compiler_command(
    source_path: Path,
    object_path: Path,
    compiler_root: Path,
    wibo: Path,
    mwcc: Path,
) -> list[str]:
    return [
        str(wibo),
        str(mwcc),
        "-O2",
        "-enum", "int",
        "-char", "signed",
        "-str", "noreuse",
        "-proc", "arm946e",
        "-gccext,on",
        "-fp", "soft",
        "-inline", "noauto",
        "-lang=c++",
        "-Cpp_exceptions", "off",
        "-RTTI", "off",
        "-interworking",
        "-w", "off",
        "-sym", "on",
        "-gccinc",
        "-nolink",
        "-msgstyle", "gcc",
        "-str", "pool,reuse",
        "-i", str(compiler_root / "include"),
        "-d", "eur",
        "-c", str(source_path),
        "-o", str(object_path),
    ]


def run_candidate_loop(
    object_path: Path,
    modules: Sequence[Module],
    compiler_root: Path = DEFAULT_COMPILER_ROOT,
    wibo: Path | None = None,
    mwcc: Path | None = None,
    artifact_dir: Path | None = None,
    dsd_patch_dir: Path | None = None,
    source_unit_path_template: str | None = None,
) -> dict[str, Any]:
    """Recognize, individually compile, and exact-match conservative small functions."""
    if (dsd_patch_dir is None) != (source_unit_path_template is None):
        raise ValueError("--dsd-patch-dir and --source-unit-path must be used together")
    compiler_root = compiler_root.resolve()
    wibo = (wibo or compiler_root / "wibo").resolve()
    mwcc = (mwcc or compiler_root / "tools" / "mwccarm" / "2.0" / "sp2p2" / "mwccarm.exe").resolve()
    sections, functions, input_relocations = parse_elf_object(object_path)
    del sections
    records: list[dict[str, Any]] = []
    generated: list[tuple[dict[str, Any], Function, FunctionPattern, str, str, str | None]] = []

    for function in functions:
        record: dict[str, Any] = {
            "input_symbol": function.name,
            "input_section": function.section.name,
            "input_offset": function.offset,
            "input_size": function.size,
        }
        records.append(record)
        if function.size > MAX_FUNCTION_SIZE:
            record.update({"status": "too_large", "limit": MAX_FUNCTION_SIZE})
            continue
        if not (function.section.name == ".text" or function.section.name.startswith(".text.")):
            record.update({"status": "not_text"})
            continue
        relocated_sites = sorted(
            site for site in input_relocations.get(function.section.index, set())
            if function.offset <= site < function.offset + function.size
        )
        if relocated_sites:
            record.update({"status": "input_relocations", "relocation_sites": relocated_sites})
            continue
        pattern = recognize_function(_function_bytes(function))
        if pattern is None:
            record.update({"status": "unrecognized"})
            continue
        candidate_name = candidate_symbol(object_path, len(generated))
        source = render_candidate(pattern, candidate_name)
        source_unit_path = (
            format_source_unit_path(source_unit_path_template, candidate_name)
            if source_unit_path_template is not None else None
        )
        record.update({
            "status": "generated",
            "pattern": pattern.kind,
            "candidate_symbol": candidate_name,
            "source": source,
        })
        if source_unit_path is not None:
            record["source_unit_path"] = source_unit_path
        generated.append((record, function, pattern, candidate_name, source, source_unit_path))

    result: dict[str, Any] = {
        "input_object": str(object_path),
        "maximum_function_size": MAX_FUNCTION_SIZE,
        "modules": [{"name": module.name, "base": module.base, "path": str(module.path)} for module in modules],
        "functions": records,
        "compiler_commands": [],
    }
    if not generated:
        result["summary"] = {
            "recognized": 0,
            "exact_matches": 0,
            "rejected_relocations": 0,
            "compile_failures": 0,
        }
        return result

    exact_matches = 0
    rejected_relocations = 0
    compile_failures = 0
    with tempfile.TemporaryDirectory(prefix="small-function-candidates-") as temporary:
        temporary_dir = Path(temporary)
        for index, (record, input_function, pattern, candidate_name, source, source_unit_path) in enumerate(generated):
            source_path = temporary_dir / f"{candidate_name}.cpp"
            compiled_object = temporary_dir / f"{candidate_name}.o"
            source_path.write_text(source, encoding="utf-8")
            command = _compiler_command(source_path, compiled_object, compiler_root, wibo, mwcc)
            result["compiler_commands"].append(command)
            try:
                completed = subprocess.run(
                    command,
                    cwd=compiler_root,
                    check=False,
                    capture_output=True,
                    text=True,
                )
            except OSError as error:
                record.update({"status": "compiler_unavailable", "error": str(error)})
                compile_failures += 1
                for pending_record, *_ in generated[index + 1:]:
                    pending_record.update({"status": "compiler_unavailable", "error": str(error)})
                    compile_failures += 1
                break
            if completed.returncode != 0 or not compiled_object.is_file():
                record.update({
                    "status": "compile_failed",
                    "compiler_returncode": completed.returncode,
                    "compiler_stdout": completed.stdout,
                    "compiler_stderr": completed.stderr,
                })
                compile_failures += 1
                continue
            try:
                _compiled_sections, compiled_functions, compiled_relocations = parse_elf_object(compiled_object)
            except (OSError, ValueError, struct.error) as error:
                record.update({"status": "invalid_compiler_output", "error": str(error)})
                compile_failures += 1
                continue

            outputs = [function for function in compiled_functions if function.name == candidate_name]
            if len(outputs) != 1:
                record.update({"status": "compiled_symbol_missing" if not outputs else "compiled_symbol_ambiguous"})
                continue
            output_function = outputs[0]
            code = _function_bytes(output_function)
            record["compiled_size"] = len(code)
            if len(code) > MAX_FUNCTION_SIZE:
                record.update({"status": "compiled_function_too_large"})
                continue
            relocation_sites = compiled_relocations.get(output_function.section.index, set())
            function_relocations = sorted(
                site for site in relocation_sites
                if output_function.offset <= site < output_function.offset + output_function.size
            )
            if function_relocations:
                record.update({
                    "status": "rejected_relocations",
                    "relocation_sites": function_relocations,
                    "relocation_policy": "relocated functions are not matched with masked bytes",
                })
                rejected_relocations += 1
                continue
            hits = _exact_hits(code, modules)
            record["exact_hits"] = hits
            if not hits:
                record.update({"status": "no_exact_match", "compiled_bytes": code.hex()})
                continue
            if len(hits) != 1:
                record.update({"status": "ambiguous_exact_match"})
                continue

            exact_matches += 1
            record.update({"status": "unique_exact_match", "compiled_bytes": code.hex()})
            hit = hits[0]
            module = next(module for module in modules if module.name == hit["module"] and str(module.path) == hit["path"])
            integration_source_path = source_unit_path
            if integration_source_path is not None and (
                output_function.section.name != ".text"
                or output_function.offset != 0
                or output_function.section.size != output_function.size
            ):
                record["dsd_patch_error"] = (
                    "DSD stanza requires the candidate to occupy the complete .text section"
                )
                integration_source_path = None
            roots: dict[Path, str | None] = {}
            if artifact_dir is not None:
                roots[artifact_dir] = None
            if dsd_patch_dir is not None and integration_source_path is not None:
                roots[dsd_patch_dir] = integration_source_path
            artifact_dirs: list[str] = []
            dsd_patch_dirs: list[str] = []
            artifact_errors: list[str] = []
            for root, integration_source_path in roots.items():
                try:
                    artifact_path = _save_artifact(
                        root, candidate_name, pattern, input_function, object_path,
                        source, compiled_object, module, hit["offset"], code, integration_source_path,
                    )
                except OSError as error:
                    artifact_errors.append(f"{root}: {error}")
                    continue
                artifact_dirs.append(str(artifact_path))
                if integration_source_path is not None:
                    dsd_patch_dirs.append(str(artifact_path))
            if artifact_dirs:
                record["artifact_dirs"] = artifact_dirs
            if dsd_patch_dirs:
                record["dsd_patch_dirs"] = dsd_patch_dirs
            if artifact_errors:
                record["artifact_errors"] = artifact_errors

    result["summary"] = {
        "recognized": len(generated),
        "exact_matches": exact_matches,
        "rejected_relocations": rejected_relocations,
        "compile_failures": compile_failures,
    }
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Recognize conservative A32 functions <= 0x40 bytes in a relocatable ARM object, "
            "compile generated C++ with the EU MWCC toolchain, and require unique exact raw-image matches."
        )
    )
    parser.add_argument("object", type=Path, help="relocatable ARM ELF object containing function symbols")
    parser.add_argument(
        "--module", action="append", nargs=3, metavar=("NAME", "BASE", "RAW_IMAGE"), required=True,
        help="raw EU module image and load address to search; repeat for each module",
    )
    parser.add_argument("--compiler-root", type=Path, default=DEFAULT_COMPILER_ROOT,
                        help="local compiler checkout containing wibo, MWCC, and include/")
    parser.add_argument("--wibo", type=Path, help="override the wibo executable")
    parser.add_argument("--mwcc", type=Path, help="override mwccarm.exe")
    parser.add_argument("--artifact-dir", type=Path,
                        help="write candidate.cpp, candidate.o, replacement.bin, and patch.json for unique exact matches")
    parser.add_argument("--dsd-patch-dir", type=Path,
                        help="write candidate files plus integration.json and an exact delinks.txt stanza for unique matches")
    parser.add_argument("--source-unit-path",
                        help="DSD-relative path template for the candidate source; must contain {symbol} and requires --dsd-patch-dir")
    args = parser.parse_args(argv)
    if (args.dsd_patch_dir is None) != (args.source_unit_path is None):
        parser.error("--dsd-patch-dir and --source-unit-path must be supplied together")
    try:
        modules = [_module(value) for value in args.module]
        result = run_candidate_loop(
            args.object,
            modules,
            args.compiler_root,
            args.wibo,
            args.mwcc,
            args.artifact_dir,
            args.dsd_patch_dir,
            args.source_unit_path,
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"small_function_candidates.py: {error}", file=sys.stderr)
        return 2
    json.dump(result, sys.stdout, indent=2)
    sys.stdout.write("\n")
    if any(
        function.get("status") in {
            "compiler_unavailable",
            "compile_failed",
            "invalid_compiler_output",
        }
        or function.get("artifact_errors")
        or function.get("dsd_patch_error")
        for function in result["functions"]
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
