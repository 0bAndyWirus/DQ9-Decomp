# Godot Compatibility GPU 3D shader proof

The standalone shader proof is ROM-free and is not a replacement for native
game-frame parity. Production hybrid capture adds optional fragment metadata
without changing software output; the software renderer remains the exact
reference. `godot/tests/gpu3d_poc.gd` uses ROM-free native GX and shader scenes.

## Reproduce

Use a rendered backend, not Godot `--headless` (which has no usable viewport
rendering). From the repository, with Godot 4.5:

```sh
xvfb-run -a "$GODOT" \
  --path godot --script res://tests/gpu3d_poc.gd -- "$PWD/build/gpu3d-poc"
```

The script exits nonzero on failed numerical checks. It writes screenshots,
CPU-reference PNGs and `results.json` to the supplied directory. The three main
scenes are `perspective`, `overlap` and `fog_edges`, each at 256×192 and 768×576
(scales 1 and 3). A scene file is unnecessary: the script creates its own
`SubViewport`s, cameras and meshes. Each viewport has `transparent_bg`,
`own_world_3d` and disabled MSAA.

The deliberately unsupported MRT probe is separate:

```sh
xvfb-run -a "$GODOT" \
  --path godot --script res://tests/gpu3d_poc.gd -- --probe-mrt
```

This probe intentionally produces `Expected constant, function, uniform or
varying` at `layout(location = 1) out vec4 polygon_id`. Its exit code is zero
because observing that rejection is the experiment, not a working MRT shader.
Do not use this command as the positive shader test.

## Measured Compatibility behavior

Observed with Godot `4.5.stable.official.876b29033`, Mesa 26.1.6, OpenGL 4.6,
**llvmpipe (LLVM 20.1.2, 256 bits)** under Xvfb. These are API/quality observations,
not a discrete/integrated GPU performance measurement.

| Question | Observation / consequence |
| --- | --- |
| Can `POSITION` bypass Godot's camera? | Yes. Moving, rotating and changing the camera's orthographic size leaves the captured image byte-identical. CPU frustum culling still happens: set a suitable `custom_aabb`/cull margin or parent the mesh to the camera. |
| Which depth convention? | Compatibility NDC is **−1…1** with reverse Z. DS clip Z is conventional (near `−w`, far `+w`). Negating DS clip Z gives the requested less-or-equal DS-depth behavior through Godot's greater-or-equal hardware test. Equal/near/far overlap probes pass. |
| Which Y convention? | Direct DS clip Y produces a vertically inverted `SubViewport` capture in Compatibility. Negate Y as well as Z. The shared shader performs this conversion; callers must not flip the vertices a second time. |
| Are UVs perspective-correct? | Yes, using the actual clip W in `POSITION`. CPU barycentric `sum(uv/w)/sum(1/w)` matches; an affine interpolation control has much larger error. Do not pre-divide UVs or interpolate clip W affinely yourself. |
| Is a depth texture available? | Yes, to a spatial fullscreen pass in the **same viewport**, after its opaque pass. Opaque overridden clip Z is preserved. It cannot be obtained from a different viewport via its `ViewportTexture`. |
| Does `depth_draw_always` make translucent geometry visible to depth sampling? | No. A shader writing `ALPHA` enters the transparent pass; the screen/depth snapshot precedes that pass. The transparent-depth probe still reads the clear depth. |
| Can a material output a custom second target for polygon IDs? | Not through the public Compatibility spatial shader API used here. `RenderingServer.get_rendering_device()` is null, and a custom GLSL output declaration is rejected. A separate opaque ID viewport **does** work. This does not claim the underlying OpenGL hardware lacks MRT. |
| Does `render_priority` order translucent surfaces? | Yes: swapping red/blue priorities swaps their overlap color. Its range is only **−128…127**. It cannot assign 2048 unique polygon priorities. |
| Can one `ArrayMesh` contain 2048 material surfaces? | No. Surface 257 is rejected with `RenderingServer::MAX_MESH_SURFACES`. Chunk meshes at **256 surfaces**, or fall back. The exhaustive alpha probe uses four meshes. |
| Are spatial outputs byte-exact data? | Not without compensation: Compatibility's unshaded ALBEDO passes through a cubic sRGB→linear approximation and a power-law inverse, losing dark values. A nearest RF inverse-transfer table now makes all 64 RGB6 levels and their RGB555 display expansions byte-exact. Spaced ID/flag codes remain independently tested. |
| Can hinted samplers be read in helpers? | In this Godot build, helper reads generated GLSL functions before `depth_buffer`/`color_buffer` declarations, causing compilation failure. Sample `hint_depth_texture`/`hint_screen_texture` directly in `fragment()`, then pass the sampled values to helpers. |

The [Godot spatial shader reference](https://docs.godotengine.org/en/4.5/tutorials/shaders/shader_reference/spatial_shader.html)
describes `POSITION`, transparent `ALPHA`, render modes and
`CLIP_SPACE_FAR`. The [advanced post-processing documentation](https://docs.godotengine.org/en/4.5/tutorials/shaders/advanced_postprocessing.html)
explains the same-viewport depth restriction and Compatibility's NDC range.
The [shading-language uniform conversion table](https://docs.godotengine.org/en/4.5/tutorials/shaders/shader_reference/shading_language.html#uniforms)
specifies packed integer host values for boolean vector uniforms.

The compensation follows the actual Godot 4.5 Compatibility
[ALBEDO conversion](https://github.com/godotengine/godot/blob/4.5-stable/drivers/gles3/shaders/scene.glsl)
and [transfer functions](https://github.com/godotengine/godot/blob/4.5-stable/drivers/gles3/shaders/tonemap_inc.glsl),
not an assumed ideal sRGB transform. The immutable 256-entry RF table is built
once per process; shaders perform three nearest data lookups, not per-fragment
power functions or iterative inversion.


## Shader / mesh contract

`godot/shaders/ds_poly.gdshader` includes `ds_poly_common.gdshaderinc`.

- `ARRAY_VERTEX`: un-divided clip XYZ. Ordinary proof meshes use original DS
  vertices; production `DqGpuScene` strips use whole-screen clip coordinates.
- `ARRAY_TEX_UV2.x`: clip W; `.y` contains polygon metadata in bits0..12 and
  the strip-corner index in bits13..14. Hardware interpolation is perspective-correct.
- `ARRAY_TEX_UV`: DS 12.4 coordinates converted to texels, divided by texture size.
- Ordinary proof meshes use `ARRAY_COLOR = RGB6/63`. Production strips use
  **float RGB `ARRAY_CUSTOM0`**, since packed eight-bit vertex COLOR loses native
  six-bit interpolants before shading.
- Production `ARRAY_CUSTOM1` carries float NDC-depth plane `(dZ/dX,dZ/dY,Z)`;
  slopes are per native pixel and Z is evaluated at that strip corner.
- `ds_raster_mesh.h` clips/projects the polygon and applies DS face enables.
  Attribute samples still follow the software's28.4 integer edge walker,
  including its lack of an X prestep. Coverage instead follows the shared
  projected28.4 polygon perimeter, split at native rows **and vertex-Y events**.
  Opposite orientations evaluate a shared edge with the same arithmetic.
  Holding ceil-rounded terminal coverage endpoints leaves visible holes between
  adjacent mesh polygons; extending to their actual tips avoids that topology.
  The converter reads geometry only, never software pixels or texture colours.
- A +0.5 **native-pixel** nudge aligns native GL samples with DS integer samples.
  At scales2..6, strips overlap conservatively by **1/32 native pixel** in X/Y;
  depth comes from one polygon plane, evaluated also at the overlap corners,
  rather than fighting discontinuous per-row span depths. Scale1 uses zero
  overlap and retains native span depth/interpolation. The clear plane has no
  nudge/overlap. Texture sampling, shading, depth testing, perspective correction
  and hi-res fragment generation remain on the GPU.

Shared uniforms:

| Uniform | Data |
| --- | --- |
| `ds_texture` | Decoded RGBA8 **data** texture; no `source_color` hint. Native RGB6 expands as `(v<<2)|(v>>4)`, alpha5 as `(v<<3)|(v>>2)`. The shader recovers them with rounding to 63/31. |
| `toon_table` | 32×1 RGB data texture, nearest. Expand each raw RGB555 component with the software renderer's rule: **0→0, otherwise `2*v+1`**, then RGB6→RGBA8 expansion. This is not the usual five-bit bit-replication rule. |
| `textured` | Texture enable, incorporating DISP3DCNT bit 0 and texture format. |
| `polygon_mode` | 0 modulate, 1 decal, 2 toon/highlight. Mode3 shadow fragments stay software via the hybrid mask. |
| `highlight` | Mode 2 and DISP3DCNT bit 1. |
| `polygon_alpha` | Attribute alpha, 1…31. Alpha 0 means DS wireframe, not invisible geometry; the caller must route/skip it according to its renderer policy. |
| `alpha_test_enabled`, `alpha_test_ref` | DISP3DCNT bit 2 and five-bit ALPHA_TEST_REF. The comparison is `<`, so equality passes. Alpha-zero texels are discarded independently. |
| `texture_repeat`, `texture_flip` | `bvec2`; pass **packed integer bits**, bit 0 X and bit 1 Y, not `Vector2i`. Flip only acts on repeated axes. Integer texels are floored **before** mirroring, including negative and exact-boundary coordinates. |
| `viewport_rect` | `(bottom_left_x/256, bottom_left_y/192, inclusive_width/256, inclusive_height/192)`. Apply DS wrapped viewport endpoint arithmetic on the CPU; the shader maps it into whole-screen clip XY. Default is the full screen. |
| `span_mesh` | Production whole-screen strip coordinates and float CUSTOM0 colors; ordinary proof/fan meshes leave it false. |
| `span_margin` | `0` at scale1; `1/32` native pixel at scales2..6. Corner bits select outward X/Y overlap; positive margin also selects the CUSTOM1 polygon-depth plane. Cached materials are updated when scale changes. |
| `clear_plane` | Bypass the polygon's half-pixel phase adjustment for the full-viewport clear buffer. |
| `color_transfer` | 256×1 RF data texture, nearest/no repeat. Inverts Compatibility's ALBEDO/output transfer. |
| `display_rgb555` | Final color pass only: truncate shaded RGB6 with `>>1`, then expand RGB555 with `(v<<3)|(v>>2)`, exactly like opaque native BG0. Intermediate fog/edge color and metadata passes retain their native precision. |

Material variants are made by prepending defines to the source before assigning
`Shader.code`; the absolute `res://` include still resolves:

- Default: `unshaded, depth_draw_always, cull_disabled, blend_mix`, no built-in
  lighting/shadows/fog. Writes `ALPHA`, so it is in the transparent pipeline.
- `#define DS_OPAQUE`: no `ALPHA` write; use only for truly opaque material runs.
- `#define DS_NO_DEPTH_WRITE`: transparent `depth_draw_never`; use when polygon
  attribute bit 11 does not request translucent depth updates.
- `#define DS_ADDITIVE`: `blend_add`, `depth_draw_never`. This is a demonstrated
  Godot material option, **not** a substitute for DS mode-3 shadow semantics.

The arithmetic follows `Gpu3D::Impl::shade_pixel`: six-bit integer-style
modulation, endpoint-aware decal alpha, red-indexed toon lookup, saturating
highlight addition, and native five-to-six-bit alpha expansion before modulation.
Group only **adjacent** polygons in the already sorted sequence. Merging
nonadjacent equal-material polygons changes translucent order. More than 256
ordering-sensitive translucent runs needs software fallback or a separately
verified ordering technique; clamping priorities is not an exact solution.

## Fog and edge pass

`ds_poly_id.gdshader` uses the same ArrayMesh geometry in a second viewport with
ID override materials. It is opaque and applies the same texture alpha test.
By default, `polygon_id`, `polygon_fog` and `translucent_polygon` are uniforms.
Set `vertex_metadata=true` to allow different polygon IDs/fog flags within one
material-run surface: pack UV2.y as `id | (fog?64:0) | (translucent?128:0) |
(alpha5<<8)`. Production strips additionally pack `(corner<<13)` for the vertex
coverage adjustment; all polygon metadata bits0..12 remain identical. The flat
integer varying preserves those bits; the corner bits never enter ID/alpha
decoding. UV2.x remains W.
Encoded RGB bytes are:

- R = `64 + polygon_id*3` (ID 0…63).
- G = `64 + flags*48` (bit 0 fog, bit 1 translucent polygon).
- B = `64 + alpha5*6`.
- Empty pixels have alpha zero, distinguishing the clear plane.

These spaced codes avoid the unstable low range of Godot's spatial color
conversion. The 256 combinations of IDs and flags are explicitly checked.

`ds_post.gdshader` is a fullscreen spatial quad in that **ID viewport**. It
samples that viewport's opaque screen and depth hints, plus `color_texture`
from the separately rendered color viewport. The mesh resource is shared, not
rebuilt to obtain the IDs. Postprocessing follows software order: edges, then
fog. Required uniforms are `disp3dcnt`, `fog_color`, `fog_offset`, `clear_id`,
`clear_color`, `clear_depth` (15-bit), `fog_table` (32×1 raw byte data), and
`edge_color_table` (8×1 RGB6-expanded data). `debug_depth` is a probe mode.

The shader reproduces `apply_fog`'s integer density calculation:

1. Reconstruct Z depth as `floor((1-reverse_depth)*4194303)<<2`; clear depth is
   `(CLEAR_DEPTH&0x7fff)<<9 | 0x1ff`.
2. `depth15=depth24>>9`, `step=0x400>>(DISP3DCNT>>8 & 15)`,
   `offset=FOG_OFFSET&0x7fff`.
3. At zero step, use table 0 for `depth15<=offset`, otherwise table 31.
   Otherwise clamp relative depth to zero, use table 0 through one step,
   table 31 from 32 steps, and linearly interpolate integer table entries
   `segment-1` and `segment` in between.
4. Density 127 becomes weight 128. Blend RGB6 by integer division by 128,
   unless DISP3DCNT bit 6 requests alpha-only fog. Blend alpha5 with the same
   weight. Raw RGB555 fog color expands as 0→0, otherwise `2*v+1`.

Edges follow `apply_edges`: check **right, down, left, up** in that order;
mark a differing-ID neighbor only when current depth is not nearer; use the
neighbor ID's `id>>3` color. Screen boundaries use clear ID/depth and the current
ID. DISP3DCNT bit 4 selects alpha 16 rather than 31 for edge color blending.
At high resolution an edge is one **output** pixel wide, not three duplicated
DS pixels at scale 3.

Production enables `opaque_mask_output`: the final quad writes alpha1 to
**replace** the ID framebuffer, not blend metadata into the color. This is safe
only through the engine-A mask, whose 1 pixels have native alpha31 and no 2D
blend. Mask0 pixels (partial clear, antialiased outer edges, blended layers)
remain software and their GPU alpha is deliberately not a native RGBA result.
Rear-plane metadata distinguishes partial clear from opaque geometry; restore
its original RGB6/A5 from `CLEAR_COLOR`, including the clear-fog bit, before
edges/fog. This avoids loss from premultiplied clear color and reproduces
edge marking against the rear plane. Only actual partial-alpha fragments with
effects require native translucent history; a partial clear or fog value alone
no longer disables the whole frame. No comparison mask is trimmed.

Alpha31 DS format1/6 polygons remain in their original translucent **ordering**
and retain the native translucent-polygon edge-skip flag. Their actual alpha31
fragments enter opaque depth/metadata; partial texels enter the separate
single-layer colour pass. The software history mask, not GPU comparison error,
determines which pixels may be displayed.


This post path is a **Z-buffered opaque-mask display** renderer, not general DS
RGBA compositing. Production excludes unsupported fragment histories per pixel:

- Capture permits one partial fragment over an alpha31 destination. A
  strict-depth rejected partial fragment does not consume a layer or contaminate
  eligibility only if its opaque destination is closer by more than 255 depth
  units and, after rendering, every pixel in its full 3×3 final neighbourhood has
  reliable opaque depth closer by more than 255 depth units. Capture tracks the
  provenance of the last actual depth write separately from framebuffer alpha:
  opaque uniform clear and alpha31 untextured polygons or indexed formats 2/3/4
  without colour-zero transparency qualify. Cutout, direct, compressed and
  variable-alpha textures, translucent depth writes, shadows and bitmap clear
  do not qualify, even when a native sample is alpha31. Alpha-test-rejected and
  zero-alpha fragments do not replace depth provenance. Any depth-accepted nonzero
  dot in that neighbourhood fails the guard because it may be native-only.
  Screen-border pixels fail the full-neighbourhood guard. Ties, near depths,
  depth-tolerance mode (bit14) and other ambiguous cases fall back to software.
  The guard is a conservative
  heuristic, not proof that native and GPU depth or coverage are identical.
  Capture excludes overlapping partial touches (including duplicate
  IDs), translucent depth writes (bit11), disabled blending, partial
  destinations, changes to the accumulated fog flag, and later opaque touches.
  Shadow/wire footprints remain excluded; a depth-failed shadow can still affect
  stencil. Effective zero-alpha non-shadow/non-wire fragments cannot consume
  eligibility, even when depth-rejected; alpha-test-rejected fragments do not
  consume a layer.
  One-native-pixel3×3 dilation protects neighbouring edge/AA reads; it is never
  adjusted from comparison errors.
- The worker combines the owned, delayed mask with the LCD3D selection mask.
- `ds_translucent.gdshader` reads the Compatibility opaque screen snapshot.
  It computes RGB6 integer blend weights `(alpha+1)/32` and `(31-alpha)/32`,
  with truncation, then writes alpha1 so GL does not apply a second blend.
  Eligible destinations have alpha31, making native max-alpha31 exact.
  The pass does not write depth or modify opaque polygon ID/fog/edge metadata.
- Single-layer eligibility avoids any need to approximate duplicate-ID stencil
  or multi-layer colour history. Surfaces preserve sorted order and remain
  chunked at256; no2048-priority workaround is claimed. Integer blending occurs
  before shared edge/fog processing and final RGB555 quantization, including
  frames whose edge/fog enables are both zero.
- Ordinary opaque polygons do not consult shadow stencil. A shadow touch only
  invalidates its footprint, so shadows no longer require whole-frame fallback.
- W-buffer, bitmap clears, geometry overflow/invalid indices and unavailable
  bounded texture uploads still fall back. W-buffer depth is proportional to
  reciprocal interpolated inverse-W, not affine interpolated vertex W; changing
  vertex Z alone would be wrong and has not been enabled.

Do not display the unmasked opaque pass as general straight-alpha output.
Also note that `write_fragment` normally uses strict less-than, with a special
front-facing-over-opaque-back-facing less-or-equal case and a 255-unit depth
comparison tolerance for attribute bit 14. The requested hardware less-or-equal
path is therefore approximate for coplanar polygons; this proof does not claim
identical DS depth history.

## Synthetic verification and limits

The public `gpu3d_poc.gd` fixtures exercise RGB6/RGB555 transfer, native GX
geometry, perspective interpolation, texture formats, alpha, ID metadata,
wrap/flip/clamp, fog, edge marking, and camera override behavior. They use
independently authored scenes, not captured game geometry.

Compare the same synthetic scene's software reference and GPU readback. Keep
missing alpha in the comparison footprint; do not narrow a mask to hide error.
An affine-UV negative control must fail the perspective test. Rendering probes
need a rendered Godot backend and the native test fixtures described in
[Building DQ9-Decomp](BUILDING.md); a headless import is not a rendering test.

Nearest texture discontinuities, coplanar ties, compressed-texture boundaries,
one-dot polygons, and alpha-zero wireframes need explicit treatment. The filled
GPU path is not a general DS rasterizer, and hardware less-or-equal depth does
not reproduce every native tolerance or front/back-facing exception.

## Watertight high-resolution strips

Coverage follows the original projected 28.4 perimeter through each native row
and vertex-Y event. A small high-resolution overlap closes snapped T-junctions.
A single polygon depth plane is used at high resolution to avoid discontinuous
per-row depths exposing geometry behind the surface. Scale 1 retains native
depth/interpolation and zero overlap. The software rasterizer and hybrid-mask
eligibility remain unchanged.

`gpu3d_cracks.gd` checks interior native polygon-ID footprints against actual
and texture-alpha-disabled GPU ID passes. Its synthetic controls distinguish
background stripes from legitimately textured or alpha-cutout pixels. This
diagnostic never narrows the ordinary color-error mask.

## Single-layer capture isolation

The default software path performs no optional fragment-capture operations.
Diagnostic buffers are allocated on capture enable; bands own separate
rows/counters, and reduction follows the barrier. Synthetic regressions compare
classification, masks, diagnostics, and software output across thread modes.

The single-layer pass deliberately supports only one partial layer over an
opaque destination. Unsupported history stays software-owned; neither a small
average error nor a successful synthetic fixture establishes general
translucency/stencil support or full-game compatibility.

## Local snapshot tests

`test_gpu_scene.gd` takes caller-selected local image, save, input, and output
paths plus explicit frame/checkpoint selections. `test_gpu_visible.gd` uses
caller-supplied application checkpoints. No game-specific defaults, geometry
censuses, captured records, or gameplay quality results are published here.

Keep local saves, snapshots, geometry, textures, screenshots, and diagnostic
bundles private. The public tests and documentation contain original algorithms
and synthetic fixtures only. Hardware-GPU performance and non-Compatibility
backend behavior require their own exercised evidence.
