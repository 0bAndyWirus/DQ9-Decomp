#!/usr/bin/env python3
"""Add (or remove) an application-menu entry for the locally built dq9 on Linux.

The entry starts `dq9 --launcher`, which remembers your ROM path and options. It references
your local build only; nothing from the game is copied and no icon is installed.

Usage:
  python tools/install_desktop_entry.py                 # ~/.local/share/applications/dq9-remastered.desktop
  python tools/install_desktop_entry.py --uninstall
  python tools/install_desktop_entry.py --exe build/game/game/dq9 --dir /tmp/apps   # custom locations
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTRY_NAME = "dq9-remastered.desktop"


def quote_exec_argument(argument: str) -> str:
    """Desktop Entry Exec quoting: double quotes, backslash-escape \" ` $ and \\, and write % as %%."""
    escaped = "".join("\\" + c if c in '"`$\\' else ("%%" if c == "%" else c) for c in argument)
    return f'"{escaped}"'


def desktop_entry(exe: Path, abi: Path | None) -> str:
    command = [quote_exec_argument(str(exe)), "--launcher"]
    if abi is not None:
        command += ["--abi-config", quote_exec_argument(str(abi))]
    return "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        "Name=DQ9-Remastered",
        "Comment=Native host for a locally built Dragon Quest IX (bring your own image)",
        "Exec=" + " ".join(command),
        f"Path={exe.parent}",
        "Terminal=false",
        "Categories=Game;",
        "StartupNotify=true",
        "",
    ])


def applications_dir() -> Path:
    data_home = os.environ.get("XDG_DATA_HOME")
    return (Path(data_home) if data_home else Path.home() / ".local" / "share") / "applications"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--exe", type=Path, default=ROOT / "build" / "game" / "game" / "dq9")
    p.add_argument("--abi-config", type=Path, default=ROOT / "build" / "gen" / "abi.ini")
    p.add_argument("--dir", type=Path, default=None, help="applications directory (default: XDG data home)")
    p.add_argument("--uninstall", action="store_true")
    args = p.parse_args(argv)
    apps = args.dir or applications_dir()
    target = apps / ENTRY_NAME
    legacy = apps / "dq9-decomp.desktop"
    exe = args.exe.resolve()
    abi = args.abi_config.resolve()
    # Only remove an unmodified entry for this installation, not another user's
    # customized shortcut or an entry pointing to another checkout.
    legacy_contents = {
        desktop_entry(exe, option).replace("Name=DQ9-Remastered\n", "Name=DQ9-Decomp\n").encode("utf-8")
        for option in (None, abi)
    }
    def remove_legacy() -> None:
        if legacy.is_file() and legacy.read_bytes() in legacy_contents:
            legacy.unlink()
    if args.uninstall:
        target.unlink(missing_ok=True)
        remove_legacy()
        print(f"removed {target}")
        return 0
    if not exe.is_file():
        print(f"{exe} does not exist; build it first with tools/build_port.py", file=sys.stderr)
        return 1
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(desktop_entry(exe, abi if abi.is_file() else None), encoding="utf-8")
    remove_legacy()
    print(f"installed {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
