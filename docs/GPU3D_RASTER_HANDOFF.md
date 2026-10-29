# GPU3D raster clean-room handoff

## Boundary

The blind implementer must not have read the prior raster implementation or implementation-bearing material, and must not consult it while doing the replacement. A separate integration owner maintains this handoff and validates the result. This document, the interface contract, and the specification describe externally observable project compatibility; they are not a provenance clearance or a claim of hardware accuracy.

The complete reading allowlist is:

- `runtime/gpu3d/src/rasterizer.h`, for declarations and data types;
- `docs/GPU3D_RASTER_INTERFACE.md` and the sanitized `docs/GPU3D_RASTER_SPEC.md`;
- `runtime/gpu3d/tests/fixtures/test_raster_goldens.cpp`, `runtime/gpu3d/tests/fixtures/raster_goldens.txt`, and `runtime/gpu3d/tests/fixtures/test_raster_boundaries.cpp`, plus any explicitly supplied sanitized fixture inputs and goldens; do not follow their includes into forbidden files;
- GBATEK hardware documentation linked from the specification.

Do **not** read the existing `runtime/gpu3d/src/rasterizer.cpp`, `rasterizer_simd.h`, `rasterizer_impl.h`, `gpu3d.cpp`, `docs/GPU3D_EDGE_WALKER.md`, audit reports, `test_gpu3d.cpp`, other implementation-bearing tests or sources, or git history for `runtime/gpu3d`. Do not read other emulator source. Search results, diffs, code review comments, compilation diagnostics, and discussions must not expose forbidden source or its structure. The integration owner filters any feedback into input/output behavior and fixture results. File names listed as forbidden are not permission to inspect them.

The replacement owns `rasterizer.cpp`, `rasterizer_simd.h`, and `rasterizer_impl.h`; existing contents are forbidden. Start replacement-owned files from empty files without inspecting their former contents. `rasterizer.h` is the shared interface: retain the declarations and types and satisfy their semantics. Request any missing contract or build information from the integration owner as sanitized behavior or diagnostics, not source excerpts. Do not modify unrelated renderer code.

## Evidence and acceptance

Fixture outputs are frozen pre-refactor compatibility evidence, not independently observed hardware truth. Never regenerate expected values from the replacement; integration may add cases only from the pristine pre-refactor baseline, recording their origin. Match the complete fixture outputs inline and banded, including scaled and auxiliary fields. The integration owner runs the remaining tests and existing pinned ladder without exposing forbidden material. Test success does not resolve provenance; separately review the clean-room boundary and audit before making any licensing claim.
