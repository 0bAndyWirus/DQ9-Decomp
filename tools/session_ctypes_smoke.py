#!/usr/bin/env python3
"""Run the dsr_session C ABI headlessly and compare dq9 per-frame hashes."""
from __future__ import annotations

import argparse
import ctypes
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

class Input(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_uint16), ("pen_x", ctypes.c_int32),
                ("pen_y", ctypes.c_int32), ("pen_down", ctypes.c_uint8),
                ("lid_closed", ctypes.c_uint8)]

class Config(ctypes.Structure):
    _fields_ = [("rom_path", ctypes.c_char_p), ("save_path", ctypes.c_char_p),
                ("language", ctypes.c_uint8), ("deterministic_rtc", ctypes.c_uint8),
                ("rtc_epoch_seconds", ctypes.c_int64),
                ("rtc_timezone_offset_minutes", ctypes.c_int32),
                ("backup_type", ctypes.c_char_p)]  # appended in session_c.h; NULL keeps the ABI/save-size choice

def fnv_screen(pixels: ctypes.POINTER(ctypes.c_uint32)) -> int:
    value = 0xCBF29CE484222325
    for i in range(256 * 192):
        pixel = pixels[i]
        for shift in (16, 8, 0):
            value ^= (pixel >> shift) & 0xFF
            value = value * 0x100000001B3 & 0xFFFFFFFFFFFFFFFF
    return value

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--dq9", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=300)
    args = parser.parse_args()
    missing = [name for name in ("rom", "save", "script") if not getattr(args, name).is_file()]
    if missing:
        # CTest SKIP_RETURN_CODE: the local game fixtures are optional and never shipped.
        print("SKIP: no local fixture file for --" + ", --".join(missing))
        return 77
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.dsr_session_create.argtypes = [ctypes.POINTER(Config), ctypes.c_char_p, ctypes.c_size_t]
    lib.dsr_session_create.restype = ctypes.c_void_p
    lib.dsr_session_destroy.argtypes = [ctypes.c_void_p]
    lib.dsr_session_step.argtypes = [ctypes.c_void_p, ctypes.POINTER(Input), ctypes.c_char_p, ctypes.c_size_t]
    lib.dsr_session_step.restype = ctypes.c_int
    lib.dsr_session_get_screen.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.dsr_session_get_screen.restype = ctypes.POINTER(ctypes.c_uint32)
    lib.dsr_session_frame_count.argtypes = [ctypes.c_void_p]
    lib.dsr_session_frame_count.restype = ctypes.c_uint64

    with tempfile.TemporaryDirectory(prefix="dsr-session-ctypes-") as temp:
        tmp = Path(temp)
        session_save = tmp / "session.sav"
        shutil.copyfile(args.save, session_save)
        hash_file = tmp / "dq9.hashes"
        dq9_save = tmp / "dq9.sav"
        shutil.copyfile(args.save, dq9_save)
        command = [str(args.dq9.resolve()), "--rom", str(args.rom.resolve()), "--save", str(dq9_save),
                   "--headless", "--frames", str(args.frames), "--input-script", str(args.script.resolve()),
                   "--rtc-epoch", "0", "--rtc-timezone-offset", "0", "--hash-file", str(hash_file)]
        result = subprocess.run(command, check=False)
        if result.returncode:
            raise RuntimeError(f"dq9 exited {result.returncode}")
        reference = [tuple(int(part, 16) for part in line.split()[1:])
                     for line in hash_file.read_text().splitlines()]

        config = Config(os.fsencode(args.rom.resolve()), os.fsencode(session_save), 1, 1, 0, 0)
        error = ctypes.create_string_buffer(1024)
        handle = lib.dsr_session_create(ctypes.byref(config), error, len(error))
        if not handle:
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            script = {}
            for line in args.script.read_text().splitlines():
                parts = line.split()
                if len(parts) == 4:
                    frame, mask, x, y = map(int, parts)
                    script[frame] = (mask, x, y)
            observed = []
            for frame in range(args.frames):
                mask, x, y = script.get(frame, (0, -1, -1))
                state = Input(mask, x, y, int(x >= 0 and y >= 0), 0)
                if lib.dsr_session_step(handle, ctypes.byref(state), error, len(error)):
                    raise RuntimeError(error.value.decode(errors="replace"))
                observed.append((fnv_screen(lib.dsr_session_get_screen(handle, 0)),
                                 fnv_screen(lib.dsr_session_get_screen(handle, 1))))
            if lib.dsr_session_frame_count(handle) != args.frames:
                raise RuntimeError(f"session frame counter mismatch: {lib.dsr_session_frame_count(handle)}")
            if len(reference) != args.frames or observed != reference:
                mismatch = next((i for i, pair in enumerate(zip(observed, reference)) if pair[0] != pair[1]), None)
                raise RuntimeError(f"hash mismatch at frame {mismatch}; ctypes={len(observed)}, dq9={len(reference)}")
            print(f"PASS: {args.frames} frames; top and bottom hashes match dq9")
        finally:
            lib.dsr_session_destroy(handle)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
