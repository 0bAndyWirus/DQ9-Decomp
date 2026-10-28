#include "cartridge_b7_redirect.h"

namespace dsr::detail {

std::uint32_t cartridge_b7_redirect(std::uint32_t address) {
    return address < 0x8000u ? 0x8000u + (address & 0x1ffu) : address;
}

} // namespace dsr::detail
