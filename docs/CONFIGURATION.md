# Configuration (`frontend.conf`)

The SDL host stores its settings in a plain-text `frontend.conf` (one `key=value` per line, `#`
starts a comment). The launcher writes it; you can also edit it by hand while the game is closed.
Unknown keys and out-of-range values are ignored, so a typo never prevents the game from starting.

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
| `debug_tool` | `0` | Capture/annotation tool (see [Launcher and debug tool](LAUNCHER_AND_DEBUG.md)) |
| `capture_memory_dumps` | `1` | Include RAM/VRAM snapshots in debug bundles |

Command-line options override these for one run (`dq9 --help`).
