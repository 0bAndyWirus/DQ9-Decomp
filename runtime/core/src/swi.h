#pragma once

#include <cstdint>

namespace dsr {
struct Cpu;

namespace detail {

// Handles synchronous BIOS data SWIs for the DS ARM7/ARM9. The caller passes
// the decoded SWI number (ARM SWI bits 16-23, or the Thumb 8-bit immediate).
// Returns false for control-flow SWIs and functions not implemented here.
bool handle_data_swi(Cpu* c, std::uint32_t imm);

} // namespace detail
} // namespace dsr
