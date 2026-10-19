# Getting started: build your own local copy

This repository does **not** contain the game. You need your own legally obtained copy of the European multilingual release of *Dragon Quest IX: Sentinels of the Starry Skies* (a `.nds` file). The scripts here turn that file into a program that runs on your PC. Everything they create stays on your computer, and you must never upload or share it.

## Status (verified)

Built from a fresh clone on Linux with a local EU image, the program starts, plays the Square Enix and video-codec logos, and reaches the opening credits with the **START** prompt in all five languages (checked headless at frame 1,500 with `--language en|fr|de|es|it`). **Not verified:** play beyond that point, Windows, Steam Deck, audio quality, performance, or saves. Treat this as an early experimental build, not a finished port.

## What you need

- Linux with a C++20 compiler, CMake 3.20+, Ninja, SDL2 development files, Python 3.10+ (Windows: see [Building on Windows](BUILDING_WINDOWS.md); not verified).
- Python packages: `PyYAML`, `capstone`, `unicorn`.
- The `ds-decomp` tool (`dsd`, version 0.12.x) from <https://github.com/AetiasHax/ds-decomp>, downloaded by you.
- Your own `.nds` file. This project cannot help you obtain one.

## Build steps

```sh
sudo apt install build-essential cmake ninja-build git python3 python3-venv libsdl2-dev pkg-config
git clone https://github.com/0bAndyWirus/DQ9-Decomp.git
cd DQ9-Decomp
python3 -m venv .venv && . .venv/bin/activate
python -m pip install PyYAML capstone unicorn

python tools/build_port.py --rom "/path/to/your-copy.nds" --dsd /path/to/dsd
```

The build takes about five minutes. At the end it prints the exact command to start the game, which looks like this:

```sh
build/game/game/dq9 --rom "/path/to/your-copy.nds" --abi-config build/gen/abi.ini --language en
```

`--abi-config` is required: without it the game stalls on blank screens. The build creates that file for you (`build/gen/abi.ini`).

What the script does, in plain words: it unpacks your file with `dsd`, translates the game's ARM code into C++ on your machine, works out the game's internal thread layout from your own file, and compiles it with the open-source hardware runtime in this repository. Protected overlay code is resolved from your own image with `tools/unwrap_protected_overlay.py`. This is a form of circumventing the cartridge's code protection; it works only on your own file and nothing derived from it is distributed. Whether that is permitted where you live is your responsibility; see [LEGAL.md](../LEGAL.md).

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
