#!/bin/sh
# Launch the Godot build from anywhere.
# Usage: tools/run_godot.sh [--menu] [language] [Godot user options...]
# Select an image with --rom PATH or DQ9_ROM; --menu opens the start/settings screen.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
GODOT=${GODOT:-godot}
command -v "$GODOT" >/dev/null || { echo "Godot 4.5 not found: $GODOT (set GODOT=...)" >&2; exit 1; }
[ -f "$ROOT/godot/bin/libdq9_godot.so" ] || { echo "Missing extension: build it with tools/build_both.sh --godot-only --gen DIR (or DQ9_GEN)" >&2; exit 1; }
MENU=0
if [ "${1:-}" = "--menu" ]; then MENU=1; shift; fi
LANGUAGE=en
case "${1:-}" in
  ''|-*) ;;
  *) LANGUAGE=$1; shift ;;
esac
ROM=${DQ9_ROM:-}
SAVE=${DQ9_SAVE:-$HOME/.local/share/dq9-decomp/godot.sav}
HAS_ROM=0
HAS_SAVE=0
EXPECT=
for argument in "$@"; do
  case "$EXPECT" in
    --rom) ROM=$argument; HAS_ROM=1; EXPECT= ;;
    --save) SAVE=$argument; HAS_SAVE=1; EXPECT= ;;
    *)
      case "$argument" in --rom|--save) EXPECT=$argument ;; esac ;;
  esac
done
[ -z "$EXPECT" ] || { echo "$EXPECT requires a value" >&2; exit 2; }
if [ -z "$ROM" ] && [ "$MENU" -eq 0 ]; then
  echo "Select an already available compatible image with --rom PATH or DQ9_ROM, or use --menu" >&2
  exit 1
fi
mkdir -p "$(dirname "$SAVE")"
[ "$HAS_ROM" -eq 1 ] || [ -z "$ROM" ] || set -- --rom "$ROM" "$@"
[ "$HAS_SAVE" -eq 1 ] || set -- --save "$SAVE" "$@"
set -- --language "$LANGUAGE" "$@"
[ "$MENU" -eq 1 ] || set -- --autostart "$@"
exec "$GODOT" --path "$ROOT/godot" -- "$@"
