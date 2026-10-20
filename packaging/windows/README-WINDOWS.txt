DQ9-Decomp - Windows x64 source build kit
=========================================

No game and no binary is included. This kit contains public source and build
scripts only. You supply your own legally obtained European multilingual .nds
dump; this project cannot help you obtain one. Every generated file, executable,
extracted asset and save stays private on your own computer. Never upload or
distribute the resulting dq9.exe or the Godot DLLs: they embed game-derived code.


QUICK START (3 steps)
---------------------
1. Install Python 3.10 or newer from https://www.python.org/downloads/windows/
   and tick "Add python.exe to PATH" in the installer.
2. Extract this ZIP somewhere with about 5 GB free (not inside a read-only
   folder), then double-click  BUILD-DQ9.bat
   It asks for your ROM: drag the .nds file into the window and press Enter.
   Wait for "Build complete". The compile step is the long one.
3. Double-click  PLAY-DQ9.bat  and press Launch.

Prefer a command line? From the extracted folder:
    BUILD-DQ9.bat "C:\path\game.nds"
    PLAY-DQ9.bat
(In PowerShell put .\ in front: .\BUILD-DQ9.bat ... )


THE GODOT VERSION (optional, higher-resolution 3D experiments)
--------------------------------------------------------------
After BUILD-DQ9.bat has finished once:
    BUILD-GODOT.bat     builds the Godot extension and downloads the Godot 4.5
                        editor (SHA-512 verified)
    PLAY-GODOT.bat      starts it; choose your ROM in the launcher
Or build both at once:  BUILD-DQ9.bat "C:\path\game.nds" --godot
Full guide (controls, layouts, settings, saves, troubleshooting):
    docs\USING_GODOT.md


WHAT THE BUILD DOES
-------------------
 [1/5] checks Python, tools, free disk space and the ROM header
 [2/5] extracts the ROM with ds-decomp (pinned release, checksum verified)
 [3/5] analyses the ARM9/ARM7 code
 [4/5] translates it to C++ on your computer
 [5/5] compiles dq9.exe with Zig (downloaded through pip) and SDL2
The window shows one line per step and a compile progress bar. The full output is
written to build\build-log.txt. Read the lines under a failed step first.
Running BUILD-DQ9.bat again only rebuilds what changed.

The save chip is set to EEPROM 64 KB (the European release) while building.
Without it a fresh game stalls at "The quick save log is corrupted".
Another chip: set DQ9_BACKUP_TYPE (for example flash512k) before building.


REQUIREMENTS
------------
- Windows 10/11 x64.
- Python 3.10+ (see above). The Microsoft Store "python" shortcut does not count.
- Internet access during the build (Python packages, ds-decomp, SDL2 SDK, and
  for Godot the godot-cpp bindings and editor).
- About 5 GB of free disk space and a writable extracted folder.
- Your own .nds dump; no Nintendo BIOS is required or included.


PLAYING
-------
PLAY-DQ9.bat opens the launcher. Choose your ROM and language, check
ROM / SAVE -> Save chip = EEPROM 64 KB, then Launch. Choices are remembered.
Languages: en, fr, de, es, it.
Direct start without the launcher:  PLAY-DQ9.bat "C:\path\game.nds" en
(this uses the chip written to the generated abi.ini, not the launcher's choice).
Hold a key for about a third of a second: sub-frame taps are missed, as on a
real DS. Keep backups of your saves and keep all local output out of any ZIP.


IF SOMETHING GOES WRONG
-----------------------
- "Python 3.10 or newer was not found": install it and tick Add to PATH, then
  open a new window.
- "[FAIL] ROM ...": the file is not a valid .nds image or the path is wrong.
- Build failed: open build\build-log.txt and read the end of it.
- A link error "Access is denied": an antivirus scanner holds a fresh file. The
  build retries once by itself; run BUILD-DQ9.bat again if it still fails.
- Blank screens in the game: the generated build\gen\abi.ini is missing; run
  BUILD-DQ9.bat again.


LEGAL
-----
Protected overlay code is resolved from your own image with
tools/unwrap_protected_overlay.py. This is a form of circumventing the
cartridge's code protection; it works only on your own file and nothing derived
from it is distributed. Whether that is permitted where you live is your
responsibility; see LEGAL.md in this kit.
This project is not affiliated with Nintendo, Square Enix or Level-5.


VERIFICATION STATUS
-------------------
A Windows build from this kit completed on a real Windows machine (v0.2.8), and
a CI job builds and tests the ROM-free runtime and packages this kit. Running
the game on Windows has not been confirmed yet, and the Godot Windows build
steps were written but not yet run on Windows. A cross-build or runtime test
does not establish Windows game compatibility. No save-state support is claimed.
See docs/BUILDING_WINDOWS.md for the Python build flow, Linux cross-builds, MSVC
runtime tests and verification limits.
