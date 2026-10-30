#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GODOT="${GODOT:-godot}"
ROM="${DQ9_ROM:-}"
SAVE="${DQ9_SAVE:-}"
INPUT=""
FRAMES=120
CHECKPOINTS=""
COLD_CACHE=false
EXTENSION_BIN="${DQ9_GODOT_BIN:-$ROOT/godot/bin}"
NATIVE="${DQ9_NATIVE:-$ROOT/build/game-fast/game/dq9}"
usage() {
  cat <<'EOF'
Usage: tools/godot_smoke.sh --rom PATH --save PATH --input PATH --checkpoints LIST [--frames N]
       tools/godot_smoke.sh --cold-cache [--extension-bin PATH]
--cold-cache runs only the ROM-free fresh-registration check,
using a disposable source-only project (never deletes your project's .godot cache).
--extension-bin selects existing built libraries; defaults to DQ9_GODOT_BIN or godot/bin.
Select an already available compatible image with --rom PATH or DQ9_ROM.
The source save (--save or DQ9_SAVE), input script and comma-separated capture
checkpoints must be supplied. Frames default to 120 and must include every checkpoint.
The existing positional ROM SAVE INPUT FRAMES form remains accepted with --checkpoints.
Environment: GODOT (executable or PATH command), DQ9_NATIVE (native executable).
EOF
}
position=0
while (($#)); do
  case "$1" in
    --cold-cache) COLD_CACHE=true; shift ;;
    --extension-bin)
      (($# >= 2)) || { echo "$1 requires a value" >&2; exit 2; }
      EXTENSION_BIN=$2; shift 2 ;;
    --rom|--save|--input|--frames|--checkpoints)
      (($# >= 2)) || { echo "$1 requires a value" >&2; usage >&2; exit 2; }
      case "$1" in
        --rom) ROM=$2 ;;
        --save) SAVE=$2 ;;
        --input) INPUT=$2 ;;
        --frames) FRAMES=$2 ;;
        --checkpoints) CHECKPOINTS=$2 ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    -*)
      echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      case "$position" in
        0) ROM=$1 ;;
        1) SAVE=$1 ;;
        2) INPUT=$1 ;;
        3) FRAMES=$1 ;;
        *) echo "Unexpected argument: $1" >&2; usage >&2; exit 2 ;;
      esac
      position=$((position + 1)); shift ;;
  esac
done
if ! command -v "$GODOT" >/dev/null; then echo "Godot executable not found: $GODOT" >&2; exit 2; fi
if [[ "$COLD_CACHE" == true ]]; then
  [[ -d "$EXTENSION_BIN" ]] || { echo "Extension directory not found: $EXTENSION_BIN" >&2; exit 2; }
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/dq9-godot-cold.XXXXXX")"
  trap 'rm -rf "$WORK"' EXIT
  mkdir -p "$WORK/project/tests" "$WORK/project/bin"
  cp "$ROOT/godot/project.godot" "$ROOT/godot/dq9.gdextension" "$ROOT/godot/export_presets.cfg" "$WORK/project/"
  cp -R "$ROOT/godot/scripts" "$ROOT/godot/scenes" "$ROOT/godot/shaders" "$WORK/project/"
  cp "$ROOT/godot/tests/test_cold_extension.gd" "$WORK/project/tests/"
  # Copy libraries only, not arbitrary images/saves from a local bin directory.
  library_count=0
  for library in "$EXTENSION_BIN"/*.so "$EXTENSION_BIN"/*.dll "$EXTENSION_BIN"/*.dylib; do
    [[ -f "$library" ]] || continue
    cp "$library" "$WORK/project/bin/"
    library_count=$((library_count + 1))
  done
  ((library_count > 0)) || { echo "No extension libraries found: $EXTENSION_BIN" >&2; exit 2; }
  [[ ! -e "$WORK/project/.godot" ]]
  "$GODOT" --version
  "$GODOT" --headless --path "$WORK/project" --script res://tests/test_cold_extension.gd
  exit 0
fi
if [[ -z "$ROM" || -z "$SAVE" || -z "$INPUT" || -z "$CHECKPOINTS" ]]; then
  echo "--rom/DQ9_ROM, --save/DQ9_SAVE, --input and --checkpoints are required" >&2
  exit 2
fi
if [[ ! -f "$ROM" || ! -f "$SAVE" || ! -f "$INPUT" ]]; then
  echo "ROM, source save, and input script must be existing files" >&2
  exit 2
fi
[[ "$FRAMES" =~ ^[1-9][0-9]*$ ]] || { echo "--frames must be a positive integer" >&2; exit 2; }
[[ "$CHECKPOINTS" =~ ^[1-9][0-9]*(,[1-9][0-9]*)*$ ]] || { echo "--checkpoints must be a comma-separated list of positive frames" >&2; exit 2; }
IFS=, read -r -a checkpoint_frames <<<"$CHECKPOINTS"
for checkpoint in "${checkpoint_frames[@]}"; do
  ((checkpoint < FRAMES)) || { echo "Checkpoint $checkpoint must be below --frames $FRAMES" >&2; exit 2; }
done
# ROM-free host regressions are part of the same gate as exact emulation.
"$GODOT" --headless --path "$ROOT/godot" --script res://tests/test_extension_lifecycle.gd
"$GODOT" --headless --path "$ROOT/godot" --script res://tests/test_logic.gd
"$GODOT" --headless --path "$ROOT/godot" --script res://tests/test_issue_store.gd
if ! command -v xvfb-run >/dev/null; then echo "xvfb-run is required for the GPU compositor regression" >&2; exit 2; fi
xvfb-run -a "$GODOT" --path "$ROOT/godot" --script res://tests/test_gpu_composite.gd
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dq9-godot-smoke.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp "$SAVE" "$WORK/godot.sav"
ROM="$(realpath "$ROM")"
SAVE="$(realpath "$SAVE")"
INPUT="$(realpath "$INPUT")"
cp "$SAVE" "$WORK/native.sav"
cp "$SAVE" "$WORK/clicks.sav"
cp "$SAVE" "$WORK/restart.sav"
xvfb-run -a "$GODOT" --path "$ROOT/godot" --script res://tests/test_ui_layout.gd
cp "$SAVE" "$WORK/scene.sav"
cp "$SAVE" "$WORK/visible.sav"
xvfb-run -a "$GODOT" --path "$ROOT/godot" --script res://tests/test_ui_clicks.gd -- --rom "$ROM" --save "$WORK/clicks.sav" --autostart
xvfb-run -a "$GODOT" --path "$ROOT/godot" --script res://tests/test_restart.gd -- --rom "$ROM" --save "$WORK/restart.sav" --autostart
xvfb-run -a "$GODOT" --path "$ROOT/godot" --script res://tests/test_gpu_scene.gd -- "$ROM" "$WORK/scene.sav" "$INPUT" "$WORK/scene" "$FRAMES" "$CHECKPOINTS"
xvfb-run -a "$GODOT" --path "$ROOT/godot" --script res://tests/test_gpu_visible.gd -- --rom "$ROM" --save "$WORK/visible.sav" --app-input "$INPUT" --app-output "$WORK/visible" --app-checkpoints "$CHECKPOINTS"
xvfb-run -a -s "-screen 0 3072x1920x24" "$GODOT" --path "$ROOT/godot" --script res://tests/test_issue_reviewer.gd -- "$ROM" "$SAVE" "$INPUT" "$NATIVE" "${DQ9_REVIEW_OUTPUT:-$WORK/reviewer}"
if ! "$NATIVE" --headless --rom "$ROM" --save "$WORK/native.sav" --frames "$FRAMES" --input-script "$INPUT" --rtc-epoch 0 --rtc-timezone-offset 0 --hash-file "$WORK/native.hashes" >"$WORK/native.log" 2>&1; then
  printf 'Native DQ9-Remastered failed; log: %s\n' "$WORK/native.log" >&2
  cat "$WORK/native.log" >&2
  exit 1
fi
godot_status=0
"$GODOT" --headless --path "$ROOT/godot" --script res://tests/smoke.gd -- "$ROM" "$WORK/godot.sav" "$INPUT" "$FRAMES" "$WORK/godot.hashes" "$WORK/native.hashes" || godot_status=$?
checker_status=0
"$ROOT/.venv/bin/python" "$ROOT/tools/check_godot_hashes.py" "$WORK/native.hashes" "$WORK/godot.hashes" --frames "$FRAMES" || checker_status=$?
if (( checker_status != 0 )); then exit "$checker_status"; fi
exit "$godot_status"
