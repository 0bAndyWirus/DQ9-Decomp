# 3D render timing contract

This is a ROM-free compatibility contract for the current runtime, not a claim of independently measured hardware timing. The scanline-214 render boundary is a reference-emulator compatibility target; the cycle-level schedule and synchronous whole-frame rendering below describe this runtime. No independent hardware measurement of these details is claimed.

## Clock and events

Time is in runtime ticks, measured from reset, which starts at the beginning of line 0. Each line lasts 2,130 ticks; a frame contains 263 lines. For frame index `f` starting at zero, line `n` starts at `f * 560190 + n * 2130`. The HBlank flag becomes set 1,606 ticks after each line start and is cleared at the next line start. At line start, the new VCOUNT and its compare flag/interrupt are established before any 3D completion callback.

| Event | First occurrence (ticks from reset) | Recurrence |
| --- | ---: | --- |
| Line 192 start; 3D geometry swap/flush | 408,960 | every 560,190 ticks |
| Line 213 start | 453,690 | every 560,190 ticks |
| Line 213 HBlank | 455,296 | every 560,190 ticks |
| Line 214 start; synchronous 3D render and completion callback | 455,820 | every 560,190 ticks |
| Line 262 start; display VBlank flag clears | 558,060 | every 560,190 ticks |
| Next line 0 start | 560,190 | every 560,190 ticks |

The display VBlank flag sets at line 192 and clears at line 262. At the completion callback VCOUNT is 214, display VBlank is set, and HBlank is clear; when a line-214 VCOUNT interrupt is enabled, its request is already set. Rendering occurs once at that boundary even without a new geometry swap. A tick ending exactly on an event includes that event; a tick ending one cycle earlier does not.

## Data and presentation epochs

The line-192 geometry swap/flush does not itself rasterize. Texture and clear-image VRAM contents and mappings can be updated after the swap, and the render at line 214 observes their then-current values. An upload after the synchronous render does not retroactively change its output; the next render observes the later data even without a further swap. This is a whole-frame approximation, not a claim about partial scanline updates on hardware.

The visible 2D scanlines in a frame use the 3D result rendered in the previous frame; the newly rendered result at line 214 is for subsequent visible scanlines. The completion callback observes the completed new render, not the already presented 2D frame.

## ROM-free regression coverage

`runtime/core/tests/render_timing_tests.cpp` checks the line-213 HBlank edge and line-214 render edge at one-tick granularity across three consecutive frames, event ordering as seen by the callback, a render each frame without geometry swaps, texture and clear-image uploads between swap and render, late texture writes, and alternating 2D/3D presentation epochs. These tests exercise public machine and GPU observations; they do not constitute independent emulator or hardware measurements.
