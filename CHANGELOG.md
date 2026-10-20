# Changelog

Notable changes to the public tooling and runtime. This repository ships no game, no generated
game code and no game binaries; see [LEGAL.md](LEGAL.md). Performance figures are from one
machine (headless 3,000-frame English boot run) and are not general claims.

## Unreleased

- **Wrong save chip is noticed:** when the program reads the cartridge backup like an EEPROM (2-byte addresses, nothing more) while the emulated chip is a flash type, the runtime prints a hint on stderr and shows a notice in the window ("SAVE CHIP WRONG? TRY EEPROM 64 KB (LAUNCHER)"). Output is unchanged; the choice is still yours (`--backup-type`, launcher Save chip). The Godot host shows the same notice (stats flag `chip_mismatch`, `dsr_session_chip_mismatch_suspected()`). Verified with the DQ9 EU image on Linux in both hosts: shown with `flash512k`, not with `eeprom64k`.
- **Save chip is inferred when left on auto:** a flash chip never answers a read that ends right after its 3-byte address, while a 2-byte-address EEPROM does. If the program does that before anything was written and no chip was chosen, the runtime now switches to a 64 KB EEPROM at once (the answers given so far are identical for both chips) and says so on stderr. This removes the "quick save log is corrupted" trap for the default setup: a 28,500-frame scripted run with no chip flag reproduced the per-frame output of a run with `--backup-type eeprom64k` and of a separate local development build. Explicit flash choices are never overridden (a notice is shown instead). Verified on Linux with the DQ9 EU image only; no title table is involved.
- **Wrong save chip is noticed:** when the program reads the cartridge backup like an EEPROM (2-byte addresses, nothing more) while the emulated chip is a flash type, the runtime prints a hint on stderr and shows a notice in the window ("SAVE CHIP WRONG? TRY EEPROM 64 KB (LAUNCHER)"). Output is unchanged; the choice is still yours (`--backup-type`, launcher Save chip). Verified with the DQ9 EU image on Linux: shown with `flash512k`, not with `eeprom64k`.

## v0.2.6

- **The generated `abi.ini` is found automatically:** `dq9` and the Godot host use `build/gen/abi.ini` of the tree they live in when neither `--abi-config` nor `DQ9_ABI_CONFIG` is set. Previously the Godot host (which cannot take a flag) showed blank screens unless the environment variable was set; a 3,000-frame run without the file differed from frame 50 on and never left a blank screen.
- **Save chip choice in the launchers:** the SDL launcher's ROM / SAVE page and the Godot settings panel have a *Save chip* option (stored as `backup_type`), so the "quick save log is corrupted" trap can be fixed without editing files or rebuilding. Godot gets `DqMachine.set_backup_type()`, `dsr_session_config.backup_type` and `--backup-type`; debug replay bundles pass the chip to `replay.sh`.
- **Portable mode:** put an empty `portable.txt` next to the program and settings, saves and screenshots are kept in `portable/config` and `portable/data` beside it instead of the user profile (useful for a USB stick or an unzipped Windows kit). Verified on Linux; not run on Windows.

## v0.2.5

- **Clear errors for bad outputs and scripts:** an unwritable `--dump-dir` or `--dump-audio` target, and an `--input-script` with a malformed line (which used to silently drop everything after it), now print a message and exit non-zero instead of aborting or being ignored.
- **Remembered window size is fitted to the desktop:** a size saved on a larger monitor (or typed by hand, e.g. 100000x100000) is cut to the usable display area instead of opening an oversized window.
- **Command line:** a value that is not a number (`--frames abc`) now prints `dq9: invalid value for --frames` and exits with status 2 instead of aborting with an uncaught exception.
- **Save write failures no longer end the session:** a read-only folder, full disk or locked file used to abort the process with an uncaught exception (in a window, at the first autosave). The runtime now prints one warning, keeps the data and retries at the next autosave and at exit; headless runs exit with status 1 if the final write failed.
- **Save size notice:** when an existing save file does not fit the selected cartridge backup type (for example a 512 KB file with `--backup-type eeprom64k`), the runtime now says so on stderr instead of silently starting with a blank save; the old file is still kept as `.bak`.

## v0.2.4

- **First-run language:** the launcher (and the Godot host) preselects the game language that matches the system language (fr/de/es/it; anything else stays English) when no settings file exists yet; an existing choice is never changed.
- **Robustness check (public build, `--backup-type eeprom64k`):** three random-input runs of 58,500 frames each (the scripted replay,
  then 30,000 frames of random key holds and bottom-screen touches) finished without a crash, a "no recompiled entry" fatal or a
  stall (about 5,200-6,000 distinct top-screen frames each); sampled frames showed field exploration and the equipment and item
  menus. A live windowed session also responded to a mouse click on the touch-screen menu icon, not only to keys.
- **Windows CI/build:** a parallel build that fails to link a freshly written `.exe` ("Access is denied" / "file in use", seen intermittently in CI) is retried once serially, in the MSVC workflow and in `tools/build_port.py` on Windows; a real error fails the retry too.
- **Verification note:** after replaying the scripted input into the first in-game area, a short live windowed session on a
  virtual display responded to keyboard input (the in-game menu opened and closed, the character moved). Key presses must last
  at least a few frames, as on the DS itself, which polls its keys periodically; a synthetic tap shorter than one frame is missed.

## v0.2.3

- **Godot host:** the bottom bar is a dark toolbar with tooltips and a hotkey hint, and the on-screen statistics have an outline so they stay readable over bright scenes.
- **Cartridge backup chip:** `dq9 --backup-type TYPE`, `tools/build_port.py --backup-type TYPE` and the `[cartridge] backup_type`
  abi.ini key select the emulated backup chip. With the default guess the title screen reports a corrupt quick-save log on a
  fresh save; with `eeprom64k` a scripted 28,500-frame headless run passes the title menu into a field scene with per-frame
  output identical to a separate development build. The runtime still ships no per-title table.
- **Hi-res dumps:** `--dump-frames` with `--gpu3d-scale` now writes both `_top_hires.ppm` and `_bottom_hires.ppm` (the screen the 3D engine drives is composed, the other upscaled). Verified on the public build over a 28,500-frame scripted run: native hashes identical at scales 1 and 2, 3D areas differ from a plain upscale on both routes.

## v0.2.2

- **Speed (output-identical):** OBJ rejection before affine/tile setup and a hoisted object test in the 2D
  compositor; on a separate local development build the 28,500-frame gameplay script (3D scenes) ran about
  15% faster than before v0.2.1 with identical per-frame hashes and WAV output.
- **3D speed (output-identical):** large polygon lists are prepared in parallel chunks on the render pool; ~2% shorter on the 28,500-frame gameplay run, identical hashes/audio and identical for 0, 2, 3 and 6 render threads.
- **Pause:** `P` pauses and resumes without the debug tool (rebindable; a notice stays on screen while paused).
- **Build safety:** `tools/build_port.py` records which image its workspace was extracted from and refuses to silently rebuild an older image when you give it a different one; `--rebuild-workspace` clears the tool's own folders (never the image) and starts clean.
- **Saves:** the previous run's save is kept as `<save>.bak` (once per run, before the first overwrite).
- **Build safety (more):** `--rebuild-workspace` also refuses when the image sits inside the generated-code folder.
- **Benchmarking:** headless runs end with `dq9: N frames in S s (X FPS, Yx real time)`, a line to paste into reports.
- **Controllers:** hot-plugging a controller shows a short on-screen notice.
- **No audio device:** the game now starts and plays silently (with an on-screen notice) when SDL cannot open an audio device, instead of refusing to start.
- **Window:** the windowed size is remembered between runs (choosing a window scale in the launcher forgets it, so the scale you pick wins).
- **Launcher:** drop a `.nds` file on the window to set the ROM path.
- **Screenshots:** F12 follows the hi-res 3D overlay (saved at the hi-res size while it is showing).
- **Linux:** `python tools/install_desktop_entry.py` adds an application-menu entry that starts the launcher
  (`--uninstall` removes it); CI now also builds and tests the runtime on Linux.
- **Docs:** `docs/CONFIGURATION.md` lists every `frontend.conf` key with defaults and folders.
- **Godot host:** the settings and pause panels are centered and bounded to the window (the pause panel used to
  open off-center and run past the bottom edge).
- **Windows build:** fixed an intermittent parallel-build race when several executables copied `SDL2.dll` into the
  same folder (seen in CI).

## v0.2.1

Host and tooling polish on top of v0.2.0.

- **Speed (output-identical):** the 2D compositor, SPU and card-ROM polling were tightened; on the
  author's machine the same headless run went from about 590 to about 720 FPS (+22%) in the
  RelWithDebInfo build. Gates: per-frame screen hashes identical in en/fr/de/es/it, the English WAV
  byte-identical, and a 28,500-frame input script identical in hashes and audio to the previous
  binary. `tools/build_port.py` now builds `Release` by default: the same output, about 7% faster
  again and a ~10x smaller executable (`--config RelWithDebInfo` keeps symbols).
- **Verification beyond the boot screens:** the same optimizations and the v0.2.0 3D refactor were
  replayed on a separate local development build that reaches real 3D gameplay: identical per-frame
  hashes and WAV over a 28,500-frame input script at scale 1 and at hi-res scale 2, with 2x frames
  visibly sharper in 3D areas (no hardware-parity claim).
- **Host:** master volume and mute (`-`, `=`, `M`; `volume` setting), on-screen notices for hotkeys
  (layout, swap, filter, integer scaling, hi-res scale, volume, screenshot) and a FAST-FORWARD badge,
  optional pause when the window loses focus, and automatic save flushing every 30 s while the save
  has unsaved changes (`--autosave-frames`) so a crash or power loss costs at most ~30 s.
- **Launcher:** language list you can click, a live ROM status line (found / not found / unusable),
  Options rows for hi-res 3D, volume and pause-when-unfocused, a HOTKEYS page with all rebindable
  keys, and a fixed tab label.
- **Godot host:** settings panel with sections, ROM status, live key/pad labels on the binding
  buttons and a pinned Start row; hold Tab to fast-forward, F12 for a two-screen PNG.
- **Errors:** the DS image header is checked up front (junk or truncated files get a clear message,
  also shown in the launcher); a missing-entry fatal explains that the image differs from the one the
  program was built from; `dq9 --help` is grouped and complete.
- **Build:** `tools/build_port.py` runs preflight checks (Python, pip packages, CMake/Ninja,
  compiler, disk space, image header) before the long steps; `--check` runs only those.

## v0.2.0

- Tab fast-forward and F12 screenshot hotkeys with pad combos (Select+R, Select+Start), a launcher
  HOTKEYS page, optional hi-res 3D overlay (`--gpu3d-scale`, unit-tested only), an output-identical
  2D compositor speed-up, and a Windows source build kit (no binaries; CI builds the runtime with
  Zig on Windows, but a game run on Windows is unverified).
- No save states: guest threads run on host fibers, which cannot be serialized.
