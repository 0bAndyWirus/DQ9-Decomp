# DqMachine GDExtension API

`DqMachine` is a Godot `Node` implemented by the `dq9_godot` extension listed in `res://dq9.gdextension`. It hosts the shared native runtime. A generated-game build requires a separately supplied generated-code directory; this source project ships neither generated game code nor binaries.

The session reads caller-supplied runtime metadata from `DQ9_ABI_CONFIG` when no
native session path is provided. CPU-layout fields are defined in
`runtime/include/dsr/abi_config.h`; supplied CPU sections must be complete,
while omitted sections leave those guest-thread overrides disabled with a
diagnostic. Optional cartridge backup settings use the same private INI file.
The extension bundles no title-specific layout or values.

## Lifecycle and thread ownership

- `load_game(rom_path: String, save_path: String, language: int) -> String` creates a session and returns an empty string on success or an error message. It does not start emulation. Language is 0..5.
- A dedicated owner thread creates and exclusively owns the session, handles frame commands and destroys it. `start()` resumes frame execution. `stop()` pauses frame execution but keeps the owner thread/session alive so it can safely be resumed or stepped; destruction and a subsequent `load_game` stop and join it.
- `set_paused(bool)` and `is_paused() -> bool` control run state. `step_once()` while paused requests exactly one worker-owned frame.
- `step_frames(n, mask, x, y, down)` is an additional deterministic-test API, available while paused/not running; it applies one fixed input state for each of `n` frames synchronously via the owner-thread mailbox.

- Optional deterministic replay: call `set_rtc_epoch(epoch_seconds, timezone_offset_minutes)` before `load_game` to make the guest RTC advance from emulated time rather than host time. The timezone offset must be in -840..840 minutes. Production hosts that omit it use the session's normal RTC behavior.
- `set_speed(float)` selects frames per second relative to the 59.8261 Hz normal rate. `1` is realtime; `0` is unpaced; values are clamped to 0..16.
- `set_buttons(mask)` uses DS bits A=1, B=2, Select=4, Start=8, Right=16, Left=32, Up=64, Down=128, R=256, L=512, X=1024, Y=2048. `set_touch(x,y,down)` uses bottom-screen pixel coordinates and clamps to 0..255, 0..191.
- `flush_save()` requests the owner thread to flush the loaded session save and waits for completion.
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
`tests/test_godot_editor_import.py` pins the official Linux editor's exact
upstream crash signature, verifies the generated extension cache, and runs five
isolated strict runtime/lifecycle and warm-editor probes. It skips clearly if
Godot or the native extension is absent. A changed cold-import signature fails
the known-bug test so the packaging workaround can be revisited, never silently
accepting a different crash. `tests/test_extension_lifecycle.gd` also checks the
engine-class bindings and resource release, and is included in the smoke gate.

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

`get_session_start_save()` returns the save bytes captured when `load_game()` was called; `get_input_history()` returns one `frame buttons x y` row per completed frame (zero-based frame numbering, `-1 -1` when touch is up). `get_rtc_epoch()` and `get_rtc_timezone_offset_minutes()` return the deterministic RTC values configured before load. The Godot launcher uses these to create replayable `user://debug` bundles; see `tools/debug_bundle.py replay`.

Signals: `frame_ready(frame: int)` is emitted when a new frame is uploaded; `error(message: String)` reports worker-step errors.

`get_stats()` returns `frame`, `ticks`, `fps`, `audio_queue_ms` (ring fill), `underruns` (AudioStreamGeneratorPlayback skips), `dropped_audio_ms`, `running`, and `paused`; `audio_frames_pushed` is an additional cumulative count of frames accepted by generator playback.
Replay diagnostics also expose `replay_active` and the zero-based `replay_target`.

## Issue reviewer and status store

The launcher and F10 debug menu expose **Issue reviewer…**; F7 opens/closes it.
`scripts/issue_store.gd` lists both `$XDG_DATA_HOME/dq9-decomp/debug`
(default `~/.local/share/dq9-decomp/debug`) and Godot `user://debug`. It reads
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
`session_start.sav` to an OS-cache temporary save, loads the recorded language
and deterministic RTC, then invokes worker replay. Original evidence and the
normal save are never used as writable replay saves. The reviewer shows progress
and ETA, paused software screenshots/diffs, explicit live control and a return
action. Cancel offers a normal-game return; returning/closing the game removes
the replay copy after the worker has stopped. Original audio solo/mute is
warned about rather than silently reproducing a debug mixer configuration.

ROM-free tests: `tests/test_issue_store.gd`. The xvfb integration
`tests/test_issue_reviewer.gd -- <rom> <launch-save> <inputs> <native-dq9>
<output-dir>` generates a local 300-frame session, verifies all worker hashes
against native, routed UI actions, save-copy safety, immutable original files,
and captures 1280×800 and 3072×1920 layouts. Committed fixture text is explicitly
synthetic; screenshots, input and save data from local sessions stay local.
`tools/godot_smoke.sh` includes these and existing layout/click/restart tests;
set `DQ9_NATIVE=/path/to/dq9` for a worktree build and optionally
`DQ9_REVIEW_OUTPUT=/local/output` to retain reviewer captures.


## Optional GPU 3D host compositor

The launcher defaults to **Software (exact)**. Settings `renderer_gpu3d` (`0` software,
`1` GPU hi-res experimental) and `gpu3d_scale` (`2`..`6`, default `3`) persist in
`user://settings.cfg`. Software mode creates no GPU overlay node.

The host's `set_gpu3d_frame(texture: Texture2D, mask: Texture2D,
screen_is_top: bool = true)` accepts a high-resolution SubViewport texture and a
native 256×192 R8 mask from the same frame. Null inputs remove the layer for a
software fallback. `gpu3d_screen_is_top` defaults to true; a producer must supply
the physical engine-A screen after POWCNT1 swapping. The mask is sampled nearest:
zero pixels discard, preserving the software composition; nonzero pixels show
GPU colour. The overlay is a child of that physical screen's texture, so all
seven layouts, merged-overlay occlusion and touch routing are preserved.

The launcher reads `get_gpu3d_texture()`, `get_gpu3d_mask_texture()` and
`get_gpu3d_screen_is_top()` on `frame_ready`, and sends renderer settings through
`set_gpu3d_enabled(bool)` and `set_gpu3d_scale(int)`.

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
Capture now permits one nonzero partial fragment over opaque history, including
partial fragments rejected by the depth test (the GPU performs that test too).
It excludes overlapping partial touches (including duplicate IDs), translucent
depth updates (attribute bit11), partial depth-equal/tolerance cases (bit14),
blending-disabled replacements, partial destinations, fog-flag changes, and
shadow/wire footprints. An opaque fragment touched after a partial one is also
excluded: moving it to the GPU opaque pass could otherwise change history.
Alpha-test-rejected/zero fragments do not contribute a partial layer. Decal
texel alpha changes colour, not alpha31 fragment opacity.
The one-native-pixel3×3 exclusion dilation still runs after the band barrier.
No comparison error changes the mask and quality thresholds are unchanged.

The worker owns the captured mask with the delayed geometry, then combines it
with that scene's LCD engine-A `three_d_mask` in one byte pass. The compositor
sees `mask = three_d_mask AND opaque_only_mask`; excluded pixels stay software
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
single-screen layouts. Polygon/texture diagonals gain samples, not new source
textures, geometry or hi-res 2D assets.

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
