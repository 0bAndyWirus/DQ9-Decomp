#!/usr/bin/env python3
"""Exercise launcher attract lifecycle and compare 600 gameplay frames in isolation."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from checkpoint import hashes, load_meta, segment, sha
from dq9_config import godot_config_path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--ladder', type=Path, required=True)
    parser.add_argument('--segment', required=True)
    parser.add_argument('--godot', type=Path, default=Path(os.environ.get('GODOT', str(Path.home() / 'Applications/godot/Godot_v4.5-stable_linux.x86_64'))))
    parser.add_argument('--verbose', action='store_true', help='request Godot ObjectDB leak details')
    args = parser.parse_args()
    try:
        rom = args.rom.resolve(strict=True)
        godot = args.godot.resolve(strict=True)
        seg = segment(args.ladder.resolve(strict=True), args.segment)
        meta = load_meta(seg)
        if meta['frames'] < 600:
            raise ValueError('checkpoint must contain at least 600 frames')
        if meta['extra_args']:
            raise ValueError('checkpoint extra_args are unsupported by Godot')
        with tempfile.TemporaryDirectory(prefix='dq9-attract-regression-') as tmp:
            work = Path(tmp)
            save_a, save_b = work / 'baseline.sav', work / 'attract.sav'
            hash_a, hash_b = work / 'baseline.hashes', work / 'attract.hashes'
            shutil.copyfile(seg / 'start.sav', save_a)
            shutil.copyfile(seg / 'start.sav', save_b)
            env = os.environ.copy()
            env.pop('DQ9_SAVE', None)
            for key, directory in [('XDG_DATA_HOME', 'data'), ('XDG_CONFIG_HOME', 'config'), ('XDG_CACHE_HOME', 'cache')]:
                env[key] = str(work / directory)
            env['DQ9_ABI_CONFIG'] = str(godot_config_path(work))
            command = ['xvfb-run', '-a', str(godot), '--path', str(ROOT / 'godot'),
                       '--script', 'res://tests/test_attract_regression.gd', '--',
                       '--rom', str(rom), '--save-a', str(save_a), '--save-b', str(save_b),
                       '--source-save', str((seg / 'start.sav').resolve()),
                       '--input', str((seg / 'input.txt').resolve()),
                       '--hash-a', str(hash_a), '--hash-b', str(hash_b),
                       '--language', meta['language'], '--rtc-epoch', str(meta['rtc_epoch']),
                       '--rtc-timezone-offset', str(meta['rtc_timezone_offset_minutes'])]
            if args.verbose:
                command.insert(3, '--verbose')
            result = subprocess.run(command, env=env, timeout=600, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, errors='replace')
            print(result.stdout, end='', flush=True)
            if result.returncode:
                print(f'FAIL: Godot exited {result.returncode}', file=sys.stderr)
                return 1
            if 'ObjectDB instances leaked at exit' in result.stdout or 'resources still in use at exit' in result.stdout:
                print('FAIL: Godot reported leaked objects or resources at exit', file=sys.stderr)
                return 1
            baseline, attracted = hashes(hash_a, 600), hashes(hash_b, 600)
            different = [frame for frame in range(600) if baseline[frame] != attracted[frame]]
            if different or sha(save_a) != sha(save_b):
                print(f'FAIL: {len(different)} differing frames' + (f', first {different[0]}' if different else '') + ('; save differs' if sha(save_a) != sha(save_b) else ''), file=sys.stderr)
                return 1
            print('PASS: 600 top/bottom frame hashes and final saves match without and after attract')
            return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
