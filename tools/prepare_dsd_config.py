#!/usr/bin/env python3
"""Adapt ds-rom extraction metadata for the pinned dsd 0.12.x schema."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


class ConfigError(ValueError):
    """The extracted ROM configuration cannot be safely adapted."""


def _scalar(text: str, key: str) -> int:
    match = re.search(rf"^{re.escape(key)}:\s*(\d+)\s*$", text, re.MULTILINE)
    if match is None:
        raise ConfigError(f"missing numeric config field {key}")
    value = int(match.group(1))
    if value > 255:
        raise ConfigError(f"config field {key} is outside the byte range")
    return value


def adapt_config(text: str) -> str:
    """Return a dsd 0.12-compatible config without changing extraction paths."""
    if re.search(r"^padding:\s*$", text, re.MULTILINE):
        return text if text.endswith("\n") else text + "\n"

    section_padding = _scalar(text, "section_padding_value")
    file_image_padding = _scalar(text, "file_image_padding_value")
    padding = (
        "padding:\n"
        f"  arm9: {section_padding}\n"
        f"  arm9_overlay_table: {section_padding}\n"
        f"  arm9_overlays: {section_padding}\n"
        f"  arm7: {section_padding}\n"
        f"  arm7_overlay_table: {section_padding}\n"
        f"  arm7_overlays: {section_padding}\n"
        f"  fnt: {section_padding}\n"
        f"  fat: {section_padding}\n"
        f"  banner: {section_padding}\n"
        f"  file_image: {file_image_padding}\n"
        f"  rom: {section_padding}\n"
    )
    separator = "" if not text or text.endswith("\n\n") else "\n"
    if text and not text.endswith("\n"):
        separator = "\n\n"
    return text + separator + padding


def prepare_config(source: Path, destination: Path) -> None:
    if source.resolve() == destination.resolve():
        raise ConfigError("output config must not overwrite the extracted source config")
    try:
        adapted = adapt_config(source.read_text(encoding="utf-8"))
        destination.write_text(adapted, encoding="utf-8")
    except OSError as error:
        raise ConfigError(str(error)) from error


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create a local dsd 0.12.x config sidecar from ds-rom extraction metadata."
    )
    parser.add_argument("source", type=Path, help="extracted ROM config.yaml")
    parser.add_argument("destination", type=Path, help="new sidecar path; source is never modified")
    args = parser.parse_args()
    try:
        prepare_config(args.source, args.destination)
    except ConfigError as error:
        print(f"Cannot prepare dsd config: {error}", file=sys.stderr)
        return 2
    print(f"Prepared local dsd config: {args.destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
