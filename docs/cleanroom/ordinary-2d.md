# Ordinary 2D colour effects contract

## Status and sources

This is a **project compatibility contract**, not a claim of independent authorship, hardware validation, or complete DS conformance. Phase 1 isolates the existing implementation; a later blind rewrite must use this contract and the tests, not the isolated old `.inc`. [GBATEK, LCD I/O Color Special Effects](https://problemkaputt.de/gbatek-lcd-i-o-color-special-effects.htm) documents BLDCNT, BLDALPHA, BLDY, target bits, coefficient saturation, channel formulae, intervening pixels, and semi-transparent OBJ precedence. The rules below also specify project behavior where GBATEK does not establish this implementation's ordering or rounding. In particular, the project's bottom-up compositing and backdrop preprocessing are compatibility behavior, not a hardware claim. 3D-over-2D forced blending and RGB6 arithmetic are independent and remain governed by [blend-3d.md](blend-3d.md).

## Inputs and arithmetic

Colors comprise independent integer RGB channels in 0–31. BLDCNT bits 0–5 select first targets, bits 6–7 select the effect (0 none, 1 alpha, 2 brighten, 3 darken), and bits 8–13 select second targets. Layer IDs/bit positions are BG0–BG3 = 0–3, OBJ = 4, backdrop = 5. EVA and EVB are the 5-bit fields of BLDALPHA; EVY is the low 5 bits of BLDY. Each coefficient is saturated to 16 before use. For a source `s` and already-composed lower colour `d`, each output channel is:

```text
alpha:    min(31, floor((s * EVA + d * EVB) / 16))
brighten: s + floor((31 - s) * EVY / 16)
darken:   s - floor(s * EVY / 16)
```

Floors are integer truncation, independently per channel; there is no rounding adjustment. Effect output alpha is 31. A copied colour retains its alpha. The arithmetic interface accepts arbitrary unsigned alpha factors and clamps them, while brightness helpers require an already-clamped factor 0–16. The register path clamps EVY, including raw values 17–31. Normal semi-transparent OBJ uses BLDALPHA. Bitmap OBJ with nonzero OAM alpha nibble `n` uses EVA `n+1`, EVB `16-(n+1)` instead; a zero nibble hides that OBJ. Normal OBJ carries alpha 31 and bitmap OBJ carries the resulting 2–16 coefficient. These coefficients apply whenever that semi-transparent pixel blends, including if alpha mode is selected normally.

## Selection and composition

Visible, opaque pixels are painted bottom-to-top over a backdrop, retaining the layer ID of the most recently painted pixel and its already-processed colour. Transparent or window-hidden layers are not painted. The immediate lower layer is an eligible second target only if its ID differs from the source ID and its corresponding second-target bit is set; an intervening non-target blocks a target behind it. The renderer first chooses the topmost visible OBJ, so it does not blend OBJ against OBJ.

For an ordinary 2D source, select exactly one action in this order:

1. A semi-transparent OBJ with an eligible lower target blends, irrespective of the mode, first-target bit, or window effects permission. This takes precedence over brightness.
2. Otherwise, if the window permits effects and the source is a first target, mode 1 blends only when the immediate lower layer is eligible; modes 2 and 3 brighten or darken respectively.
3. Otherwise copy the source unchanged. In particular a semi-transparent OBJ without an eligible lower target can still receive selected brightness.

Before this pixel loop, if backdrop is selected as first target and the mode is brighten or darken, the renderer applies brightness to the backdrop itself, regardless of the window effects bit. This preprocessed colour is also the colour seen by layers above it. When no second-target bits are set and either the mode is none or there are no first-target bits, the copy-only fast path selects the first visible layer rather than composing every lower layer; it has the same output under those conditions. The output conversion and master brightness have separate contracts; the portable output path reuses the brightness channel arithmetic with a clamped coefficient.

## ROM-free regression coverage

`runtime/gpu2d/tests/ordinary_2d_tests.cpp` independently computes expected results and checks all 32 × 32 source/destination channel pairs against all 32 × 32 raw alpha coefficient pairs (1,048,576 RGB test cases, all three channel positions), all 32 channel values × 17 already-clamped brightness coefficients for both directions (544 cases), and all 6 × 6 layer pairs × 64 × 64 target masks × 4 modes × 2 window permissions × 2 semi flags (2,359,296 selection cases). It also checks all target masks and modes for backdrop selection and copy-only eligibility. The test's failures remain active in release builds.

The `ordinary_composition` and `ordinary_semi_obj` renderer tests cover immediate-layer blocking, alpha blending, window-disabled effects, backdrop preprocessing with a saturated coefficient, forced normal OBJ blending, and copying without a second target. Existing `bitmap_obj` covers bitmap OBJ alpha and `random_text_reference` covers many composite register/window combinations. These are ROM-free compatibility tests; no independent hardware oracle or full scene ordering proof is claimed.

## Blind replacement boundary

The narrow interface is `runtime/gpu2d/src/ordinary_2d.h`. The old implementation to discard and replace is `runtime/gpu2d/src/ordinary_2d.inc`; callers in `gpu2d.cpp` remain in place. The 3D forced-target branch and `blend_3d` implementation are outside this replacement. The interface returns the effect number 0–3; copy-only and backdrop selection are explicit helpers. The implementation is inline because these operations occur in the per-pixel hot path. RGB5 color arguments must expose byte-sized `r`, `g`, `b`, and `alpha` members and be constructible from those four channel values.
