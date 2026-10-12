#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE="$ROOT/.venv/bin/cmake"
JOBS="${JOBS:-6}"
GODOT_CPP_DIR="${GODOT_CPP_DIR:-$HOME/Applications/godot/godot-cpp}"
GEN="${DQ9_GEN:-}"
ROM="${DQ9_ROM:-}"
SAVE="${DQ9_SAVE:-}"
INPUT=""
FRAMES=120
CHECKPOINTS=""
DO_SDL=1
DO_GODOT=1
DO_TEST=0
usage() {
  cat <<EOF
Usage: tools/build_both.sh [--sdl-only | --godot-only] [--gen DIR] [--jobs N] [--test]
       [--rom PATH] [--save PATH] [--input PATH] [--frames N] [--checkpoints LIST]

Separately supplied generated sources are required: --gen DIR or DQ9_GEN.
Godot --test also requires --rom/DQ9_ROM, --save/DQ9_SAVE, --input and --checkpoints.
Environment: GODOT (editor executable), GODOT_CPP_DIR (godot-cpp 4.5 checkout), JOBS (default 6).
EOF
}
while (($#)); do
  case "$1" in
    --sdl-only) DO_SDL=1; DO_GODOT=0; shift ;;
    --godot-only) DO_SDL=0; DO_GODOT=1; shift ;;
    --test) DO_TEST=1; shift ;;
    --gen|--jobs|--rom|--save|--input|--frames|--checkpoints)
      (($# >= 2)) || { echo "$1 requires a value" >&2; usage >&2; exit 2; }
      case "$1" in
        --gen) GEN=$2 ;;
        --jobs) JOBS=$2 ;;
        --rom) ROM=$2 ;;
        --save) SAVE=$2 ;;
        --input) INPUT=$2 ;;
        --frames) FRAMES=$2 ;;
        --checkpoints) CHECKPOINTS=$2 ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be a positive integer" >&2; exit 2; }
[[ -n "$GEN" ]] || { echo "Separately supplied generated sources are required; use --gen DIR or DQ9_GEN" >&2; exit 2; }
for cpu in arm9 arm7; do
  [[ -f "$GEN/$cpu/fn_table.cpp" ]] || { echo "Missing $GEN/$cpu/fn_table.cpp in the supplied generated-code directory" >&2; exit 1; }
done
GEN="$(cd "$GEN" && pwd)"
if ((DO_TEST && DO_GODOT)) && [[ -z "$ROM" || -z "$SAVE" || -z "$INPUT" || -z "$CHECKPOINTS" ]]; then
  echo "Godot --test requires --rom/DQ9_ROM, --save/DQ9_SAVE, --input and --checkpoints" >&2
  exit 2
fi
if ((DO_SDL)); then
  export CCACHE_BASEDIR="$ROOT" CCACHE_NOHASHDIR=1
  "$CMAKE" -S "$ROOT" -B "$ROOT/build/game-ui" "-DDQ9_GENERATED_DIR=$GEN" -DDQ9_BUILD_SDL=ON -DDQ9_BUILD_GODOT=OFF
  "$CMAKE" --build "$ROOT/build/game-ui" -j "$JOBS"
  if ((DO_TEST)); then "$ROOT/.venv/bin/ctest" --test-dir "$ROOT/build/game-ui" --output-on-failure; fi
fi
if ((DO_GODOT)); then
  if [[ ! -f "$GODOT_CPP_DIR/CMakeLists.txt" ]]; then
    echo "godot-cpp checkout missing at $GODOT_CPP_DIR (set GODOT_CPP_DIR)" >&2
    exit 2
  fi
  export CCACHE_BASEDIR="$ROOT" CCACHE_NOHASHDIR=1
  "$CMAKE" -S "$ROOT" -B "$ROOT/build/godot-ext" "-DDQ9_GENERATED_DIR=$GEN" -DDQ9_BUILD_SDL=OFF -DDQ9_BUILD_GODOT=ON -DGODOT_CPP_DIR="$GODOT_CPP_DIR"
  "$CMAKE" --build "$ROOT/build/godot-ext" -j "$JOBS"
  if ((DO_TEST)); then
    "$ROOT/tools/godot_smoke.sh" --rom "$ROM" --save "$SAVE" --input "$INPUT" --frames "$FRAMES" --checkpoints "$CHECKPOINTS"
  fi
fi
printf '\nRun SDL app:\n  %q --launcher\n' "$ROOT/build/game-ui/game/dq9"
printf 'Run Godot launcher:\n  %q --menu\n' "$ROOT/tools/run_godot.sh"
