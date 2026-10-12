"""Host-only paths, advisory locks and process affinity for native tooling."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
from typing import IO


def executable_name(name: str, *, windows: bool | None = None) -> str:
    windows = os.name == "nt" if windows is None else windows
    return name + ".exe" if windows and not name.lower().endswith(".exe") else name


def venv_program(root: Path, name: str, *, windows: bool | None = None) -> Path:
    windows = os.name == "nt" if windows is None else windows
    return root / ".venv" / ("Scripts" if windows else "bin") / executable_name(name, windows=windows)


def data_root(root: Path) -> Path:
    if (root / "ROM").is_dir():
        return root
    # Development worktrees live under the primary checkout's build/ directory.
    if (root.parent.parent / "ROM").is_dir():
        return root.parent.parent
    return root


def lock_file(handle: IO, *, unlock: bool = False) -> None:
    """Lock one shared integration lock file nonblocking; OSError means busy."""
    if os.name == "nt":
        import msvcrt
        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK if unlock else msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(handle, fcntl.LOCK_UN if unlock else fcntl.LOCK_EX | fcntl.LOCK_NB)


def _set_windows_affinity(process: subprocess.Popen, cpu: int) -> None:
    import ctypes
    from ctypes import wintypes
    if not 0 <= cpu < ctypes.sizeof(ctypes.c_size_t) * 8:
        raise ValueError("CPU affinity index is outside the Windows process mask")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    set_mask = kernel.SetProcessAffinityMask
    set_mask.argtypes = (wintypes.HANDLE, ctypes.c_size_t)
    set_mask.restype = wintypes.BOOL
    if not set_mask(wintypes.HANDLE(process._handle), ctypes.c_size_t(1 << cpu)):
        raise ctypes.WinError(ctypes.get_last_error())


def run_with_affinity(command: list[str], cpu: int | None = None, **kwargs) -> subprocess.CompletedProcess:
    """Run with an optional CPU pin; retain taskset's existing Linux semantics."""
    if cpu is None or cpu == -1:
        return subprocess.run(command, **kwargs)
    if os.name != "nt":
        return subprocess.run(["taskset", "-c", str(cpu), *command], **kwargs)
    check = kwargs.pop("check", False)
    # No command-shell wrapper: ROM/executable paths can contain spaces, '&',
    # '%' and Unicode. Pin immediately after process creation, before waiting.
    with subprocess.Popen(command, **kwargs) as process:
        try:
            _set_windows_affinity(process, cpu)
        except BaseException:
            process.kill()
            process.wait()
            raise
        stdout, stderr = process.communicate()
        result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if check:
        result.check_returncode()
    return result
