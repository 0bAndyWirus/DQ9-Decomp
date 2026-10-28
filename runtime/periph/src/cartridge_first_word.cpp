#include "cartridge_first_word.h"

namespace dsr::detail {

// First-word compatibility delay, in machine ticks; see
// docs/cleanroom/cartridge-timing.md (not a hardware timing claim).
std::int64_t cartridge_first_word_ticks(std::uint32_t rom_control) {
    const std::uint32_t gap = rom_control & 0x1fffu;
    const std::uint32_t clock = (rom_control & (1u << 27)) ? 8u : 5u;
    return (8u + gap) * clock + 4u;
}

} // namespace dsr::detail
