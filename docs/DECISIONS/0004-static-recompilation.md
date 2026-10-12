# ADR-0004: Static recompilation plus native DS platform services

- **Status:** Accepted.
- **Date:** 2026-10-03

## Context

The objective is original tooling and a native host runtime for local research. Hand-written matching work and ahead-of-time translation solve different problems; both must respect 32-bit guest pointers and DS memory semantics. The public repository distributes neither game-derived sources nor a complete game-generation workflow.

The charter forbids an emulator, interpreter or JIT as the port.

## Decision

1. **Ahead-of-time translation.** `recomp/` translates caller-supplied ARM9 (ARMv5TE) and ARM7 (ARMv4T) code and metadata into native C++ functions. Direct calls use per-function slot pointers; there is no instruction interpreter or JIT in the host runtime. A game host requires a separately supplied generated-code directory.
2. **Native platform services.** `runtime/` reimplements the DS hardware the game programs against — memory map, IRQ/timers/DMA/IPC, BIOS SWIs, 2D/3D graphics (software renderer for Classic), SPU, game card/backup, SPI/RTC — plus an SDL2 host frontend. These are the port's platform layer, the same role a renderer/audio backend plays in any decompilation-based port.
3. **Guest contexts on fibers.** Runtime context-switch services preserve guest execution while each Machine remains on one host thread. Title-specific calling-convention metadata must be provided privately by the caller.
4. **Matching remains separate.** Readable matching-source tools check local compiler output independently of the runtime build. `Machine::replace_function` supports hash-guarded native replacements using guest-memory accessors; it does not establish that a game is fully matched or compatible.
5. **Data locality.** Generated code, manifests and builds are ROM-derived and stay under ignored `build/`/`extracted/`; only the tools and runtime are tracked.

## Consequences

- A local game's correctness depends on recompiler semantics, the completeness of separately supplied code metadata, and runtime hardware fidelity. Synthetic unit and differential tests do not establish complete game behavior.
- Hardware behaviour must be accurate enough for the game; a local DeSmuME build is used only as a verification oracle (traces, screenshots), never shipped.
- Builds are personal: a binary contains translated game code and must not be distributed.
- Steam Deck uses the same x86_64 code; it is built in Valve's Steam Runtime (sniper) SDK container for library compatibility.

## Alternatives

- **Decompile-first port:** requires completed matching work and portable handling of guest-pointer assumptions; no completion estimate is asserted.
- **Bundled emulator:** excluded by the charter.
