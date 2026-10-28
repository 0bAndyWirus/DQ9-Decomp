# SPU ADPCM and PSG table contract

## Scope and provenance

This is a ROM-free compatibility contract for three isolated lookup operations, not a completed independent rewrite or provenance clearance. The existing implementation was moved unchanged in behavior to `runtime/spu/src/sound_tables.inc`; its authoring origin remains unresolved. A blind replacement should use only this contract, the narrow interface `runtime/spu/src/sound_tables.h`, and the ROM-free tests, not the old implementation. The [GBATEK DS sound channel](https://problemkaputt.de/gbatek.htm#dssoundchannels015) and [DS sound notes](https://problemkaputt.de/gbatek.htm#dssoundnotes) sections document the IMA-ADPCM tables and the eight PSG duty waveforms. No independent hardware measurement is claimed here.

## Interface and outputs

All inputs are unsigned. `adpcm_step(index)` takes an index in 0–88 inclusive and returns the corresponding decimal value, in order:

```
7 8 9 10 11 12 13 14 16 17 19 21 23 25 28 31
34 37 41 45 50 55 60 66 73 80 88 97 107 118 130 143
157 173 190 209 230 253 279 307 337 371 408 449 494 544 598 658
724 796 876 963 1060 1166 1282 1411 1552 1707 1878 2066 2272 2499 2749 3024
3327 3660 4026 4428 4871 5358 5894 6484 7132 7845 8630 9493 10442 11487 12635 13899
15289 16818 18500 20350 22385 24623 27086 29794 32767
```

`adpcm_index_adjust(magnitude)` takes a three-bit magnitude in 0–7 and returns respectively `-1, -1, -1, -1, 2, 4, 6, 8`. `psg_high(duty, phase)` takes a duty and phase each in 0–7; its output is true exactly for the `1` bits below, read left to right as phases 0 through 7:

| duty | phases 0–7 |
| ---: | :--- |
| 0 | 00000001 |
| 1 | 00000011 |
| 2 | 00000111 |
| 3 | 00001111 |
| 4 | 00011111 |
| 5 | 00111111 |
| 6 | 01111111 |
| 7 | 00000000 |

Out-of-range inputs are outside the interface contract. The caller uses the current index for the step, the low three code bits for the adjustment, and clamps the new index to 0–88. For PSG it advances its position modulo eight before looking up the output; high maps to 32767 and low to -32768. These caller details preserve current project behavior, not necessarily hardware behavior: GBATEK describes low as -32767 and gives a separately rounded ADPCM delta and a different negative saturation rule, whereas this runtime uses one integer division by eight and clamps the predictor to -32768..32767. This task does not change decoding, sample scheduling, mixing, or those discrepancies.

## Regression coverage

`runtime/spu/tests/sound_tables_tests.cpp` directly checks every one of the 89 step outputs, all eight index adjustments, and all 64 duty/phase outputs, without ROM or reliance on assertions enabled by build type. Existing SPU tests additionally exercise the decoder and channel behavior. The lookup tests establish the specified finite domains only; they do not establish hardware conformance or independent authorship.
