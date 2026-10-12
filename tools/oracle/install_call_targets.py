#!/usr/bin/env python3
"""Install original call-target glue using caller-local JSON anchor regexes.

--anchors must name a JSON object with include, trace, arm, and thumb string
patterns. Each pattern must match exactly once (Python re.MULTILINE).
The header is inserted after include, call state after trace, and target
recording after arm/thumb. Anchors should match complete source lines after
the instruction-entry trace and execution points; matched text is preserved.
No emulator source statements or default anchor patterns are bundled.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
import sys

HEADER = Path(__file__).with_name("call_targets.h").resolve()


def instrument(source: str, anchors: dict[str, str], header: Path = HEADER) -> str:
    include = f'#include "{header.as_posix()}"'
    if "dq9_oracle::is_call" in source:
        if include not in source:
            raise ValueError("call-target hook already installed with a different header path")
        return source
    if not isinstance(anchors, dict) or set(anchors) != {"include", "trace", "arm", "thumb"}:
        raise ValueError("anchors must contain exactly include, trace, arm, and thumb patterns")
    additions = {
        "include": [include],
        "trace": [
            "const bool oracle_is_call = dq9_oracle::is_call(ARMPROC);",
            "const u32 oracle_call_source = ARMPROC.instruct_adr;",
        ],
        "arm": ["if (oracle_is_call) dq9_oracle::record_call(PROCNUM, ARMPROC, oracle_call_source, oracle_frame);"],
        "thumb": ["if (oracle_is_call) dq9_oracle::record_call(PROCNUM, ARMPROC, oracle_call_source, oracle_frame);"],
    }
    replacements = []
    for key, lines in additions.items():
        pattern = anchors[key]
        if not isinstance(pattern, str) or not pattern:
            raise ValueError(f"anchor {key} must be a nonempty regex string")
        try:
            matches = list(re.finditer(pattern, source, re.MULTILINE))
        except re.error as error:
            raise ValueError(f"invalid anchor {key}: {error}") from error
        if len(matches) != 1 or not matches[0].group():
            raise ValueError(f"expected exactly one nonempty oracle source anchor: {key}")
        match = matches[0]
        indent = re.match(r"[ \t]*", match.group()).group()
        insertion = "\n".join(indent + line for line in lines)
        original = match.group()
        separator = "" if original.endswith("\n") else "\n"
        suffix = "\n" if original.endswith("\n") else ""
        replacements.append((match.start(), match.end(), original + separator + insertion + suffix))
    ordered = sorted(replacements)
    if any(left[1] > right[0] for left, right in zip(ordered, ordered[1:])):
        raise ValueError("oracle source anchors must not overlap")
    for start, end, replacement in reversed(ordered):
        source = source[:start] + replacement + source[end:]
    return source


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="private DeSmuME src/armcpu.cpp")
    parser.add_argument("--anchors", type=Path, required=True,
                        help="local JSON object with include/trace/arm/thumb anchor regexes")
    args = parser.parse_args(argv)
    original = args.source.read_text()
    updated = instrument(original, json.loads(args.anchors.read_text()))
    if updated != original:
        args.source.write_text(updated)
    print(f"Call-target hook installed: {args.source}; rebuild the private oracle")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"oracle hook: {error}", file=sys.stderr)
        raise SystemExit(2)
