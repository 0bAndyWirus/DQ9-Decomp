// Runtime entry points called by generated (recompiled) code, plus the
// function table each generated CPU image exports.
#pragma once

#include <cstddef>
#include <cstdint>

#include "dsr/cpu.h"

namespace dsr {

// Called at function entries and loop headers when c->budget <= 0.
// pc is the guest address of the block being entered (resume point used as
// the IRQ return address). May deliver IRQs (nested guest calls) and/or
// yield this CPU's fiber to the scheduler; returns when the CPU may continue.
void dsr_checkpoint(Cpu* c, std::uint32_t pc);

// Call a guest address not resolved at recompile time (BX/BLX register,
// LDR/POP pc to non-return targets, ambiguous overlay targets). Bit 0 is the
// interworking Thumb bit and is ignored for lookup. Returns when the callee
// returns. A tail jump is emitted as dsr_call(...) followed by `return`.
void dsr_call(Cpu* c, std::uint32_t target);

// SWI/SVC. imm is the raw instruction immediate (ARM: 24 bits, Thumb: 8
// bits); pc is the address of the SWI instruction.
void dsr_swi(Cpu* c, std::uint32_t imm, std::uint32_t pc, bool thumb);

// PSR transfer. field_mask bit0=c, bit1=x, bit2=s, bit3=f (MSR field bits).
std::uint32_t dsr_mrs_cpsr(Cpu* c);
void dsr_msr_cpsr(Cpu* c, std::uint32_t value, std::uint32_t field_mask);
std::uint32_t dsr_mrs_spsr(Cpu* c);
void dsr_msr_spsr(Cpu* c, std::uint32_t value, std::uint32_t field_mask);

// Exception return (MOVS/SUBS pc, LDM {..pc}^): CPSR <- SPSR. Generated code
// sets nothing else and immediately returns from the guest function.
void dsr_exception_return(Cpu* c);

// User-bank register access for LDM/STM with ^ and without pc.
std::uint32_t dsr_user_reg_read(Cpu* c, unsigned reg);
void dsr_user_reg_write(Cpu* c, unsigned reg, std::uint32_t value);

// Coprocessor transfers (CP15 on ARM9; anything else is fatal/diagnosed).
std::uint32_t dsr_mrc(Cpu* c, unsigned cp, unsigned op1, unsigned crn, unsigned crm,
                      unsigned op2, std::uint32_t pc);
void dsr_mcr(Cpu* c, unsigned cp, unsigned op1, unsigned crn, unsigned crm, unsigned op2,
             std::uint32_t value, std::uint32_t pc);

// Undefined / unsupported instruction or BKPT reached at run time.
[[noreturn]] void dsr_undefined(Cpu* c, std::uint32_t pc, std::uint32_t insn);

// Return-path verification (enabled with -DDSR_RET_CHECK). Generated code
// calls DSR_RET(c, target) right before every return-like exit with the
// guest target value, and DSR_POSTCALL(c, expected) after each direct call
// with the guest return address the caller expects.
void dsr_note_return(Cpu* c, std::uint32_t target);
void dsr_check_return(Cpu* c, std::uint32_t expected);

// Function-entry trace (enabled with -DDSR_TRACE): generated code calls
// DSR_ENTRY(c, module_id, addr) first thing in every function, where module_id
// is the manifest module_id of the generated body (-1 main, -2 itcm, -3 dtcm,
// 0.. overlay id), so overlays sharing a RAM address are distinguishable. The
// runtime writes rows `frame cpu pc r0 r1 r2 r3 sp lr module_id` (the oracle
// calls.txt columns plus a trailing signed-decimal module_id) to the file
// `DSR_TRACE_FILE`, up to `DSR_TRACE_LIMIT` rows. Optional inclusive
// `DSR_TRACE_START_FRAME`/`DSR_TRACE_END_FRAME` filters bound rows by frame;
// `DSR_TRACE_ADDR` restricts rows to one guest entry address.
void dsr_trace_entry(Cpu* c, std::int32_t module_id, std::uint32_t addr);

#ifdef DSR_RET_CHECK
#define DSR_RET(c, target) ::dsr::dsr_note_return((c), (target))
#define DSR_POSTCALL(c, expected) ::dsr::dsr_check_return((c), (expected))
#else
#define DSR_RET(c, target) ((void)0)
#define DSR_POSTCALL(c, expected) ((void)0)
#endif

#ifdef DSR_TRACE
#define DSR_ENTRY(c, module_id, addr) ::dsr::dsr_trace_entry((c), (module_id), (addr))
#else
#define DSR_ENTRY(c, module_id, addr) ((void)0)
#endif

// ---- Function tables exported by generated code ----

inline constexpr std::int32_t kModuleArm9Main = -1;
inline constexpr std::int32_t kModuleArm9Itcm = -2;
inline constexpr std::int32_t kModuleArm9Dtcm = -3;
inline constexpr std::int32_t kModuleArm7Main = -4;
inline constexpr std::int32_t kModuleArm7Wram = -5;
inline constexpr std::int32_t kModuleArm7MainRam = -6; // ARM7 autoload into main RAM
// Values >= 0 are ARM9 overlay IDs.

struct FnEntry {
    std::uint32_t addr;   // guest entry address, bit 0 clear
    std::uint32_t size;   // bytes of the module image the body was built from
    std::int32_t module;  // kModule* or overlay id
    std::uint32_t thumb;  // 1 if the entry is Thumb code
    std::uint64_t hash;   // FNV-1a 64 of the guest bytes [addr, addr + size)
    GuestFn* slot;        // every direct call goes through *slot; overrides replace it
    const char* name;     // caller-supplied symbol name (e.g. "synthetic_entry")
};

struct FnTable {
    const FnEntry* entries;
    std::size_t count;
};
struct GuestAddressRange {
    std::uint32_t begin;
    std::uint32_t size;
};

using DispatchRegionFn = void (*)(Cpu*, std::uint32_t entry_address);

// Shared decoded regions accept any guest address in [begin, begin + size);
// the region body validates instruction boundaries through its dispatcher.
// Hashing skips data ranges such as mutable literal pools.
// Kept separate from FnTable so existing function-entry table ABI is stable.
struct DispatchRegionEntry {
    std::uint32_t begin;
    std::uint32_t size;
    std::int32_t module;
    std::uint32_t thumb;
    std::uint64_t hash;
    const GuestAddressRange* data_ranges;
    std::size_t data_range_count;
    DispatchRegionFn body;
};

struct DispatchRegionTable {
    const DispatchRegionEntry* entries;
    std::size_t count;
};

const DispatchRegionTable& gen_dispatch_regions_arm9();
const DispatchRegionTable& gen_dispatch_regions_arm7();

// Fatal path used when a region descriptor accepted an address that is not
// one of that region's decoded instruction boundaries.
[[noreturn]] void dsr_unresolved_target(Cpu* c, std::uint32_t target);


// Defined by the generated sources for each CPU image.
const FnTable& gen_table_arm9();
const FnTable& gen_table_arm7();

inline std::uint64_t fnv1a64(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace dsr
