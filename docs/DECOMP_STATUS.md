# Decompilation status and workflow

As of 2026-10-29, original matching/integration tooling and runtime sources are
present. This page describes the tooling workflow. Only aggregate matching
totals are published (see below). Per-function results, addresses, symbols,
disassembly, and per-unit statistics remain private and are never published
(see [LEGAL.md](../LEGAL.md)).

## Workflow

1. Supply local analysis metadata and candidate sources to the matching tools.
   No instructions for producing game-derived inputs are shipped.
2. Check candidate compiler output against the local reference before claiming
   an exact match.
3. Integrate verified units transactionally; retain changes only when the
   required local checks pass.
4. Keep candidate sources, reference bytes, reports, and per-function or per-unit results private. Aggregate totals only may be published.

## Aggregate totals

Reported by the local status tool on 2026-10-29: 14,660 of 14,794 functions
(99.1%) and 2,935,178 of 2,958,434 code bytes (99.21%) match byte-exactly, in
13,294 of 13,371 units. Of the matched functions, 11,935 are written in C/C++
(some with inline assembly) and 2,725 are matched as assembly only. All 38
`check_modules` checks passed after the last integration that day; no full-ROM
checksum is claimed. These are totals only; no function, address or symbol is
disclosed.

## Constraints

- Only matching code that is verified byte-exact is counted.
- Nothing derived from the game is committed: no sources, bytes or symbol dumps.
- A game host needs a separately supplied generated-code directory; the public
  repository does not provide an end-to-end generator workflow.
- Matching-source work and static recompilation are distinct tool workflows.
  Runtime tests alone do not establish either complete matching or gameplay.
