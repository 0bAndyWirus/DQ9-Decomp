# DqMachine GDExtension API

`DqMachine` is a Godot `Node` implemented by the `dq9_godot` extension listed in `res://dq9.gdextension`. It hosts the shared native runtime. A generated-game build requires a separately supplied generated-code directory; this source project ships neither generated game code nor binaries.

The session reads caller-supplied runtime metadata from `DQ9_ABI_CONFIG` when no
native session path is provided. CPU-layout fields are defined in
`runtime/include/dsr/abi_config.h`; supplied CPU sections must be complete,
while omitted sections leave those guest-thread overrides disabled with a
diagnostic. Optional cartridge backup settings use the same private INI file.
The extension bundles no title-specific layout or values.

## Lifecycle and thread ownership

- `set_backup_type(name: String)` / `get_backup_type() -> String` choose the emulated cartridge save chip (`auto`, `eeprom64k`, ... as the native `--backup-type`); call before `load_game`, an unknown name makes `load_game` return an error. Empty or `auto` keeps the ABI config / save-size choice.
- `load_game(rom_path: String, save_path: String, language: int) -> String` creates a session and returns an empty string on success or an error message. It does not start emulation. Language is 0..5.
- A dedicated owner thread creates and exclusively owns the session, handles frame commands and destroys it. `start()` resumes frame execution. `stop()` pauses frame execution but keeps the owner thread/session alive so it can safely be resumed or stepped; destruction and a subsequent `load_game` stop and join it.
- `set_paused(bool)` and `is_paused() -> bool` control run state. `step_once()` while paused requests exactly one worker-owned frame.
- `pause_at_frame_boundary()` pauses, waits for the owner to complete in-flight or previously requested steps, and uploads the latest completed frame without requesting another guest frame. Callers restore the previous pause state.
- `step_frames(n, mask, x, y, down)` is an additional deterministic-test API, available while paused/not running; it applies one fixed input state for each of `n` frames synchronously via the owner-thread mailbox.

- Optional deterministic replay: call `set_rtc_epoch(epoch_seconds, timezone_offset_minutes)` before `load_game` to make the guest RTC advance from emulated time rather than host time. The timezone offset must be in -840..840 minutes. Production hosts that omit it use the session's normal RTC behavior.
- `set_widescreen_camera(enabled: bool)` selects the guest 16:9 camera before `load_game`; it defaults to false and cannot change a loaded session. Godot Full mode enables it.
- `set_speed(float)` selects frames per second relative to the 59.8261 Hz normal rate. `1` is realtime; `0` is unpaced; values are clamped to 0..16.
- `set_buttons(mask)` uses DS bits A=1, B=2, Select=4, Start=8, Right=16, Left=32, Up=64, Down=128, R=256, L=512, X=1024, Y=2048. `set_touch(x,y,down)` uses bottom-screen pixel coordinates and clamps to 0..255, 0..191.
- `flush_save() -> bool` requests the owner thread to flush the loaded session save and waits for the actual result. `true` means pending writes completed or there was nothing to write; `false` means a write failed or no loaded session can accept the request. Failures leave the in-memory data dirty so the next flush can retry; they do not stop emulation. The Godot host does not schedule periodic flushes: existing calls are debug-bundle capture and session close, followed by a final best-effort shutdown flush. Debug-bundle capture reports a failure rather than copying a stale disk save as the current state.
- `get_save_backups() -> Array` requests the loaded session's managed backup list
  on its owner thread. Each dictionary has `path: String` and `size: int` (raw
  bytes). Entries retain **native filename order, newest first**, not filesystem
  modification-time order. Names include UTC nanoseconds and a collision
  sequence, so sorting human-readable second-resolution labels would lose the
  native ordering. An unloaded machine returns an empty list.
- `get_save_backup_error() -> String` returns the error from the most recent
  `get_save_backups()` request, or an empty string when listing succeeded,
  including a successful empty list. Read it immediately after listing: an empty
  array alone does not distinguish no backups from a filesystem/listing failure.
  The getter performs no new scan. The panels display listing failures and leave
  the restore list empty rather than reporting that there are no backups.
- `get_save_capacity() -> int` returns the loaded cartridge's actual resolved
  save capacity in bytes, or zero when no session is loaded. This is not merely
  the requested chip string returned by `get_backup_type()`.
- `restore_save(path: String) -> String` asks the owner thread to use the native
  atomic save restore. Empty return means success; any nonempty string is an
  error. The destination machine must be **freshly loaded and unstarted**:
  `start`, stepping or replay makes direct restore invalid, even after pausing
  or calling `stop`. The raw source must exactly match the actual cartridge
  capacity; restore never changes the chip. The runtime rechecks the source,
  preserves the existing destination in a backup and atomically replaces its
  bytes. An empty path selects the newest managed backup. These calls neither
  start nor step emulation.
- Worker-side input replay: `set_input_script(rows: PackedStringArray) -> String`
  accepts zero-based `frame buttons x y` rows on a freshly loaded, stopped
  session. Missing frames have no input, duplicate frames use the last row, and
  touch is down when x is nonnegative, matching native `dq9 --input-script`.
  `start_replay(target_frame: int, record_hashes: bool = false) -> String` runs
  unpaced through the zero-based note frame inclusive, ignoring live input,
  then pauses exactly there. Both return an empty string on success.
  `replay_finished(frame)` is emitted on the main thread **after** the target
  images and `frame_ready` are updated. Audio is silent during replay; queued
  seek audio is cleared before live control resumes. `cancel_replay()` stops
  playback; destruction still joins the worker and flushes its copied save.
  Diagnostic `record_hashes=true` collects every replay frame's two RGB FNV-1a
  hashes, even if presentation skips frames; `get_replay_hashes()` returns native
  hash-file rows. Normal replay does not calculate/store these hashes.

## Read-only RAM snapshots

`register_snapshot_range(address: int, size: int) -> String` registers a
nonempty range wholly within canonical main RAM (`0x02000000` through
`0x023fffff`) or one potential ITCM/DTCM window (`0x00000000` through
`0x00007fff`, `0x00800000` through `0x00803fff`, or `0x01ff8000` through
`0x01ffffff`). Each TCM window may match either live ITCM or DTCM.
At most 16 ranges totalling 65,536 bytes may be registered. It returns an
empty string on success or an error for an invalid range or limit violation.
Register ranges after a successful `load_game()`; a subsequent load clears
them. `clear_snapshot_ranges()` removes all registrations.
`get_ram_snapshot() -> Dictionary` returns a consumed frame's snapshot as
`{"frame": int, "ranges": Array}`; each range contains `address: int` and
`bytes: PackedByteArray`. It is empty until a frame with the current
registration has been consumed. Copies follow ARM9 effective RAM/TCM mapping,
including enabled TCM overlays of main RAM and physical TCM mirroring. DTCM
page mappings take precedence; non-page-mapped TCM uses ITCM-first slow-path
precedence. A range may span mappings, but any invalid byte makes the entire
frame snapshot unavailable, not zero-filled. The session owner copies bytes
directly without bus reads; snapshots are read-only copies, not guest memory
references.

The copy is taken at the end of the emulated frame step. It is not necessarily
the guest state that produced the displayed scanout: 2D scanlines 0–191 and
the GPU scene are rendered earlier. Use the frame tag for correlation, not as
a guarantee of pixel-exact temporal correspondence.


The Godot project caps its host frame loop at 60 Hz so renderers without effective vertical sync do not busy-spin and starve the emulation thread. Input is copied under the worker mailbox mutex. The worker converts completed session frames into three preallocated RGBA screen slots and publishes only slot indices under that mutex. It replaces stale unconsumed frames; if no slot is available, it skips that presentation update rather than waiting. The main thread claims a published slot under the mutex, copies pixels into reusable byte buffers outside the lock, then releases the slot before updating the stable `Image`/`ImageTexture` resources. Godot APIs, signals and AudioStreamGenerator access run only on the main thread.

Native teardown is explicit: `NOTIFICATION_PREDELETE` stops/joins the session
owner thread and releases audio, image/texture and GPU resources before Godot
destroys child nodes. The engine's pre-shutdown hook releases any still-live
presentation nodes; the scene-level module terminator repeats that cleanup
idempotently before godot-cpp unregisters classes. Each `Machine` owns its own
band pool and shuts it down before VRAM/device destruction. There is no static
renderer pool or static Godot `Ref<>`; DLL exit-time destructors do not join
renderer threads under Windows' loader lock.
Godot's generator playback itself borrows a raw generator pointer while
`AudioServer` can retain playback for an asynchronous stop/fade-out. Each
playback therefore holds an engine-owned generator metadata reference. The
source survives that final mix without extension-owned references or an
exit-time static destructor.

The **official Godot 4.5 editor**, independently of this teardown, can abort on
a first cold `--headless --editor --import --quit`: its deferred
`EditorHelp::_gen_extensions_docs` runs after `cleanup_doc` deleted the doc
receiver. This is [upstream #111048](https://github.com/godotengine/godot/issues/111048)
and [#111645](https://github.com/godotengine/godot/issues/111645), not a runtime
extension unload failure. Runtime probes always require exit zero.
`tests/test_godot_editor_import.py` accepts only the official Linux 4.5 editor's
exact upstream crash signature on a cold import (a unit test checks that matcher
against the signature and near-miss variants, since the race does not reproduce
on every run), verifies the generated extension cache, and runs five isolated
strict runtime/lifecycle and warm-editor probes. It skips clearly if Godot or the
native extension is absent. Any other cold-import crash fails, never silently
accepting a different crash. `tests/test_extension_lifecycle.gd` also checks the
engine-class bindings and resource release, and is included in the smoke gate.
A cold import with the official Linux 4.7.2 editor has also been observed to
abort after writing the extension cache, with no diagnostic on stderr; a
debugger stops first at a segmentation fault in the engine. This newer failure
remains a failing regression, not an accepted replacement for the pinned 4.5
signature. Import failures include the actual process return code and engine
version so a signal termination is not mistaken for a missing-class failure.
Both native classes are registered at scene initialization regardless of the
display backend; headless mode does not intentionally omit either class.
On a clean checkout, a standalone `--script` probe must explicitly load
`res://dq9.gdextension`, as the application does: the editor-generated
`.godot/extension_list.cfg` is not present yet. The lifecycle probe loads the
real extension and checks its load status before asserting registration.
`tests/test_godot_editor_import.py` covers this cold runtime path separately
from editor import, so the smoke gate needs no editor warm-up or rendered
backend for the lifecycle checks.

When GPU 3D is enabled, each slot also owns a bounded copy of the captured
`RenderFrame`: at most 2048 polygons and 6144 transformed vertices, in the
software renderer's opaque-then-translucent sorted draw order, plus tables,
registers and the engine-A compositor mask. Texture decoding runs on the owner
thread **immediately after scanline-214 software rendering**, before any later
guest VRAM writes. Geometry/order refer to the render buffer swapped at192 and
rendered at214, not to a newly accumulating GX list. LCD rows0..191 in frame N
use the preceding frame's rendered scene, so the worker rotates owned scene
snapshots with a **one-frame delay**, pairing the prior scene with this frame's
mask, screen swap and software pixels. The first frame after load/enable has no
prior snapshot and reports `warmup`. Godot resources and mesh rebuilding remain
on the main thread, before the slot is released. Decoded uploads survive replaced
publications until the main thread acknowledges their keys.


## Presentation and audio

`get_top_texture()` and `get_bottom_texture()` return stable `ImageTexture` objects (256×192 RGBA8), updated in place as completed frames are consumed. `get_top_image()` and `get_bottom_image()` return the same main-thread `Image` backing data for deterministic frame hashing/tests. `get_audio_player()` returns a child `AudioStreamPlayer` using an `AudioStreamGenerator` at the Godot output mix rate. Worker-produced interleaved stereo signed-16 audio enters a bounded ring protected by a separate audio mutex. The worker holds that mutex only to copy PCM into the ring; it never waits for main-thread resampling or generator access. The main thread snapshots only enough PCM to fill the generator's available space toward a 40 ms target for rendered backends under the mutex, then resamples and pushes outside it with ±1% adaptive rate correction. The headless backend fills its 100 ms generator buffer because its dummy mixer does not provide realtime output pacing.

Returning from unpaced playback to any paced speed, or reducing a
faster-than-1x speed, trims the source ring to its newest emulated frame.
The already bounded generator queue is retained;
normal playback is not stopped/restarted. Pause/stop retain queued PCM, and
resume re-primes the generator before unpausing it. Neither policy changes the
emulated PCM stream or the existing adaptive resampling policy.

`get_session_start_save()` returns the save bytes captured when `load_game()` was called; `get_input_history()` returns one `frame buttons x y` row per completed frame (zero-based frame numbering, `-1 -1` when touch is up). `get_rtc_epoch()` and `get_rtc_timezone_offset_minutes()` return the deterministic RTC values configured before load. The Godot launcher uses these to create replayable `user://debug` bundles; see `tools/debug_bundle.py replay`.

Signals: `frame_ready(frame: int)` is emitted when a new frame is uploaded; `error(message: String)` reports worker-step errors.

`get_scene() -> int` returns the latest owner-thread sample after a completed
guest frame, or `DqMachine.SCENE_OTHER` before loading and after a new load
resets the state. Constants are `SCENE_OTHER = 0`, `SCENE_BATTLE = 1`,
`SCENE_CHARACTER_CREATION = 2`, `SCENE_INTRO = 3`, and `SCENE_TITLE = 4`.
INTRO and TITLE are reserved but not currently detected; absent another
established class, they remain OTHER and are unvalidated for automatic
presentation switching. Unrecognized or unavailable guest state is OTHER.
`scene_changed(scene: int)` is emitted on the main thread for each sampled
transition, in order, even when presentation skips frames. Delivery occurs in
`_process`, after that process call's `frame_ready` if a frame was uploaded;
synchronous `step_frames` queues signals until processing resumes. `get_scene()`
can already reflect a later sample than the signal argument. Reload discards
pending notifications and resets to OTHER without emitting a reset signal.
These signals are presentation-timed, not guest-frame-boundary callbacks.
A future guest-camera consumer must apply scene changes on the owner thread at
a guest-frame boundary, not directly from a Godot signal. The classifier has
not been validated for automatic layout switching; see
[scene detection](../docs/SINGLE_SCREEN_SCENES.md) for its limitations.

`get_stats()` returns `frame`, `ticks`, `fps`, `audio_queue_ms` (ring fill), `underruns` (AudioStreamGeneratorPlayback skips), `dropped_audio_ms`, `running`, and `paused`; `audio_frames_pushed` is an additional cumulative count of frames accepted by generator playback.
`audio_ring_overflows` counts source frames overwritten only at physical ring
capacity (separate from intentional latency drops); `audio_ring_max_ms` is the
source ring high-water mark. `fast_forward_discarded_ms` is the subset of
`dropped_audio_ms` discarded specifically on fast-forward release. These
counters and the high-water mark reset on `load_game`.

Diagnostic builds configured with `-DDQ9_AUDIO_TESTS=ON` additionally expose
`set_test_audio_capture(enabled)` and `take_test_audio()`. Capture is disabled
until explicitly enabled; toggling it clears the diagnostic buffer.
`take_test_audio()` drains the collected **source** stereo signed-16
little-endian PCM, before ring drops, resampling, volume and Godot mixing.
Use short, bounded local runs; diagnostic capture is intentionally unbounded
and is absent from production builds. See
`tests/audio_pcm.gd` and the audio checks in `docs/USING_GODOT.md`.
Replay diagnostics also expose `replay_active` and the zero-based `replay_target`.
`loaded` reports whether a live session is available. `last_save_ok` is the result
of the most recent completed explicit or shutdown flush, published under the
worker mutex; it starts as `true` on each `load_game` and is not proof that the
guest has saved any progress. Both launcher settings and pause/debug panels show
the latest confirmed **Saved** / **Save failed** result, or that no flush has
been observed yet. The launcher retains the existing long failure toast on a
`true` -> `false` transition and one recovery toast after a later successful
flush, not on every presented frame. A new session's initial `true` is not a
confirmed recovery from an earlier failure. A failed close flush remains visible
in the launcher and is logged before the final best-effort shutdown retry; that
retry's result is not confirmed by the panel. Ordinary closing still discards
the in-memory session, so repair the path and retry before closing.

The panels list timestamped backups for the actual current game's launch
configuration, not newly edited settings. Issue-replay copies are identified
and restore stays disabled throughout replay and subsequent live play from the
issue. Without a game, an explicit/panel-open refresh loads a temporary native
machine against the typed ROM/save/chip plus ABI configuration, obtains capacity
and backups, and frees it without running guest frames. The UI preserves native
ordering, bounds the visible list to the newest 50 entries, and offers capacity
mismatch hints; the native restore remains authoritative.

`main.gd` exposes `_confirm_restore(path: String)` for the modal confirmation and
`_restore_backup(path: String)` for the confirmed action. It snapshots the
requested chip and ABI/ROM/save/language configuration plus actual capacity.
When replacing an open game it stages the chosen source privately (a successful
first-write flush may rotate the original backup), pauses and flushes the old
session, and **aborts without freeing it on flush failure**, restoring its prior
pause state. After a successful flush it fully closes that machine, loads a new
unstarted one with the same configuration, rechecks capacity, performs native
restore and restarts. A later load/restore error leaves the game closed; a
stopped-settings restore succeeds without starting the game. Temporary copies
are removed after the attempt. These are cartridge-save restores, not save states.

The C boundary `dsr_session_save(session) -> int` in
`runtime/include/dsr/session_c.h` returns **0 on success**, including no pending
writes, and **nonzero on write failure or a NULL handle**. Like stepping and
destruction, it must run on the session owner thread. Both the session library
and its consumers must be rebuilt for the changed return type.

Local save/restore checks, from the repository root:

```sh
godot --headless --path godot --script res://tests/test_logic.gd
DQ9_ABI_CONFIG="/absolute/path/to/generated/abi.ini" \
  xvfb-run -a godot --path godot --script res://tests/test_save_restore.gd -- --rom "/absolute/path/to/local-rom.nds"
```

Replace the example paths with your locally generated ABI configuration and
private ROM. The second command requires a matching locally built extension;
it exercises native backup ordering, capacity/paused-session refusal,
confirmation/restart with byte equality, isolation from edited settings,
stopped restore and listing-error reporting. Saves and screenshots are disposable
local test output; do not upload them or the ROM. These commands are checks to
run, not evidence of verification on a particular platform.


## Launcher save slots

`SaveSlots` stores slot metadata in `user://save_slots/<id>/slot.json` and
slot-owned cartridge data in `save.sav` in that directory. The manager creates,
imports, exports and deletes slots with UI confirmation; these are cartridge
saves, not save states. `migrate_legacy(path, name)` registers the existing save
path in place without copying or modifying it. Deleting a legacy slot removes
its metadata, not the referenced file.

`create_slot(name)`, `migrate_legacy(path, name)` and `import_slot(path, name)`
return dictionaries containing `id` and `error`. `export_slot(id, path)`,
`delete_slot(id)` and `update_playtime(id, seconds)` return an error string;
`list_slots()` and `get_slot(id)` retrieve metadata. `backup_rows(id, machine)`
and `create_manager(machine = null)` support the manager; its
`slot_selected(id, path)` and `restore_requested(id, backup_path)` signals leave
launch and native backup restore to the host. Restore is still governed by the
fresh-session, capacity and flush requirements above.

## Issue reviewer and status store

The launcher and Developer controls expose **Issue reviewer…**; F7 opens/closes it.
`scripts/issue_store.gd` lists `$XDG_DATA_HOME/dq9-remastered/debug`
(default `~/.local/share/dq9-remastered/debug`), the legacy
`$XDG_DATA_HOME/dq9-decomp/debug` root, and Godot `user://debug`. Existing
bundles remain in place; the reviewer reads both native roots without copying
or overwriting their evidence. For duplicate bundle directory names, the first
root takes precedence: the new native root, legacy native root, then Godot.
Godot's `user://` identity remains unchanged. It reads
existing bundle files, sorts by capture date, and filters all/open (including
testing)/solved, comment search and optional auto notes. `issue_show_auto`
persists through `host_logic.gd` and defaults to false.

Only `status.json` inside each bundle is written:

This schema example is synthetic; its timestamp, frame and note are not session evidence.

```json
{
  "status": "testing",
  "updated": "2026-10-12T12:00:00Z",
  "history": [{"time": "2026-10-12T12:00:00Z", "status": "testing", "note": "Started deterministic replay"}],
  "last_test": {"time": "2026-10-12T12:00:10Z", "frame": 299, "result_pixels_differing": null, "build": "dq9_godot sha256:..."}
}
```

Statuses are `open`, `testing`, `solved`, `wontfix`; missing sidecars mean open.
`last_test` is empty before a replay, uses a null pixel result before comparison,
and records a count after comparison. Transitions and retest notes append
history. A difference count is informational, never an automatic verdict.
Reports are plain text exported outside the bundle, with every issue/status
and review history, including hidden auto notes.

`main.gd` flushes/releases the current game through `_release_machine`, copies
`session_start.sav` to an OS-cache temporary save, applies the recorded language,
cartridge backup selection and deterministic RTC before load, then invokes worker
replay. `env.txt` records `backup_type: <canonical-name>` using native
`--backup-type` names, with `auto` for an unset request. Missing or invalid backup
metadata in older bundles selects `auto`, never the current normal-game setting.
Replay does not change that setting; a capture during replay records the replay
machine's own selection. Original evidence and the
normal save are never used as writable replay saves. The reviewer shows progress
and ETA, paused software screenshots/diffs, explicit live control and a return
action. Cancel offers a normal-game return; returning/closing the game removes
the replay copy after the worker has stopped. Original audio solo/mute is
warned about rather than silently reproducing a debug mixer configuration.
Godot bundles record the active widescreen mode; the reviewer restores Full
before loading so its guest state is replayed correctly. Older bundles without
this field default to Off. Full bundles require the Godot reviewer rather than
silently replaying different guest state in SDL.

ROM-free tests: `tests/test_issue_store.gd`. The xvfb integration
`tests/test_issue_reviewer.gd -- <rom> <launch-save> <inputs> <native-dq9>
<output-dir> [frames-requiring-save-write]` generates a local session from an
empty launch save with explicit `eeprom64k`, verifies every worker hash against
native, then captures/reloads the production replay and checks metadata, output
and saved bytes. With the optional frame count, it also requires actual persisted
64 KB saves from the worker, production capture and reloaded capture: supply a
local input sequence and enough frames to reach a save write. Omitting it keeps
the existing 300-frame UI/parity probe without requiring an early save write.
The test also checks missing/invalid backup metadata falling back to Auto,
unchanged ordinary settings, routed UI actions, save-copy safety, immutable
original files, and 1280×800 and 3072×1920 layouts. Committed fixture text is
explicitly synthetic; screenshots, input and save data from local sessions stay local.
`tools/godot_smoke.sh` includes these and existing layout/click/restart tests;
set `DQ9_NATIVE=/path/to/dq9` for a worktree build and optionally
`DQ9_REVIEW_OUTPUT=/local/output` to retain reviewer captures.

## Single-screen presentation

Layout index 7 composites configurable bottom-screen regions over the top
screen, without changing guest rendering or memory. Regions are painted in
order over the top display, including its GPU 3D layer; touch hits are mapped
back into the bottom-screen source. See [Building and running](../docs/BUILDING.md#run-with-an-existing-local-image)
for `single_screen_shapes` configuration and defaults.


## Optional GPU 3D host compositor

The launcher defaults to **Software (exact)**. Settings `renderer_gpu3d` (`0` software,
`1` GPU hi-res experimental), `gpu3d_scale` (`2`..`6`, default `3`) and
`widescreen_mode` (`0` Off, `1` Picture, `2` Full; default Off) persist in
`user://settings.cfg`. Software mode creates no GPU overlay node.
The live launcher background uses a separate, disposable attract machine. When
the GPU producer is available it presents 3D at the configured `gpu3d_scale`,
independently of the gameplay renderer choice; otherwise it uses smoothly
filtered software output. This affects presentation only, not the gameplay
machine or its save.
Picture widens GPU 3D presentation without changing guest state; game-side
culling can cause pop-in. Full also widens the guest camera and uses the wide
GPU target without Picture's clip-X correction, widening the field of view once.
Full is experimental, changes guest state and requires separate replay/parity
baselines. Entering or leaving Full is a next-launch choice, not a live change.
Switching between Off and Picture is live unless a Full transition is pending.
Both retain native 2D and touch coordinates. Widescreen needs GPU hi-res.
Unsupported GPU frames fall back to the native software screen; in Full the
modified camera remains active, so fallback is not Classic and may appear
horizontally compressed. For valid Full GPU frames, the guest's 256-wide 3D
raster represents the entire wide GPU target, while native 2D remains centered.
Presentation remaps the 3D source into the centered native composition and
remaps its captured opaque-only eligibility to the same coordinates before
combining it with the LCD 3D selection mask. This keeps GPU pixels aligned with
2D occlusion and software-owned pixels rather than treating the wide raster as
the centered native region. Separate presentation-screen textures supply the
underlying 2D composition and GPU-excluded pixels; native screen getters remain
the hardware-screen result for replay evidence and comparison. This correction
does not change guest screens or frame timing.
The source mapping samples destination pixel centers, applying BG0 scroll in
native coordinates before mapping into the full-width 3D raster; scroll positions
outside the 512-pixel wrap range remain transparent. Native 2D and window
coordinates are not widened.
The GPU overlay remains unscrolled and has no per-line 2D brightness pass:
nonzero BG0 scroll and color-changing master brightness therefore exclude it.
Pixels needing 2D effects or other unsupported hybrid histories still use the
software composition; the coordinate correction does not remove those limits.

In Full's side regions, presentation samples the full-wide coordinates of the
software 3D raster and its raw opaque-only eligibility mask captured with the
matching delayed geometry. Safe pixels use the GPU; unsafe pixels use the
software raster's RGB5-display colour only when its alpha is opaque. If the
software alpha is not opaque, the GPU remains visible instead: there is no
matching side-region DS 2D composition to reproduce the missing blend. Thus
partial-alpha side pixels are not equivalent to DS 2D blending. This side
fallback is Full-only; Off and Picture are unchanged.

The host's `set_gpu3d_frame(texture: Texture2D, mask: Texture2D,
screen_is_top: bool = true)` accepts a high-resolution SubViewport texture and a
native 256×192 R8 mask from the same frame. Null inputs remove the layer for a
software fallback. `gpu3d_screen_is_top` defaults to true; a producer must supply
the physical engine-A screen after POWCNT1 swapping. The mask is sampled nearest:
zero pixels discard, preserving the software composition; nonzero pixels show
GPU colour. The overlay is a child of that physical screen's texture, so layout
placement and occlusion follow that screen.
Wide GPU presentation uses a neutral black side backdrop for the 3D clear
plane, including behind partial geometry, while retaining widened geometry.
Coverage protects partial fragments and adjacent edge pixels during postprocessing.
Clear depth and alpha still participate in rendering; transparent clear planes
retain their original output. Native textures, touch and Off mode are unaffected.

The launcher reads `get_gpu3d_texture()`, `get_gpu3d_mask_texture()` and
`get_gpu3d_screen_is_top()` on `frame_ready`, and sends renderer settings through
`set_gpu3d_enabled(bool)`, `set_gpu3d_scale(int)`,
`set_gpu3d_widescreen_enabled(bool)` and `set_gpu3d_guest_widescreen(bool)`.
`set_widescreen_camera(bool)` must be set before loading; its EU projection
hook fails closed if the expected function is absent or its hash differs.

ROM-free coverage: `scripts/gpu3d_stub.gd` produces a coloured SubViewport and R8
mask. Run `xvfb-run -a "$GODOT" --path godot --script
res://tests/test_gpu_composite.gd`; it checks all seven layouts, both physical
screens and scales 2/3/6, fallback/software mode, and routed clicks. Captures are
written only to ignored `build/gpu-composite/`. The same regression and
`tests/test_logic.gd` run at the start of `tools/godot_smoke.sh` (requires Xvfb).

## Optional GPU 3D scene producer

- `set_gpu3d_enabled(bool)` opts into GPU scene capture. It defaults to **false**:
  the disabled path does not capture or copy GPU state, decode textures, or
  allocate a GPU scene. Software rendering always remains active and unchanged.
- `set_gpu3d_scale(int)` clamps to 1..6 (default 3). The `DqGpuScene` child owns an
  independent transparent, no-MSAA `SubViewport` of `256*N × 192*N` pixels.
  Widescreen rounds its width to the nearest integer to `256*N*4/3`,
  retaining the height and centering the native screen.
- `get_gpu3d_texture() -> Texture2D`, `get_gpu3d_mask_texture() -> Texture2D`
  return the scene and 256×192 R8 engine-A mask for the last consumed frame.
  Both are null for disabled/unsupported frames, so the host must retain its
  software screen. Mask byte **1**, normalized to 1/255, selects fully opaque
  unblended 3D pixels; byte 0 leaves the software compositor result untouched.
  Sample the mask with nearest filtering.
- `get_gpu3d_screen_is_top() -> bool` follows captured POWCNT1 bit 15; it does
  not assume engine A is always the top screen.
- `get_gpu3d_stats() -> Dictionary` exposes `valid`, `fallback`, `frame`,
  `polygon_count`, `captured_polygon_count`, `vertex_count`,
  `mesh_polygon_count`, `skipped_wireframe`, `surface_count`, `textures_cached`,
  `decoded_this_frame`, `vram_generation`, `source_frame`, `effects_active`,
  `mesh_count`, `color_valid`, and `scale`. Without an allocated
  scene it returns `valid=false`, `fallback="disabled"`, and zero surfaces/cache.

Textures are keyed by TEXIMAGE_PARAM, palette base and `vram_generation`. A
generation change evicts the entire worker decode/main-thread ImageTexture cache.
At most **256 new textures are decoded and uploaded per frame**. Additional
referenced keys are decoded on later frames; incomplete frames use software.
The frame ring carries reference-counted decoded bytes rather than live VRAM
pointers, and does not lose uploads when a realtime presentation is skipped.

### HD texture replacement and dumps

`set_hd_textures_enabled(enabled: bool)` and `get_hd_textures_enabled() -> bool`
control replacement textures; `set_hd_texture_dump_enabled(enabled: bool)` and
`get_hd_texture_dump_enabled() -> bool` control original-texture dumps. Both
default to false and apply only to the experimental GPU 3D producer. The
launcher persists the replacement setting in `user://settings.cfg`; dumping is
session-only. Neither modifies guest VRAM, the software renderer, or SDL.

The content ID is SHA-256 of the following byte stream (all halfwords and
32-bit values little-endian):

1. ASCII `DQ9TEX1` followed by a zero byte; then
   `texture_param & 0x3ff00000` as a 32-bit value.
2. Raw texture bytes: for format 1, 4 or 6, one byte per pixel; format 2,
   one byte per four pixels; format 3 or 5, one byte per two or four
   pixels respectively; format 7, two bytes per pixel.
3. For format 5, all metadata halfwords masked with `0xc000` in tile
   order, followed by each tile's effective palette halfwords in tile order:
   two for modes 1 and 3, three for mode 0 and four for mode 2. The metadata
   low 14 address bits select palette data but are omitted from the hash.
   For formats 1, 2, 3, 4 and 6, append respectively 32, 4, 16, 256 and 8
   palette halfwords. Format 7 has no palette bytes.

When texture or palette VRAM banks overlap, their bytes are OR-combined before
decoding and identity construction, matching the software renderer.

The identity omits VRAM addresses and wrap flags. Its lowercase hexadecimal
value names both `user://texture_dumps/<sha256>.png` and
`user://texture_overrides/<sha256>.png`. Dumping writes cached originals and newly
decoded originals. PNG channel depth is limited to 8 bits, encoded files to
20 MiB, dimensions to 2048 per axis and decoded RGBA to 16 MiB. The aggregate
replacement cache is limited to 64 MiB per VRAM generation. Missing, invalid,
oversized, or over-budget overrides use the original. If either the original
or replacement has fractional alpha, replacement dimensions must be integer
multiples of the original and its alpha must exactly match a nearest-neighbor
expansion of the original; otherwise the original is used. This preserves the
DS-derived partial-fragment eligibility.
Toggle replacement off and on to reload changed files. Original decoded images
remain available for immediate toggling and dumping; replacements with the
same identity share a GPU resource across sampling modes. The texture caches
are cleared on generation changes. Normalized UV sampling
preserves DS clamp, repeat and mirrored-repeat behavior at replacement
resolution. Unsupported GPU regions retain the software fallback.

Game-derived dumps and replacement assets must remain local; do not distribute
them with the source or issue bundles. See [Using Godot](../docs/USING_GODOT.md#hd-texture-replacements)
for the authoring workflow.


`DqGpuScene` is a registered presentation node; `DqMachine` creates it lazily.
Its `ArrayMesh` uses maximal adjacent runs of matching texture/material state
(including viewport and alpha/depth-write class), keeping sorted polygons
adjacent within each surface. Runs are split into meshes at Godot's 256-surface
limit; each chunk gets a single ordered priority (at most seventeen with both
colour passes), rather than one limited priority per polygon. GX assembly is
captured as perimeter-ordered triangles/quads. The Godot mesh converter clips
these polygons into strips whose attributes reproduce the **28.4 integer-edge
spans**, including their lack of an X prestep. Their continuous coverage follows
the shared projected polygon perimeter, split at native rows and every vertex-Y
event, rather than holding rounded terminal edges away from adjoining polygons.
At scales2..6, strips overlap by1/32 native pixel in X/Y and use one geometric
polygon-depth plane to avoid per-row depth discontinuities. Scale1 has zero
overlap and retains native span depth. Texture/shading and hi-res fragments still
run on the GPU; conversion never reads software pixels or decoded texture colours.
Float RGB6/63 uses `ARRAY_CUSTOM0`; `ARRAY_CUSTOM1` contains float NDC-Z slopes
and corner depth. UVs are captured texel coordinates divided by texture size,
clip W is UV2.x, and UV2.y bits13..14 select the overlap corner (polygon metadata
bits0..12 are unchanged). A half-DS-pixel nudge aligns native pixel centres with
the software edge walker. Cached material uniforms follow paused scale changes,
including returning from scale6 to unexpanded scale1.
Texture/mask sampling is nearest without mipmaps. A shared 256-entry RF
colour-transfer table compensates Godot Compatibility's unshaded sRGB round
trip, including its previously lost dark RGB6 levels.
Wireframe-alpha-zero and mode3 shadow polygons remain software-only. The opaque
pass discards actual partial fragments, not entire A3I5/A5I3 textures; UV2.y
retains native polygon ID/fog/alpha and translucent-order edge-skip metadata.
A second colour pass renders single-layer partial polygon/texel fragments.
It samples the opaque screen snapshot and computes the DS RGB6 integer blend
`(src*(alpha+1) + dst*(31-alpha))/32`, rather than ordinary GL alpha blending.
The destination is required to have alpha31, so native max-alpha remains31.
This pass never writes depth or replaces opaque polygon-ID/fog metadata.

Hybrid presentation uses the software renderer's optional `opaque_only_mask`,
captured with geometry at scanline214. `Gpu3D::set_capture_opaque_only(bool)` is
off by default; the Godot owner thread enables it only while GPU3D is enabled.
Capture permits one nonzero partial fragment over opaque history. A strict-depth
rejected partial fragment does not consume a layer or otherwise contaminate
eligibility only when it is behind an opaque destination by more than 255 depth
units, and after rendering every pixel in its full 3×3 final neighbourhood has
reliable opaque depth closer by more than 255 depth units. Provenance tracks the
last actual depth write, not just the accumulated framebuffer alpha. An opaque
uniform clear or an alpha31 untextured polygon or indexed texture format 2/3/4
without colour-zero transparency qualifies; cutout, direct, compressed and
variable-alpha textures, translucent depth writes, shadows and bitmap clear do
not. Zero-alpha and alpha-test-rejected fragments do not replace depth provenance.
The guard also rejects any depth-accepted nonzero dot in that neighbourhood, since
it may be native-only.
Screen-border pixels cannot satisfy the full-neighbourhood guard. Ambiguous
pixels fall back to software, including ties, near depths and
depth-equal/tolerance mode (bit14). This guard
is a conservative heuristic, not proof that native and GPU depth or coverage
are identical. Shadow stencil effects remain software-owned even when a shadow
fragment fails the colour depth test.
It excludes overlapping partial touches (including duplicate IDs), translucent
depth updates (attribute bit11), blending-disabled replacements, partial
destinations, fog-flag changes, and shadow/wire footprints. An opaque fragment
touched after a partial one is also excluded: moving it to the GPU opaque pass
could otherwise change history. Effective zero-alpha non-shadow/non-wire
fragments cannot consume eligibility, even when depth-rejected; alpha-test
rejections do not contribute a partial layer. Decal texel alpha changes colour,
not alpha31 fragment opacity. The one-native-pixel3×3 exclusion dilation still runs after
the band barrier. No comparison error changes the mask and quality thresholds
are unchanged.
Raw capture causes and touch counts remain diagnostics of coverage, including
depth-rejected fragments; they are not themselves the eligibility history.

The worker owns the captured mask with the delayed geometry. In Off and Picture,
it combines that mask directly with the scene's LCD engine-A `three_d_mask`. In
Full, it first maps the captured eligibility from the full-width guest 3D raster
to the centered native composition, then combines it with `three_d_mask`. In
either case the compositor sees the intersection; excluded pixels stay software
and native-resolution even at scale3. Single-layer colour surfaces preserve the
sorted sequence and reuse the span conversion/arrays of their opaque surfaces.
Because eligible pixels have at most one partial touch, the opaque screen
snapshot is sufficient; multi-layer/stencil/depth-write histories are not
approximated. Final RGB555 quantization follows blending and edges/fog in the
metadata viewport; it also runs on otherwise-effect-free frames.

The shared metadata viewport applies opaque fog/edge effects. Partial clear/fog
alpha is supported through the display mask: the post pass restores rear-plane
RGB6/A5 from `CLEAR_COLOR` before effects. Whole-frame software fallback remains
for W-buffering, bitmap clears, overflowing/invalid geometry and unavailable/
over-budget textures, plus warmup/disabled states; never for translucency or
shadows alone. W depth is reciprocal interpolated clip-W, not affine vertex Z,
so a vertex-only W-as-Z mapping is not correct. This path remains Z-buffered.
For diagnostics,
`DqGpuScene.get_raw_texture()/get_raw_mask_texture()` expose the unprocessed
color pass when `color_valid` is true, even if final presentation must fall back.
GPU rasterization remains approximate at sub-pixel texture/depth boundaries;
native opaque coplanar tolerance and one-dot coverage are not fully reproduced.
The software path is the exact reference/default. Intermediate 3D color remains
RGB6; the final GPU color pass truncates it to RGB555 and expands to RGB8 like
opaque software BG0. Transparent SubViewport readback is premultiplied RGBA;
numerical comparison unassociates it without excluding missing-alpha pixels.
`get_gpu3d_stats()` includes `three_d_pixels` (the original eligible LCD3D
selection), `gpu_pixels` (the combined mask count), and `gpu_pixel_share`.
Test builds additionally expose raw union pixel counts for partial polygon alpha,
partial texel alpha, shadows, wireframes and opaque decal texels, plus the eight
largest partial-touch polygons (`partial_top_polygons`: geometry index, ID,
alpha, texture format, shading mode, fragment-touch pixels). Cause unions overlap;
the diagnostic counts include rejected fragments and are not final visible shares.
Frame validity and pixel coverage are separate: a valid frame may have zero
eligible GPU pixels.

The host status overlay can report, for example, `GPU3D: 3x valid 100%   3D pixels 100%` (illustrative values, not captured results).
`valid` is the proportion of the last 120 **presented** GPU-enabled frames
with a valid producer (not the fraction of the screen). `3D pixels` is the
current combined mask's share of eligible LCD 3D pixels; 2D text, sprites and
backgrounds do not become hi-res. Invalid frames show the actual fallback
reason, and a valid frame without selected 3D pixels says `no 3D pixels`.

With GPU hi-res selected, **F6** or **3D A/B (F6)** switches only presentation
between the hi-res overlay and the exact software image. It works instantly
while paused because it keeps the same producer/geometry/textures alive;
it does not advance the game, change the saved renderer setting, or introduce
a warmup frame. Selecting Software in settings still disables capture and
releases the GPU resources. Nearest filtering remains the pixel-art default
for both native screens and the independent hi-res overlay. At the default
1280×800 vertical layout each screen is only about 492×369: 3x already renders
more samples than the window can show, and 6x is mainly useful in larger or
single-screen layouts. Higher rendering scale adds samples, not new geometry or
hi-res 2D assets; texture detail changes only when a replacement is supplied.

Engine A can appear on either physical screen; the producer follows the hardware
screen-swap state and the status names the active screen.

Linear filtering smooths high-resolution pixel steps but creates interpolated
colours rather than additional geometry or texture detail. Keep nearest as the
default and use paused F6 A/B to inspect the difference. The experimental path is
not pixel-perfect: sub-pixel coverage and texture/depth boundaries can differ.
A valid producer and high pixel share report availability, not visual parity.

Rendered integration test (configure `-DDQ9_GPU_SCENE_TESTS=ON`; requires a real
graphics backend, e.g. Xvfb):
`Godot --path godot --script res://tests/test_gpu_scene.gd -- <rom> <save-copy>
<input-script> <output-dir> <frames> <checkpoints> [language=1]
[rtc-epoch=0] [timezone-minutes=0] [--ranges=first-last,...] [--record-only]`
Supply frame counts and comma-separated checkpoints for your own local input
sequence; there are no game-specific checkpoint defaults.
The probe checks an alternating-GX temporal fixture with zero silhouette differences,
mesh/cache/mask ownership, toggle/shutdown, scale3 geometry bounds and scale3/6
interior cracks. A ROM-free four-triangle GX fixture exercises fractional-Y
shared tips, zero cracks and a byte-identical scale6→1 round trip.
Test builds copy `Gpu3D::raster_view().output` and final opaque polygon IDs at
the same scanline214 boundary. `get_reference_image()` exposes owned RGB6/A5;
`get_reference_polygon_ids()` returns49152 ID bytes from that same snapshot,
not the later LCD frame. `DSR_GPU_CAPTURE_IDS` is enabled only for this test
configuration; normal builds neither allocate nor store the ID reference.
All combined-mask pixels are compared,
including missing-alpha pixels. Range mode renders and measures every frame,
reports GPU-valid ratio, pixel-weighted GPU share, MAE and >8-level pixel rate,
and checks >=80% valid frames and per-frame MAE<4 / >8 rate<5%.
`--record-only` records failed targets honestly without turning them into a gate.
Comparisons also report complete software coverage (not a hybrid acceptance
gate). Unsupported frames retain null compositor textures and report their
fallback. Test captures include actual hybrid composites at scales1/3.
Crack measurement uses an eroded, locally colour-stable native-ID footprint
without altering the production/quality mask. It counts narrow >48-RGB8-level
background outliers missing the native ID in both ordinary and texture-alpha-
disabled GPU metadata passes; legitimate hi-res cutout texels are reported
separately. Every checkpoint at scales3/6 must have **zero crack pixels**.
Captures include both ID passes, a crack-highlight PNG and scale6 composites.

Real-app presentation regression (no synthetic producer; use a **copy** of
the save and an isolated `XDG_DATA_HOME` to leave normal settings untouched):

```sh
XDG_DATA_HOME="$OUT/user" xvfb-run -a -s '-screen 0 3200x2100x24' \
  "$GODOT" --path godot --script res://tests/test_gpu_visible.gd -- \
  --rom "$ROM" --save "$SAVE_COPY" --app-input "$INPUT" --app-output "$OUT" \
  --app-checkpoints "$CHECKPOINTS" --language en --app-rtc 0 --app-timezone 0
```

This loads `user://settings.cfg` through `main.tscn`, activates the real Start
button, and uses its native machine and production `frame_ready` connection.
It reloads the stopped session with deterministic RTC after exercising startup.
At each checkpoint it records 120 consecutive producer-validity/texture/mask
samples and the real 1280×800 / 3072×1920 window composites with GPU scales3/6,
plus frozen-scene GPU1x, masks, a temporary tint and linear-filter probe.
Status-on captures accompany HUD-hidden A/B measurements: switching A/B changes
the host status text, not the underlying DS pixels.
The assertions require a visible correctly parented/aligned overlay, genuinely
new subpixel samples versus GPU1x, measurable masked-window differences,
software-exact mask-0 texel interiors, F6 input/paused-frame preservation,
and runtime UI selection/scale persistence, enable/disable, and software-only
warmup after re-enable (never a cached texture from the deleted old viewport).
`--app-start-scale 6` tests persisted scale6; `--app-start-software` tests
enabling through settings during a software-started session.

Choose checkpoints that exercise the supplied local session's rendering states.
ROMs, saves, debug bundles, screenshots, audio and metrics remain local; neither
the host nor these tests uploads them. Never include session data in public packages.
See `docs/GPU3D_SHADER_NOTES.md` for shader design and limitations.
