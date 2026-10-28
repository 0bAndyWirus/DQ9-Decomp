# Blind implementation handoff

These behavioral contracts and synthetic tests preserve project compatibility; they do not resolve the provenance of the existing code or establish hardware accuracy. The implementer must not read the existing implementations or other runtime source. Delete and independently rewrite each of these exact files, preserving its associated interface:

| Behavior | Delete and replace (do not read) | Read-safe interface and behavioral contract |
|---|---|---|
| RGB555 host expansion | `runtime/gpu2d/src/rgb555_expansion.inc` | `runtime/gpu2d/src/rgb555_expansion.h`, `docs/cleanroom/rgb555.md` |
| 3D-over-2D blending | `runtime/gpu2d/src/blend_3d.inc` | `runtime/gpu2d/src/blend_3d.h`, `docs/cleanroom/blend-3d.md` |
| Cartridge first-word scheduling | `runtime/periph/src/cartridge_first_word.cpp` | `runtime/periph/src/cartridge_first_word.h`, `docs/cleanroom/cartridge-timing.md` |
| Scanline-214 render scheduling | `runtime/core/src/render_boundary.cpp` | `runtime/core/src/render_boundary.h`, `docs/cleanroom/render-timing.md` |

The headers include or declare the replacement files; inspect the header text only, not the contents of the included `.inc` files. The pixel operations must remain inline-able. Also read the applicable ROM-free tests in `runtime/gpu2d/tests/gpu2d_tests.cpp`, `runtime/periph/tests/periph_tests.cpp`, `runtime/core/tests/core_tests.cpp`, and `runtime/core/tests/render_timing_tests.cpp`; they are regression examples, not proof of independent hardware measurements. Tests must state expected outputs only, never an implementation technique. A pre-rewrite GPU2D test asserted the old expansion's reciprocal constants; it was removed after review. The new expansion uses a different, independently derived reciprocal. These contracts apply only to the four isolated behaviors. The separately unresolved documented-fact items and the wider provenance audit are outside this handoff.

## Documented 2D, cartridge and sound behavior handoff

For this separate rewrite, the implementer may read only this section, the three contracts, the three named interface headers, and the named ROM-free tests below, plus the cited GBATEK documentation. Do not read other runtime source, prior implementations, history, audit reports, build output containing source, or other material. Delete each named implementation without reading it, then independently implement its existing interface from the contracts and GBATEK. Do not change callers or tests. Keep per-pixel and sound operations inline-able. Record a provenance statement identifying material consulted and affirming the isolation.

| Behavior | Delete and replace without reading | Read-safe interface and contract | Read-safe tests |
|---|---|---|---|
| Ordinary 2D effects | `runtime/gpu2d/src/ordinary_2d.inc` | `runtime/gpu2d/src/ordinary_2d.h`, `docs/cleanroom/ordinary-2d.md` | `runtime/gpu2d/tests/ordinary_2d_tests.cpp` |
| B7 redirect | `runtime/periph/src/cartridge_b7_redirect.cpp` | `runtime/periph/src/cartridge_b7_redirect.h`, `docs/cleanroom/b7-redirect.md` | `runtime/periph/tests/periph_tests.cpp` |
| ADPCM and PSG tables | `runtime/spu/src/sound_tables.inc` | `runtime/spu/src/sound_tables.h`, `docs/cleanroom/sound-tables.md` | `runtime/spu/tests/sound_tables_tests.cpp` |

The headers may include replacement files; read header text only, not the included implementation. Tests define expected outputs rather than an implementation method. These compatibility contracts do not by themselves establish hardware conformance or resolve other provenance items.
