# Using the Godot version

The Godot host is an alternative front end for the same native runtime as the SDL `dq9` program. It adds seven screen layouts, key and gamepad rebinding, an optional higher-resolution 3D path, a pause/debug panel and replayable debug notes. It is **experimental**: use the SDL build (`PLAY-DQ9.bat`) when you simply want to play.

Like the SDL build, it needs your own European `.nds` file and code generated from it on your computer. Nothing here ships a game, and the DLLs you build embed game-derived code, so never share them.

## 1. Build it (Windows)

1. Run `BUILD-DQ9.bat "C:\path\game.nds"` once, as described in `README-WINDOWS.txt`. This creates the generated code the Godot extension links against.
2. Double-click **`BUILD-GODOT.bat`**. It
   - downloads the pinned godot-cpp 4.5 bindings (SHA-256 checked),
   - builds `godot\bin\dq9_godot.windows.template_release.x86_64.dll` and `godot\bin\dsr_session.dll`,
   - downloads the Godot 4.5 editor to `build\tools\godot\` (SHA-512 checked).

   To do everything in one go: `BUILD-DQ9.bat "C:\path\game.nds" --godot`.

If you already have a Godot 4.5 editor, set the `GODOT` environment variable to its `.exe` and it will be used instead of the downloaded one. The project and its bindings target Godot 4.5; other versions are untested.

Output goes to the terminal as short steps; the full log is `build\build-log.txt`.

## 2. Start it

Double-click **`PLAY-GODOT.bat`**. The first start imports the project once (a few seconds, needs a writable kit folder), then the launcher opens.

- **Launcher:** press **Browse** and choose your `.nds`, pick the language, then **Start**. The ROM path, language, layout, volume and key bindings are remembered in Godot's `user://settings.cfg`.
- **Direct start:** `PLAY-GODOT.bat "C:\path\game.nds" fr` starts the game at once. The language argument is optional and defaults to `en`.
- **Save file:** `%APPDATA%\dq9-decomp\godot.sav` when started with the batch file. It is separate from the SDL build's save.

### Save chip

The European release needs the **EEPROM 64 KB** save chip. `BUILD-DQ9.bat` writes that into `build\gen\abi.ini`, and the Godot host finds that file by itself, so a normal build needs no action. If the game stalls at "The quick save log is corrupted", open the settings panel (ROM / SAVE) and set **Save chip** to **EEPROM 64 KB**, then restart. Changing the chip does not convert an existing save file; use a fresh save file for a different chip.

## 3. Controls

| DS button | Keyboard | Gamepad |
|---|---|---|
| A / B | Z / X | east / south face buttons (same positions as on a DS) |
| X / Y | A / S | north / west buttons |
| L / R | E / Q | shoulder buttons |
| Start / Select | Enter / Backspace | start / back |
| D-pad | arrow keys | D-pad or left stick |
| Touch screen | mouse click on the bottom screen | - |

Hold a key for about a third of a second; very short taps can be missed by the game, as on real hardware.

| Hotkey | Action |
|---|---|
| **F10** or **Esc** | Pause and open the debug panel |
| **F11** | Fullscreen on/off |
| **F12** | Save a PNG of both screens to `user://screenshots/` |
| **Tab** (hold) | Fast-forward; the chosen speed returns on release |
| **F6** | Compare GPU hi-res 3D with software 3D |
| **F7** | Issue reviewer |

Rebind a button: open settings, click the button's row, then press the new key or gamepad button/axis.

## 4. Screen layouts

Choose from the **Layout** list in the toolbar or settings: Vertical, Horizontal, Top Focus, Bottom Focus, Single Top, Single Bottom and Merged Overlay. In Merged Overlay the bottom screen floats over the top one: **Alt + left-drag** moves it, **Alt + right-drag** resizes it, and normal clicks stay DS touch input. Other options: nearest-neighbour or smooth scaling, integer scaling, and window scale 1x-3x.

## 5. Higher-resolution 3D (experimental)

In settings, set the 3D renderer to **GPU hi-res (experimental)** and pick a scale from 2x to 6x. 3D scenes are then drawn through Godot's GPU instead of the exact software rasteriser; 2D screens are unchanged. The toolbar shows the renderer state (valid-frame and 3D-pixel percentages, or the reason it fell back to software). Press **F6** to flip between GPU and software instantly and judge the difference yourself. There is no pixel-parity claim for this path; choose **Software (exact)** if anything looks wrong.

## 6. If something goes wrong

| Message or symptom | Fix |
|---|---|
| "No Godot 4.5 editor found" | Run `BUILD-GODOT.bat`, or set `GODOT` to your Godot 4.5 `.exe`. |
| "Missing godot\bin\... .dll" | Run `BUILD-GODOT.bat` (after `BUILD-DQ9.bat`). |
| "Run BUILD-DQ9.bat first" | `BUILD-GODOT.bat` needs `build\gen`; build the game first. |
| Start shows an extension error | The DLL was built with a different Godot version or is missing; rebuild with `BUILD-GODOT.bat` and use Godot 4.5. |
| Blank screens | `build\gen\abi.ini` is missing; rerun `BUILD-DQ9.bat`. |
| Stalls at "quick save log is corrupted" | Set **Save chip** to **EEPROM 64 KB** and use a fresh save (see above). |
| Import fails | The kit folder must be writable; do not run from inside the ZIP or a protected folder. |

The build log is `build\build-log.txt`. Do not post it publicly without reading it: it contains local paths.

## 7. Debug notes (optional)

From the pause panel you can capture a note: a frame-indexed input history, the starting save, a screenshot and settings, under `user://debug`. These stay on your computer and can contain paths, saves and audio. They can be replayed with the SDL build through `tools/debug_bundle.py`. See [godot/README.md](../godot/README.md) for developer details and tests.

## Linux

Build the extension with `python tools/build_port.py --rom PATH --godot` (or `--godot-only` after a game build), then run `tools/run_godot.sh --menu`. The binding checkout is downloaded and verified the same way. Windows is the platform this guide targets; Godot support on Windows has not been run end to end by the author.
