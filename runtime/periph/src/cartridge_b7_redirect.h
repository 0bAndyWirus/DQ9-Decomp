#pragma once

#include <cstdint>

namespace dsr::detail {

// Resolve the B7 command's initial address, before streaming any data.
std::uint32_t cartridge_b7_redirect(std::uint32_t address);

} // namespace dsr::detail
