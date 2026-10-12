// Original glue for the private local DeSmuME oracle; no emulator sources.
// Include after armcpu.h/MMU.h, and call before/after the instruction handler.
#pragma once

#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <tuple>

namespace dq9_oracle {

inline bool call_coverage_enabled() {
    static const bool enabled = std::getenv("DS_ORACLE_CALL_TARGETS") != nullptr;
    return enabled;
}

// Explicit link instructions and the ARMv4T mov-lr/indirect-transfer idiom.
// Ordinary returns, switch branches, and untaken conditional calls are excluded.
inline bool is_call(const armcpu_t& state) {
    if (!call_coverage_enabled()) return false;
    const u32 op = state.instruction;
    const u32 pc = state.instruct_adr;
    if (state.CPSR.bits.T) {
        if ((op & 0xf800) == 0xf800 || (op & 0xf800) == 0xe800) return true;
        if ((op & 0xff87) == 0x4780) return true; // blx Rm
        const bool bx = (op & 0xff87) == 0x4700 && ((op >> 3) & 15) != 14;
        const bool mov_pc = (op & 0xff87) == 0x4687 && ((op >> 3) & 15) != 14;
        return (bx || mov_pc) && state.R[14] == ((pc + 2) | 1);
    }
    const bool executes = (op >> 28) == 14 || TEST_COND(op >> 28, CODE(op), state.CPSR);
    if (!executes) return false;
    if ((op & 0xfe000000) == 0xfa000000) return true; // blx immediate
    if ((op & 0x0f000000) == 0x0b000000) return true; // bl immediate
    if ((op & 0x0ffffff0) == 0x012fff30) return true; // blx Rm
    const bool bx = (op & 0x0ffffff0) == 0x012fff10 && (op & 15) != 14;
    const bool mov_pc = (op & 0x0ffffff0) == 0x01a0f000 && (op & 15) != 14;
    const bool ldr_pc = (op & 0x0c10f000) == 0x0410f000;
    return (bx || mov_pc || ldr_pc) && state.R[14] == pc + 4;
}

// Called only after an instruction actually executed, before prefetch changes PC.
// The live 32-byte fingerprint preserves overlay provenance at shared addresses.
inline void record_call(int cpu, const armcpu_t& state, u32 source, u64 frame) {
    using Key = std::tuple<int, u32, bool, std::array<u8, 32>>;
    static std::set<Key, std::less<>> seen;
    static std::ofstream output(std::getenv("DS_ORACLE_CALL_TARGETS"));
    const bool thumb = state.CPSR.bits.T != 0;
    const u32 address = state.next_instruction & (thumb ? ~1u : ~3u);
    static const char digits[] = "0123456789abcdef";
    std::array<u8, 32> bytes;
    for (u32 offset = 0; offset < 32; ++offset) {
        bytes[offset] = _MMU_read08(cpu, MMU_AT_DEBUG, address + offset);
    }
    const auto key = std::tie(cpu, address, thumb, bytes);
    const auto position = seen.lower_bound(key);
    if (position != seen.end() && !seen.key_comp()(key, *position)) return;
    seen.emplace_hint(position, cpu, address, thumb, bytes);
    output << cpu << ' ' << std::hex << address << ' ' << (thumb ? 't' : 'a') << ' ';
    for (u8 value : bytes) output << digits[value >> 4] << digits[value & 15];
    output << ' ' << source << ' ' << std::dec << frame << '\n';
    output.flush();
}

} // namespace dq9_oracle
