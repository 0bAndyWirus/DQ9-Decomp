# Decompilation status and workflow

As of 2026-10-13, original matching/integration tooling and runtime sources are
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

Reported by the local status tool on 2026-10-11 (about 2.3 days before this
page; not re-verified since): 14,555 of 14,794 functions (98.4%) and 2,859,980
of 2,958,414 code bytes (96.67%) match byte-exactly, in 13,187 of 13,342 units.
27 further candidates were awaiting integration. The last full 38-module
`check_modules` run predates these totals, and no full-ROM checksum is claimed.
These are totals only; no function, address or symbol is disclosed.

## Constraints

- Only matching code that is verified byte-exact is counted.
- Nothing derived from the game is committed: no sources, bytes or symbol dumps.
- A game host needs a separately supplied generated-code directory; the public
  repository does not provide an end-to-end generator workflow.
- Matching-source work and static recompilation are distinct tool workflows.
  Runtime tests alone do not establish either complete matching or gameplay.
