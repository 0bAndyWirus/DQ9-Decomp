# Decompilation status and workflow

As of 2026-10-12, original matching/integration tooling and runtime sources are
present. This page describes the tooling workflow, not game coverage.
Per-function results, addresses, symbols, disassembly, and corpus statistics
remain private and are never published (see [LEGAL.md](../LEGAL.md)).

## Workflow

1. Supply local analysis metadata and candidate sources to the matching tools.
   No instructions for producing game-derived inputs are shipped.
2. Check candidate compiler output against the local reference before claiming
   an exact match.
3. Integrate verified units transactionally; retain changes only when the
   required local checks pass.
4. Keep candidate sources, reference bytes, reports, and progress totals private.

## Constraints

- Only matching code that is verified byte-exact is counted.
- Nothing derived from the game is committed: no sources, bytes or symbol dumps.
- A game host needs a separately supplied generated-code directory; the public
  repository does not provide an end-to-end generator workflow.
- Matching-source work and static recompilation are distinct tool workflows.
  Runtime tests alone do not establish either complete matching or gameplay.
