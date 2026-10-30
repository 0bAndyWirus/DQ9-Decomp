# DQ9-Remastered — Godot host

This Godot 4.5 host uses the `dq9_godot` GDExtension and the shared native runtime; the SDL application remains a separate build. This source project distributes no binaries or generated game code. Select an already locally available compatible image; do not copy image files or local session data into this directory or commit them.

New to this host? Start with the [Godot user guide](../docs/USING_GODOT.md) (Windows one-click build, controls, layouts, troubleshooting). This file is the developer reference.

## Build and run

The extension build requires a separately supplied generated-code directory. Enable `DQ9_BUILD_GODOT` in the root CMake build and set `GODOT_CPP_DIR` to an external godot-cpp 4.5 checkout. The build writes the extension to `godot/bin/`; `dq9.gdextension` loads it when present. Open this directory in Godot 4.5, or run:

```sh
godot --path godot
# Optional arguments after --:
godot --path godot -- --rom "$DQ9_ROM" --save user://dq9.sav --language en --autostart
```

Select a local image using `--rom`, `DQ9_ROM` when no saved path exists, or Browse. `--rom` overrides the saved selection. The launcher remembers paths, language, layout, filter/scaling, volume and input bindings in `user://settings.cfg`. The save defaults to `user://dq9.sav`.

`DQ9_ABI_CONFIG` selects caller-supplied local runtime metadata for guest-thread
overrides and optional cartridge backup settings. When it is unset the host looks for
`build/gen/abi.ini` above the project folder (the file `tools/build_port.py` writes) and uses it;
without any such file the game stalls on blank screens. The settings panel has a **Save chip** option
(also `--backup-type NAME` after `--`) for the cartridge backup memory type. No title-specific ABI layout
or values are included. See [Building DQ9-Remastered](../docs/BUILDING.md).

The launcher supports the seven host layouts (vertical, horizontal, top/bottom focus, single top/bottom, merged overlay); F11 toggles fullscreen and F10 opens the pause/debug panel; hold Tab to fast-forward (the pause panel's speed returns on release) and press F12 to save a PNG of both screens to `user://screenshots/`. The settings panel shows whether the ROM path exists and the current key/pad binding on each button. In merged overlay, Alt-left-drag moves the bottom display and Alt-right-drag resizes it; normal clicks remain DS touch input. The DS buttons use `ds_*` InputMap actions; defaults are Z/X for A/B, A/S for X/Y, Enter/Backspace, arrows, E/Q and SDL-style gamepad buttons. Select a binding in settings then press a key or gamepad button/axis. Mouse/touch input maps through the visible bottom-screen rectangle.

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

`env.txt` includes `backup_type: <canonical-name>` (the requested native
`--backup-type` choice, or `auto` when unset). In-app issue replay applies this
before loading its copied launch save, independently of the normal-game Save
chip setting. Old or invalid metadata defaults to Auto. Capturing during replay
records the replay machine's choice, not the normal settings.

For the real fresh-save capture/reload regression, use the existing reviewer
probe with a local input sequence and a frame count long enough to write save
data:

```sh
xvfb-run -a -s "-screen 0 3072x1920x24" godot --path godot \
  --script res://tests/test_issue_reviewer.gd -- \
  "$ROM" "$SOURCE_SAVE" "$INPUT" "$NATIVE_DQ9" "$OUTPUT" "$FRAMES"
```

This generates an empty launch save with explicit EEPROM 64 KB, compares all
native/Godot frame hashes, captures and reloads through the production issue
replay path, and requires 64 KB persisted saves with identical bytes. The source
save remains untouched. Without `$FRAMES`, the existing 300-frame UI/parity
probe remains available but does not require a save write that early.

These debug files can contain local image paths, saves, screenshots, input and audio. They stay local and are never uploaded by the host. Local packaging does not publish them.
