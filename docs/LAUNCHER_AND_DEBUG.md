# Launcher and in-game debug capture

## Start

Use the SDL2 launcher before starting the game:

```sh
build/game-ui/game/dq9 --launcher
```

The launcher writes `frontend.conf` beside the existing frontend settings, preserving the older frontend keys. On the very first run (no `frontend.conf` yet) the language is preselected from the system language when the European release ships it (French, German, Spanish, Italian; otherwise English). The ROM / SAVE page also has a **Save chip** button (cycle with Enter or a click): it selects the emulated cartridge backup memory and is remembered as `backup_type` in `frontend.conf`. Auto uses the build's setting or an existing save's capacity, otherwise the flash default; choose EEPROM 64 KB for the European release. A `--backup-type` given on the command line is shown there as the starting value. The Language page lists English, French, German, Spanish and Italian; click one or use the arrow keys/D-pad. A status line next to Launch shows whether the configured ROM path exists (green) or not (amber), so a wrong path is visible before launching; Launch is never blocked. The Options page also has the experimental Hi-res 3D scale (off, 2x-4x). It also has Master volume (0-100%) and Pause when window unfocused (off by default: the game idles and drops queued audio while the window lacks focus, and resumes when it returns; it never applies to --pause-at-frame replays). Controls shows the current keyboard/controller bindings; select a DS input, choose keyboard or controller with left/right, then press the new key/button. Reset restores defaults. Dropping a .nds file on the launcher window sets the ROM path. ROM and save paths can be edited as text, or Auto Detect uses `$DQ9_ROM` and the local `ROM/` directory; the default save is `<data_dir>/dq9.sav`. Options persist screen layout, scale, fullscreen, integer/linear scaling, audio queue cap, controller selection/deadzone, touch behavior, debug tool, frame-time overlay, and memory-dump preference. `--rom`, `--save`, and `--language` remain available for direct CLI launches.

The direct launch form is unchanged:

```sh
build/game-ui/game/dq9 --rom "$DQ9_ROM" --save "$DQ9_SAVE" --language en
```
Relative ROM paths are resolved in this order: the current working directory,
the directory containing `frontend.conf`, then a repository checkout above the
executable. The launcher persists a resolved absolute ROM path, so launching
the same selection from another directory works. Auto Detect also searches the
config directory's `ROM/` and the checkout's `ROM/` (or `rom/`). A missing ROM
prints the requested path, search locations, and an absolute-path example, and
exits with status 2. `tools/run_sdl.sh --menu` changes to the checkout directory
before opening the launcher.

F1–F4 and Tab change launcher pages and reset selection to the first row;
F5 launches. The ROM/save fields are text pickers: Enter begins/accepts editing,
and Escape cancels the edit without changing the stored path. There is no
native file-browser dialog. Escape outside editing cancels the launcher;
in-game it only closes a debug menu or cancels a note, and does not quit the
game. Use the window close button to quit and flush the save. F11 or Alt+Enter
toggles fullscreen. At large window sizes, the two DS screens retain their 4:3
aspect ratio and unused space is black letterboxing; touch coordinates refer
only to the displayed bottom-screen rectangle.
Keyboard and controller buttons are sampled afresh each poll, so releasing a
binding releases its DS input. Launcher controller navigation/rebinding opens
connected SDL GameController devices and refreshes them on hot-plug.


`--debug-tool` enables capture and the in-game controls for a direct launch; `--frame-time-overlay` enables only the frame-time display. Every game start prints `debug tool: ON/OFF (hotkeys F10 menu, F9 note, F8 step)` to stderr after ROM validation. When enabled, the game window shows a five-second startup banner plus clickable Note/Pause/MENU buttons at the top-right; the buttons remain available when the detailed HUD is hidden. With both options off, there is no debug session, capture ring, overlay drawing, input recording, or extra audio mixing. Headless/test options such as `--headless --frames N --input-script FILE` continue to be accepted unchanged.
For direct CLI launches these two switches are authoritative: a previous
launcher's saved Debug tool setting does not silently enable frontend hotkeys
without a matching game debug session. Launcher launches use the selected
persisted settings instead.

## Steam Controller and Steam Deck

The SDL frontend requests SDL's HIDAPI, Steam Controller, and Steam Deck paths when the installed SDL exposes those hints, opens GameController devices, supports hot-plug, and lets the launcher select among connected controllers. An optional `gamecontrollerdb.txt` in the config directory is loaded through SDL's mapping API. The four SDL back paddles can be selected as bindings when SDL exposes them. Stick dead-zone and d-pad-from-stick behavior are configurable; gyro and touchpad input are intentionally ignored.

When the game is added to Steam and launched with Steam Input enabled, Steam commonly presents the Steam Controller/Deck layout as an Xbox-style GameController, so the standard SDL mapping is used. With Steam Input disabled, behavior depends on the raw SDL/HIDAPI mapping and the installed SDL mapping database. No physical Steam Controller or Steam Deck hardware is claimed as tested here.

## Debug controls

Debug actions are active only when **Debug tool** is enabled in the launcher or `--debug-tool` is passed:

| Keyboard | Action |
|---|---|
| F10, toolbar MENU button, or controller Guide/Steam+Start chord | Open the debug menu |
| Space / Pause-Break | Pause/resume; while paused the last frame remains on screen and queued game audio is cleared/muted |
| F8 | Run exactly one emulated frame, then remain paused |
| F2 | Cycle normal, 0.5x, 0.25x host pacing; while a note is open, save it |
| F3 | Toggle unpaced fast-forward |
| F9 / Ctrl+Shift+N / click Note | Pause and open an annotation directly |
| Enter | Insert a newline in a note; Ctrl+Enter or F2 saves the bundle; Escape cancels |
| F4 | Add a non-pausing marker and low-resolution top/bottom thumbnails |
| F1 | Show/hide hotkey help |
| D (HUD visible) | Toggle RAM/VRAM snapshots for the next bundle |
| T (HUD visible) | Toggle the four-slot raw texture-memory inspector |
| F5/F6 (visible mixer), or Ctrl+Shift+F5/F6 | Select previous/next mixer channel |
| F7/F12 (visible mixer), or Ctrl+Shift+F7/F12 | Mute selected channel / toggle solo |

Debug-consumed keys do not also trigger general frontend hotkeys, even when
multiple taps arrive in one input poll. With the debug tool enabled, F3/F4
are exclusively fast-forward/mark (not lid/integer scaling). F5/F6/F7 retain
filter/layout/hi-res-scale during normal play with the mixer hidden; while the mixer
is visible, or with Ctrl+Shift, they exclusively control mixer channels.
F11 remains fullscreen. Disable the debug tool to use F3/F4 for lid/integer
scaling, or remap those general hotkeys to unused keys on the launcher's
HOTKEYS page (fifth tab) or in `frontend.conf`. Normal-play defaults other than
these: Tab fast-forward (hold), F12 screenshot, F7 hi-res 3D scale, F8 swap
(F8 is single-step while the debug tool is on).

The debug menu is the single place to select every debug action. It opens with F10, the toolbar MENU button, or the controller Guide/Steam+Start chord. The game remains paused while the menu is open; closing restores the pause state from before opening it. Navigate with Up/Down and activate with Enter, click a row with the mouse, or use the controller d-pad and A (B closes). Rows are Pause/Resume (Space / Pause), Step 1 frame (F8), Slow 0.5x/0.25x (F2), Fast-forward (F3, ON/OFF), Add note (F9 / Ctrl+Shift+N), Drop marker (F4), Quick bundle now (Menu), Frame-time graph (G, ON/OFF), Audio bar (B, ON/OFF), CPU PC (C, ON/OFF), Texture slots (V, ON/OFF), Audio mixer (M, ON/OFF), Texture inspector (T, ON/OFF), Screenshot only (S), Show debug folder path (O), Help (F1), and Close (Esc / F10). The menu action opens Add note without unpausing; saving or canceling the note restores the original pause state.

While an annotation box is open, all keyboard input is routed to the text editor. Printable characters arrive through SDL text input; Backspace/Delete, arrows, Home/End, and Enter edit the note. Gameplay mappings and debug hotkeys are suppressed, and no DS button state reaches `Machine::set_input`. Events are applied in arrival order even when multiple events are drained in one poll. The visible footer reads “Ctrl+Enter or F2 = save, Esc = cancel”; Enter inserts a newline.

The Note, Pause, and MENU controls are clickable with the mouse and are drawn by the same SDL renderer as the game image. The mouse buttons are separate from DS touch input, so they remain available when touch/mouse mapping is disabled. SDL text input stays enabled until Ctrl+Enter/F2 saves or Escape cancels.

The existing controller shortcuts remain available outside the menu: hold Guide/Steam and press d-pad Right to step, left/right shoulder for slow/fast, X to mark, Y to annotate, d-pad Up for help, d-pad Down for texture view, d-pad Left for mixer, and A/B to mute/solo. In the mixer, Guide+d-pad Up/Down selects channels. A one-second Select+Start hold pauses/resumes. During annotation, d-pad navigates the ASCII keyboard and A inserts the selected character; Start saves and B cancels. Mouse is used for DS touch only when mouse/touch is enabled in launcher settings.

Bare letter debug shortcuts (D/T/G/B/C/V/M/S/O) only operate with the detailed HUD visible; they do not steal gameplay mappings while it is hidden. Controller Guide+A/B mute/solo only operates with the visible audio mixer. Any active solo or mute shows a persistent red `AUDIO DEBUG` warning even with the HUD/menu closed, and the same warning appears in the SDL window title. Toggling logs `[audio-debug]` to stderr. These settings are session-only, default to no solo/mutes at launch, and are never written to frontend configuration or saves.

The game renderer does not request display vsync; host pacing uses a 59.826 Hz steady-clock deadline, independent of the display refresh. After a missed deadline, overdue slots are discarded instead of running a burst of catch-up frames. In debug-tool sessions, the overlay reports frame number, wall-clock frame rate/frame-time graph, audio queue backlog, ARM9 PC, and mapped 3D texture slot count. Frame-time samples include presentation and host pacing; `--headless` replays are unpaced. The texture inspector visualizes each 128 KiB slot as a byte-intensity heatmap; it is not a texture decoder.

The bitmap HUD/editor font submits pixel rectangles in bounded 256-rectangle stack batches, rather than one SDL draw command per lit glyph pixel. This preserves the 1x bitmap appearance without a per-frame glyph allocation.

`empty_audio_queue_events_approx` estimates starvation only while playback is running, SDL's software queue is empty, and the time since the previous submission exceeds its queued PCM plus one device buffer. Initial priming and explicit debug pauses are excluded; queued digital silence is not starvation. SDL supplies no hardware xrun counter, so this remains an estimate, not proof of an audible dropout. Explicit and emergency queue clears pause the device and require a fresh 40 ms prime before playback resumes. Note saves write synchronously while the game/audio is intentionally paused, then use this priming path; their file I/O need not complete within one frame.

## Issue bundles

F9 pauses and opens a multi-line plain-text annotation. Saving writes a private local issue bundle below `<data_dir>/debug/` and appends an entry to `index.tsv`. The following listing is illustrative, not a captured game record:

```text
local-note-example/
  comment.txt
  screenshot_top.png       screenshot_top.ppm
  screenshot_bottom.png    screenshot_bottom.ppm
  screenshot_both.png      screenshot_both.ppm
  state.txt                texture_state.txt
  audio_tail.wav           audio_stats.txt
  input_tail.txt           session_input.txt
  session_input.txt.frames session_start.sav
  save_before.sav          save_at_note.sav (SHA-256 metadata in comment.txt)
  env.txt
  replay.sh
  vram.bin                 mainram.bin          # when snapshots are enabled
  texture_slot0.png ... texture_slot3.png       # when inspector is open
```

`comment.txt` contains the exact plain-text note followed by timestamp, frame, emulated ticks, wall-clock metadata, and the recorded RTC epoch/timezone offset. `save_before.sav` is the launch-time save path/size/FNV-1a64 reference; `session_start.sav` is the immutable launch-time replay baseline. At capture, the GameCard save is flushed and copied as `save_at_note.sav`; its SHA-256 appears in `comment.txt`. `session_input.txt` is the full input stream in `frame buttons touch_x touch_y` grammar, with a `.frames` sidecar; `input_tail.txt` contains its last 600 frames. `replay.sh` runs the same executable headlessly through the noted frame, using a fresh copy of the baseline, recorded input, and fixed RTC base, and dumps the frame/audio plus RAM/VRAM for comparison.

Native captures also record the requested `backup_type` in `env.txt` and preserve explicit `--backup-type` overrides in both `replay.sh` and `replay.ps1`. An explicit native `auto` override is distinct from omitting the flag and inheriting the ABI configuration. Godot captures store their selected chip in the same metadata; in-app issue replay applies it before loading its copied launch save, independently of the normal-game setting. Missing metadata in older captures selects Auto. This matters for an empty launch save: early screen hashes can match even when a replay chose the wrong chip, so the replay regression also compares persisted save bytes and capacity.

Both `comment.txt` and `state.txt` start with `audio_debug: solo=<channel or -1> muted=<channel list or none>` before the note/status, so a debug mixer filter cannot be missed when interpreting an audio report. Bundle readers preserve the exact user note separately from this header.
Direct `--record-input FILE` also writes `FILE.rtc`; replaying `--input-script FILE` auto-loads the recorded epoch and timezone unless explicit RTC options override them.

`state.txt` records the machine status, ARM9/ARM7 registers, selected display/VRAM/GX/sound registers, and the SPU channel table. The runtime currently does not expose active overlay IDs or retained per-polygon texture command details, which are explicitly marked unavailable in `state.txt` / `texture_state.txt`. `audio_stats.txt` includes last-second RMS/peak/clipping, queue backlog, queue-drop and estimated starvation events, average measured emulation FPS, and the last 120 frame durations. The frame-rate estimate uses completed-frame time including presentation and pacing; headless replay remains unpaced. `audio_tail.wav` is a rolling five-second stereo PCM tail. `env.txt` stores build ID, SDL version, audio device/driver, output size, controller name/GUID, and ROM/save/language details. It does not record monitor refresh rate.
Markers are written under the session directory's `markers.log`; each marker also writes 64x48 PNG thumbnails. When enabled, automatic capture watches for six consecutive frame times over 33.4 ms, more than 1% clipped output for 100 ms, or a newly logged unimplemented I/O access. Fatal errors that abort the process cannot be captured after the abort. A ten-second instant-replay movie, full texture/palette decoding, guest-state save/load, per-polygon command dump, and fatal-process recovery are not provided; machine snapshots are not faked.


## Quick reload

`replay <bundle> --interactive` replays from the launch-time baseline and recorded input without host pacing or audio output until the noted frame, then opens the game window paused with the debug tool enabled. It creates a writable save copy under a fresh replay output directory; `session_start.sav` and `save_at_note.sav` are never passed to the game as writable saves. Resume or step normally after the window opens. Example:
During an interactive `--pause-at-frame N` replay, `--input-script` is applied through zero-based frame N inclusive. After reaching the target and resuming, live keyboard, controller, and touch input take over; headless replay stops at N.

```sh
.venv/bin/python tools/debug_bundle.py replay "$LOCAL_BUNDLE" --interactive
```

This is deterministic input replay, not a true savestate: the current core cannot serialize its suspended host Fiber continuations or opaque GPU/SPU state. Replay uses `--rtc-epoch` plus the launch-time timezone offset and advances the clock from emulated ticks, so replay-time wall-clock changes do not affect the guest RTC. Host pacing and diagnostic measurements do not feed guest emulation. Replay cost grows with session length; no wall-time estimate or complete determinism guarantee is asserted here.

## Local review and public issue reporting

Use the helper to inspect and replay a capture **locally**:

```sh
.venv/bin/python tools/debug_bundle.py list
.venv/bin/python tools/debug_bundle.py show "$LOCAL_BUNDLE"
.venv/bin/python tools/debug_bundle.py replay "$LOCAL_BUNDLE"
```

Never share saves, memory/texture dumps, screenshots, audio, or complete bundles
publicly, including helper export archives. They can contain game-derived
content and personal paths. Keep replay inputs and save baselines private.
Before posting a GitHub issue, prepare a short description of the tooling
problem and redact paths, identifiers, captured content, and game-derived
addresses or values. Do not attach the bundle.

## Godot issue reviewer

Open **Issue reviewer…** in Start / Settings or the F10 debug menu, or press
**F7**. It shows previous SDL and Godot notes together, newest first, with
open/solved counts, status badges, thumbnails and capture details. Search
comments or choose All, Open / testing or Solved. **Show auto notes** is off by
default; its setting is remembered.

1. Select a note to inspect its original screens, comment, state and audio
   statistics. A warning identifies captures made with audio solo/mute.
2. Choose **Replay to issue**. The current game is stopped cleanly and its save
   flushed. Replay uses a temporary **copy** of the bundle's launch save, its
   language/RTC and exact frame-synchronous recorded input at Max speed.
   Progress and ETA are shown, then the game pauses at the noted frame.
3. Choose **Compare with original** for original/current/difference images and
   a differing-pixel count. This is information, not pass/fail: a fix can
   legitimately change the pixels. **Play from here** resumes with live controls
   so you can test the symptom yourself. Reopen with F7 to review the result.
4. Optionally enter an observation, then **Mark solved**, **Reopen**, **Won't
   fix**, or **Add retest note**. Status and dated history are stored in
   `status.json`; all original bundle files remain untouched.
5. **Return to normal game** restarts the normal game from its flushed save.
   **Cancel replay** also offers this return. Replay saves are discarded only
   after the replay worker stops; they never overwrite your normal save.

**Open bundle folder** opens the local evidence directory. **Export report…**
writes a text report to a location you choose; inspect and redact it before
posting issue text. Never post complete captures, saves, dumps, or bundle
archives publicly. If an image has moved, select its existing local path in
Start / Settings before replaying. Replay is not a savestate: a long session
must execute again from its private launch-time save.
