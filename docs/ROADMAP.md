# Roadmap

Ordered gates; no delivery dates or completion estimates. A failed gate stops dependent port work rather than being papered over. Linux, Windows and Steam Deck remain in every build/platform plan.

## 0. Feasibility and legal review

**Work:** Confirm authoritative EU release identification/provenance without sharing an image; map existing EU reverse-engineering/tool support and legal constraints; establish ownership/licensing policy.

**Depends on:** Nothing.

**Acceptance:** Sourced region/revision evidence; documented differences/unknowns; counsel/rights assessment appropriate to planned conduct; data-handling policy. No assets enter repository.

**Gate F0:** If the target cannot be reliably identified, or planned reverse-engineering/asset handling lacks a defensible legal path, pause port implementation and resolve before proceeding.

## 1. Reproducible reference baseline

**Work:** Define an owner-local workflow to identify/hash a compatible European copy without copying it into the repo; document tool versions and emulator/debugger configuration; record versioned black-box observations, input traces and reference captures for representative boot, menu, field, battle, audio, save/load and timing paths. Record source/tool provenance and uncertainty.

**Depends on:** F0 and lawful local reference access.

**Acceptance:** Repeatable, privacy-preserving local procedure; independently repeatable observations; measurable source simulation cadence/timing and known tolerances; no uploaded/distributed game data.

**Status (2026-10-12):** Local-input verification and structural-inventory tools are present. Public tests use synthetic fixtures; no image fingerprints, game traces, or gameplay acceptance results are distributed. A complete local compatibility baseline remains a separate requirement.

**Gate F1:** Do not port game subsystems based on assumed USA/JPN equivalence or unmeasured frame timing.

## Full European decompilation workstream (active objective)

**Work:** Account for every EU ARM9/ARM7 executable region, ARM9/ARM7 overlay, in-memory ARM9 section, and executable game-logic script. Use USA/JPN symbols and implementations as evidence only after verifying each correspondence against the EU image. Reconstruct code, data boundaries, relocations, and script semantics; keep ROM-derived payloads local under the project data policy.

**Depends on:** F0, lawful local access to the identified EU image, and the F1 reference/toolchain baseline.

**Acceptance:** Every executable byte/function is assigned to a module and classified; no untracked code gaps remain; each claimed matching module builds or otherwise compares byte-for-byte against the exact EU reference. All game-logic script formats are classified and their semantics needed for a full decompilation are documented. Non-code resources are inventoried and their formats/roles classified; counting files alone does not qualify.

**Status (2026-10-12):** Matching and transactional integration tools are present. Game-derived candidates, metadata, results, and coverage totals remain private. The public tree makes no claim of complete decompilation; see [workflow status](DECOMP_STATUS.md).

## Required native Linux game execution

**Work:** Validate a local x86_64 Linux host with a separately supplied generated-code directory and an already locally available compatible image. Preserve game behavior through original runtime services; synthetic tests or a window alone do not prove native full-game behavior. No game-derived inputs or resulting binaries may be published.

**Depends on:** Complete EU code and executable-script analysis, resource-format understanding, and a proven behavioral reference path. Reverse-engineering results must be EU-specific; USA/JPN implementations are references only after matching is verified.

**Acceptance:** Build and launch the actual native game process on the target device; verify an end-to-end real game path with input, state transition, rendering/audio as applicable, and locally supplied resources. Demonstrate that execution is native code rather than an emulator. Keep full-game behavior and full decompilation as project objectives; a prototype or isolated vertical slice does not satisfy this requirement.

**Status (2026-10-20):** Original ARM/Thumb recompiler components, hardware runtime services, and SDL/Godot host sources are present. `tools/build_port.py` builds a host from your own image that boots to the opening credits (checked headless in all five languages). Scripted Linux runs also reached an early field scene and short live input checks responded; see [the dated observations and limits](GETTING_STARTED.md#status-reviewed-2026-10-20). No game build is shipped. Full-game behavior, sustained interactive play, device performance, and Windows / Steam Deck hardware compatibility remain unclaimed.


## 2. Native platform and presentation prototype

**Work:** Build a small non-game C++ prototype on Linux, Windows and SteamOS/Deck: window/input/audio service seams, SDL GPU/Vulkan and D3D12 feasibility, renderer snapshots, Classic/Enhanced presentation toggle over a synthetic deterministic simulation. Measure shader portability, controller/window lifecycle, build/dependency reproducibility, latency, and output. Compare SDL GPU with bgfx on requirements.

**Depends on:** F1 baseline criteria and build setup.

**Acceptance:** Same recorded inputs yield identical synthetic simulation state across presentation toggle/backends; target builds and visible interaction work on each target; backend shader/render limitations and costs recorded. This prototype must be explicitly labeled non-game.

**Gate F2:** Choose renderer and platform libraries only after this evidence. If required parity/target support fails, revise architecture before subsystem migration.

## 3. Vertical-slice port feasibility

**Work:** Select one small, representative game behavior/data path only after EU binary structures/code are understood. Port or reuse legally available logic; compare original/reference and port using the recorded trace, state, timing, rendering and audio evidence.

**Depends on:** F0–F2, EU-specific analysis and local reference.

**Acceptance:** Trace-level deterministic simulation parity for the slice; stable resource lifecycle; measured timing; documented unported boundaries and repeatable Linux/Windows builds with Deck validation plan.

**Gate F3:** Continue broad port only if the slice demonstrates a credible path to game-wide behavior, assets and platform parity. Otherwise stop or re-scope with explicit owner approval.

## 4. Incremental subsystem coverage

**Work:** Expand only in dependency order from proven slices (platform/time/input, core state, scripts/data, world, battle, UI, audio, save/load, then network features if in scope). Maintain EU-specific evidence and parity tests. No enhancements ahead of fidelity.

**Depends on:** F3 pass; each subsystem requires its own behavioral evidence and acceptance criteria.

**Acceptance:** Per-subsystem compatibility matrix and tests; no unsupported subsystem represented as complete; all first-release targets continuously built/tested.

## 5. Faithful Classic release candidate

**Work:** Integrate complete required single-player flows; validate all required targets and controller usability, timing, persistence, audio, recovery and asset-locality. Establish formal release/legal review.

**Depends on:** All required subsystem gates passed.

**Acceptance:** Measured representative behavior parity and explicit residual deviations; local Linux, Windows, and Steam Deck/SteamOS builds validated on hardware. Publish original sources and synthetic tests only: no game data, generated game code, captures, or compiled game builds.

## 6. Optional Enhanced mode

**Work:** After Classic is accepted, add higher resolution/widescreen/effects incrementally, with mode-toggle transition and paired deterministic gameplay trace checks.

**Depends on:** Faithful Classic acceptance.

**Acceptance:** Toggle does not change simulation state, RNG, event order, save state or timing; Enhanced visuals have independent quality/performance targets. Optional publicly licensed upscaler integration only after a separate decision; never depend on leaked/unreleased DLSS.
