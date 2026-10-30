# Band-parallel rendering implementation contract

Public implementation notes reviewed 2026-10-12. This document describes original runtime algorithms and synthetic test requirements. It contains no gameplay benchmark routes, captured-scene statistics, or game-derived baselines.

## Scope and ownership

With two or more workers, GPU3D partitions the native `[0,192)` screen into eight disjoint 24-row bands. Drawing completes before a serial edge pass; fog and packing then run with disjoint row ownership. Zero or one worker selects one full-frame pass on the caller, without pool submissions or band scratch storage.

Pixel arithmetic, polygon order, and emulated timing must remain unchanged. Guest execution, command processing, reset, source replacement, and IO/mapping writes cannot overlap a render job. GPU/reset/configuration APIs remain Machine-owner-thread-only.

GPU2D engines remain sequential. The concurrent-engine and banded-edge experiments were not retained; their proposed changes are not current runtime APIs or implementation instructions.

## Pool contract

`runtime/gpu3d/src/band_pool.h` exposes `dsr::BandPool(int threads = -1)`, `worker_count()`, and blocking `run(int bands, const std::function<void(int)>&)`.

- Default construction reads the rendering-thread policy; explicit configuration selects inline execution or a bounded worker count.
- Workers are created lazily for the first nonempty job and reused afterward.
- Every submitted band completes before `run` returns. A borrowed callback must remain alive through the barrier.
- Completion order with multiple workers is unspecified. Callbacks must be safe for concurrent invocation.
- There is no per-run queue allocation or callback copying inside the pool.
- All bands execute even if one callback throws; the first observed exception is rethrown on the caller after completion.
- Partial thread-creation failure joins created workers before propagating the failure.
- Concurrent callers are serialized. Recursive submission to the same pool is forbidden.
- Destruction requires no submitting callers and wakes/joins idle workers.

`dsr_band_pool_tests` exercises inline affinity, ordering, completion barriers, exceptions, reusable workers, and concurrent submitters. The pool does not make Machine generally thread-safe.

## Mutable-state boundaries

| State | Ownership during rendering |
| --- | --- |
| Framebuffer RGB/alpha/depth, IDs, fog, stencil, facing, and translucent classification | One band owns each pixel during drawing and fog/packing. Do not reset history between opaque and translucent passes. |
| Polygon shading and fallback texel cache | Band-owned scratch, refreshed for each overlapping polygon. No mutable cache is shared across workers. |
| Decoded texture images | Prepared and pinned on the Machine thread; workers borrow immutable data. |
| Render vertices/polygons, viewport/swap/raster state | Frozen until both blocking render stages complete. |
| Opaque/translucent draw order and prepared polygon bounds | Prepared once on the caller, then read-only. Do not clip or sort independently per band. |
| Edge-walker and interpolation scratch | Invocation-local; preserve the original sequence of additions. |
| Geometry commands, matrix stacks, FIFO/IRQ state | Machine thread only, outside raster jobs. |
| Optional capture diagnostics | Band-owned rows/counters; reduce after the draw barrier. |

Const callbacks are not automatically thread-safe: an injected texture source may hide mutable counters or caches. Shared test counters need atomic operations; immutable test storage and stateless readers are preferable. Keep overlap reads live and preserve wired-OR bank semantics rather than choosing one bank pointer.

## Ordering and cross-row dependencies

Shadow stencil, duplicate translucent-ID rejection, accumulated fog, facing/equal-depth rules, alpha blending, and optional translucent depth writes are pixel-local but polygon-order-dependent.

Edge marking reads neighbor depth and polygon IDs across band boundaries in right/down/left/up priority. It follows the drawing barrier and precedes fog. Anti-alias edge alpha and rear-plane behavior must remain unchanged; never restrict neighbor reads to the local band.

DDA values advance by successive additions. Replacing skipped-row advancement with `step * skipped_rows` changes floating-point rounding. Clipping a polygon to a band also changes interpolants. Neither is an acceptable shortcut.

Do not publish `raster_view()` or output midway through drawing, edges, capture-mask dilation, or packing. Frontend copies occur only after the render call completes.

## Persistent decoded-texture cache

`texture.cpp` decodes native RGB6/A5 images for indexed, A3I5, A5I3, direct-color, and compressed textures. It preserves wrapping, palette aliases, overlaps, unmapped reads, and compressed palette rounding. Scalar decoding remains the fallback/reference path.

`DecodedTextureCache` keys include texture parameters, palette base, and source generation. Its decoded payload and metadata are bounded. `begin_frame` starts a pinning epoch; old-generation records become invisible to lookup, while allocations can be reused by a later acquire. Reset and source replacement clear cached identities.

Preparation visits and pins required images before workers draw. Pinned images cannot be evicted in that frame. Capacity overflow uses the exact band-owned scalar/polygon-cache path, never stale data. Workers neither populate the cache nor update its LRU state or generation.

The GPU2D adapter reads `vram_generation()` on the Machine thread at render start. Sources without a generation callback retain live per-polygon behavior. Mapping changes invalidate the token; physical writes to currently texture/palette-owned storage also invalidate it. CPU-visible uploads followed by remapping to texture storage are observed before sampling. Reset clears both the source token and renderer cache.

`dsr_texture_cache_tests` covers bulk/scalar decoding, bounded and pinned records, reuse/invalidation, source replacement, and synthetic CPU/DMA/capture upload paths. Cache tests must preserve overlaps, unmapped slots, palette aliases, and ignored transfer forms.

## SIMD and 2D row batching

Four-fragment GPU3D spans retain exact perspective correction, color/depth conversion, successive attribute additions, clipping, and scalar tails. Depth rejection selects active lanes before texture access; accepted lanes write in original X order. Rejected shadow-mask lanes still update stencil. Capture-enabled spans retain scalar fragment visitation.

Supported x86-64 builds select the available vector implementation; other targets retain a scalar path. Sanitizer builds disable problematic multiversioning attributes, not the batch algorithm. Do not infer execution of a particular target clone from compilation alone.

GPU2D text backgrounds are sampled in tile-row spans. Map entries and aligned tile rows are read once where valid, while indexed/extended-palette paths preserve bytewise overlap reads. Scroll/tile boundaries, mosaic, windows, blending, and brightness ordering remain unchanged. This optimization does not parallelize the two engines.

The blended GPU2D compositor prepares back-to-front layer orders once per
scanline, including each possible OBJ priority and the no-OBJ case. Pixels
select that immutable order while retaining window/opacity filtering and
sequential blending. Equal-priority OBJ remains above backgrounds; background
ties retain descending layer-number paint order. The ROM-free
`composition_order` GPU2D test exhausts enable/priority combinations and checks
order-sensitive synthetic blending with deterministic random masks and colors.

## Synthetic identity and mutation requirements

`runtime/gpu3d/tests/test_gpu3d.cpp` compares complete synthetic scenes and named framebuffer fields between inline and threaded modes. Fixtures exercise:

- Geometry crossing every band boundary, with independently pinned nonclear pixels.
- Indexed/direct/compressed and mixed-alpha textures, wrap/flip/clamp, overlaps, and unmapped banks.
- Neighbor edge priority, anti-aliasing, fog, and rear-plane behavior.
- Shadows, repeated translucent IDs, equal-depth facing rules, and depth-write cases.
- Wireframes, one-dot polygons, clipping, viewport wrapping, fractional slopes, and Z/W depth.
- Retained geometry, live clear/texture changes, reset, source replacement, and thread reconfiguration.
- Optional capture masks and diagnostics without changing software output.

A skipped-band mutation must fail independently of the inline reference. An overlapping-band mutation must expose unsafe ownership through a race detector or an order-sensitive fixture. Moving edges before the barrier, removing shading invalidation, or fast-forwarding DDA arithmetic must not silently pass the relevant fixture.

Use CTest as documented in [Building DQ9-Remastered](BUILDING.md). Local game-image comparisons need separately supplied inputs and baselines; they are not part of this public document or evidence of compatibility. Keep those images, saves, traces, generated sources, and resulting binaries private.

## Performance reporting

Benchmark only with identical compiler flags, source inputs, thread policy, and affinity. Separate instruction/cycle measurements from wall time and hardware-GPU performance. Pool microbenchmarks do not establish whole-host gains. Shared-machine or cross-compiler measurements cannot justify a quiet-machine speed claim.

Report actual exercised environments and limitations without publishing game-derived route/checkpoint data or census totals. Correctness and explicit ownership remain requirements even when an optimization is faster.
