# Getting started (plain-language guide)

Read this first if you are not a programmer and want to know "how do I play this?".

## The short answer

**You cannot play Dragon Quest IX with this repository.** There is no download, no installer and no `.exe` to double-click. This is a research project: it publishes tools and test code, not a game.

The repository deliberately contains no game, no game data, no code generated from the game and no compiled game builds, and it never will (see [LEGAL.md](../LEGAL.md)). Nothing here is a finished PC port, and no claim is made that the game is fully playable natively.

## If you just want to play Dragon Quest IX

Use hardware or software built for it:

- A Nintendo DS, DS Lite, DSi or 3DS with your own cartridge, or
- An established Nintendo DS emulator with a dump of a cartridge you own.

This project is not a replacement for either, and it does not help you obtain the game.

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
