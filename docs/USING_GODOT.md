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

Double-click **`PLAY-GODOT.bat`**. It opens the launcher directly, including on a clean project: the app loads its extension explicitly, without a first-start editor import or `.godot` cache. A normal cold launch with the real extension and rendered game frames has been checked on Linux with Godot 4.5; **the Windows game launch remains unverified**. ROM-free Windows CI checks the launcher process and argument forwarding, not the game.

- **Launcher:** press **Browse** and choose your `.nds`, pick the language, then **Start**. The ROM path, language, layout, volume and key bindings are remembered in Godot's `user://settings.cfg`.
- **Direct start:** `PLAY-GODOT.bat "C:\path\game.nds" fr` starts the game at once. The language argument is optional and defaults to `en`.
- **Save file:** `%APPDATA%\dq9-decomp\godot.sav` when started with the batch file. It is separate from the SDL build's save.

### Save chip

The European release needs the **EEPROM 64 KB** save chip. `BUILD-DQ9.bat` writes that into `build\gen\abi.ini`, and the Godot host finds that file by itself, so a normal build needs no action. If the game stalls at "The quick save log is corrupted", open the settings panel (ROM / SAVE) and set **Save chip** to **EEPROM 64 KB**, then restart. Changing the chip does not convert an existing save file; use a fresh save file for a different chip.

### Save status and restoring backups

**Start / Settings** and **Paused / Debug** both contain **Save backups**, a
persistent save-status label, a timestamped list and **Restore…**. The list uses
the native backup filename order, newest first, and shows UTC timestamps and raw
byte sizes; the full filename is available as a tooltip. A differently sized
backup is marked incompatible and cannot be restored. Restore never selects a
different save chip or converts a save.

- With a game open, both panels inspect that game's actual launch ROM, save,
  language, requested chip and ABI configuration, even if settings have since
  been edited. During issue replay they show the replay copy; restore is
  disabled until you return to the normal game, including after **Play from here**.
- With no game open, choose/type the ROM, save path and chip, then press
  **Refresh backups** (or Enter in a path field). The host loads a temporary
  native machine to obtain its real capacity and managed backup list, without
  starting or stepping emulation. Merely listing does not write a save. Lists
  refresh on panel-open or request, not continuously while typing or playing;
  exceptionally large lists display only the newest 50 entries.
  Listing errors (for example, an unreadable backup directory) are shown as errors
  with restore unavailable, never as a successful "no backups" result. Fix the
  path/permissions and refresh to retry.
- Select a compatible backup and press **Restore…**. The confirmation identifies
  the destination and warns that progress after the selected backup is replaced:
  this restores cartridge save data, **not** the current game frame/save state.
  Cancelling changes nothing.
- Confirming while a game is open first preserves the selected source in a
  private temporary file, flushes the live save, closes the old machine, then
  restores through a freshly loaded, unstarted native machine and restarts.
  **If the live flush fails, restore is cancelled and the live session is kept**;
  repair the save path and retry. A later load/restore failure leaves the game
  closed with an error. Restoring from stopped settings leaves it closed.
  The native runtime rechecks the raw source size against the selected chip's
  actual capacity and backs up the current on-disk save before atomic replacement.

**Saved** means the last completed explicit flush succeeded (or had no pending
writes), not that the guest has recorded new progress. **Save failed** stays
visible after a failure, including a failed close flush; a later successful
flush updates it and shows the existing recovery notice. A new load's initial
success flag is not treated as a successful retry. There is no periodic host
flush: debug-note capture and close/restore preflight use the existing flush
requests. Ordinary close still has a final best-effort shutdown retry after a
failed close flush, whose result is not confirmed by the panel; repair and retry
before closing rather than relying on that retry.

For native API callers, the extension provides `get_save_backups()`,
`get_save_capacity()` and `restore_save(path)`; see
[the lifecycle API](../godot/API.md#lifecycle-and-thread-ownership) for the
fresh-loaded/unstarted requirement and return values.


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
| "Could not start Godot executable" | Rerun `BUILD-GODOT.bat`, or set `GODOT` to a working Godot 4.5 Windows x64 executable. Launch directly; an editor import is not a prerequisite. |

The build log is `build\build-log.txt`. Do not post it publicly without reading it: it contains local paths.

## 7. Debug notes (optional)

From the pause panel you can capture a note: a frame-indexed input history, the starting save, a screenshot and settings, under `user://debug`. These stay on your computer and can contain paths, saves and audio. They can be replayed with the SDL build through `tools/debug_bundle.py`. See [godot/README.md](../godot/README.md) for developer details and tests.

## Linux

Build the extension with `python tools/build_port.py --rom PATH --godot` (or `--godot-only` after a game build), then run `tools/run_godot.sh --menu`. The binding checkout is downloaded and verified the same way. Windows is the platform this guide targets; Godot support on Windows has not been run end to end by the author.

### Audio and frame pacing

The native worker targets **59.8261 emulated frames/second** at 1×; the Godot
window has a separate 60 FPS presentation cap. Source audio enters a bounded
stereo ring before the host resamples it to Godot's mix rate. The rendered host
targets 40 ms in the generator, with the existing ±1% queue correction. This is
not a promise of 40 ms speaker latency: the source ring, mixer and output device
add their own latency.

- Releasing **Tab** trims old fast-forward source audio to the newest emulated
  frame, including when the selected return speed is 0.25×, 0.5× or 2×. It keeps
  the short generator queue instead of stopping/restarting the audio player.
  Reducing a faster-than-1× speed applies the same source trim.
- Pause/stop keep buffered PCM. Resume waits for the retained generator queue to
  be primed again before unpausing, rather than exposing an almost-empty buffer.
  This changes host delivery only, not the emulated samples.
- `get_stats()` separates source-ring fill/high water, intentional dropped
  milliseconds and physical-capacity overwrites. `underruns` is Godot's skip
  counter, including startup skips; it is not a count of verified audible pops.
  Fast-forward deliberately drops source audio rather than building seconds of
  latency. Non-1× playback does not implement time stretching: slow motion can
  underrun, and faster playback can drop samples.

#### Local measurement (Linux, Godot 4.5)

A real-extension Xvfb run with PulseAudio and Mesa llvmpipe used a fresh EEPROM
64 KB save, fixed RTC, English and 3,000 no-input frames:

| Measurement | Observed |
|---|---|
| 1× emulation pacing | 3,000 frames in 50.142 s, approximately 59.83 FPS |
| Steady host frame interval, after the first two seconds | 16.666 ms average; 16.667 ms maximum reported delta |
| Main-thread process monitor, same interval | 5.96 ms average; 14.10 ms maximum |
| Source ring / combined source + generator high water | 50.17 ms / 73.46 ms |
| 1× drops / physical ring overwrites | 0 ms / 0 frames |
| Generator skips | 4 at startup; no additional skips during the run |
| Unpaced-to-1× release, immediate source backlog | 199.98 ms before trim; 16.74 ms after, discarding 183.23 ms |

The original host retained approximately 200 ms on release; its new release
regression fails against the original implementation and passes with the trim.
A separate rendered launcher run exercised the actual Tab handler through
3,000 frames, including release back to 1×; its post-startup main-thread monitor
averaged 6.56 ms with a 12.26 ms maximum. Return-to-0.25×/0.5×/2× checks exercise
the same handler. A ten-cycle pause/manual-step/resume regression also fails
against the original host and passes with re-priming. These are local Linux
measurements, not Windows, hardware, subjective listening or general performance
guarantees. Shared-machine load and the software GL driver affect these numbers.

#### Reproduce locally

Use your own ROM and compatible generated code; keep saves, PCM and logs outside
the tracked source. Run realtime probes with a real audio driver, **not**
`--headless`/Dummy, whose mixer does not provide realtime pacing. These scripts
find the local ABI configuration like the launcher; `DQ9_ABI_CONFIG` can override
it explicitly. Use a fresh, different save path for each comparison.

```sh
python tools/build_port.py --godot-only --quiet
ctest --test-dir build/godot -R dq9_audio_ring_tests --output-on-failure
xvfb-run -a "$GODOT" --path godot --audio-driver PulseAudio \
  --script res://tests/audio_pass.gd -- "$ROM" /local/normal.sav normal
xvfb-run -a "$GODOT" --path godot --audio-driver PulseAudio \
  --script res://tests/audio_pass.gd -- "$ROM" /local/fast.sav fast-forward
xvfb-run -a "$GODOT" --path godot --audio-driver PulseAudio \
  --script res://tests/test_tab_audio.gd -- "$ROM" /local/tab.sav
xvfb-run -a "$GODOT" --path godot --audio-driver PulseAudio \
  --script res://tests/test_pause_audio.gd -- --rom "$ROM" \
  --save /local/pause.sav --backup-type eeprom64k --autostart
```

`audio_pass.gd` also accepts `pause`. `test_tab_audio.gd` accepts an optional
return speed and end-frame count, for example `0.5 2100`. It samples the UI one
host tick after release, when the producer may already have refilled the ring;
the direct 1× probe and ROM-free native test check immediate retention.
The native test needs no ROM and checks stereo order, wraparound, exact capacity,
latency trimming, oversized pushes, stale consumer snapshots, release retention
and counter reset.

For a source-PCM A/B check, opt into diagnostic capture after the normal build:

```sh
cmake -S . -B build/godot -DDQ9_AUDIO_TESTS=ON
cmake --build build/godot --target dq9_godot -j 4
xvfb-run -a "$GODOT" --path godot --audio-driver PulseAudio \
  --script res://tests/audio_pcm.gd -- "$ROM" /local/godot.sav /local/godot.pcm
build/game/game/dq9 --rom "$ROM" --save /local/native.sav \
  --backup-type eeprom64k --headless --frames 3000 --language en \
  --rtc-epoch 1700000000 --dump-audio /local/native.wav
python -c 'import pathlib,wave; w=wave.open("/local/native.wav","rb"); assert w.readframes(w.getnframes()) == pathlib.Path("/local/godot.pcm").read_bytes()'
```

The measured comparison was **byte-identical: 1,641,179 stereo source frames,
6,564,716 bytes**, including non-silent audio. This compares the real extension's
emulated signed-16 little-endian PCM before drops/resampling against the WAV data
chunk, not the WAV header or Godot's resampled floating-point/device output.
Match source/generated code, fresh-save bytes, chip, language, RTC and inputs;
the supplied probe uses no input. Diagnostic capture is absent by default and
must be explicitly enabled in a diagnostic build; keep captures short because
its buffer is unbounded. Restore `-DDQ9_AUDIO_TESTS=OFF` and rebuild afterward.

## Local Windows Desktop export (experimental)

The tracked **Windows Desktop** preset targets **x86_64**, with a separate PCK.
Build the Windows extension first with `BUILD-GODOT.bat`. Confirm that
`godot/bin/` contains both
`dq9_godot.windows.template_release.x86_64.dll` and `dsr_session.dll`.
The extension descriptor registers the session DLL as a Windows dependency;
the preset also includes `bin/*.dll`. Do not rename either library.

1. Use a matching Godot editor and export templates (the extension is built with
   Godot 4.5 bindings). Install the matching templates yourself through the
   editor's **Manage Export Templates** dialog. The repository neither downloads
   nor pins export templates.
2. Keep your image, extracted files and saves **outside** the `godot/` project.
   Review the project folder before exporting: only original host resources and
   the locally built libraries belong there. The presets exclude common game
   image/save patterns (`*.nds`, `*.srl`, `*.rom`, `*.iso`, `*.bin`, `*.sav`,
   `*.sav.*`, `*.dsv`, `*.dsv.*`) and standalone tests. Filters are not a content
   scanner; a renamed private file still must never enter the project.
3. Open `godot/project.godot`, choose **Project > Export > Windows Desktop**,
   then **Export Project**, or run locally:

   ```powershell
   & 'C:\path\Godot.exe' --headless --path godot --export-release 'Windows Desktop' 'C:\local-output\dq9-godot.exe'
   ```

4. Keep the executable, PCK and exported DLLs together. Select your external
   image/save in the launcher. The European release still requires EEPROM 64 KB
   for a fresh save. Export does not bundle an image, save or ABI configuration.

**Windows export and the exported game have not been verified on Windows.**
The preset was parsed on Linux by Godot 4.5's Windows export dry run in a
source-only probe project; it stopped with the expected missing-template error.
This is not evidence of a completed Windows export. Generated game code and
binaries remain local: do not publish the export, even if its project resources
contain no image or save.

### ROM-free cold-cache extension check (Linux)

With an already built Linux extension and a Godot 4.5 editor:

```sh
GODOT=/path/to/Godot_v4.5-stable_linux.x86_64 \
  tools/godot_smoke.sh --cold-cache --extension-bin /path/to/godot/bin
```

`--extension-bin` defaults to `DQ9_GODOT_BIN`, then this checkout's `godot/bin`.
The command copies only host sources and library files into a disposable project
with **no `.godot/` directory**, explicitly loads the real GDExtension, creates a
`DqMachine`, and checks its image bindings. It uses no image or save and leaves
the checkout's import cache untouched. Dynamically linked session libraries must
remain accessible through the extension's normal loader paths.
Success prints the editor version and
`PASS: cold-cache extension loaded; DqMachine created; image bindings available`.
The standalone test rejects pre-registered classes so a warm cache cannot
silently substitute for the fresh-registration check.

This runtime check is separate from editor import and export validation.
No abort is accepted as success. Cold headless editor import can abort (observed
with Godot 4.7.2, upstream [#111645](https://github.com/godotengine/godot/issues/111645));
a Linux 4.5 import with the existing extension also aborted during shutdown,
although its standalone cold-cache check passed. Do not treat this as a passing
editor-import gate or broaden crash tolerances.
