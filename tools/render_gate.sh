#!/bin/sh
# Build and compare frame hashes with caller-supplied replay baselines.
# BUILD_DIR must already be configured with separately supplied generated code.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
B=build/game-ui
BUILD_SET=0
QUICK=0
ROM=${DQ9_ROM:-}
SAVE=${DQ9_SAVE:-}
INPUT=
FRAMES=
BASELINE=
LONG_FRAMES=
LONG_BASELINE=
usage() {
  cat <<'EOF'
Usage: tools/render_gate.sh [BUILD_DIR] [--quick] --rom PATH --save PATH --input PATH
       --frames N --baseline FILE [--long-frames N --long-baseline FILE]
Select an already available compatible image with --rom PATH or DQ9_ROM.
The source save may also be selected with DQ9_SAVE. Replay lengths, input and
baseline hashes are caller-supplied; --quick skips an optional longer comparison.
EOF
}
while [ "$#" -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1; shift ;;
    --rom|--save|--input|--frames|--baseline|--long-frames|--long-baseline)
      [ "$#" -ge 2 ] || { echo "$1 requires a value" >&2; usage >&2; exit 2; }
      case "$1" in
        --rom) ROM=$2 ;;
        --save) SAVE=$2 ;;
        --input) INPUT=$2 ;;
        --frames) FRAMES=$2 ;;
        --baseline) BASELINE=$2 ;;
        --long-frames) LONG_FRAMES=$2 ;;
        --long-baseline) LONG_BASELINE=$2 ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      [ "$BUILD_SET" -eq 0 ] || { echo "Unexpected argument: $1" >&2; exit 2; }
      B=$1; BUILD_SET=1; shift ;;
  esac
done
if [ -z "$ROM" ] || [ -z "$SAVE" ] || [ -z "$INPUT" ] || [ -z "$FRAMES" ] || [ -z "$BASELINE" ]; then
  echo "--rom/DQ9_ROM, --save/DQ9_SAVE, --input, --frames and --baseline are required" >&2
  exit 2
fi
positive_integer() {
  case "$1" in ''|0*|*[!0-9]*) return 1 ;; *) return 0 ;; esac
}
positive_integer "$FRAMES" || { echo "--frames must be a positive integer" >&2; exit 2; }
for path in "$ROM" "$SAVE" "$INPUT" "$BASELINE"; do
  [ -f "$path" ] || { echo "Required replay file not found: $path" >&2; exit 2; }
done
if [ -n "$LONG_FRAMES" ] || [ -n "$LONG_BASELINE" ]; then
  positive_integer "$LONG_FRAMES" && [ -f "$LONG_BASELINE" ] || {
    echo "--long-frames requires a positive integer and --long-baseline an existing file" >&2
    exit 2
  }
fi
export CCACHE_BASEDIR=$PWD CCACHE_NOHASHDIR=1
.venv/bin/cmake --build "$B" -j6 >"$B/gate-build.log" 2>&1 || { echo "BUILD FAILED: see $B/gate-build.log"; exit 1; }
.venv/bin/ctest --test-dir "$B" >"$B/gate-ctest.log" 2>&1 || { echo "CTEST FAILED: see $B/gate-ctest.log"; exit 1; }
check() {
  frames=$1; ref=$2
  work=$(mktemp -d "${TMPDIR:-/tmp}/dq9-gate.XXXXXX")
  cp "$SAVE" "$work/s.sav"
  "$B/game/dq9" --headless --rom "$ROM" --save "$work/s.sav" --frames "$frames" \
    --input-script "$INPUT" --rtc-epoch 0 --rtc-timezone-offset 0 \
    --hash-file "$work/h.txt" >"$work/run.log" 2>&1 || { echo "RUN FAILED ($frames): $work/run.log"; exit 1; }
  if cmp -s "$work/h.txt" "$ref"; then echo "PASS: $frames frames identical to baseline"; rm -rf "$work";
  else echo "FAIL: $frames frames differ from baseline ($work/h.txt vs $ref)"; exit 1; fi
}
check "$FRAMES" "$BASELINE"
if [ "$QUICK" -ne 1 ] && [ -n "$LONG_FRAMES" ]; then check "$LONG_FRAMES" "$LONG_BASELINE"; fi
echo "GATE OK"
