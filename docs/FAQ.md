# Frequently asked questions

Short answers about what DQ9-Remastered is and is not. The authoritative documents are linked from each answer: [README](../README.md), [LEGAL.md](../LEGAL.md), [Project charter](PROJECT_CHARTER.md), [Decompilation status](DECOMP_STATUS.md), [Roadmap](ROADMAP.md) and [Building](BUILDING.md).

## Can I play Dragon Quest IX on PC using this repository?

Only from your own copy, and only partly. The repository ships no game, no generated game code and no builds, but `tools/build_port.py` turns your own `.nds` file into a local program that was verified to reach the opening credits and START prompt in English, French, German, Spanish and Italian. A scripted Linux run also reached an early field scene, and a short live input check was performed; these are not full-game compatibility or playability validation. See the [plain-language guide](GETTING_STARTED.md) and the [README status](../README.md#status).

## Does the project contain ROMs, assets or decompiled game code?

No. The repository contains original code only: recompiler, hardware runtime, SDL2 and Godot host sources, tools, tests and documentation. It does not contain ROMs, extracted assets, scripts or text, decompiled or disassembled game code, generated game sources, symbol or address dumps, saves, debugger dumps or compiled builds. Public prereleases provide source-and-scripts kits only, not game executables or binary game releases. The [MIT License](../LICENSE) covers the original project code only, not the game or other third-party works. Ownership of a copy does not by itself authorise copying, decompiling, circumventing protections or distributing derivatives. See [LEGAL.md](../LEGAL.md) and the [data boundary](PROJECT_CHARTER.md#data-and-legal-boundary).

## Which Dragon Quest IX version is researched?

The European multilingual release of *Dragon Quest IX: Sentinels of the Starry Skies* for Nintendo DS. Community work on the USA/Japan releases is not assumed to apply unchanged to Europe; the [charter](PROJECT_CHARTER.md#non-goals-of-the-public-repository) and [research log](RESEARCH_LOG.md) explain the boundary and credit prior work.

## Is the game supported on Linux, Windows or Steam Deck?

Those are host targets, not validated compatibility claims. Linux desktop is the immediate development target. Windows has native and cross-build configurations, but neither cross-linking nor generic desktop tests establish Windows or Steam Deck hardware compatibility; Steam Deck still needs separate device, controller, display, audio and performance validation. Windows has a source build kit (`BUILD-DQ9.bat` / `PLAY-DQ9.bat`, no game or binary included) that builds on your own PC from your own `.nds`; CI builds its ROM-free runtime with the same Zig toolchain, but the author has not run a game on Windows. Details: [charter](PROJECT_CHARTER.md#host-targets), [Windows](BUILDING_WINDOWS.md), [Steam Deck](../packaging/steamdeck/README.md).

## What can I build or test without game data?

The runtime libraries and their synthetic tests. Without `DQ9_GENERATED_DIR`, CMake builds those components, not a playable game. The commands are in the [README quick start](../README.md#quick-start-runtime-and-tests-only) and [Building](BUILDING.md); prerequisites are a C++20 compiler, CMake 3.20+, SDL2 development files and Python 3.10+. Tests that need local images, generated code, an external emulator or Godot have extra prerequisites and are outside the default suites; unavailable tests are not passes.

## How is matching progress different from game compatibility?

Byte-exact matching measures whether locally written source compiles to the same machine code as the original. Behavioral parity means the game actually runs correctly end to end. They are different measures, and matching progress says nothing about playability. [DECOMP_STATUS.md](DECOMP_STATUS.md) publishes aggregate matching totals only, with their date and limits, and states that matching-source work and static recompilation are separate workflows.

## How can I contribute without sharing copyrighted material?

Contribute original code, tests built on synthetic fixtures, documentation and reproducible, sanitized bug reports. Never include game-derived bytes, assets, text, symbol or address dumps, decompiled source, saves or captures. See [CONTRIBUTING.md](../CONTRIBUTING.md) and [LEGAL.md](../LEGAL.md).
