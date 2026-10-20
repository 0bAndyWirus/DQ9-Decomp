# Getting started: build your own local copy

This repository does **not** contain the game. You need your own legally obtained copy of the European multilingual release of *Dragon Quest IX: Sentinels of the Starry Skies* (a `.nds` file). The scripts here turn that file into a program that runs on your PC. Everything they create stays on your computer, and you must never upload or share it.

## Status (reviewed 2026-10-20)

Built from a fresh clone on Linux with a local EU image, the program starts, plays the Square Enix and video-codec logos, and reaches the opening credits with the **START** prompt in all five languages (checked headless at frame 1,500 with `--language en|fr|de|es|it`). With the cartridge backup chip set correctly (`--backup-type eeprom64k`; see [If something fails](#if-something-fails)), a scripted 28,500-frame headless run goes on past the title menu through the opening sequence into a field scene, and its per-frame output was identical to a separate local development build (an observation about scripted runs, not a playability claim). After such a replay, a short live windowed session (on a virtual display) responded to keyboard input: the in-game menu opened and closed and the character moved. **Not verified:** sustained interactive play, play beyond the first area, Windows (a build kit exists and is built by CI, but the author has not run a game on Windows), Steam Deck, audio quality, or in-game save/load correctness. Treat this as an early experimental build, not a finished port.

## What you need

- Linux with a C++20 compiler, CMake 3.20+, Ninja, SDL2 development files, Python 3.10+ (Windows: see [Windows build kit](#windows-build-kit-unverified-on-hardware) below).
- Python packages: `PyYAML`, `capstone`, `unicorn`, `cmake`, `ninja` (the build script runs CMake as `python -m cmake`, so the pip `cmake` wheel is required even if your system has CMake).
- The `ds-decomp` tool (`dsd`, version 0.12.x). If you do not pass `--dsd`, the build script downloads the pinned v0.12.1 release from <https://github.com/AetiasHax/ds-decomp> and verifies its SHA-256; pass `--dsd /path/to/dsd` to use a copy you already have.
- Your own `.nds` file. This project cannot help you obtain one.

## Build steps

```sh
sudo apt install build-essential cmake ninja-build git python3 python3-venv libsdl2-dev pkg-config
git clone https://github.com/0bAndyWirus/DQ9-Decomp.git
cd DQ9-Decomp
python3 -m venv .venv && . .venv/bin/activate
python -m pip install PyYAML capstone unicorn cmake ninja

python tools/build_port.py --rom "/path/to/your-copy.nds" --backup-type eeprom64k
```

Before anything slow runs, the script checks Python, the pip packages, CMake/Ninja, a C++ compiler, free disk space and the image header, and stops with a `FAIL` line naming the fix; `python tools/build_port.py --check --rom PATH` runs only those checks.

The build takes about five minutes. At the end it prints the exact command to start the game, which looks like this:

```sh
build/game/game/dq9 --rom "/path/to/your-copy.nds" --abi-config build/gen/abi.ini --backup-type eeprom64k --language en
```

The game needs the generated `build/gen/abi.ini` or it stalls on blank screens. You no longer have to pass it: when neither `--abi-config` nor `DQ9_ABI_CONFIG` is given, `dq9` (and the Godot host) use the `build/gen/abi.ini` of the tree they were built in (found next to the program or up to five folders above it) and say so on stderr. Pass `--abi-config PATH` to use another file.

For a fresh European-release save, choose **EEPROM 64 KB**. The build command above writes this
choice into your local ABI configuration; the runtime's generic Auto fallback is not sufficient.

What the script does, in plain words: it unpacks your file with `dsd`, translates the game's ARM code into C++ on your machine, works out the game's internal thread layout from your own file, and compiles it with the open-source hardware runtime in this repository. Protected overlay code is resolved from your own image with `tools/unwrap_protected_overlay.py`. This is a form of circumventing the cartridge's code protection; it works only on your own file and nothing derived from it is distributed. Whether that is permitted where you live is your responsibility; see [LEGAL.md](../LEGAL.md).

On Linux, `python tools/install_desktop_entry.py` adds an application-menu entry that starts the launcher (`--uninstall` removes it); it only points at your local build.

## Windows build kit (unverified on hardware)

Windows builds from your own `.nds` with a **source-and-scripts kit** (no game and no binary is included, because the executable embeds code generated from your game). Install Python 3.10+ (with "Add python.exe to PATH"), extract the kit, then:

```bat
BUILD-DQ9.bat "C:\path\game.nds"
PLAY-DQ9.bat
```

In the launcher select your ROM and language, then choose **EEPROM 64 KB** under
**ROM / SAVE → Save chip** before Launch. The choice is remembered for later launcher starts.

Details, requirements and the build-time caveat are in [Building on Windows](BUILDING_WINDOWS.md). Continuous integration builds the ROM-free runtime with the same Zig toolchain on Windows and packages the source-only kit. A local Windows build was reported for v0.2.8, but a completed build is not a game-run check. **The author has not run a game on Windows**; if you try it, please report success or failure without attaching game files.

## Controls and hotkeys

Defaults for the SDL window (rebind them on the launcher's HOTKEYS page or in `frontend.conf`; DS buttons are rebound on the CONTROLS page):

| Action | Keyboard | Controller |
| --- | --- | --- |
| Fast-forward (4x, unpaced) | hold **Tab** (`key_fast_forward`) | hold Select + R |
| Screenshot (both screens, PNG in `<save folder>/screenshots/`; at the hi-res size while the hi-res 3D overlay is showing) | **F12** (`key_screenshot`) | Select + Start |
| Hi-res 3D scale 1x -> 2x -> 3x -> 4x -> 1x | **F7** (`key_gpu3d_scale`) | - |
| Volume down / up (10% steps) | **-** / **=** (`key_volume_down`, `key_volume_up`; setting `volume=0-100`, also on the Options page) | - |
| Mute / unmute | **M** (`key_mute`) | - |
| Pause / resume (holds the last frame, drops queued audio) | **P** (`key_pause`) | - |
| Cycle screen layout | F6 | Select + L |
| Swap screens | F8 | Select + R3 (right-stick click) |
| Toggle fullscreen | F11 (Alt+Enter also works) | - |
| Linear/nearest filter | F5 | - |
| Integer scaling | F4 | - |
| Close/open the DS lid | F3 | - |

Each hotkey shows a short notice in the window (layout, swap, filter, integer scaling, hi-res scale, screenshot saved), and a FAST-FORWARD badge appears while fast-forward is held.

Pad combos never pass the combo buttons to the game while held. With `--debug-tool` enabled, F3/F4/F8 are taken by the debug tool, and **M** toggles the mixer instead of mute while the debug HUD is visible (see [Launcher and debug tool](LAUNCHER_AND_DEBUG.md)). F7 used to be the screen-swap key; configuration files that still bind both actions to F7 keep the new F7 meaning and move the swap to F8.

## Optional hi-res 3D (experimental, off by default)

`--gpu3d-scale N` (1-4), the `gpu3d_scale` setting or F7 re-renders the 3D scene at N times the native resolution and composes it over the top screen (or over the bottom screen when the DS routes the 3D engine there). Scale 1 is the default and its output is bit-identical to builds without this feature (checked on 3,000-frame headless runs in all five languages and a 28,500-frame input script; hash files identical). The scaled render is covered by unit tests. With the cartridge backup chip set correctly (see below) the public build reaches real 3D scenes in a scripted run: over 28,500 frames the native frame hashes are identical at scales 1 and 2, and sampled 2x frames differ from a plain upscale of the native frame in 3D areas (both when the 3D engine drives the top screen and when it drives the bottom screen); inspection showed visibly sharper edges and textures, at about +50% frame time at 2x on the author's machine. Hi-res output is not claimed to match the hardware. Headless runs with `--dump-frames` also write `frame_NNNN_top_hires.ppm` and `frame_NNNN_bottom_hires.ppm`.

## Saves

In the native SDL host the cartridge save (`dq9.sav` in your data folder, or `--save PATH`) is written atomically on exit and, in a window, automatically every 30 seconds while it has unsaved changes (`--autosave-frames N`, 0 disables; default off with `--headless`). When writes succeed, a crash or kill can therefore lose up to about 30 seconds of cartridge-save changes instead of the whole session. This is not a save state; see the limits below.

Before the first overwrite of an existing save in each session, the runtime keeps its exact bytes in a timestamped file under `<save folder>/backups/`. The last **five** session backups for that save are retained; native `--save-backups N` changes this (1–10000). Further writes in that session do not create more backups. Rotation recognizes only this runtime's timestamped backup filenames for that exact save; unrelated files and older `<save>.bak` files are left alone. If the required backup cannot be made, the pending save is not overwritten and the flush reports failure.

The SDL window shows a six-second **SAVED** badge after the first successful write and after recovery. A failed autosave shows a persistent **SAVE FAILED (WILL RETRY)** badge without blocking the game; other notices and pause/resume do not hide it. Check folder permissions, free space and file locks while leaving the session open. Pending data stays in memory and normal autosave retries. Closing, killing or losing power before a successful write can lose pending changes. The runtime also logs a warning; the native program exits nonzero if its final flush fails.

To restore, close any running native/Godot session using that save, then run the native executable with the same ROM/ABI/save settings and the **explicit chip type that created the backup**:

```sh
build/game/game/dq9 --rom "/path/to/your/image.nds" --abi-config build/gen/abi.ini \
  --save "/path/to/dq9.sav" --backup-type eeprom64k --restore-save newest
```

Replace `newest` with a chosen backup path to restore that file. This command validates the backup byte count against the selected chip capacity, backs up the current save before replacing it atomically, and exits without running frames. It refuses mismatched capacities rather than converting or reinterpreting the chip. `auto` is not a way to infer a missing target save's chip from a backup; select the correct type explicitly. Keep backups private, just like saves.

Restore stages its safety snapshot under a `.pending` filename until the target commit succeeds. If filesystem permissions change during that operation, a raw `.pending` snapshot may remain; it is never rotated or selected by `newest`. The runtime logs its path if a completed restore cannot publish the snapshot under its normal backup name. It can be restored by explicitly passing that path after repairing permissions. Retention cleanup failures warn separately and retain extra backups; they do not turn an already-persisted save into a write failure.

The Godot host uses the same default five timestamped backups and reports failure/recovery for explicit flushes (debug-bundle capture and session close); it does not currently schedule periodic autosaves. Its Settings and Pause panels now list backups with UTC dates, sizes and capacity-mismatch warnings, and offer a confirmed **Restore…** action. An open game is flushed and stopped before restore, then restarted from the restored cartridge save; a stopped game stays stopped. Restore is disabled throughout issue replay, including Play from here. Backup retention is not a Godot setting. See [Godot save controls](USING_GODOT.md#save-status-and-restoring-backups) for failure handling; the native restore command above remains available after closing Godot.

## Known limits

- No save states: guest threads run on host fibers, which cannot be serialized. In-game saves use the cartridge save file only.
- Hi-res 3D has no hardware parity claim; edge marking and dots stay one output pixel wide at higher scales.
- Windows game runs and Steam Deck hardware are unverified. Scripted Linux runs and short live input checks are not full-game compatibility or playability validation.
- Performance figures in the release notes are from one machine and one headless run; they are not general claims.

## Languages

The European release contains English, French, German, Spanish and Italian. Pass `--language en|fr|de|es|it`. At the opening-credits screen I confirmed the start prompt appears in English ("Begin your adventure."), French ("Commencer l'aventure."), German ("Beginne dein Abenteuer!"), Spanish ("Comenzar la aventura.") and Italian ("Inizia l'avventura."). Only this European release is targeted.

## If something fails

- `dsd: command not found` / wrong version: pass the full path with `--dsd`.
- Missing Python module: activate the virtual environment and re-run the `pip install` line.
- `cannot use ...: does not look like a Nintendo DS image` / `image is truncated`: the file is not a complete DS image (wrong file, partial copy). The launcher shows the same warning next to Launch.
- `The existing workspace ... was extracted from a different image` (or `has no record of the image`): the build keeps the extracted data of one image under `extracted/workspace`. To build a different image, or one extracted by an older script version, run the printed `python tools/build_port.py --rebuild-workspace --rom ...` command; it removes only the tool's own `extract`, `config` and `build` folders there and the generated code, never your image or other folders.
- Title screen says **"The quick save log is corrupted"** on a fresh save: the emulated cartridge backup chip may not match the one the game expects. Auto selects a supported existing save's capacity, otherwise a 512 KB flash chip; it does not guess a protocol from incomplete read transactions. For the European release choose **EEPROM 64 KB** in the launcher, run `dq9 ... --backup-type eeprom64k`, set `[cartridge]` / `backup_type = eeprom64k` in `build/gen/abi.ini`, or build with `python tools/build_port.py --rom ... --backup-type eeprom64k`. An address-only read can trigger a mismatch hint, but the same sequence can be a cancelled flash read: the hint never changes the chip or save data. Changing the selected chip can change the save size; a differently sized save is not loaded (the runtime reports this), and the old file is kept in the timestamped `backups/` folder on the next save. Valid names: `auto`, `eeprom512b`, `eeprom8k`, `eeprom64k`, `eeprom128k`, `flash256k`, `flash512k`, `flash1m`, `flash8m`.
- Blank screens: the generated `abi.ini` was not found. Run the program from inside the repository's build tree, or pass `--abi-config build/gen/abi.ini` (Godot: set `DQ9_ABI_CONFIG`).
- Anything else: open an issue with your OS, compiler version and the last 30 lines of output. **Do not attach game files, saves or screenshots of the game.**

## Developers: runtime tests without a game

```sh
cmake -S . -B build/runtime -DCMAKE_BUILD_TYPE=Debug
cmake --build build/runtime -j
ctest --test-dir build/runtime --output-on-failure
python -m unittest discover -s tests -v
```

## Glossary

- **ROM / image**: a file copy of a game cartridge. Not included here.
- **Static recompilation**: translating the original program into code for a modern PC ahead of time.
- **Runtime**: the code that imitates the DS hardware (graphics, sound, buttons) on a PC.
- **Decompilation**: rewriting machine code as readable source. That source is not part of this repository and is not needed to run the game.

[FAQ](FAQ.md) · [Configuration](CONFIGURATION.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md) · [Building](BUILDING.md)
