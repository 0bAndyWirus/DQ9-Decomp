#pragma once

#include <cstdint>

namespace dsr::detail {

// Returns the machine ticks until data becomes ready for a nonempty transfer.
std::int64_t cartridge_first_word_ticks(std::uint32_t rom_control);

} // namespace dsr::detail
