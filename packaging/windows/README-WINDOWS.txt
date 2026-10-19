DQ9-Decomp - Windows x64 source build kit

No game or binary is included. This kit contains public source and build
scripts only. You supply your own legally obtained European multilingual
.nds dump; this project cannot help you obtain one. Every generated file,
executable, extracted asset and save must stay private on your own computer.
Never upload or distribute the resulting dq9.exe: it embeds game-derived code.

Requirements
- Windows 10/11 x64.
- Python 3.10+ from https://www.python.org/downloads/windows/ . Enable
  "Add python.exe to PATH" in the installer.
- Internet access to download Python packages, ds-decomp and the SDL2 SDK.
- About 5 GB of free disk space and a writable extracted kit directory.
- Your own .nds dump; no Nintendo BIOS is required or included.

Extract the ZIP, open Command Prompt in its directory, then run:
  BUILD-DQ9.bat "C:\path\game.nds"

The script creates .venv, installs the tools, verifies a pinned ds-decomp
release download, and builds from your own file. It asks no questions.
Compilation can take 30-90 minutes; not measured by the author.
Keep the terminal open and read any error before trying again.

After the build, launch from the same directory:
  PLAY-DQ9.bat "C:\path\game.nds" en

Languages: en, fr, de, es, it. Omitting the language selects en.
In PowerShell, prefix these commands with .\, for example:
  .\BUILD-DQ9.bat "C:\path\game.nds"
  .\PLAY-DQ9.bat "C:\path\game.nds" en
The launcher supplies build\gen\abi.ini and does not enable the debug tool.
Keep backups of your saves and keep all local output out of the kit ZIP.

Protection-step legal note (from docs/GETTING_STARTED.md):
Protected overlay code is resolved from your own image with `tools/unwrap_protected_overlay.py`. This is a form of circumventing the cartridge's code protection; it works only on your own file and nothing derived from it is distributed. Whether that is permitted where you live is your responsibility; see [LEGAL.md](../LEGAL.md).
The root copy of this note is accompanied by LEGAL.md in this kit.

The first ROM run on Windows is unverified. The Windows kit's CI job builds
and tests the ROM-free runtime with Zig; check the actual CI result before
claiming that it passed. The author has not run a game on Windows.
A cross-build or runtime test does not establish Windows game compatibility.

See docs/BUILDING_WINDOWS.md for the Python build flow, Linux cross-builds,
MSVC runtime tests and verification limits. No save-state support is claimed.
This project is not affiliated with Nintendo, Square Enix or Level-5.
