#!/usr/bin/env python3
"""Exercise --dump-frames before ROM loading using a locally built Linux dq9."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile


LIMIT_MESSAGE = "dq9: --dump-frames is limited to 1000000 frames total"


def limit_resources() -> None:
    # A regression to the old unbounded loop must fail without exhausting the host.
    resource.setrlimit(resource.RLIMIT_AS, (512 * 1024 * 1024, 512 * 1024 * 1024))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dq9", type=Path, required=True)
    args = parser.parse_args()
    executable = args.dq9.resolve()
    env = os.environ.copy()
    env.pop("DQ9_ROM", None)
    env.pop("DQ9_ABI_CONFIG", None)
    rejected = [
        ["--dump-frames", "0-9223372036854775807"],
        ["--dump-frames", "-9223372036854775808-9223372036854775807"],
        ["--dump-frames", "0-1000000"],
        ["--dump-frames", "0-2000000/2"],
        ["--dump-frames", "0-499999,500000-1000000"],
        ["--dump-frames", "0-499999", "--dump-frames", "500000-1000000"],
        ["--dump-frames", "0-499999", "--dump-frames", "0-500000"],
    ]
    accepted = [
        ["--dump-frames", "9223372036854775807-9223372036854775807"],
        ["--dump-frames", "9223372036854775804-9223372036854775807/2"],
        ["--dump-frames", "-9223372036854775808-9223372036854775807/9223372036854775807"],
        ["--dump-frames", "0-499999", "--dump-frames", "500000-999999"],
    ]
    with tempfile.TemporaryDirectory(prefix="dq9-dump-frames-") as temp:
        for options in rejected:
            result = subprocess.run([str(executable), "--headless", *options],
                                    cwd=temp, env=env, text=True, capture_output=True,
                                    timeout=15, preexec_fn=limit_resources)
            if result.returncode != 2 or LIMIT_MESSAGE not in result.stderr:
                raise AssertionError(
                    f"{options!r}: expected status 2 and frame-limit message, "
                    f"got {result.returncode}\n{result.stdout}\n{result.stderr}")
            if "no ROM given" in result.stderr:
                raise AssertionError("frame-limit rejection must precede the ROM requirement")
        for options in accepted:
            result = subprocess.run([str(executable), *options, "--help"],
                                    cwd=temp, env=env, text=True, capture_output=True,
                                    timeout=15, preexec_fn=limit_resources)
            if result.returncode != 0 or "usage: dq9" not in result.stdout:
                raise AssertionError(
                    f"{options!r}: valid frame selection did not reach --help, "
                    f"got {result.returncode}\n{result.stdout}\n{result.stderr}")
    print("ROM-free dump-frame CLI regressions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
