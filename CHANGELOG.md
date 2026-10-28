# Changelog

Notable changes to the public tooling and runtime. This repository ships no game, no generated
game code and no game binaries; see [LEGAL.md](LEGAL.md). Performance figures are from one
machine (headless 3,000-frame English boot run) and are not general claims.

## v0.5.0

- **Godot single-screen layout:** display configurable bottom-screen regions over the top screen, with colour-key transparency and touch mapping. The scene detector is a stub; regions do not change automatically by scene. SDL remains unchanged.
- **Read-only snapshots:** registered guest-memory ranges can be captured with a completed frame for host-side inspection. This does not provide save states or a validated scene classifier.
- **Godot checkpoint runner:** select the Start button by node name instead of a hardcoded button width (unmodified runs exited 2 on the current host), and `chain --control` compares a reload chain against an uninterrupted control run where the segment languages match.
- **Clean-room rewrites (output-identical):** RGB555 host expansion, 3D-over-2D blending, cartridge first-word timing and the scanline-214 render boundary were rewritten from behavioural contracts in [docs/cleanroom/](docs/cleanroom/) with exhaustive or dense ROM-free tests. Eight scripted runs (164,534 frames, saves, audio) are unchanged. Not a hardware-accuracy claim.

## v0.4.5

- **GPU3D edge walker rewrite:** the scanline edge walker (polygon edge setup and per-row span stepping) was rewritten from the hardware description in [docs/GPU3D_EDGE_WALKER.md](docs/GPU3D_EDGE_WALKER.md). Output is byte-identical to v0.4.4: 3,000-frame boot hash streams in en/fr/de/es/it, the English WAV and saves, plus four scripted runs totalling 113,811 frames (screens, save and audio) with an explicit ABI configuration and EEPROM64K. Not a parity or provenance claim beyond this component.

## v0.4.4

- **GPU2D output conversion:** exact four-wide SSE2 conversion of 2D scanlines to output colour including master brightness (keeps the `floor(channel*255/31)` expansion; portable scalar tables remain). Single-thread instructions retired versus v0.4.3, medians of three: boot -3.0% (cycles -3.9%), movie -2.7% (-2.2%), activity -1.4% (-2.0%). Output is byte-identical.

## v0.4.3

- **GPU3D framebuffer packing:** non-fog pixels are packed four at a time with SSE2 (scalar fallback elsewhere and for fog groups/tails). Single-thread instructions retired versus v0.4.2, medians of three: boot -5.0% (cycles -3.1%), movie -4.2% (-2.9%), activity -2.2% (-1.7%). Output (five-language boot hashes, English WAV, render-thread 0/2/3 streams, saves) is byte-identical. Two further attempts (window span fill, toon-table cache) were measured and rejected.

## v0.4.2

- **Output-identical speedups, measured by instruction count:** SPU work is scheduled by deadline and silent samples skip mixing, regular BG/OBJ palette colours are cached and updated on write, and hardware timers synchronise only at overflow deadlines or timer I/O (IRQ/cascade ordering preserved). Single-thread user instructions retired versus v0.4.1, medians of three: boot 1,500 frames -19.4% (cycles -15.7%), 6,000-frame movie -13.3% (-10.4%), 12,500-frame activity run -8.1% (-7.2%); with two render threads wall-clock medians improved 9.8% / 8.7% / 5.7% (secondary metric). Five-language boot hashes, English WAV, render-thread 0/2/3 streams and saves are byte-identical to v0.4.1. A GPU3D empty-target fill optimisation was rejected because it cost cycles. Earlier wall-clock-only perf attempts in v0.4.0 were inconclusive on this noisy machine; the instruction-count method replaces them.

## v0.4.1

- **Security hardening:** save temp files are created exclusively and without following links (a pre-planted symlink no longer gets truncated); `BUILD-DQ9.bat` rejects a `DQ9_BACKUP_TYPE` that is not a supported chip name (previously a value containing `&` ran extra commands); the diagnostics ZIP now redacts every absolute path in the log tail; the SDL SDK archive is SHA-256 pinned before extraction; `--dump-frames` ranges are bounded and overflow-safe; CI actions are pinned to commit SHAs.
- **Timing accuracy:** timer overflow IRQs now end a CPU slice at the exact event, and positive CPU budget displaced by a forced IRQ checkpoint is carried instead of discarded. Early timer seed error versus the oracle dropped from 1,263 to 1,089 counts (13.8%); card-load timing is unchanged; ten frame records of the 3,000-frame English boot hash stream change (first at frame 271). Not a parity claim.
- **Godot audio:** releasing fast-forward trims stale source audio to the newest frame (backlog 199.98 to 16.74 ms), pause/resume re-primes retained audio, with ROM-free ring tests and diagnostics. Linux measurement: 59.83 FPS at 1x, zero drops in normal play, emulated PCM byte-identical to the SDL dump. Windows audio and listening quality are not verified.

## v0.4.0

- **Timing model (changes frame phase):** ARM9 charges 161/64 cycles per instruction with fractional carry (derived from 22.4 M oracle instructions over a cartridge load), Slot-1 holds READY through a block with a bounded card-DMA drain, and data/delay SWIs use the oracle's cycle totals. Card-command timing at the title-to-preview load moves from 1035/1050/1099 to 1035/1045/1090 (oracle 1035/1044/1087); the oracle's top screen matches pixel-exactly at native +7 frames. Boot hash streams change from frame 49 (about 2,200 of 3,000 frames) because of phase; saves are unaffected. Regenerate local guest code after updating. Not a complete parity claim.
- **RTC and touch:** the RTC serial protocol (command order, GPIO direction, falling-edge sampling) now returns the programmed time instead of zeros, and touch ADC conversion is exactly reversible at 16 counts per pixel so the guest sees the pixel that was pressed.
- **GPU3D:** homogeneous clipping interpolates inside to outside and snaps the clipped coordinate to +/-w, clipped subpixel slivers keep rasterising, and A3I5 alpha expansion truncates identically in scalar and bulk decoders. Captured-scene differences against the oracle fell from 923 to 793 pixels in one comparison; the boot hash streams do not change.
- **Docs and CI:** setup, save/restore and CLI documentation audited (every `dq9` flag documented), offline Markdown link tests, weekly scheduled Linux/Python CI and cancellation of superseded PR runs.
- **Tests:** randomized stale-state and uniform-clear GPU3D regressions. Seven runtime optimisation experiments were measured and none was kept: gains did not survive paired medians on a noisy machine.

## v0.3.1

- **Godot save restore:** settings and pause panels list timestamped save backups and restore one with confirmation through the native atomic, capacity-checked replacement (never converts chip type), with a persistent Saved / Save failed status. Verified on Linux with the real extension (exact 65,536-byte restore); Windows is not verified.

## v0.3.0

- **Support bundle:** `COLLECT-DIAGNOSTICS.bat` / `tools/collect_diagnostics.py` write one privacy-filtered ZIP (tool and configuration summaries, bounded log tail, script integrity hashes); no game data, saves or binaries, paths redacted.
- **SDL first-run launcher:** validated typed or dropped ROM paths, inline ABI errors, remembered build-derived save-chip settings, Start/Quit controls and a console-free Windows GUI-subsystem executable with persistent diagnostic logging. The Windows status fixture still proves status propagation. Native Windows game execution is not verified.
- **Save safety:** rotating timestamped session backups (default 5, `--save-backups N`), capacity-checked atomic `--restore-save newest|PATH` (never converts chip type), and independent SDL SAVED / persistent SAVE FAILED indicators. Godot shares the default backups; its restore UI is not implemented (native restore documented).
- **SDL audio recovery:** a starved queue rebuilds its playback reserve without discarding PCM, with cumulative drop/underrun/backlog diagnostics. The emulated sample stream is byte-identical. A host that cannot run in real time still cannot produce continuous audio.
- **Godot:** Windows Desktop x86_64 export preset (unverified on Windows), a ROM-free cold-cache extension check, and `PLAY-GODOT` no longer runs a headless editor import first: that import aborts on Godot 4.5 cold start, and the host loads its extension explicitly instead (verified on Linux; Windows launcher covered by ROM-free CI fixtures only).
- **Performance:** GPU2D blended compositing prepares layer order per scanline; output is byte-identical (33 hash/WAV comparisons) and gameplay is about 4% faster on one shared machine (boot gain within noise).
- **Fixes:** `tools/session_ctypes_smoke.py` now declares the appended `backup_type` field of `dsr_session_config` (a missing field made the library read past the allocation), and the optional CTest skips instead of failing when no local fixtures are supplied.

## v0.2.9

- **Cleaner Windows kit:** `BUILD-DQ9.bat` asks for the ROM (drag the file into the window) when none is given, finds Python 3.10+ itself (Microsoft Store stub ignored) with a clear message when missing, sets the EU save chip (`eeprom64k`, override with `DQ9_BACKUP_TYPE`) so a fresh save no longer stalls, and shows `[n/5]` steps with one summary line each and a compile progress bar (`[####----] 42%  482/1134`) instead of raw tool output. `tools/build_port.py --quiet` writes full output to `build/build-log.txt` and prints the last lines only when a command fails.
- **Godot on Windows:** new `BUILD-GODOT.bat` / `PLAY-GODOT.bat`, `build_port.py --godot`, `--godot-only` and `--fetch-godot-editor` (pinned godot-cpp 4.5 and Godot 4.5 editor downloads with checksum checks), `run_godot.ps1` finds the downloaded editor, and a new user guide `docs/USING_GODOT.md`. The Windows Godot steps were not run on Windows yet.
- **Kit README** rewritten as a quick start with troubleshooting and an honest verification status.

## v0.2.8

- **Replay keeps the save-chip choice:** native debug captures record `backup_type` and include explicit overrides in both shell and PowerShell replay commands, including explicit Auto over an ABI setting. Godot captures record the selection too; in-app issue replay applies it before loading the copied launch save, without borrowing or changing normal-game settings. Older captures without this metadata retain Auto behavior.
- **Fresh-save replay regression:** an EEPROM64K capture previously replayed into a 512 KB flash save despite matching early screen hashes. The regression now starts from an empty save and compares persisted save bytes/capacity as well as frames, screenshots and RTC/ticks. Explicit EEPROM, explicit Auto over ABI EEPROM, and inherited ABI EEPROM cases pass.
- **Standalone Godot replay scripts:** target the standard local `build/game/game/dq9` executable (`.exe` on Windows) and change to their own bundle directory, so relative replay files work when invoked from elsewhere.
- **Interrupted input recordings fail clearly:** an incomplete final `frame mask x y` entry now exits with status 2 instead of being silently ignored at EOF. Empty files and trailing whitespace remain valid.
- **Readable notices in small windows:** save warnings, pause feedback and the fast-forward badge fit supported 1x/2x windows instead of losing their action text off the right edge. The renderer uses a smaller font or bounded word-wrap; long device/path text is visibly elided without per-frame heap allocation.
- **Windows launcher failures stay failures:** no-argument `PLAY-DQ9.bat` captures the child exit status after the program returns, without delayed expansion that could corrupt `!` in paths. A real Windows CI probe exercises native failure codes and file access through paths containing spaces and `!`.
- **First-run setup selects the right chip:** Linux examples explicitly choose EEPROM64K, and Windows build-completion/kit instructions open the launcher and ask for EEPROM 64 KB before launching. Generic runtime/ABI defaults remain unchanged.

## v0.2.7

- **Visible save-write failures:** the native window shows a long save-path warning when autosave first fails and one recovery notice when a later flush succeeds. The Godot host exposes the actual synchronous flush result and shows failure/recovery transitions without adding a periodic-save timer; a failed close flush is also reported. Failures keep pending save data for retry and do not stop play; the native final-save exit status is unchanged. Debug bundles now reject a failed flush instead of labelling an older disk save as current.
- **Session API:** `dsr_session_save` now returns an `int` (0 on success, nonzero on write failure or NULL); `DqMachine.flush_save()` returns `bool`, and `get_stats().last_save_ok` publishes completed flush results under the worker mutex. Rebuild the session library and extension together.
- **Cold-cache Godot smoke:** the lifecycle probe explicitly loads the real extension, like the app, instead of requiring an editor-generated extension cache. A cold-checkout regression covers registration and teardown without editor import.

- **Save-chip mismatch hint:** an address-only backup read may indicate EEPROM addressing or a cancelled flash read. Both hosts show a notice suggesting the Save chip setting; the runtime also prints a hint. This is informational only: it never changes the chip, write-enable latch or save data.
- **Save safety:** cancelled normal and fast flash reads preserve the write-enable latch and subsequent writes. Read-transaction shape is ambiguous, so automatic protocol-based chip switching was removed before this release. Auto uses an existing save's capacity, otherwise the flash default; use `--backup-type eeprom64k` or the launcher for the European release.

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
