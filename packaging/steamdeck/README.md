# DQ9-Decomp on the Steam Deck

This directory contains Steam Deck / SteamOS packaging support for the native
`dq9` host. Game-host packages (they include code generated from the game) are local-only and must never be uploaded or distributed,
including through CI, releases, or public issue attachments. This source
project distributes no binaries, compatible images, or generated game sources.

## Local build

Requirements: Docker and a separately supplied generated-code directory.

```sh
tools/build_deck.sh --gen "$GENERATED_DIR" --tarball
```

Use `--jobs N` to limit parallel compilation. The script uses Valve's public
Steam Runtime 3 "sniper" SDK image
(`registry.gitlab.steamos.cloud/steamrt/sniper/sdk`). It pulls the stock image
as needed; it does not build or push a custom image.

The build selects the newest SDK GCC for C++20 and statically links `libstdc++`
and `libgcc`. SDL2 remains dynamically linked. The ABI check rejects a binary
requiring glibc newer than 2.31 or dynamic C++ runtime symbols. The script prints
the required libraries and highest glibc symbol version.

Outputs are `build/deck/dq9-deck/` and, with `--tarball`,
`build/deck/dq9-deck.tar.gz`. A local package contains:

| File | Purpose |
| --- | --- |
| `bin/dq9` | native host executable |
| `dq9.sh` | local image selection and Deck-friendly SDL defaults |
| `dq9.desktop` | desktop template, filled by `--install-desktop` |
| `README.md` | local setup notes |

## Local installation and image selection

Use `<install-dir>` below for the directory containing `dq9.sh` and `bin/dq9`.
Select an already locally available compatible image using `--rom` or `DQ9_ROM`:

```sh
"<install-dir>/dq9.sh" --rom "$DQ9_ROM"
```

The launcher also accepts a saved local path in
`~/.config/dq9-decomp/launcher.conf` as `ROM=...`. Lookup order is `--rom`,
`DQ9_ROM`, then that configuration file. There is no required image location
or filename. Other options are passed through to `bin/dq9`, for example
`--language fr` or `--save PATH`.

`DQ9_ABI_CONFIG` or `--abi-config PATH` selects caller-supplied local runtime
metadata for guest-thread overrides and cartridge settings. No title-specific
layout or values are included in the source project or package.

If no configured local image exists, the launcher logs an error to
`~/.local/share/dq9-decomp/launcher.log` and displays it through zenity or kdialog
when available.

## Desktop entry and Steam shortcut

In Desktop Mode:

1. Run `"<install-dir>/dq9.sh" --install-desktop` to fill the shipped desktop
   template with the actual installation path and write
   `~/.local/share/applications/dq9-decomp.desktop`. Do not install the unfilled
   template directly.
2. Steam → *Games* → *Add a Non-Steam Game to My Library…* → *Browse…* → select
   `<install-dir>/dq9.sh` (use the *All Files* filter), or select the
   **DQ9-Decomp** menu entry if installed.
3. Leave *Compatibility* unset: this is a native Linux host, not a Proton
   executable. Set `--rom` in launch options to the already available local
   image, or configure `DQ9_ROM` or `launcher.conf` as above.

The shortcut appears in the *Non-Steam* collection in Gaming Mode.

## Controls and Steam Input

The built-in mapping is positional, like the DS:

| Deck | DS |
| --- | --- |
| B (right) / A (bottom) / Y (top) / X (left) | A / B / X / Y |
| D-pad or left stick | D-pad |
| L1 / R1 | L / R |
| View / Menu | Select / Start |
| touchscreen | stylus on the bottom-screen image |
| R3 | toggle stylus mode: right stick moves the pen, R2 presses it |
| View + L1 | cycle screen layout |
| View + R1 (hold) | fast-forward |
| View + R3 | swap screens |
| View + Menu | screenshot |

The **Gamepad with Mouse Trackpad** Steam Input template also lets the trackpad
move the mouse pointer and click the bottom screen. Leave native touchscreen
input at its default to avoid duplicate stylus input.

## Display and pacing

The Deck profile requests a fullscreen 1280×800 window with a top-focus layout.
Actual display/controller behavior still requires device-side validation.
Cycle layouts with View+L1 or edit `layout=` in `frontend.conf`.
The frontend's own filtering and integer scaling are controlled by
`linear_filter=` and `integer_scaling=`; leave Gamescope scaling at its defaults.

Host pacing follows the DS refresh rate (approximately 59.83 Hz) through its
own frame deadline, independently of display vsync. Gamescope's frame limiter
can throttle presentation; it must never be used to accelerate guest simulation.

The launcher defaults `SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0`,
`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`,
`SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0`, and `SDL_TOUCH_MOUSE_EVENTS=0`.
Existing environment settings take precedence.

## Saves and configuration

| What | Default location |
| --- | --- |
| Battery save | `~/.local/share/dq9-decomp/dq9.sav` (`--save PATH` overrides) |
| Frontend settings | `~/.config/dq9-decomp/frontend.conf` |
| Launcher local image path | `~/.config/dq9-decomp/launcher.conf` |
| Launcher error log | `~/.local/share/dq9-decomp/launcher.log` |

`~` denotes the current user's home directory. `XDG_CONFIG_HOME` and
`XDG_DATA_HOME` override the corresponding defaults. Back up the save before
updating a local package; Steam Cloud does not sync non-Steam shortcuts.

The native save policy also applies to a local Deck build: dirty saves flush
periodically in a window, with five timestamped session backups by default.
See [save safety and restore](../../docs/GETTING_STARTED.md#saves) and the
[CLI reference](../../docs/CONFIGURATION.md#native-command-line-reference).
A fresh European-release save needs `--backup-type eeprom64k` or the equivalent
generated ABI/launcher setting; this is not automatic title detection.
These documented controls and policies are not evidence of a Steam Deck hardware run.
