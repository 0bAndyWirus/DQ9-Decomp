# Godot build: technical decisions

Status: technical direction recorded 2026-10-11. Supersedes the historical [host design](GODOT_HOST_DESIGN.md); the rendering roadmap stays in [REMASTER_STAGE1.md](REMASTER_STAGE1.md). Game hosts require a separately supplied generated-code directory; no end-to-end generator workflow or binaries are shipped.

## Deliverable: two separate builds from one core

| Build | Target | Host | Output |
|---|---|---|---|
| SDL | `dq9` (existing, `game/`) | SDL2 window, launcher, debug tool | `build/<dir>/game/dq9` |
| Godot | `dq9_godot` GDExtension + `godot/` project | Godot 4.5 | `godot/bin/libdq9_godot.<platform>.so` + `godot/project.godot` |

Both link the same libraries: `dsr_core`, `dsr_gpu2d`, `dsr_gpu3d`, `dsr_spu`, `dsr_periph`, separately supplied `dq9_gen_arm9/arm7`, and `dsr_session` (frame step, screens, audio, input, save). Neither host may change emulation results: identical local inputs must preserve software screen hashes and simulation state.

## Decisions

1. **In-process GDExtension, not process + shared memory.** One address space and direct audio hand-off avoid a separate frame-transport protocol. Revisit only if Godot's threading behavior or crash isolation requires it.
2. **godot-cpp 4.5 built with CMake**, pinned to tag `godot-4.5-stable` and kept outside the repo. Select it with `GODOT_CPP_DIR`; check engine/binding compatibility for the actual versions used.
3. **Emulation runs on one dedicated worker thread** that owns the `Machine` for its whole life (guest threads are host Fibers; a Machine must not migrate between OS threads). The Godot main thread never calls into the Machine; it exchanges data through a lock-protected mailbox: input state in, latest finished frame (two RGBA screens + frame counter) and audio out.
4. **Frame pacing is the worker's job** (59.8261 Hz absolute deadlines, as in the SDL frontend); the Godot side presents the newest finished frame each render tick (no waiting). Pause/step/fast-forward are worker commands.
5. **Audio:** worker pushes drained stereo s16 into a lock-free ring buffer; an `AudioStreamGenerator` playback in Godot pulls from it. Target queue about 40 ms with the same adaptive-rate correction logic as the SDL frontend (`frontend_logic`), shared as pure functions.
6. **Video, stage 1:** two `ImageTexture`s (NEAREST/LINEAR selectable), displayed by `TextureRect`s inside Godot layout scenes (vertical, horizontal, top-focus, bottom-focus, single, merged overlay). The software rasterizer output is the reference; GPU re-rendering of captured `RenderFrame`s (hi-res 3D, texture replacement) comes later per `REMASTER_STAGE1.md` and must keep a 1x pixel-exact mode.
7. **Input:** Godot `InputMap` actions for the 12 DS buttons (+ touch from the bottom-screen `TextureRect`, mouse and touchscreen), Godot's joypad handling (SDL-backed, Steam Input compatible). Rebinding UI is Godot-side and stored in `user://`.
8. **Save files and paths:** `user://dq9.sav` default; ROM path chosen in a Godot settings scene or `--rom` argument; the ROM is never copied into the project or committed.
9. **Debug tooling parity:** the Godot build reuses the existing bundle format (`tools/debug_bundle.py` stays the consumer): note, screenshots, state, audio tail, input history, replay script. Capture code lives in the shared library, not in either host.
10. **Source layout:** `runtime/session/` (shared), `godot/` (extension sources `src/`, project files, CMake), `game/` (SDL host, unchanged behaviour). Top-level CMake options: `DQ9_BUILD_SDL` (default ON) and `DQ9_BUILD_GODOT` (default OFF, needs `GODOT_CPP_DIR`).

## Milestones and gates

| Milestone | Done when |
|---|---|
| G0 `dsr_session` + C ABI | Shared frame stepping preserves software screen hashes; ctypes comparisons use the same local inputs |
| G1 extension skeleton | `libdq9_godot.so` builds; `godot --headless --path godot --quit-after N` loads it, steps frames, screens match reference hashes |
| G2 playable | window with both screens, keyboard/gamepad/touch input, audio through `AudioStreamGenerator`, save persisted |
| G3 hosting features | layouts, settings/rebinding, fullscreen/scaling filters, debug menu + bundles |
| G4 rendering stage 1 | `RenderFrame` capture to Godot (see `REMASTER_STAGE1.md`), 1x parity mode first |

For persistence, the proposed cross-host reload-parity gate is: after Godot
gameplay changes a save, restart Godot and native independently from copies of
that flushed save, replay the same reload input, and require every screen hash
and final save byte to match. `tools/godot_checkpoint.py chain A B` exercises
this comparison; it does not establish uninterrupted gameplay continuity.

The optional `chain A B --control` also runs A and B's frame-offset inputs in
one uninterrupted native process. It requires A's recorded hashes, a
non-static overlap of at least 30 consecutive paired screen hashes reaching
the end of either B run, and identical final saves. The control retains A's
RTC while restarted B uses B's RTC. A boot/reload input applied to an already
running game need not reach the reloaded state; a stronger continuity gate
requires a matched in-game save/load scenario with inputs that demonstrably
converge on the same state.

Godot and godot-cpp are external development dependencies, not bundled. Configure their paths for the local installation; see [Building DQ9-Decomp](BUILDING.md). Packages and generated game builds remain local-only and must never be uploaded.
