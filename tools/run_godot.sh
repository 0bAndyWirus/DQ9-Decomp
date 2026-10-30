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
SAVE=${DQ9_SAVE:-}
if [ -z "$SAVE" ] && [ "$MENU" -eq 0 ]; then
  data_base=${XDG_DATA_HOME:-$HOME/.local/share}
  SAVE=$data_base/dq9-remastered/godot.sav
  if [ ! -f "$SAVE" ]; then
    if [ -f "$data_base/dq9-decomp/godot.sav" ]; then
      SAVE=$data_base/dq9-decomp/godot.sav
    elif [ -f "$HOME/.local/share/dq9-decomp/godot.sav" ]; then
      SAVE=$HOME/.local/share/dq9-decomp/godot.sav
    fi
  fi
fi
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
[ -z "$SAVE" ] || mkdir -p "$(dirname "$SAVE")"
[ "$HAS_ROM" -eq 1 ] || [ -z "$ROM" ] || set -- --rom "$ROM" "$@"
[ "$HAS_SAVE" -eq 1 ] || [ -z "$SAVE" ] || set -- --save "$SAVE" "$@"
set -- --language "$LANGUAGE" "$@"
[ "$MENU" -eq 1 ] || set -- --autostart "$@"
# Import missing resources before starting the game. The .import files name the
# cache artifacts, which may be absent even when the cache directory exists.
need_import=0
for metadata in "$ROOT"/godot/assets/*/*.import; do
  [ -f "$metadata" ] || continue
  imported=
  while IFS= read -r line; do
    case "$line" in
      path=\"res://*\") imported=${line#path=\"res://}; imported=${imported%\"}; break ;;
    esac
  done < "$metadata"
  if [ -z "$imported" ] || [ ! -f "$ROOT/godot/$imported" ]; then need_import=1; break; fi
done
if [ "$need_import" -eq 1 ]; then
  # Import in a scratch project without the native extension: Godot's editor
  # can abort while generating extension documentation on a cold project.
  scratch=$(mktemp -d "${TMPDIR:-/tmp}/dq9-godot-import.XXXXXXXX")
  trap 'rm -rf "$scratch"' EXIT HUP INT TERM
  cp -R "$ROOT/godot/assets" "$scratch/assets"
  printf 'config_version=5\n' > "$scratch/project.godot"
  "$GODOT" --headless --path "$scratch" --editor --import --quit
  [ -d "$scratch/.godot/imported" ] || { echo "Godot did not create an imported resource cache" >&2; exit 1; }
  mkdir -p "$ROOT/godot/.godot/imported"
  cp -R "$scratch/.godot/imported/." "$ROOT/godot/.godot/imported/"
  rm -rf "$scratch"
  trap - EXIT HUP INT TERM
  for metadata in "$ROOT"/godot/assets/*/*.import; do
    [ -f "$metadata" ] || continue
    imported=
    while IFS= read -r line; do
      case "$line" in
        path=\"res://*\") imported=${line#path=\"res://}; imported=${imported%\"}; break ;;
      esac
    done < "$metadata"
    [ -n "$imported" ] && [ -f "$ROOT/godot/$imported" ] || {
      echo "Godot failed to import $metadata" >&2; exit 1;
    }
  done
fi
exec "$GODOT" --path "$ROOT/godot" -- "$@"

