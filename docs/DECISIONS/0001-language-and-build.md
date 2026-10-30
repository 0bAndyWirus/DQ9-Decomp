# ADR-0001: C++20 and CMake project foundation

- **Status:** Accepted for the foundation; revisit with evidence before large-scale porting.
- **Date:** 2026-10-02

## Context

The project's objective is a native application for Windows, Linux and SteamOS. Existing DQIX matching decompilation is C++, and common C++ toolchains/build configurations cover the intended desktop targets. A small standard-library-only core allows early behavior tests without committing to platform/render dependencies.

## Decision

Use C++20 for native code and CMake for the cross-platform build description. Keep runtime service boundaries explicit; host-specific graphics, audio, and input dependencies belong in the frontends. Build CI should cover Windows and Linux, with SteamOS / Steam Deck device verification assessed independently.

## Consequences and evidence

This provides one project build description across target toolchains; CMake's generators map to native systems. Original runtime code and synthetic tests imply no game compatibility. Current host builds and their external dependencies are documented in [Building DQ9-Remastered](../BUILDING.md); the original decision date is not a current verification record.

## Alternatives

- **Rust:** strong native option, but less direct reuse of the existing C++ code ecosystem; reassess if decompilation code cannot or should not be reused.
- **Separate native build descriptions:** no demonstrated need; increases maintenance across three targets.
