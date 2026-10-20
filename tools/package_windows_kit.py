#!/usr/bin/env python3
"""Archive HEAD as a public Windows source kit, never a generated game build."""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_SUFFIXES = {".nds", ".sav", ".bin", ".exe", ".dll"}
ROOT_FILES = {
    "BUILD-DQ9.bat": "tools/windows/BUILD-DQ9.bat",
    "PLAY-DQ9.bat": "tools/windows/PLAY-DQ9.bat",
    "BUILD-GODOT.bat": "tools/windows/BUILD-GODOT.bat",
    "PLAY-GODOT.bat": "tools/windows/PLAY-GODOT.bat",
    "README-WINDOWS.txt": "packaging/windows/README-WINDOWS.txt",
}


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def extract_head(archive: Path, staging: Path) -> set[str]:
    """Git supplies the archive; still refuse links or paths outside staging."""
    paths = set()
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            destination = (staging / member.name).resolve()
            if not destination.is_relative_to(staging.resolve()):
                raise ValueError(f"Unsafe git archive member: {member.name}")
            if not (member.isfile() or member.isdir()):
                raise ValueError(f"Unsupported git archive member: {member.name}")
            if member.isfile():
                paths.add(member.name)
        if hasattr(tarfile, "data_filter"):
            source.extractall(staging, filter="data")
        else:
            source.extractall(staging)
    return paths


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True, help="release label, for example v0.2.0")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.tag):
        parser.error("--tag must contain only letters, digits, dots, underscores and hyphens")
    dirty = git("status", "--porcelain", "--untracked-files=no")
    if dirty.strip():
        print("WARNING: tracked files have uncommitted changes; source is archived from HEAD. "
              "Root launchers and README are copied from this checkout.", file=sys.stderr)

    dist = ROOT / "build" / "dist"
    name = f"dq9-decomp-windows-kit-{args.tag}"
    staging = dist / name
    output = dist / f"{name}.zip"
    if staging.exists() or output.exists():
        raise SystemExit(f"Refusing to overwrite an existing kit: {staging} or {output}")
    dist.mkdir(parents=True, exist_ok=True)
    staging.mkdir()
    with tempfile.TemporaryDirectory(prefix="windows-kit-", dir=dist) as temporary:
        temporary = Path(temporary)
        archive = temporary / "head.tar"
        subprocess.run(["git", "archive", "--format=tar", f"--output={archive}", "HEAD"],
                       cwd=ROOT, check=True)
        head_paths = extract_head(archive, staging)
        for destination, source in ROOT_FILES.items():
            shutil.copyfile(ROOT / source, staging / destination)

        forbidden = [item.relative_to(staging).as_posix() for item in staging.rglob("*")
                     if item.is_file() and item.suffix.lower() in FORBIDDEN_SUFFIXES]
        if forbidden:
            raise SystemExit("Source kit contains forbidden binary/private files:\n" + "\n".join(sorted(forbidden)))

        # Only HEAD members that are also tracked may bypass the generic C++ heuristic.
        tracked = set(git("ls-files", "-z").decode("utf-8").split("\0"))
        allowlist = temporary / "head-paths.json"
        allowlist.write_text(json.dumps(sorted(head_paths & tracked)), encoding="utf-8")
        subprocess.run([sys.executable, str(ROOT / "tools/audit_package.py"),
                        "--source-tree", str(allowlist), str(staging)], cwd=ROOT, check=True)

        with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as kit:
            for item in sorted(staging.rglob("*")):
                if item.is_file():
                    kit.write(item, (Path(name) / item.relative_to(staging)).as_posix())
    print(f"Source and scripts only: {output}")
    print("No game image, generated code or game executable may be added to this kit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
