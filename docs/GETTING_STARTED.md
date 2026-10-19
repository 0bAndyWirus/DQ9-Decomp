# Getting started: build your own local copy

This repository does **not** contain the game. You need your own legally obtained copy of the European multilingual release of *Dragon Quest IX: Sentinels of the Starry Skies* (a `.nds` file). The scripts here turn that file into a program that runs on your PC. Everything they create stays on your computer, and you must never upload or share it.

## Status (verified)

Built from a fresh clone on Linux with a local EU image, the program starts, plays the Square Enix and video-codec logos, and reaches the opening credits with the **START** prompt in all five languages (checked headless at frame 1,500 with `--language en|fr|de|es|it`). **Not verified:** play beyond that point, Windows (a build kit exists and is built by CI, but the author has not run a game on Windows), Steam Deck, audio quality, or saves. Treat this as an early experimental build, not a finished port.

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

python tools/build_port.py --rom "/path/to/your-copy.nds"
```

The build takes about five minutes. At the end it prints the exact command to start the game, which looks like this:

```sh
build/game/game/dq9 --rom "/path/to/your-copy.nds" --abi-config build/gen/abi.ini --language en
```

`--abi-config` is required: without it the game stalls on blank screens. The build creates that file for you (`build/gen/abi.ini`).

What the script does, in plain words: it unpacks your file with `dsd`, translates the game's ARM code into C++ on your machine, works out the game's internal thread layout from your own file, and compiles it with the open-source hardware runtime in this repository. Protected overlay code is resolved from your own image with `tools/unwrap_protected_overlay.py`. This is a form of circumventing the cartridge's code protection; it works only on your own file and nothing derived from it is distributed. Whether that is permitted where you live is your responsibility; see [LEGAL.md](../LEGAL.md).

## Windows build kit (unverified on hardware)

Windows builds from your own `.nds` with a **source-and-scripts kit** (no game and no binary is included, because the executable embeds code generated from your game). Install Python 3.10+ (with "Add python.exe to PATH"), extract the kit, then:

```bat
BUILD-DQ9.bat "C:\path\game.nds"
PLAY-DQ9.bat "C:\path\game.nds" en
```

Details, requirements and the build-time caveat are in [Building on Windows](BUILDING_WINDOWS.md). Continuous integration builds the ROM-free runtime with the same Zig toolchain on Windows and packages the kit; the generated game itself has only been cross-built on Linux. **The author has not run a game on Windows**; if you try it, please report success or failure without attaching game files.

## Controls and hotkeys

Defaults for the SDL window (rebind them on the launcher's HOTKEYS page or in `frontend.conf`; DS buttons are rebound on the CONTROLS page):

| Action | Keyboard | Controller |
| --- | --- | --- |
| Fast-forward (4x, unpaced) | hold **Tab** (`key_fast_forward`) | hold Select + R |
| Screenshot (both screens, PNG in `<save folder>/screenshots/`) | **F12** (`key_screenshot`) | Select + Start |
| Hi-res 3D scale 1x -> 2x -> 3x -> 4x -> 1x | **F7** (`key_gpu3d_scale`) | - |
| Volume down / up (10% steps) | **-** / **=** (`key_volume_down`, `key_volume_up`; setting `volume=0-100`, also on the Options page) | - |
| Mute / unmute | **M** (`key_mute`) | - |
| Cycle screen layout | F6 | Select + L |
| Swap screens | F8 | Select + R3 (right-stick click) |
| Toggle fullscreen | F11 (Alt+Enter also works) | - |
| Linear/nearest filter | F5 | - |
| Integer scaling | F4 | - |
| Close/open the DS lid | F3 | - |

Each hotkey shows a short notice in the window (layout, swap, filter, integer scaling, hi-res scale, screenshot saved), and a FAST-FORWARD badge appears while fast-forward is held.

Pad combos never pass the combo buttons to the game while held. With `--debug-tool` enabled, F3/F4/F8 are taken by the debug tool (see [Launcher and debug tool](LAUNCHER_AND_DEBUG.md)). F7 used to be the screen-swap key; configuration files that still bind both actions to F7 keep the new F7 meaning and move the swap to F8.

## Optional hi-res 3D (experimental, off by default)

`--gpu3d-scale N` (1-4), the `gpu3d_scale` setting or F7 re-renders the 3D scene at N times the native resolution and composes it over the top screen (or over the bottom screen when the DS routes the 3D engine there). Scale 1 is the default and its output is bit-identical to builds without this feature (checked on 3,000-frame headless runs in all five languages and a 28,500-frame input script; hash files identical). The scaled render is verified by unit tests only: the public build currently does not reach a scene with 3D, so no hi-res game frame has been inspected, and hi-res output is not claimed to match the hardware. Headless runs with `--dump-frames` also write `frame_NNNN_top_hires.ppm`.

## Saves

The cartridge save (`dq9.sav` in your data folder, or `--save PATH`) is written atomically on exit and, in a window, automatically every 30 seconds while it has unsaved changes (`--autosave-frames N`, 0 disables; default off with `--headless`). A crash, kill or power loss therefore costs at most about 30 seconds of progress instead of the whole session. This is not a save state; see the limits below.

## Known limits

- No save states: guest threads run on host fibers, which cannot be serialized. In-game saves use the cartridge save file only.
- Hi-res 3D is unverified on real game scenes and makes no parity claim; edge marking and dots stay one output pixel wide at higher scales.
- Windows: the kit's ROM run is unverified; Steam Deck is unverified; play beyond the opening credits is unverified.
- Performance figures in the release notes are from one machine and one headless run; they are not general claims.

## Languages

The European release contains English, French, German, Spanish and Italian. Pass `--language en|fr|de|es|it`. At the opening-credits screen I confirmed the start prompt appears in English ("Begin your adventure."), French ("Commencer l'aventure."), German ("Beginne dein Abenteuer!"), Spanish ("Comenzar la aventura.") and Italian ("Inizia l'avventura."). Only this European release is targeted.

## If something fails

- `dsd: command not found` / wrong version: pass the full path with `--dsd`.
- Missing Python module: activate the virtual environment and re-run the `pip install` line.
- Blank screens: you forgot `--abi-config build/gen/abi.ini`.
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

[FAQ](FAQ.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md) · [Building](BUILDING.md)
