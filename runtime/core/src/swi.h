#pragma once

#include <cstdint>
#include <optional>

namespace dsr {
struct Cpu;

namespace detail {

// Handles synchronous BIOS data SWIs for the DS ARM7/ARM9. The caller passes
// the decoded SWI number (ARM SWI bits 16-23, or the Thumb 8-bit immediate).
// Returns the reference BIOS handler's cycle result, including handled
// zero-cycle results. nullopt means an unsupported/control-flow SWI or null CPU.
// The caller owns dispatch overhead and the generated instruction's base cycle.
std::optional<std::uint32_t> handle_data_swi(Cpu* c, std::uint32_t imm);

} // namespace detail
} // namespace dsr
