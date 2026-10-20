# Architecture

See [ADR-0004](DECISIONS/0004-static-recompilation.md) for why the port is a static recompilation with native DS platform services.

## Public build boundary

The repository publishes original tooling and runtime sources, not a complete
game-generation workflow. A game host consumes a separately supplied
generated-code directory; no instructions for producing it are shipped.

```mermaid
flowchart LR
  GEN[Separately supplied generated-code directory] --> BUILD[CMake host build]
  RT[Original runtime and frontend sources] --> BUILD
  BUILD --> EXE[Local host executable]
  IMAGE[Already locally available compatible image] --> EXE
```

Without generated code, CMake builds runtime libraries and tests rather than
a playable game. Generated game sources, local images, saves, captures, and
compiled game builds remain private and must not be published.

## Components

| Path | Role |
|---|---|
| `recomp/` | ARM/Thumb decoder and C++ emitter: native functions, basic blocks, live guest-memory loads, jump-table handling, and cycle-budget checkpoints. |
| `tools/arm7_analyze.py` | ARM7 function discovery using caller-supplied image and load metadata: autoload parsing, recursive descent, and pointer/jump-table scanning. |
| `runtime/include/dsr/` | Contract: `cpu.h` (guest CPU state), `mem.h` (16 KiB page-table memory access with hardware alignment rules), `rt.h` (helpers called by generated code and the `FnTable` format), component headers. |
| `runtime/core` | `Machine`: memory map, IO routing, IRQ/IE/IF/IME, timers, DMA, IPC sync/FIFO, divider/sqrt, CP15/TCM, BIOS SWI HLE, direct boot, dispatch for indirect/overlay calls, scheduler interleaving ARM9/ARM7 fibers, guest-thread fiber mapping. |
| `runtime/gpu2d` | Engines A/B, VRAM banking, palette/OAM, windows, blending, capture, master brightness (software, per scanline). |
| `runtime/gpu3d` | Geometry engine and software rasterizer. |
| `runtime/spu` | 16-channel SPU: PCM8/PCM16/ADPCM decoding across channels, PSG on channels 8–13, noise on 14–15, and capture; ADPCM deltas use the reference table’s single integer division by eight. Capture 0/1 sample on channel 1/3 timer overflows, even when the playback channel is stopped, independently of the 1024-tick output DAC ([hardware contract](https://problemkaputt.de/gbatek-ds-sound-capture.htm)). |
| `runtime/periph` | Game card and local backup storage, SPI power/firmware/touch, and RTC. Title-specific settings belong to private caller inputs. |
| `runtime/frontend` | SDL2 window/layouts, keyboard/gamepad/touch, audio resampling, 59.8261 Hz pacing, Steam Deck defaults; over-cap audio queues discard stale backlog and append newly resampled audio. |
| `game/` | `dq9` executable: owns the host loop (input → `Machine::run_frame` → present/audio/pace), headless/scripted mode and frame dumps. |
| `tools/oracle` | Comparison tooling for a separately available reference emulator; emulator sources, images, traces, and captures are not bundled. |

## Execution flow

- Generated code keeps guest registers in `dsr::Cpu`; memory goes through page tables (fast path) or the core's slow path (IO, palette, OAM).
- Each guest function decrements `Cpu::budget`; at function entries and loop headers a non-positive budget calls `dsr_checkpoint`, which delivers IRQs (HLE BIOS IRQ entry, guest handler via `dsr_call`) or yields the CPU fiber to the scheduler.
- The scheduler runs on the host main fiber and advances DS bus time (33,513,982 Hz) by scanline events; ARM9 runs two cycles per tick.
- Indirect branches resolve through the generated `FnTable`; addresses shared by several overlays retain the original first-matching FNV-1a64 resolution. A per-CPU/per-target cache reuses the result only after byte-exact, page-by-page comparison with the previous live image. Its snapshot covers the longest candidate examined through the first match (including rejected earlier candidates), or every candidate when none matched. Any change, including an ordinary code store without cache maintenance, causes rehashing. Current page mappings are consulted every time; slow/IO-backed ranges are not cached, and no hooks are added to guest stores or DMA.
- Shared-code region `MOV pc, lr` returns targeting the same region resume through its dispatcher; external returns leave the region after recording the guest return target.
- Guest execution contexts map onto host fibers; context-switch primitives are runtime services, while scheduling decisions remain in generated code.

`MachineConfig::guest_abi` supplies optional CPU context layouts without embedding
title-specific offsets in the runtime. `SessionConfig::abi_config_path` or
`DQ9_ABI_CONFIG` selects the caller's local INI metadata; the SDL host exposes
`--abi-config PATH`. Omitted CPU sections disable those thread overrides with a
diagnostic. Public field definitions live in `runtime/include/dsr/abi_config.h`;
no title layouts are shipped.

### Runtime diagnostics

Set `DSR_WATCH=addr[:length][,addr[:length]...]` to log writes intersecting selected guest-memory byte ranges; numbers accept decimal or `0x` hexadecimal, and an address without a length watches one byte. Watched pages use the core's slow write path, which logs frame, CPU, guest PC, thread context, Fiber identity, address, width, old/new values, guest registers, and CPSR. Generated stores report their exact instruction PC; other runtime-originated writes fall back to the CPU's most recently dispatched guest PC. `runtime/core/THREADS.md` documents the ARM9 stack identity checks and `DSR_TRACE_THREADS` diagnostics.

Set `DSR_VERIFY_DISPATCH=1` to compare every reused ambiguous resolution with the original live byte-hash resolver and abort on a different selected entry. Cache misses already execute that resolver; slow/IO reads are not repeated. This diagnostic deliberately restores the hashing cost on hits; leave it unset for performance measurements.

## Presentation invariants

- Presentation (layout, scale, filter, fullscreen) is frontend-only and never feeds back into the machine.
- The machine advances exactly one DS frame per `run_frame`; host pacing never speeds up simulation.
- Classic mode renders at native 256×192 per screen. Optional enhanced presentation must leave simulation state identical; see [render capture](REMASTER_STAGE1.md).

### GPU raster hot paths

GPU2D uses page-mapped VRAM reads with an overlap fallback, scanline text-map/byte/palette caching, direct OAM reads with hoisted object-matrix state, and priority lists limited to enabled backgrounds. GPU3D prepares shading/texture-wrap state per polygon and skips empty spans. These implementation choices imply no game-wide performance or parity result.
GPU2D scanline and mosaic pixels keep their finite priority/opacity/blend flags in one byte beside the native colours (eight bytes per pixel). OBJ traversal is ascending OAM index, so equal-priority candidates retain the existing winner; the winner carries its mosaic-enable bit directly rather than storing an index solely to reread OAM.
GPU3D uniform clears copy a complete trivially-copyable framebuffer pixel, including its representation padding, to permit full-width stores. The named fields retain the register clear colour/depth/ID/fog and default translucent-ID/stencil/facing values; clear-image sampling and per-band boundaries are unchanged.
Generic GPU3D texture sampling corrects integer truncation for negative fractional coordinates, avoiding a float-floor roundtrip on the baseline ISA. Format-specialized spans retain float floor so their AVX2 paths still use the hardware rounding instruction. The input domain is unchanged: a finite floored coordinate must fit in `int`. Scalar-reference coverage tests neighbouring floats at every texel boundary from −65,536 through 65,536 and random float bit patterns across the defined conversion domain.
GPU3D rejects failed-depth fragments before texture sampling and colour shading. Depth-failed ID-zero shadow masks still update stencil; successful fragments retain the same shading and blend order. Regression coverage checks that both filled and wireframe hidden polygons avoid texture lookups while visible polygons still sample.
Texture-palette address slots 6 and 7 mirror physical slots 1 and 2, matching the DS palette wrap.
Palette and OAM mirror their 2 KiB storage throughout their 16 MiB ARM9 windows; byte writes are ignored. Overlapping VRAM banks OR on reads and receive writes together. Texture and clear-image access retain live mapping semantics, including overlaps; cached texture data must be invalidated when its source changes. Hardware overlap discussion is recorded in [melonDS's VRAM notes](https://melonds.kuribo64.net/board/thread.php?pid=3975#3975).
The 3D swap/flush at scanline 192 does not rasterize. Rasterization starts at scanline 214 (the reference's rigorous-timing boundary), after the guest's VBlank texture uploads; texture and clear-image mappings are resolved live then. This renderer still rasterizes a whole frame synchronously, not scanline by scanline, so VRAM writes after scanline 214 affect the next render rather than partially updating the current image.
GPU3D texture decoding keeps RGB in native 6-bit texture-cache precision. In 4×4-compressed palette modes 1 and 3, interpolation follows DeSmuME's 5→8-bit expansion, blend, then cache-format conversion; blending the original 5-bit components first can round textured pixels differently.
Shadow colour polygons preserve their interpolated vertex RGB and bypass texture shading; only ID-zero depth-failed masks set stencil. Repeated translucent polygon IDs reject the entire fragment, independently of alpha-blend enable, without updating depth. Clear-image colour/depth slots read unmapped VRAM as zero independently rather than reverting to the register clear plane. Engine A selects the 3D BG0 layer only through `DISPCNT.BG0_3D`, including display mode 6.
GPU3D `BOX_TEST` wraps each Q4.12 endpoint to 16 bits and clips all six box faces against the six homogeneous view planes; a box-face polygon must retain at least three vertices to intersect. A single-plane rejection test is insufficient for boxes separated at frustum corners.
GPU3D geometry commands retain complete position/vector matrix pairs and use wrapping matrix-product results, while vertex transforms saturate. Matrix stack addressing wraps its six-bit pointer and still transfers data on overflow/underflow. `VTX_DIFF` adds signed Q0.12 deltas with 16-bit coordinate wrapping. Texture generation preserves the transformed coordinate range and applies distinct TEXCOORD (`>>12`) and normal/vertex (`>>24`) accumulators. Each polygon captures its viewport at assembly, so later VIEWPORT commands do not relocate already-submitted geometry.
GPU3D preserves direct-port command boundaries in the FIFO: a direct packet arriving between a packed command and its arguments executes without consuming or replacing those arguments. An incomplete packed command resumes afterward. GXSTAT test busy (bit 0) and push/pop stack busy (bit 14) count accepted commands until retirement, including commands stalled behind SWAP_BUFFERS; FIFO clear and reset discard their accounting. Complete direct-port packets held behind full queues are retried when capacity returns, so their staged parameter counts cannot leave busy permanently set. STORE/RESTORE do not assert bit 14, matching `FIFO.cpp::IsMatrixStackCommand` in the reference emulator.

GPU2D's BG mode table follows DeSmuME `GPU.cpp::_mode2type`: mode 1 keeps BG2 as text and makes BG3 affine. Extended bitmap sizes are 128×128, 256×256, 512×256 and 512×512; their screen-base field selects 16 KiB blocks independently of the global tile-map base. Large mode-6 bitmaps start at BG VRAM address zero. BG0 displays 3D only when DISPCNT's 3D-selection bit is set. Text extended palettes use both the BG's slot (including BG0/BG1's slot-2/3 selection) and the tile entry's 256-colour palette number; indexed bitmaps use the ordinary palette.

Bitmap OBJ mode is supported, with zero OAM alpha transparent and nonzero alpha selecting EVA=alpha+1, EVB=16−EVA independently of BLDALPHA. Odd tile numbers remain valid for 256-colour OBJ, including 1D boundaries of 32/64/128/256 bytes. BG mosaic retains rendered colours across vertical mosaic lines (including palette/register changes), while OBJ mosaic uses the screen-space grid after selecting the winning OBJ. The 3D layer does not use BG mosaic.

Display capture reads the composed engine-A layers before display-mode selection and master brightness. FIFO capture drains a full 256-pixel line even for 128-wide capture, preserves FIFO alpha, and is separate from FIFO display consumption; VRAM-only copies force the captured alpha bit. VRAM mapping includes F/G's additional 32 KiB aliases, H/I's engine-B aliases, and CPU BG/OBJ window mirrors, including page-table aliases. Invalid F/G extended-BG-palette offsets 2/3 are unmapped.

Rendering remains per scanline at HBlank entry (`runtime/core/src/timing.cpp`), before HBlank DMA/guest handlers change registers for the next line. Affine reference points advance after the rendered line using that line's PB/PD; writes to X and Y replace only the corresponding reference coordinate. Screen swap and master brightness are sampled for each line, not once per frame. Synthetic GPU2D regressions exercise modes 0–6 on both engines, text screen blocks/flips, bitmap dimensions/bases, palettes, OBJ boundaries/double-size, mosaic, windows/blending, capture, and HBlank reference writes. They do not establish sub-scanline register timing or hardware behaviour for reserved VRAM/LCDC mappings. DeSmuME's affine bitmap-OBJ path uses OAM alpha directly while its normal path uses alpha+1; GPU2D consistently uses the normal-path alpha+1 rule. DeSmuME's undocumented LCDC overflow alias heuristic and last-mapped-bank precedence are not adopted (GPU2D retains wired-OR reads for overlapping banks).




## Platform packaging

`game/CMakeLists.txt` installs `bin/dq9` on Linux. On Windows it installs `dq9.exe`, the configured `SDL2.dll` (`DSR_SDL2_RUNTIME_DLL`), `run_game.bat`/`.ps1`, and `README-WINDOWS.txt`. MSVC receives equivalent generated-code options plus `/bigobj`. `dq9` links `Threads::Threads` explicitly because Steam Runtime sniper's glibc 2.31 keeps pthreads in `libpthread`.

`tools/build_deck.sh` builds a local Deck package in the sniper SDK. `tools/build_windows.sh` cross-builds SDL and/or the Godot GDExtension with Zig and an external SDL2 SDK. `tools/package_windows.sh` stages an explicit binary/resource whitelist and performs heuristic audits. All packages are local-only and must never be uploaded. Windows CI builds/tests public runtime code without publishing binaries; reference-source and generated-code prerequisites are described in [Windows builds](BUILDING_WINDOWS.md).
