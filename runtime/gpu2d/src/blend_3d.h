#pragma once

#include <array>
#include <cstdint>

namespace dsr::cleanroom {

// Blend a 3D RGB6 pixel over an RGB5 pixel using the five-bit polygon alpha.
// Returns three RGB5 channels in red, green, blue order.
inline std::array<std::uint8_t, 3> blend_3d(
    const std::array<std::uint8_t, 3>& source, std::uint8_t alpha,
    const std::array<std::uint8_t, 3>& target);

} // namespace dsr::cleanroom

#include "blend_3d.inc"
