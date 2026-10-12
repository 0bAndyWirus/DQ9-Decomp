#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "${DQ9_ZIG_PYTHON:-$ROOT/.venv/bin/python}" -m ziglang c++ -target x86_64-windows-gnu "$@"
