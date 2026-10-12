# DQ9-Decomp — Godot host

This Godot 4.5 host uses the `dq9_godot` GDExtension and the shared native runtime; the SDL application remains a separate build. This source project distributes no binaries or generated game code. Select an already locally available compatible image; do not copy image files or local session data into this directory or commit them.

## Build and run

The extension build requires a separately supplied generated-code directory. Enable `DQ9_BUILD_GODOT` in the root CMake build and set `GODOT_CPP_DIR` to an external godot-cpp 4.5 checkout. The build writes the extension to `godot/bin/`; `dq9.gdextension` loads it when present. Open this directory in Godot 4.5, or run:

```sh
godot --path godot
# Optional arguments after --:
godot --path godot -- --rom "$DQ9_ROM" --save user://dq9.sav --language en --autostart
```

Select a local image using `--rom`, `DQ9_ROM` when no saved path exists, or Browse. `--rom` overrides the saved selection. The launcher remembers paths, language, layout, filter/scaling, volume and input bindings in `user://settings.cfg`. The save defaults to `user://dq9.sav`.

`DQ9_ABI_CONFIG` selects caller-supplied local runtime metadata for guest-thread
overrides and optional cartridge backup settings. No title-specific ABI layout
or values are included. See [Building DQ9-Decomp](../docs/BUILDING.md).

The launcher supports the seven host layouts (vertical, horizontal, top/bottom focus, single top/bottom, merged overlay); F11 toggles fullscreen and F10 opens the pause/debug panel. In merged overlay, Alt-left-drag moves the bottom display and Alt-right-drag resizes it; normal clicks remain DS touch input. The DS buttons use `ds_*` InputMap actions; defaults are Z/X for A/B, A/S for X/Y, Enter/Backspace, arrows, E/Q and SDL-style gamepad buttons. Select a binding in settings then press a key or gamepad button/axis. Mouse/touch input maps through the visible bottom-screen rectangle.

## Development and tests

The native extension is loaded explicitly when launching a clean project without an editor cache. If it is unavailable, Start shows an error instead of simulated game screens. Run pure host logic checks with:

```sh
godot --headless --path godot --script res://tests/test_logic.gd
godot --headless --path godot --quit-after 10
```

The local graphical replay probe `tests/test_end_to_end.gd` accepts `--rom`,
`--save`, `--qa-input`, `--qa-out`, `--qa-frames` and `--qa-checkpoints`.
Supply a comma-separated checkpoint list for the local input sequence; the
probe has no game-specific checkpoint defaults. It requires a rendered backend
and xdotool.

Temporary library output (`bin/`) and Godot editor cache (`.godot/`) are ignored. Screenshots and replayable debug-note bundles are written below `user://debug`; bundles contain frame-indexed input history, the session-start save, the save-at-note with SHA-256, deterministic RTC metadata, and a `replay.sh` invocation understood by `tools/debug_bundle.py replay`. `comment.txt` starts with free-text note and includes `frame: N`; `index.tsv` uses the tab-separated format consumed by `tools/debug_bundle.py list/show`. Godot captures can be replayed with the SDL `dq9` binary.

These debug files can contain local image paths, saves, screenshots, input and audio. They stay local and are never uploaded by the host. Local packaging does not publish them.
