# Stage 1: rendering and UI over the shared runtime

Status reviewed 2026-10-12: read-only render-state capture and Godot host sources are present. Optional GPU3D presentation uses a conservative hybrid mask; exact software output remains the default/reference. See the current [Godot plan](GODOT_PORT_PLAN.md), [host API](../godot/API.md), and [shader notes](GPU3D_SHADER_NOTES.md). This document publishes implementation contracts, not captured-scene records or game performance measurements.

## 1. Goal and guardrails

Keep `dsr::Machine` as the simulation owner. Presentation can display native software screens or consume captured draw data, but observation must not change registers, guest memory, timing, command order, RNG, DMA, or saves.

Capture a coherent frame on the Machine-owner thread after `run_frame()`. Existing GPU arrays are live storage, not immutable cross-thread objects. Copy any data that must outlive its view into preallocated renderer-owned slots. A bounded frame handoff may drop/coalesce presentation frames; it must never advance simulation faster or stall guest execution for GPU readback.

A game host requires a separately supplied generated-code directory and an already locally available compatible image. `tools/build_port.py` runs the local pipeline (currently not bootable); no game binaries are shipped.

## 2. Existing draw-data APIs

### Machine and GPU3D

`runtime/include/dsr/gpu3d.h` exposes the following read-only views:

| API | Current content | Lifetime / limitation |
| --- | --- | --- |
| `geometry_view()` | Transformed vertices, polygon records, active counts, generation, swap mode, overflow flags | Borrowed GPU storage; consume before the next relevant swap/render or direct mutation. |
| `draw_order_view()` | Opaque and translucent polygon index lists | Borrowed completed-render order; invalidated by the next render start/reset. |
| `raster_view()` | Raster/clear/fog/toon/edge state, software output, optional capture metadata | Live GPU state; copy for asynchronous use. |
| `texture_view(const Gpu2D&)` | Resolved texture/palette source views | Mappings and bytes remain Machine-owned and can change on the next step. |
| `line(y)` | Packed software 3D output row | Preserve as the native compositor's reference, not a general ARGB image. |

`Machine::gpu2d()` / `gpu3d()` provide component access; `capture_render_frame()` combines their views into `dsr::RenderFrame`. A transformed geometry view is not a command history. No getter can reconstruct consumed FIFO writes or their timing after the fact.

The renderer must retain polygon attributes, texture/palette parameters, viewport state, and draw order. Reordering equal-material surfaces across an ordering dependency is not allowed. Geometry generation and overflow flags matter independently of array counts.

### Textures and palettes

GPU2D exposes native texture/palette mappings, physical bank views, and a source-generation token. Decoding preserves indexed, A3I5, A5I3, direct-color, and compressed texture formats, palette aliases, overlaps, and unmapped reads.

`vram_generation` is an invalidation token, not a count of upload transactions. Mapping-control writes and writes to current texture/palette-owned storage invalidate it. CPU-visible uploads can use direct bus pages; mandatory remapping back to texture storage must be observed before cache reuse. Never assume every upload passes through `Gpu2D::arm9_write()`.

Retaining raw VRAM pointers across a Machine step is unsafe. Source replacement/reset must also invalidate caches, even when a numeric generation matches an earlier source.

### GPU2D and scanline state

The renderer produces visible rows at HBlank from live registers, affine accumulators, palettes, OAM, and VRAM. End-of-frame memory/register state cannot reconstruct earlier rows when those values changed during active display.

Current capture includes final screens, engine state/memory, and the scanline-produced 3D-selection mask. It does not include full BG/OBJ layer planes, affine history, general window/effect history, or per-line engine-to-LCD routes. Those remain necessary for exact enhanced 2D replay; do not advertise the current record as a complete 2D command stream.

Display capture writes guest-visible VRAM and remains a runtime hardware operation. A Godot compositor is not a substitute. Hardware screen swap is likewise distinct from a host layout choice.

## 3. Implemented `RenderFrame` contract

`dsr::RenderFrame` is caller-owned fixed-size metadata containing borrowed geometry, raster, texture-slot, engine-register/memory, and final-screen views. `capture_render_frame(const Machine&, frame_number, destination)` copies fixed-size register tables but does not allocate or copy geometry/VRAM.

Schema 2 includes clear state, DISP3DCNT, alpha-test reference, fog color/offset/density, toon and edge tables, geometry generation/overflow, and texture invalidation metadata. Raster tables are current register state, not per-polygon replay histories.

All content pointers refer to live Machine-owned storage. Capture and consume on its owner thread; retaining a record requires copying the referenced content before the next step or direct mutation.

Software 3D output is packed native RGB6/A5. Final top/bottom screens are packed `0xFFRRGGBB`; consumers must not confuse these formats.

### Hybrid masks and temporal pairing

`three_d_mask` is a native 256×192 row-major byte array in engine-A coordinates, independent of LCD swap. A set bit means the normal-display compositor selected opaque BG0 3D without blending or a later color transform. Occlusion, window exclusion, partial alpha, brightness, blanking, or non-normal display can leave the pixel software-owned.

`opaque_only_mask` is optional and unavailable until a capture-enabled render completes. It permits opaque history and one supported partial layer over an opaque destination. Overlapping partial touches, duplicate IDs, depth writes, depth-tolerance cases, fog-history changes, later opaque touches, shadows, and wireframes remain software-owned. A serial exclusion dilation follows the band barrier.

Godot copies the matching mask at the geometry-render boundary and delays its owned snapshot to match the LCD frame. It must not combine newly rendered geometry/masks with the preceding LCD image. GPU replacement pixels are selected only by the correctly paired combined mask, never by trimming comparison failures.

Optional capture is excluded from the default software fragment loop. Buffers/counters belong to bands while drawing and are reduced after completion. Capture on/off must preserve software output and guest state.

## 4. GPU presentation and remaining design work

The current Compatibility path uses transformed geometry, bounded material/surface grouping, decoded texture data, and integer-style DS shading. High-resolution output is an enhancement, not a claim of exact native rasterization. Unsupported state/history falls back to software.

| Feature | Requirement |
| --- | --- |
| Geometry / texture sampling | Preserve clip coordinates, UVs, native color precision, repeat/flip, viewport, and adjacent draw order. |
| Toon / highlight / fog | Retain table values, flags, and integer truncation rules; a generic engine effect is not equivalent. |
| Alpha / depth / polygon IDs | Preserve native history or leave unsupported pixels software-owned. Ordinary GL blending/stencil is insufficient. |
| Edges / clipping / one-dot / wireframe | Require explicit synthetic fixtures and a software reference; GPU defaults do not establish parity. |
| Enhanced 2D | Needs per-line layer/effect/route capture; final memory alone is insufficient. |
| Texture replacements | Godot GPU 3D uses optional content-identified replacements without mutating guest VRAM or timing; software remains the reference. |

Replacement identity includes decode metadata, referenced texture bytes, compressed metadata and effective palette bytes, not a VRAM slot. Compressed metadata's palette offset is normalized out of the identity while its effective palette is included. Missing or invalid replacements retain native decoding; no game-derived texture records or replacement assets are bundled. See the [Godot API](../godot/API.md#hd-texture-replacement-and-dumps) and [authoring workflow](USING_GODOT.md#hd-texture-replacements).

## 5. Threading, audio, input, and debug boundary

One simulation thread owns Machine throughout its lifetime. Godot UI submits input/control requests and consumes immutable copied frame slots; neither the audio callback nor renderer can call `run_frame()` or mutate Machine storage.

Drain SPU stereo PCM on the simulation side into a bounded handoff. Resampling, presentation pacing, and audio queue management are host services; they must not alter guest simulation cadence. Measure backlog independently from rendering latency.

Layouts, filtering, scaling, fullscreen, merged overlays, and touch-coordinate transforms remain presentation-only. They never write back to emulated display registers.

Reuse the shared [launcher/debug workflow](LAUNCHER_AND_DEBUG.md). Saves, RAM/VRAM snapshots, textures, geometry, screenshots, audio, and replay bundles can contain game-derived content and private paths. Keep them local; redact issue text before posting and never attach the bundles publicly.

## 6. Verification gates

- Synthetic CPU/GPU2D/GPU3D tests exercise defined hardware and view-lifetime behavior.
- Capture on/off and inline/threaded rendering must preserve software output and named framebuffer state.
- Same-frame comparisons isolate renderer differences from simulation/input differences.
- At native resolution, an exactness claim requires zero differing pixels and zero maximum channel delta, not a perceptual tolerance.
- High-resolution visual goals and hardware-GPU performance require separate measurements; a successful software fallback is not proof of complete GPU support.
- Godot/SDL host parity needs identical local inputs and compatible separately supplied generated code. Keep those inputs and outputs private.

Planned per-line capture and broader GPU effect support are not delivered by this document. Current build prerequisites are in [Building DQ9-Remastered](BUILDING.md); synthetic rendering probes are described in [shader notes](GPU3D_SHADER_NOTES.md).
