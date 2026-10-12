# Godot host: historical design proposal

> **Superseded by [GODOT_PORT_PLAN.md](GODOT_PORT_PLAN.md) — historical.** This early proposal is retained as design context, not current build instructions or a compatibility record. Use the current plan, [render-capture contract](REMASTER_STAGE1.md), and [host API](../godot/API.md).

## Decision

Godot was proposed as a presentation host over the runtime rather than a game rewrite. Both current hosts use shared runtime services and require a separately supplied generated-code directory for game execution. This public tree provides no end-to-end generator workflow.

## Original host seam considered

| Need | Existing API | Source |
|---|---|---|
| Input | `Machine::set_input(const InputState&)`; `InputState{buttons (DsButton bits), pen_down, pen_x, pen_y (bottom-screen px), lid_closed, quit_requested}` | `runtime/include/dsr/machine.h`, `frontend.h:17-39` |
| Advance | `Machine::run_frame()`, synchronous, ends at VCOUNT=262 | `machine.h` |
| Video | `Gpu2D::screen_top()` / `screen_bottom()`: 256x192 `0xFFRRGGBB` | `game/main.cpp` present call |
| Audio | `Spu::drain(int16* out, max_frames)` returns stereo frames; source rate `33'513'982.0 / Spu::kTicksPerSample` | `game/main.cpp` |
| Pacing | emulated frame rate 59.8261 Hz | `frontend.h:79` |
| Layout enum | `ScreenLayout {Vertical, Horizontal, TopFocus, BottomFocus, SingleTop, SingleBottom}` already exists in `FrontendConfig` | `frontend.h:41-51` |
| Native replacement hook | `Machine::replace_function(cpu, module, addr, expected_hash, GuestFn)`: hash-guarded replacement of one generated function; all generated direct calls use the slot | `machine.h` |
| Saves | `MachineConfig::save_path`; user dirs via `default_user_paths()` | `machine.h`, `frontend.h:53-58` |

The proposal described input, synchronous frame advance, audio drain, software-screen presentation, and host pacing as a shared service boundary. The current session library and dedicated worker arrangement are specified by the superseding plan.

## Historical proposed structure

1. Extract the loop body of `game/main.cpp` into a small library (`dsr_session`: construct from rom/save paths, `step(InputState) -> {top, bottom, audio}`), used by both `dq9` and Godot. This keeps one code path and lets the existing frame regression (`tools/regress_frames.py`) guard the shared code.
2. A GDExtension (`godot-cpp`) node `DqMachine` links `dsr_session`, the `dsr_*` static libs and the generated `dq9_gen_*` libs. It exposes `load(rom, save)`, `set_buttons(mask)`, `set_pen(x, y, down)`, `step()`, two `ImageTexture`/`Image` screens, and an `AudioStreamGenerator` push.
3. Pacing is Godot's: accumulate delta, call `step()` while the accumulator holds at least one 1/59.8261 s frame (cap catch-up to avoid a spiral of death). `Frontend::pace_frame` and the SDL window are unused.
4. Layout, scaling, filtering and the merged bottom-screen overlay are pure Godot scene work (two `TextureRect`s, a shader/Control layout); they never feed back into the simulation, matching the `frontend.h` rule that presentation does not change simulation state.

## Risks and open measurements

- Performance: measure `run_frame()` time in the actual host before choosing a threading strategy. The current plan assigns Machine ownership to one worker thread and uses a bounded screen/audio handoff.
- Audio underruns must be assessed against emulation cadence and queue state, not inferred from a single rendering measurement.
- Native software output is 256×192 per screen. Enhanced GPU presentation is separate from the software reference; keep the native-resolution comparison path and do not claim pixel parity from an enhanced screenshot.
- A texture-replacement layer needs content-based identity and a view-lifetime contract. This early proposal did not design it; later render-capture documentation describes the existing slot views and remaining design work.
- Native rewrites of game systems go through `replace_function` with the module id and entry hash. Which functions a playthrough actually calls is determined locally from a module-aware entry trace.

## Current readiness

This historical proposal does not establish current build, gameplay, or
performance readiness. Follow [GODOT_PORT_PLAN.md](GODOT_PORT_PLAN.md) and
[BUILDING.md](BUILDING.md); keep all local gameplay captures and results private.

