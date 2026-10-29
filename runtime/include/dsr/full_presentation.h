#pragma once

#include <algorithm>
#include <cstddef>

namespace dsr {

// Map a centered native LCD pixel to the Full-width 3D raster. Match the
// integer viewport width and pixel-center sampling used by the Godot host.
constexpr unsigned full_presentation_scale(int scale) {
    return static_cast<unsigned>(std::clamp(scale, 1, 6));
}
constexpr std::size_t full_presentation_source_x(std::size_t x, unsigned scale) {
    const unsigned native_width = 256U * scale;
    const unsigned target_width = (native_width * 4U + 1U) / 3U;
    return ((2U * x + 1U) * scale + target_width - native_width) * 128U / target_width;
}

} // namespace dsr
