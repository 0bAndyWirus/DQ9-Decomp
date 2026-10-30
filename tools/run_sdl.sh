#!/bin/sh
# Launch the SDL build from anywhere.
# Usage: tools/run_sdl.sh [--menu] [language] [dq9 options...]
# Select an image with --rom PATH or DQ9_ROM; --menu opens the launcher.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=$ROOT/build/game-ui/game/dq9
cd "$ROOT" # the launcher stores image paths relative to the project
[ -x "$BIN" ] || { echo "Missing $BIN: build it with tools/build_both.sh --sdl-only --gen DIR (or DQ9_GEN)" >&2; exit 1; }
MENU=0
if [ "${1:-}" = "--menu" ]; then MENU=1; shift; fi
LANGUAGE=en
case "${1:-}" in
  ''|-*) ;;
  *) LANGUAGE=$1; shift ;;
esac
ROM=${DQ9_ROM:-}
SAVE=${DQ9_SAVE:-}
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
if [ "$MENU" -eq 0 ] && [ "$HAS_SAVE" -eq 0 ] && [ -z "$SAVE" ]; then
  # The native host migrates and selects its own default. Only supply a path
  # for the old wrapper's HOME-based save when custom XDG paths hide it.
  data_base=${XDG_DATA_HOME:-$HOME/.local/share}
  if [ ! -f "$data_base/dq9-remastered/dq9.sav" ] &&
     [ ! -f "$data_base/dq9-decomp/dq9.sav" ] &&
     [ -f "$HOME/.local/share/dq9-decomp/dq9.sav" ]; then
    SAVE=$HOME/.local/share/dq9-decomp/dq9.sav
  fi
fi
if [ -n "$SAVE" ]; then
  mkdir -p "$(dirname "$SAVE")"
  [ "$HAS_SAVE" -eq 1 ] || set -- --save "$SAVE" "$@"
fi
[ "$HAS_ROM" -eq 1 ] || [ -z "$ROM" ] || set -- --rom "$ROM" "$@"
if [ "$MENU" -eq 1 ]; then exec "$BIN" --launcher "$@"; fi
exec "$BIN" --language "$LANGUAGE" --debug-tool "$@"
