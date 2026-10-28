# GPU3D rasterization behavioral specification

## Status and sources

This is an output contract for a replacement, **not a claim that the current renderer is hardware-exact or provenance-cleared**. Project compatibility means matching the pre-refactor fixture outputs and separately validated checkpoint baselines. The permitted and forbidden material for the blind implementer is listed in `GPU3D_RASTER_HANDOFF.md`; no implementation-bearing source or other emulator source is permitted.

Hardware references: GBATEK [polygon definitions and clipping](https://problemkaputt.de/gbatek-ds-3d-polygon-definitions-by-vertices.htm), [polygon attributes](https://problemkaputt.de/gbatek-ds-3d-polygon-attributes.htm), [display control](https://problemkaputt.de/gbatek-ds-3d-display-control.htm), [polygon size and antialiasing](https://problemkaputt.de/gbatek-ds-3d-toon-edge-fog-alpha-blending-anti-aliasing.htm), and [3D overview](https://problemkaputt.de/gbatek-ds-3d-overview.htm). GBATEK does not specify every fixed-point tie or arithmetic operation below. Project compatibility is a separate requirement; where neither evidence source determines an answer, it is explicitly left open rather than invented.

## Inputs and observable outputs

The input is an ordered triangle or quadrilateral, including strip-assembled primitives, with homogeneous positions, vertex colors and texture coordinates, per-polygon viewport and rendering attributes, texture data and rendering state. Rasterization consumes the geometry after homogeneous clipping, retaining its ordering and interpolated vertex data. Its observable result is the 256 by 192 pixel 3D image and the associated depth, polygon identity, stencil, alpha and edge information consumed by subsequent fragment processing. Existing public interfaces and all unaffected shading, texture, depth, fog, shadow, blending, and wireframe behavior must remain intact. Compatibility means exact output, not visual similarity. Band-parallel and inline rendering must give identical complete outputs, including auxiliary buffers.

## Geometry, clipping and facing

Hardware supports triangles, quadrilaterals and their strips. Ordinarily counterclockwise vertices are front-facing; successive strip triangles alternate their submitted winding. Polygon attribute bits 6 and 7 enable back and front faces respectively. GBATEK describes degenerate line-segment triangles as having no face and rendering regardless of these flags; crossed and concave quadrilaterals are not valid inputs with predictable hardware results. For equal depth, the existing tests require a front-facing fragment to replace an opaque back-facing fragment, but two front-facing fragments retain the first.

Clip in homogeneous space against all six view-volume planes. Far-plane intersection visibility is controlled by polygon attribute bit 12. Project compatibility requires each intersection to be evaluated from the inside endpoint toward the outside endpoint and the coordinate on the active plane to be set exactly to the corresponding signed W. Omit an intersection when the absolute value of its divisor (the difference between the endpoint plane distances) is at most `1e-20`. Changing evaluation direction or leaving the rounded intersection off the plane can change later projected pixel coverage. The two projection interfaces have different zero-W policies specified in `GPU3D_RASTER_INTERFACE.md`; fixture evidence does not establish all sign-transition cases.

The viewport is specified in lower-left-origin coordinates with inclusive endpoints. The existing full-screen and inset tests require respectively all 49,152 pixels and exactly the 32 by 32 inset, including their final row and column. Coordinate conversion and projection must preserve those results. The clipped polygon's projected vertices use a 28.4 fixed-point screen grid for the compatible raster contract.

## Coverage and arithmetic

For an ordinary nonhorizontal boundary with projected fixed-point endpoints $(X_0,Y_0)$ and $(X_1,Y_1)$, and integer screen row $y$, the existing documented boundary is the mathematical ceiling of

$$\frac{X_0(Y_1-Y_0)+(X_1-X_0)(16y-Y_0)}{16(Y_1-Y_0)}.$$

Use mathematical ceiling for negative values too; truncation toward zero is not equivalent. Sample at integer pixel rows, not pixel centers. Project compatibility requires the left integer boundary included and the right integer boundary excluded, and no filled output when their width is nonpositive. Screen clipping must not shift the attributes that would have been obtained at the surviving pixels. These statements concern ordinary filled polygons, not a general claim about all hardware polygon modes.

GBATEK separately describes hardware edge ownership: when edge marking and antialiasing are disabled, opaque non-wireframe polygons exclude the lower and right edges; translucent polygons likewise do so when alpha blending is disabled. Vertical right edges are excluded for all polygons except line segments; other modes otherwise have full edges. This hardware description is not evidence that all these cases are already implemented identically in the frozen renderer. The exact project-compatible tie rules at horizontal edges, shared vertices, coincident boundaries, clipping boundaries, and modes with edge marking or antialiasing require new output fixtures; the cited material does not establish them exhaustively. In particular do not infer a full polygon coverage rule merely from the boundary equation above.

The interface specifies seven interpolated quantities. Along a nonhorizontal boundary they vary linearly in screen Y; across a positive-width row they vary linearly in screen X. Floating-point rounding is observable: fixture outputs must match across row and band boundaries, including clipped spans. Equivalent real-number formulas need not produce equivalent floating-point outputs. The fixtures do not establish every intermediate rounding rule. Vertex color interpolation retains six-bit framebuffer precision in the specified color fixture.

## Small and degenerate geometry

GBATEK describes 0×0, 1×0, 0×1 and 1×1 polygons as producing at least one pixel; only the 0×0 case uses the one-dot W-depth cutoff, with any vertex passing sufficient for visibility and the first vertex providing color, depth and texture. The project's one-dot test requires rejection beyond the W boundary and acceptance at the inclusive boundary. Its clipped-sliver tests also require that a polygon originally spanning many pixels is not reclassified as a one-dot polygon merely because the clipped remainder is subpixel-sized: the supplied left and top boundary scenes each produce exactly one red pixel, also under banded rendering.

The exact project behavior for all-horizontal edges, coincident vertices, zero-area non-dot polygons, duplicate vertices, edge reversals, and 1×0/0×1 shapes is not fully determined by the available fixtures. GBATEK's hardware description must not be substituted silently for the current compatibility target. Projection overflow and W sign transitions likewise need further evidence beyond the policies in the interface. Establish these with black-box fixtures before claiming a complete clean-room specification.

## Required validation

1. An integration owner, not the blind implementer, runs the existing ROM-free GPU3D tests unchanged. Preserve coverage, viewport, face and equal-depth selection, clipping, one-dot W, sliver, wireframe, color, texture, shadow, fog, transparency and edge behavior. Compare complete outputs where available in inline and banded configurations.
2. Against the existing pinned `build/checkpoints/` baselines, replay **all** segments: `00-boot-title` (3,000 frames), `01-character-creation` (9,000), `02-observatory` (28,500), `03-hexagon-reload` (30,610), `04-post-shop-reload` (6,001), `05-hexagon-full` (73,311), `90-bundle-texture-issue` (6,274), and `91-bundle-texture-missing` (7,838). Match every recorded native screen hash in each `expected.hashes` exactly, plus the existing end-save and audio hashes; run twice, using copied saves. Keep the existing baseline files unchanged and write actual output elsewhere. Use `.venv/bin/python tools/checkpoint.py --dq9 <candidate> run --repeat 2 --oracle`, with the repository's normal local ROM setup. The first three have `oracle.hashes`; the tool reports overlapping oracle differences but does **not** fail on them, so inspect/report those separately and do not describe an exit-zero as an oracle match. Compare with the existing oracle expectations, not a new recording; guest frame-phase differences are not automatically raster differences.
3. Where Godot is in scope, use `tools/godot_checkpoint.py` against the same existing screen and end-save expectations; this comparison does not cover audio. No expected hash or save may be regenerated to accommodate a new rasterizer.
4. Before declaring the specification complete, capture independent black-box expected **full coverage masks and per-pixel attributes**, not merely pass/fail or central pixels, for: positive/negative fractional slopes; integer and half-grid boundaries; shared edges under reversed winding and cyclic vertex order; equal-height extrema and horizontal top/bottom; all three coincident vertices; lines and 1-pixel/subpixel polygons; clipping against every plane in both directions; W=0 and sign transitions; viewport boundaries; and color, depth and texture gradients with endpoint values chosen to expose floating-point rounding. Include both render scheduling modes. Preserve exact expected pixels or lossless fixture hashes and the fixture inputs; do not manufacture expected values from the replacement. Distinguish baseline compatibility from independently measured hardware behavior.

## Unresolved behavioral requirements

The synthetic fixture inputs and full-output hashes in `runtime/gpu3d/tests/fixtures/` pin the **project's pre-refactor compatibility outputs**, not independent hardware truth. The 131 scenes produce 2,096 snapshots across inline/banded execution, scales 1–4 and both capture modes, plus 12 ordered clip-output goldens. Hashes cover complete native output and framebuffer fields, capture data and, for scaled targets, scaled output and framebuffer fields. They constrain only the sampled inputs; collisions are theoretically possible and the hashes are not reconstructible framebuffer images.

| Previously unresolved behavior | Compatibility fixture input IDs |
| --- | --- |
| Positive and negative fractional slopes; rounding at clipped spans | 0–27, 68–75 |
| Equal-height extrema, horizontal and zero-height edges | 28–35 |
| Coincident vertices, lines, one-pixel and subpixel polygons | 32–39, 80–83 |
| Shared boundaries with winding and cyclic ordering variations, individually and paired | 40–51, 87–110 |
| Six-plane crossings and far-plane rejection | 16–27, 84–85 |
| Zero and sign-changing W, including a retained homogeneous origin | 60–67, 86 |
| Viewport boundaries, wrapping and extreme bytes | 52–59, 119–126 |
| Color, depth and texture-coordinate gradients, perspective and output rounding | 0–27, 68–75, 111–118 |
| Wireframe and one-dot behavior | 76–83 |
| Subpixel quantization | 127–130 |

The separate boundary fixture checks plane numbering and BOX_TEST edge interpolation and visibility; clipping count, order and mutation including far rejection and a multi-plane polygon; screen projection at and around the near-zero W threshold; filled projection with zero and signed tiny W; wrapped viewport coordinates and fixed-point quantization; and wire/dot coverage, depth and row exclusion. Its `clipping_divisor_threshold`, `left_clipped_span_prestep`, and `simd_four_sample_scalar_equivalence` cases pin the threshold, prestep and sample behaviors described above and in the interface. These are discrete cases, not exhaustive measurements.

The cases do not establish general rules for every tie, crossing, degenerate clipping result, W=0 input, interpolation conversion, or operation order, and do not prove agreement with hardware. Preserve the pinned outputs and existing tests; obtain further independent evidence before making broader claims. No particular traversal, vertex rotation, data structure, or source-level implementation is mandated by this specification.
