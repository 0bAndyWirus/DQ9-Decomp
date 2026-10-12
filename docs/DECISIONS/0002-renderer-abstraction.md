# ADR-0002: SDL3 GPU as the first renderer prototype

- **Status:** Proposed for prototype only; final library choice explicitly open.
- **Date:** 2026-10-02

## Context

Required APIs are Vulkan and Direct3D 12, on Linux/SteamOS and Windows. Maintainable common rendering commands are desirable, but fidelity, shader behavior, device support and platform integration have not been tested. Both SDL3 GPU and bgfx document the required API/backend/platform coverage.

## Decision

Prototype SDL3 GPU behind a narrow project-owned renderer interface, and compare it against bgfx using a non-game vertical prototype before committing. SDL GPU is attractive because SDL documents Vulkan and D3D12 drivers and a common GPU interface; SDL_shadercross supports shader-format conversion. The application must still own shader pipelines, presentation policies and backend-specific testing.

## Trade-offs

- **SDL3 GPU:** relatively direct low-level API and shared interface, but shader binaries/formats remain backend-specific; shadercross is another dependency/tool. Feature level is constrained to SDL's portability targets.
- **bgfx:** established graphics-API-agnostic renderer, documents Vulkan and D3D12 and both OS families, and includes a wider renderer-managed abstraction/tooling surface. It adds a larger abstraction/dependency and may constrain renderer-specific behavior differently.
- **Direct APIs:** maximum control but duplicates backend implementation/maintenance and shader validation. No evidence yet justifies this burden.

## Acceptance for final choice

Demonstrate the same renderer contract on Vulkan Linux/SteamOS and D3D12 Windows; validate shader/resource/presentation semantics and input/window lifecycle; document capability restrictions, build/licensing/reproducibility, Deck performance/battery, debugging, and expected fidelity. Keep simulation independent of any library choice.
