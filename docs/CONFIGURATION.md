# Configuration (`frontend.conf`)

The SDL host stores its settings in a plain-text `frontend.conf` (one `key=value` per line, `#`
starts a comment). The launcher writes it; you can also edit it by hand while the game is closed.
Unknown keys and malformed individual settings are ignored. Numeric settings such as volume, scale and audio limits are clamped to the ranges below; invalid enumerated choices such as a layout or chip name retain the previous/default value.

| Platform | Config folder | Data folder (saves, screenshots) |
| --- | --- | --- |
| Linux | `$XDG_CONFIG_HOME/dq9-decomp` or `~/.config/dq9-decomp` | `$XDG_DATA_HOME/dq9-decomp` or `~/.local/share/dq9-decomp` |
| Windows | `%APPDATA%\dq9-decomp` | `%LOCALAPPDATA%\dq9-decomp` |

**Portable mode:** an empty file named `portable.txt` next to `dq9` / `dq9.exe` keeps everything beside the
program instead of the profile folders above: settings in `portable/config/`, saves and screenshots in
`portable/data/`. Delete the file to go back; the two folders are not moved for you.

Screenshots go to `screenshots/` next to the save file. Controller mappings can be added in a
`gamecontrollerdb.txt` in the config folder (SDL_GameControllerDB format).

## Display

| Key | Default | Meaning |
| --- | --- | --- |
| `layout` | `0` | 0 vertical, 1 horizontal, 2 top focus, 3 bottom focus, 4 top only, 5 bottom only |
| `scale` | `3` | Window scale 1-8 when no size is remembered (window = 256x384 x scale) |
| `window_width`, `window_height` | `0` | Remembered windowed size (written on exit); 0 = use `scale` |
| `fullscreen` | `0` | Start fullscreen |
| `integer_scaling` | `0` | Only whole-number scale factors |
| `linear_filter` | `0` | Smooth instead of nearest-neighbour scaling |
| `gpu3d_scale` | `1` | Experimental hi-res 3D overlay, 1 (off) to 4 |
| `frame_time_overlay` | `0` | Show the frame-time display |

Transient notices and the fast-forward badge fit the renderer width: large text falls back to the
smaller bitmap size, then wraps where needed. Long paths/device names are bounded by the visible
height and end with `...` when truncated. Save-path and pause notices remain complete at 1x/2x sizes.

## Audio

| Key | Default | Meaning |
| --- | --- | --- |
| `volume` | `100` | Master volume 0-100 (host mixer only) |
| `audio_rate` | `48000` | Requested device rate, 8000-192000 |
| `audio_buffer_ms` | `200` | Audio queue cap before a clear, 20-500 |
| `pause_when_unfocused` | `0` | Idle and drop queued audio while the window is not focused |

## Input

| Key | Default | Meaning |
| --- | --- | --- |
| `key_0` ... `key_11` | `29 27 42 40 79 80 82 81 8 20 4 22` (Z X Backspace Return arrows E Q A S) | SDL scancodes for A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y |
| `gamepad_0` ... `gamepad_11` | `1 0 4 6 14 13 11 12 10 9 3 2` | SDL game-controller button ids in the same order (so the DS A button is the pad's B-position button `1`, as on Nintendo layouts) |
| `stick_deadzone` | `12000` | Left-stick dead zone, 0-30000 |
| `dpad_from_stick` | `1` | Left stick also acts as the D-pad |
| `mouse_touch_enabled` | `1` | Mouse/touch drives the bottom screen |
| `controller_guid` | empty | Prefer this controller; empty = first attached |

Hotkeys (SDL scancodes; `0` to 511, anything else is ignored; see
[Getting started](GETTING_STARTED.md#controls-and-hotkeys) for what each does):

| Key | Default | Action |
| --- | --- | --- |
| `hotkey_layout` | `63` (F6) | Cycle layout |
| `hotkey_swap` | `65` (F8) | Swap screens |
| `hotkey_fullscreen` | `68` (F11) | Toggle fullscreen |
| `hotkey_filter` | `62` (F5) | Toggle linear filter |
| `hotkey_integer` | `61` (F4) | Toggle integer scaling |
| `hotkey_lid` | `60` (F3) | Close/open the lid |
| `key_fast_forward` | `43` (Tab) | Hold to fast-forward |
| `key_screenshot` | `69` (F12) | Screenshot |
| `key_gpu3d_scale` | `64` (F7) | Cycle hi-res 3D scale |
| `key_volume_down`, `key_volume_up` | `45`, `46` (`-`, `=`) | Volume in 10% steps |
| `key_mute` | `16` (M) | Mute / unmute |
| `key_pause` | `19` (P) | Pause / resume |

If both `hotkey_swap` and `key_gpu3d_scale` are `64` (an older file that predates the hi-res key),
the swap moves to F8.

## Session

| Key | Default | Meaning |
| --- | --- | --- |
| `language` | `en` | `en`, `fr`, `de`, `es`, `it` |
| `rom_path`, `save_path` | empty | Remembered by the launcher |
| `backup_type` | ABI setting on first launcher run, otherwise `auto` | Remembered save-chip choice using the CLI chip names below. Saved choices win over ABI defaults; explicit `--backup-type`, including `auto`, wins for that launch. |
| `debug_tool` | `0` | Capture/annotation tool (see [Launcher and debug tool](LAUNCHER_AND_DEBUG.md)) |
| `capture_memory_dumps` | `1` | Include RAM/VRAM snapshots in debug bundles |

Command-line options override these for one run (`dq9 --help`).

## Native command-line reference

Run `build/game/game/dq9 --help` for the installed build's options (`--help` / `-h`
prints usage and exits). These are runtime options, not arguments to
`tools/build_port.py`. Paths and diagnostic outputs stay local.

| Option | Meaning |
| --- | --- |
| `--rom PATH` | Your own `.nds` image; alternatively set `DQ9_ROM`. |
| `--save PATH` | Cartridge save; defaults to `dq9.sav` in the data folder above. |
| `--language en\|fr\|de\|es\|it` | Select a language from the European multilingual image. |
| `--abi-config PATH` | Generated ABI INI; alternatively `DQ9_ABI_CONFIG`. If neither is set, search the build tree for `build/gen/abi.ini`. |
| `--backup-type TYPE` | Override ABI cartridge policy: `auto`, `eeprom512b`, `eeprom8k`, `eeprom64k`, `eeprom128k`, `flash256k`, `flash512k`, `flash1m`, `flash8m`. Explicit `auto` also overrides the ABI setting; it uses an existing supported save size, otherwise Flash512K. For a fresh European-release save select `eeprom64k`. |
| `--save-backups N` | Retain 1–10000 timestamped session backups (default 5). |
| `--restore-save newest` / `--restore-save PATH` | Restore a backup and exit without running frames. Close sessions using the target save first and explicitly select the chip type matching the backup capacity. The current save is backed up before replacement; see [save safety](GETTING_STARTED.md#saves). |
| `--autosave-frames N` | Flush dirty saves every N frames; default 1800 in a window, disabled headless. `0` disables periodic flushing, not the final flush. |
| `--launcher` | Open the settings launcher before starting a session. |
| `--gpu3d-scale N` | Experimental hi-res 3D overlay, 1–4; default 1 (off). |
| `--headless` | No window or audio device; requires `--frames` or `--pause-at-frame`. |
| `--frames N` | Stop after N frames; headless runs print a speed summary. |
| `--pause-at-frame N` | Run through zero-based frame N unpaced, then pause (or stop headless). |
| `--rtc-epoch SECONDS` | Deterministic clock epoch; time advances with emulated frames. |
| `--rtc-timezone-offset MINUTES` | Fixed offset for the emulated clock. |
| `--input-script FILE` | Replay `frame button_mask touch_x touch_y` lines. Mask follows KEYINPUT order, with X at bit 10 and Y at bit 11; `-1 -1` releases touch. |
| `--record-input FILE` | Record nonempty frame inputs in that grammar for replay with the same starting save. |
| `--hash-file FILE` | Write `frame top_fnv1a64 bottom_fnv1a64` (RGB-byte hashes). |
| `--dump-frames SPEC` | Dump selected frames, e.g. `100,500-900/100`, as top/bottom PPM and register text; hi-res PPMs are also emitted when enabled. Ranges include their endpoints when reached by the step. At most 1,000,000 frame indices may be requested across all occurrences of this option, counting duplicates and overlaps; larger requests exit with status 2 before loading a ROM. |
| `--dump-dir DIR` | Dump destination; default `build/dq9-frames`. |
| `--dump-ram`, `--dump-vram` | Add RAM/video-memory snapshots to selected frame dumps. |
| `--dump-audio FILE` | Write emitted audio to 16-bit stereo WAV, including headless runs. |
| `--debug-tool` | Enable capture/annotation controls (F10 menu, F9 note, F8 step). |
| `--frame-time-overlay` | Display frame timing without enabling the debug tool. |
| `--debug-every N` | Print machine status every N frames. |
| `--no-debug-dumps` | Omit RAM/VRAM snapshots from debug bundles. |
| `--debug-selftest`, `--debug-workflow-selftest` | Run built-in diagnostic self-tests. |

Save retention and autosave CLI options are native-host controls, not
`frontend.conf` keys. The Godot host's save controls are documented separately in
[Using Godot](USING_GODOT.md).
