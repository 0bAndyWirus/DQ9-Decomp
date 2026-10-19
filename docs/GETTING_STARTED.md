# Getting started (plain-language guide)

Read this first if you are not a programmer and want to know "how do I play this?".

## The short answer

**You cannot yet play Dragon Quest IX with this repository.** There is no download, no installer and no `.exe` to double-click, and nothing built here boots to the title screen. This is a research project: it publishes tools and test code, not a game.

The repository deliberately contains no game, no game data, no code generated from the game and no compiled game builds, and it never will (see [LEGAL.md](../LEGAL.md)). Nothing here is a finished PC port, and no claim is made that the game is fully playable natively.

## If you just want to play Dragon Quest IX

Use hardware or software built for it:

- A Nintendo DS, DS Lite, DSi or 3DS with your own cartridge, or
- An established Nintendo DS emulator with a dump of a cartridge you own.

This project is not a replacement for either, and it does not help you obtain the game.

## Languages

The European release is multilingual: English, French, German, Spanish and Italian. The SDL host accepts `--language en|fr|de|es|it` (`run_game.bat fr --rom ...` on Windows) and sets the emulated console's firmware language, which the game reads at start-up. Only the European multilingual release is targeted; other regional releases are not supported. A language flag is accepted today, but no full-game run exists to confirm each language end to end.

## Build a local host from your own copy (experimental)

`tools/build_port.py` runs the whole local pipeline: it extracts your image with [ds-decomp](https://github.com/AetiasHax/ds-decomp) (which you install yourself), builds the analysis files, generates host C++, and compiles the `dq9` executable. Everything it creates stays under `build/` and `extracted/` and must never be shared.

```sh
python tools/build_port.py --rom "path/to/your-copy.nds" \
  --dsd path/to/dsd --arm7-metadata path/to/arm7-metadata.json
build/game/game/dq9 --rom "path/to/your-copy.nds" --save my.sav --language en
```

`arm7-metadata.json` is a private file with the ARM7 autoload callback address (`{"autoload_callback": "0x..."}`); this repository does not publish game addresses. Use `--generate-only` to stop after code generation.

**Honest status of this path:** I ran it end to end on a local copy. The pipeline generates code and the executable builds, but the ARM9 output still contains thousands of unsupported sites, and the host stops with a fatal "undefined instruction" error within the first second. The step that unpacks protected code modules is deliberately not published, and no ABI configuration is supplied. So **you still cannot play the game this way.** Experimental native progress (boot logos, opening movie, title menu and character creation) was demonstrated only in a private development build and is not reproducible from this repository.

## What you *can* do here

| You are... | What to do |
|---|---|
| Curious | Read the [FAQ](FAQ.md) and [Status](../README.md#status). Press **Watch** on the repository to hear about progress. |
| A developer | Build the runtime and run the tests (steps below). |
| A contributor | Read [CONTRIBUTING.md](../CONTRIBUTING.md) and open an issue before large changes. |

## Build the runtime and run its tests (Linux)

This builds the hardware-emulation libraries and checks them with synthetic tests. No game is involved. Success means "the code compiles and its self-tests pass"; it does **not** mean a game runs.

1. Install the tools (Debian/Ubuntu names; use your distribution's equivalents):

   ```sh
   sudo apt install build-essential cmake git python3 python3-venv libsdl2-dev pkg-config
   ```

   You need a C++20 compiler, CMake 3.20 or newer, SDL2 development files and Python 3.10 or newer.

2. Get the code:

   ```sh
   git clone https://github.com/0bAndyWirus/DQ9-Decomp.git
   cd DQ9-Decomp
   ```

3. Build and test:

   ```sh
   cmake -S . -B build/runtime -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/runtime -j
   ctest --test-dir build/runtime --output-on-failure
   ```

4. Optional: run the Python tool tests.

   ```sh
   python3 -m venv .venv
   . .venv/bin/activate
   python -m pip install PyYAML capstone unicorn
   python3 -m unittest discover -s tests -v
   ```

Expected result: the build finishes and `ctest` reports the tests as passed. If it fails, open an issue with your OS, compiler version and the last 30 lines of output. Do not attach game files, saves or screenshots of the game.

Windows and Steam Deck notes: [Windows](BUILDING_WINDOWS.md), [Steam Deck](../packaging/steamdeck/README.md). These are build targets, not verified gameplay platforms.

## Glossary

- **ROM / image**: a file copy of a game cartridge. Not included here.
- **Decompilation**: rewriting a game's machine code as readable source that compiles back to the same bytes. Progress is *matching coverage*, which says nothing about whether the game is playable.
- **Static recompilation**: translating the original program into code for a modern PC ahead of time.
- **Runtime**: the code that imitates the DS hardware (graphics, sound, buttons) on a PC.
- **Oracle**: an existing emulator used only to compare screenshots against, never shipped.

## Where to go next

[FAQ](FAQ.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md) · [Building](BUILDING.md)
