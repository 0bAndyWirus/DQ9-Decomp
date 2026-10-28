# 3D-over-2D display contract

## Status and evidence

**Project compatibility contract; exact arithmetic and rounding have unresolved provenance.** This document records observable project behavior for regression and does not claim hardware validation, independent authorship, or a measured comparison with a reference emulator. Earlier project documentation records reference-emulator influence on the surrounding colour path; no independent exact 3D blend specification or hardware measurement was identified. [GBATEK's colour effects documentation](https://problemkaputt.de/gbatek-lcd-i-o-color-special-effects.htm) describes conventional 2D effect controls and target selection, but does not establish the precise RGB6/RGB5 arithmetic below.

## Inputs and result

For each visible pixel, a 3D BG0 sample has three unsigned 6-bit channels `s` (0–63) and a 5-bit alpha `p` (0–31); its immediate visible lower layer has RGB555 channels `d` (0–31). Alpha zero contributes no 3D pixel: the lower layer remains. Otherwise, if that lower layer is enabled as a second target, each result channel is

```text
floor((s * (p + 1) + 2 * d * (31 - p)) / 64)
```

The resulting RGB555 is expanded to host RGB8 using [the project RGB555 contract](rgb555.md). At `p=31` the result is `floor(s/2)`. If the visible lower layer is not a second target, and no brightness effect applies, the visible 3D colour is likewise `floor(s/2)` per channel; alpha does not attenuate it.

A visible 3D pixel over a second target blends even if the effect selector is zero, BG0 is not a first target, or the window disables colour effects. If the window hides BG0, or its alpha is zero, the lower layer remains. With no applicable second target, a permitted brightness effect on BG0 may instead apply to the truncated five-bit 3D colour. Selection depends on the immediately composed lower layer, not merely on the existence of any enabled target behind it. The lower colour can already have undergone effects.

## ROM-free regression coverage

`3d_over_backdrop_exhaustive` exercises all 32 alpha values × 64 source values × 32 destination values per channel (65,536 pixels, 196,608 channel observations), including transparent alpha and opaque alpha, using distinct permutations for the three channels. `3d_blend_selection_matrix` covers 256 combinations of four alpha values, four effect modes, first-target and second-target enablement, and window visibility/effects permission, with zero brightness coefficient. The tests use a synthetic 3D line and backdrop, not a ROM or an independent hardware oracle; they do not establish full layer-order or brightness conformance.
