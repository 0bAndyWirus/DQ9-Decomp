# Research log and source credits

Public documentation review: 2026-10-12. This page records source credits and design context, not image fingerprints, game tables, addresses, resource inventories, or captured gameplay results.

## External research references

- The [DQIX community](https://github.com/DQIX) has published reverse-engineering and tooling work consulted during research. Those projects are independent, retain their own licenses, and are not bundled here. Their existence does not establish compatibility with this runtime or authorisation to use game content.
- The [ds-decomp toolchain](https://github.com/AetiasHax/ds-decomp) informed local analysis and tooling design. Its sources and outputs are not part of this repository.
- [DeSmuME](https://github.com/TASEmulators/desmume) is an external reference emulator. Hardware behavior and values have been cross-checked against its public source; local differential tests can require that source separately. No emulator source tree or Nintendo BIOS is bundled.

These are credits, not instructions to acquire game images, BIOS files, or keys. No game-derived research records are published here.

## Platform and graphics references

- [Godot 4.5 spatial shaders](https://docs.godotengine.org/en/4.5/tutorials/shaders/shader_reference/spatial_shader.html) and [post-processing](https://docs.godotengine.org/en/4.5/tutorials/shaders/advanced_postprocessing.html) inform the current Godot rendering experiments.
- [SDL3 GPU](https://wiki.libsdl.org/SDL3/CategoryGPU) and [bgfx](https://github.com/bkaradzic/bgfx) were candidates in the historical [renderer decision](DECISIONS/0002-renderer-abstraction.md). The current SDL host uses SDL2; the proposal is not evidence of an implemented SDL3 backend.
- The [CMake generators manual](https://cmake.org/cmake/help/latest/manual/cmake-generators.7.html) describes native build-system support.
- [Valve's Steam Deck development recommendations](https://partner.steamgames.com/doc/steamhardware/recommendations) inform controller, display, offline-use, and performance requirements. They are not a compatibility certification.

## Public scope and remaining uncertainty

The tree contains original recompiler components, runtime services, host frontends, and tests. It does not provide an end-to-end generator workflow, game-derived sources or data, or compiled game builds. A game host requires a separately supplied generated-code directory.

Synthetic CPU/rendering tests can establish bounded implementation behavior. They do not establish complete game compatibility, target-device performance, legal permission, or source ownership beyond the materials actually reviewed. Keep local inputs, saves, traces, captures, and generated outputs private; report qualitative status and reproducible tooling behavior only.

See the [project charter](PROJECT_CHARTER.md), [architecture](ARCHITECTURE.md), [workflow status](DECOMP_STATUS.md), and [legal notice](../LEGAL.md).
