"""Command-line frontend for ahead-of-time ARM/Thumb recompilation."""

from __future__ import annotations

import argparse
import json
import sys

from .emitter import emit_file


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Translate a DQ9-Remastered CPU manifest into native C++")
    parser.add_argument("--manifest", required=True, help="JSON recompiler manifest")
    parser.add_argument("--out", required=True, help="generated C++ output directory")
    parser.add_argument("--extra-entries", help="text file of 'module_id addr thumb' entries")
    parser.add_argument("--chunk-size", type=int, default=400, help="functions per generated .cpp (default: 400)")
    args = parser.parse_args(argv)
    try:
        stats = emit_file(args.manifest, args.out, args.extra_entries, args.chunk_size)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(
        f"generated {stats['functions']} functions in {stats['blocks']} blocks; "
        f"{stats['extra_entries']} extra entries, {stats['jump_tables']} jump tables, "
        f"{stats['dsr_call_sites']} dsr_call sites, {stats['undefined_sites']} undefined/unsupported sites"
    )
    for site in stats["unsupported_sites"]:
        print(
            f"unsupported 0x{site['address']:08x} [{site['module']}]: "
            f"{site['instruction']} ({site['reason']})",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
