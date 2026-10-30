# Project charter

## Purpose

DQ9-Remastered develops original research tooling and native runtime services for the European multilingual release of *Dragon Quest IX: Sentinels of the Starry Skies*. Public deliverables are sources, tests, and documentation, not game data or binaries. Linux desktop, Windows, and SteamOS / Steam Deck are host targets.

## Host targets

- Windows
- Linux desktop
- SteamOS / Steam Deck

Linux desktop is the immediate development target. Windows has native and cross-build configurations; neither cross-linking nor generic desktop tests establish Windows or Steam Deck hardware compatibility. Steam Deck needs separate device, controller, display, audio, and performance validation.

## Scope and constraints

- Preserve original game simulation timing and behavior; do not require higher frame rates.
- Separate game logic, platform services, rendering, audio, input, and asset handling.
- Provide one simulation for Classic and Enhanced presentation modes; presentation settings must never alter simulation decisions or timing.
- Renderer design must support Vulkan and Direct3D 12. Higher resolution, widescreen, and effects are optional follow-on work after a faithful rendering baseline.
- Game hosts require a separately supplied generated-code directory. `tools/build_port.py` runs the local pipeline but does not yet yield a bootable game; generated game sources and all local game-derived outputs stay outside version control.
- Any future upscaler integration is optional, backend-neutral where practical, and limited to publicly released, properly licensed technology. No leaked or unauthorized DLSS versions.
- Document original tooling behavior and dependency provenance without publishing game fingerprints, tables, addresses, corpus statistics, or captured content.

## Non-goals of the public repository

- Claiming the community's USA/Japan decompilation applies unchanged to Europe.
- Shipping a ROM, copyrighted assets, or extracted content.
- Claiming a complete decompilation or port before all executable modules, game-logic scripts, and required regional behavior have explicit coverage and verification.
- Raising simulation speed to match display refresh rate.
- Starting visual enhancements before the faithful reference path is understood.

## Feasibility position

Public upstream work confirms substantial reverse-engineering resources, including USA/JPN matching decompilation, a European-only randomizer, archive tools, and script tooling. These are not equivalent to a European decompilation or portable game implementation. Regional code differences and unknown simulation/rendering/audio behavior remain blockers to estimating a faithful port. See [research](RESEARCH_LOG.md) and [gates](ROADMAP.md).

## Data and legal boundary

The project provides original tooling and runtime implementation only. Select an already locally available compatible image via `--rom` or `DQ9_ROM`; use only a copy you are authorised to use. Do not commit, upload, publish, or distribute ROMs, extracted assets, game scripts/data, generated sources, saves, dumps, or builds containing game code. Packages are local-only, never public release artifacts. This charter is project policy, not legal advice or a guarantee that any particular use is lawful. See the [legal notice](../LEGAL.md).

## Current status

As of 2026-10-12, recompiler components, runtime services, SDL and Godot hosts, local development tools, and synthetic tests are present. No game data, generated game code, or binaries are distributed. Full-game compatibility and target-device behavior are not established by this public tree. The [MIT License](../LICENSE) covers original project code; external references are credited in [NOTICE](../NOTICE).
