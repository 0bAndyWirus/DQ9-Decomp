# ADR-0003: Native Linux and Windows; SteamOS is a first-class target

- **Status:** Accepted as a target strategy; implementation choices open.
- **Date:** 2026-10-02

## Context

The first release must target Windows, Linux desktop and Steam Deck/SteamOS. Valve's guidance recommends Vulkan, broad aspect-ratio support, controller-complete interaction and offline single-player access. A Windows build under Proton is not a substitute for a native Linux build requirement.

## Decision

Maintain Windows and Linux native build paths from the start. Use Vulkan as the primary graphics prototype on Linux/SteamOS and test D3D12 on Windows; do not remove or demote either requested API. Steam Deck is treated as a named target profile with device/controller/display/performance validation, not inferred from generic desktop Linux. Preserve game functionality offline; any optional Steam integration is isolated behind a platform service and cannot be required for core single-player play.

## Consequences

More build/test work is required than a Windows-only plan. Valve's recommendations are input to design, not a certification or guarantee. Native and cross-build configurations now exist, but neither those configurations nor generic desktop tests establish Steam Deck hardware behavior or platform-service integration.
