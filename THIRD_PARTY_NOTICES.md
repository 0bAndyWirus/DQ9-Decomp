# Third-party notices

This repository's own code is covered by [LICENSE](LICENSE). The components below are either included in this repository (only the Spleen glyphs) or obtained separately by the user or CI. The list is not legal advice. When you distribute a build, include the licence texts of the exact versions you ship, including transitive components.

## Included in this repository

### Spleen 5x8 2.2.0 (BSD-2-Clause)

`runtime/frontend/src/ui_toolkit.cpp` embeds the printable ASCII glyphs of Spleen 5x8 2.2.0 by Frederic Cambus, from [spleen-5x8.bdf at revision 5116f8e](https://github.com/fcambus/spleen/blob/5116f8eac7f65da2cfa169626ec43b5d041665fe/spleen-5x8.bdf). The UI adds horizontal padding and a one-pixel line gap. The full licence text is in [third_party/spleen-LICENSE.txt](third_party/spleen-LICENSE.txt):

    Copyright (c) 2018-2026, Frederic Cambus
    All rights reserved.
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the conditions in
    third_party/spleen-LICENSE.txt are met.

## Obtained separately: linked or loaded by some builds

| Component | Licence | Notes |
|---|---|---|
| [SDL2](https://libsdl.org) | zlib | System package, or SDL 2.30.12 downloaded (SHA-256 pinned) by the Windows build helper. Distributions that copy the SDL2 library must include its licence. |
| [Godot Engine](https://godotengine.org) | MIT, plus bundled third-party components | The Windows helper downloads Godot 4.5-stable. Distributed engines or exports must carry the engine's full copyright and licence material ([guidance](https://docs.godotengine.org/en/4.5/about/complying_with_licenses.html)). |
| [godot-cpp](https://github.com/godotengine/godot-cpp) | MIT | Checkout supplied via `GODOT_CPP_DIR`, or `godot-4.5-stable` downloaded by the helper. |
| C/C++ compiler runtimes | Toolchain-specific | Zig with libc++/libc++abi, MinGW runtime DLLs, or static libstdc++/libgcc (Deck script), depending on the build. Check the runtime-library exceptions and licences of the toolchain you use. |

## Obtained separately: build, test and CI tools

These tools are named by the build instructions but are not shipped in this repository or in the source kit:

- Python (PSF License)
- PyYAML (MIT)
- Capstone (BSD-3-Clause)
- Unicorn (GPL-2.0, used only as a separate test/build tool)
- CMake (BSD-3-Clause)
- Ninja (Apache-2.0)
- Zig (MIT)
- ds-decomp/dsd (see upstream)
- Ghidra (Apache-2.0)
- objdiff (see upstream)
- wibo (see upstream)
- the Metrowerks toolchain (proprietary, user-supplied)
- GitHub Actions (pinned by commit SHA in `.github/workflows/`)

## External references (not included)

Some tests and comparisons use a separately obtained DeSmuME build or source file (GPL-2.0-or-later) as a reference oracle; see [NOTICE](NOTICE). No DeSmuME source is included in this repository.
