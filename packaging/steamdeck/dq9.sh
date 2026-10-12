#!/usr/bin/env bash
# Steam Deck launcher for DQ9-Decomp.
#
# Local image selection: --rom on the command line, $DQ9_ROM, then ROM= in
# ${XDG_CONFIG_HOME:-~/.config}/dq9-decomp/launcher.conf.
# All other arguments are passed through to bin/dq9.
#
#   dq9.sh --install-desktop   install a menu entry pointing at this bundle
set -u

here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/dq9-decomp"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/dq9-decomp"
launcher_conf="$config_dir/launcher.conf"

# Errors must be visible in Gaming Mode, where stderr goes nowhere useful:
# log them and show a dialog when a dialog tool exists.
fail() {
    mkdir -p "$data_dir"
    printf 'dq9.sh: %s\n' "$1" | tee -a "$data_dir/launcher.log" >&2
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title="DQ9-Decomp" --width=480 --text="$1" 2>/dev/null
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DQ9-Decomp" --error "$1" 2>/dev/null
    fi
    exit 1
}

if [ "${1:-}" = "--install-desktop" ]; then
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    mkdir -p "$apps"
    sed -e "s|^Exec=.*|Exec=\"$here/dq9.sh\"|" -e "s|^Path=.*|Path=$here|" \
        "$here/dq9.desktop" > "$apps/dq9-decomp.desktop"
    echo "installed $apps/dq9-decomp.desktop"
    exit 0
fi

rom_given=0
for arg in "$@"; do
    [ "$arg" = "--rom" ] && rom_given=1
done

args=()
if [ "$rom_given" = 0 ]; then
    rom="${DQ9_ROM:-}"
    if [ -z "$rom" ] && [ -f "$launcher_conf" ]; then
        rom=$(sed -n 's/^[[:space:]]*ROM[[:space:]]*=[[:space:]]*//p' "$launcher_conf" | tail -n 1)
        rom="${rom/#\~/$HOME}"
    fi
    if [ -z "$rom" ] || [ ! -f "$rom" ]; then
        fail "Select an already locally available compatible image with --rom or DQ9_ROM.

You can also save its local path as ROM= in:
  $launcher_conf"
    fi
    args=(--rom "$rom")
fi

if ldd "$here/bin/dq9" 2>/dev/null | grep -q 'not found'; then
    fail "missing system libraries:
$(ldd "$here/bin/dq9" | grep 'not found')"
fi

# Deck-friendly SDL defaults; anything already set in the environment (for
# example via Steam launch options) wins.
: "${SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS:=0}"      # Gamescope focus changes must not iconify
: "${SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS:=1}"  # keep pad input while the Steam overlay has focus
: "${SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=0}"  # positional A/B/X/Y like the DS
: "${SDL_TOUCH_MOUSE_EVENTS:=0}"                # touchscreen arrives once, as finger events
export SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS \
       SDL_GAMECONTROLLER_USE_BUTTON_LABELS SDL_TOUCH_MOUSE_EVENTS

exec "$here/bin/dq9" "${args[@]}" "$@"
