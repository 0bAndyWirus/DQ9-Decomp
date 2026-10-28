# GPU3D scanline edge walker

The walker represents projected coordinates on a 28.4 grid and samples integer
rows. For a non-horizontal edge, the intersection is represented by a signed
integer quotient and nonnegative remainder; its ceiling gives the pixel
boundary. Each row advances the quotient and remainder by a precomputed slope.
Attribute interpolation retains sequential floating-point additions, including
rows outside a rendering band, so banded and inline rendering round alike.
Horizontal and point edges retain the existing degenerate-edge behavior. Scaled
rendering quantizes projected coordinates after scaling and uses the same walker
in output-pixel row units.

## Sources consulted for this change

- `docs/ARCHITECTURE.md`, especially the 28.4 sampling and provenance notes.
- The existing `runtime/gpu3d/src/gpu3d.cpp` edge interfaces, traversal and
  interpolation behavior, reviewed for compatibility. This source was identified
  by the prior audit as having unresolved provenance; the review means this work
  is **not** a blind clean-room implementation.
- A separately derived quotient/remainder scan-conversion algorithm, produced
  without consulting repository or emulator source: for `D = 16*dy`, divide
  `x0*dy + dx*(16*y-y0)` by `D` using floor division, and separately divide
  `dx` by `dy` to obtain the integral and fractional row increments.

No DeSmuME rasterizer source was consulted for this change. This document does
not resolve the broader provenance audit or make a licensing determination.
