# Changelog

Notable changes to the public tooling and runtime. This repository ships no game, no generated
game code and no game binaries; see [LEGAL.md](LEGAL.md). Performance figures are from one
machine (headless 3,000-frame English boot run) and are not general claims.

## Unreleased

- **Speed (output-identical):** OBJ rejection before affine/tile setup and a hoisted object test in the 2D
  compositor; on a separate local development build the 28,500-frame gameplay script (3D scenes) ran about
  15% faster than before v0.2.1 with identical per-frame hashes and WAV output.
- **Launcher:** drop a `.nds` file on the window to set the ROM path.
- **Screenshots:** F12 follows the hi-res 3D overlay (saved at the hi-res size while it is showing).

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
