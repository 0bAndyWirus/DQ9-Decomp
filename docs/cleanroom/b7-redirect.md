# B7 low-address redirect contract

This is the runtime's compatibility contract for the initial address of a B7 cartridge transfer. It is a phase-1 isolation of the existing implementation, not an independent rewrite or a provenance clearance.

[GBATEK, DS Cartridge Protocol, “B7aaaaaaaa000000h (200h) - Get Data”](https://problemkaputt.de/gbatek-ds-cartridge-protocol.htm) documents a 32-bit address sent most-significant byte first, no alignment restriction, and redirecting addresses 0 through `0x7fff` to `0x8000 + (address & 0x1ff)`. It also describes 4-KiB stream wrapping, cartridge-specific behavior and ROM-size mirroring. Those additional hardware behaviors are independent of this redirect and are **not** implemented by this contract.

## Interface and observable behavior

`dsr::detail::cartridge_b7_redirect(uint32_t address)` returns `0x8000 + (address & 0x1ff)` for an input below `0x8000`, otherwise the input unchanged. This is an unsigned 32-bit address; all input bits are significant above the threshold. The caller applies it once to the address from command bytes 1–4 when starting command `0xb7`. It does not apply to other commands or to the direct `read_rom` interface.

The current stream implementation is separate: byte position `i` reads at `(redirected_address + i) modulo 2^32`, and an address beyond the loaded image returns `0xff`. It advances linearly across both `0x200` and `0x1000` boundaries, without repeating the redirect or adding 4-KiB wrap. This is existing compatibility behavior, not a claim of hardware conformance; changing it is outside this rewrite. In particular, a B7 start of `0x7fff` starts at `0x81ff`, then continues at `0x8200`, and a start at `0x8ffc` proceeds through `0x9000`. Command `0x00` and raw boot-loader reads retain their own address behavior. Timing and completion are specified separately in `cartridge-timing.md`.

## ROM-free regression coverage

`runtime/periph/tests/periph_tests.cpp` creates a synthetic image with distinct address-dependent bytes. It checks all 32,768 low command addresses through both the interface and actual B7 transfers, including unaligned starts, and checks high bits and threshold/end-of-image/32-bit-wrap examples. Four complete 8-KiB transfers check that the independent stream is not changed at 512-byte, 4-KiB, EOF or 32-bit boundaries. Raw and command `0x00` reads are checked separately. These are compatibility tests, not independent hardware measurements.

For the blind phase, replace `runtime/periph/src/cartridge_b7_redirect.cpp` using this contract and its header; do not use the old implementation as source. Keep the stream logic in `periph.cpp` unchanged.
