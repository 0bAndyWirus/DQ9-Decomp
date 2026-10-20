// Guest CPU state shared by generated (recompiled) code and the runtime.
//
// ABI contract: every recompiled guest function has the signature
//   void fn(dsr::Cpu* c);
// Guest registers live in c->r[]. Flags are kept unpacked (each 0 or 1).
// Generated code never touches banked registers or mode bits directly; it
// goes through rt.h helpers (MSR/MRS/SWI/CP15/checkpoint).
#pragma once

#include <cstdint>

namespace dsr {

struct Machine;

enum class CpuId : std::uint32_t { Arm9 = 0, Arm7 = 1 };

// CPSR mode values.
enum : std::uint32_t {
    kModeUsr = 0x10,
    kModeFiq = 0x11,
    kModeIrq = 0x12,
    kModeSvc = 0x13,
    kModeAbt = 0x17,
    kModeUnd = 0x1B,
    kModeSys = 0x1F,
};

// CPSR control bits held in Cpu::cpsr_ctrl (flags are kept separately).
enum : std::uint32_t {
    kCpsrModeMask = 0x1F,
    kCpsrT = 1u << 5,
    kCpsrF = 1u << 6,
    kCpsrI = 1u << 7,
};

// Page granularity of the fast memory tables (16 KiB).
inline constexpr unsigned kPageShift = 14;
inline constexpr std::uint32_t kPageSize = 1u << kPageShift;
inline constexpr std::uint32_t kPageMask = kPageSize - 1;
inline constexpr std::uint32_t kPageCount = 1u << (32 - kPageShift);

struct Cpu {
    // r[0..15] of the current mode. r[15] is not maintained by generated code;
    // instruction-specific PC values are emitted directly. PC-relative
    // effective addresses can be constant, but their guest memory loads stay live.
    std::uint32_t r[16];

    // Unpacked flags; each is exactly 0 or 1.
    std::uint32_t n, z, cf, v, q;

    // Mode, T, F and I bits (CPSR bits 0..7). Owned by the runtime.
    std::uint32_t cpsr_ctrl;

    // Remaining cycle budget for the current scheduler slice. Generated code
    // subtracts the static cycle estimate of each basic block and calls
    // dsr_checkpoint() at function entries and loop headers when <= 0.
    // The runtime sets it to 0 to force a prompt checkpoint (e.g. an IRQ
    // became deliverable).
    std::int64_t budget;

    // Fractional ARM9 cycles in 1/64 units, carried across generated blocks.
    // CPU time is not part of a saved guest thread or architectural state.
    std::uint32_t cycle_fraction;

    CpuId id;
    Machine* m;

    // Fast memory: host pointer for each 16 KiB guest page, or nullptr to
    // take the slow path (IO, palette, OAM, unmapped, watched pages).
    // Indexed by (addr >> kPageShift).
    std::uint8_t** page_r;
    std::uint8_t** page_w;

    // Runtime-private state (banked registers, SPSRs, current fiber, ...)
    // is kept outside this struct and reached through m.
};

using GuestFn = void (*)(Cpu*);

} // namespace dsr
