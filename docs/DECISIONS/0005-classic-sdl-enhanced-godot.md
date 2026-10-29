# ADR-0005: SDL is the Classic reference host; enhancements live only in Godot

- **Status:** Accepted
- **Date:** 2026-10-28

## Context

Host-only widening (W1) changes presentation only. Guest-aspect widening (W2) changes the game camera's projection aspect and therefore guest state, timing and replays. Other extras, including single-screen layout and high-resolution 3D, do not belong in an original-gameplay reference build. Parity tooling uses the SDL `dq9` executable as its Classic reference.

## Decision

- SDL `dq9` is the Classic host: original DS output and behaviour, with no enhancement options. It remains the reference for Classic baselines and parity checks.
- Enhancements are exposed only by the Godot host. Shared runtime code may implement an enhancement, but only Godot may enable it.
- Godot offers Picture (W1, presentation-only) and Full (W2 guest camera with W1's wide GPU target and compositor). Full is selected before boot, never toggled mid-session, and has separate deterministic baselines. The host clip-X correction is disabled in Full so the field of view is widened exactly once.

## Consequences

- SDL exposes no guest-modifying or presentation enhancement option.
- With enhancements off, Godot software screens, saves and audio must match SDL Classic baselines. Guest-modifying modes need separate baselines and never replace Classic ones.
- Widescreen requires the experimental GPU renderer. Unsupported GPU frames fall back to software; in Full the modified guest camera remains active, so fallback is not a Classic frame.
