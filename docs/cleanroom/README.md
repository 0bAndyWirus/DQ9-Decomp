# Blind implementation handoff

These behavioral contracts and synthetic tests preserve project compatibility; they do not resolve the provenance of the existing code or establish hardware accuracy. The implementer must not read the existing implementations or other runtime source. Delete and independently rewrite each of these exact files, preserving its associated interface:

| Behavior | Delete and replace (do not read) | Read-safe interface and behavioral contract |
|---|---|---|
| RGB555 host expansion | `runtime/gpu2d/src/rgb555_expansion.inc` | `runtime/gpu2d/src/rgb555_expansion.h`, `docs/cleanroom/rgb555.md` |
| 3D-over-2D blending | `runtime/gpu2d/src/blend_3d.inc` | `runtime/gpu2d/src/blend_3d.h`, `docs/cleanroom/blend-3d.md` |
| Cartridge first-word scheduling | `runtime/periph/src/cartridge_first_word.cpp` | `runtime/periph/src/cartridge_first_word.h`, `docs/cleanroom/cartridge-timing.md` |
| Scanline-214 render scheduling | `runtime/core/src/render_boundary.cpp` | `runtime/core/src/render_boundary.h`, `docs/cleanroom/render-timing.md` |

The headers include or declare the replacement files; inspect the header text only, not the contents of the included `.inc` files. The pixel operations must remain inline-able. Also read the applicable ROM-free tests in `runtime/gpu2d/tests/gpu2d_tests.cpp`, `runtime/periph/tests/periph_tests.cpp`, `runtime/core/tests/core_tests.cpp`, and `runtime/core/tests/render_timing_tests.cpp`; they are regression examples, not proof of independent hardware measurements. Tests must state expected outputs only, never an implementation technique. A pre-rewrite GPU2D test asserted the old expansion's reciprocal constants; it was removed after review. The new expansion uses a different, independently derived reciprocal. These contracts apply only to the four isolated behaviors. The separately unresolved documented-fact items and the wider provenance audit are outside this handoff.
