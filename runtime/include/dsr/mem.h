// Guest memory accessors used by generated code.
//
// Fast path: per-CPU 16 KiB page tables (Cpu::page_r / page_w) pointing at
// host memory. Slow path: dsr::slow_* implemented by the core runtime (IO
// registers, palette, OAM, overlapping VRAM, open bus, watched pages).
//
// Alignment semantics follow the hardware:
//  - ldr32: word load with rotation of the aligned word by (addr & 3) * 8
//    (LDR, SWP). rd32: plain aligned load (LDM, LDRD, internal use).
//  - ldrh/ldrsh: ARM9 (ARMv5) forces halfword alignment; ARM7 (ARMv4)
//    rotates an unaligned LDRH by 8 and turns an unaligned LDRSH into LDRSB.
//  - all stores force natural alignment.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include "dsr/cpu.h"

namespace dsr {

std::uint32_t slow_read32(Cpu* c, std::uint32_t a); // a is word aligned
std::uint32_t slow_read16(Cpu* c, std::uint32_t a); // a is halfword aligned
std::uint32_t slow_read8(Cpu* c, std::uint32_t a);
void slow_write32(Cpu* c, std::uint32_t a, std::uint32_t v,
                  std::uint32_t guest_pc = 0);
void slow_write16(Cpu* c, std::uint32_t a, std::uint32_t v,
                  std::uint32_t guest_pc = 0);
void slow_write8(Cpu* c, std::uint32_t a, std::uint32_t v,
                 std::uint32_t guest_pc = 0);

inline std::uint32_t rd32(Cpu* c, std::uint32_t a) {
    a &= ~3u;
    if (std::uint8_t* p = c->page_r[a >> kPageShift]) [[likely]] {
        std::uint32_t v;
        std::memcpy(&v, p + (a & kPageMask), 4);
        return v;
    }
    return slow_read32(c, a);
}

inline std::uint32_t rd16(Cpu* c, std::uint32_t a) {
    a &= ~1u;
    if (std::uint8_t* p = c->page_r[a >> kPageShift]) [[likely]] {
        std::uint16_t v;
        std::memcpy(&v, p + (a & kPageMask), 2);
        return v;
    }
    return slow_read16(c, a);
}

inline std::uint32_t rd8(Cpu* c, std::uint32_t a) {
    if (std::uint8_t* p = c->page_r[a >> kPageShift]) [[likely]] {
        return p[a & kPageMask];
    }
    return slow_read8(c, a);
}

inline void wr32(Cpu* c, std::uint32_t a, std::uint32_t v,
                 std::uint32_t guest_pc = 0) {
    a &= ~3u;
    if (std::uint8_t* p = c->page_w[a >> kPageShift]) [[likely]] {
        std::memcpy(p + (a & kPageMask), &v, 4);
        return;
    }
    slow_write32(c, a, v, guest_pc);
}

inline void wr16(Cpu* c, std::uint32_t a, std::uint32_t v,
                 std::uint32_t guest_pc = 0) {
    a &= ~1u;
    if (std::uint8_t* p = c->page_w[a >> kPageShift]) [[likely]] {
        const std::uint16_t h = static_cast<std::uint16_t>(v);
        std::memcpy(p + (a & kPageMask), &h, 2);
        return;
    }
    slow_write16(c, a, v & 0xFFFFu, guest_pc);
}

inline void wr8(Cpu* c, std::uint32_t a, std::uint32_t v,
                std::uint32_t guest_pc = 0) {
    // ARM9's VRAM byte writes are ignored; do not bypass the bus rule through
    // the direct-page store path.
    if (c->id == CpuId::Arm9 && a >= 0x06000000u && a < 0x07000000u) {
        slow_write8(c, a, v & 0xFFu, guest_pc);
        return;
    }
    if (std::uint8_t* p = c->page_w[a >> kPageShift]) [[likely]] {
        p[a & kPageMask] = static_cast<std::uint8_t>(v);
        return;
    }
    slow_write8(c, a, v & 0xFFu, guest_pc);
}

// LDR / SWP word load with unaligned rotation.
inline std::uint32_t ldr32(Cpu* c, std::uint32_t a) {
    return std::rotr(rd32(c, a), static_cast<int>((a & 3u) * 8u));
}

inline std::uint32_t ldrh(Cpu* c, std::uint32_t a) {
    const std::uint32_t v = rd16(c, a);
    if (c->id == CpuId::Arm7 && (a & 1u)) {
        return std::rotr(v, 8);
    }
    return v;
}

inline std::uint32_t ldrsh(Cpu* c, std::uint32_t a) {
    if (c->id == CpuId::Arm7 && (a & 1u)) {
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(rd8(c, a))));
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(rd16(c, a))));
}

inline std::uint32_t ldrb(Cpu* c, std::uint32_t a) { return rd8(c, a); }

inline std::uint32_t ldrsb(Cpu* c, std::uint32_t a) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(rd8(c, a))));
}

} // namespace dsr
