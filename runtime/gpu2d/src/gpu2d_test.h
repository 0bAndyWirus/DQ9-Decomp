#pragma once

#include "dsr/gpu2d.h"

#include <array>

namespace dsr::gpu2d_detail {

// Internal test access; no writable palette alias escapes the GPU.
struct Gpu2DTestAccess {
    static void write_bank_byte(Gpu2D& gpu, std::uint8_t bank,
                                std::uint32_t offset, std::uint8_t value);
    static std::array<std::uint8_t, 4> palette_color(const Gpu2D& gpu, std::size_t index);
    static void pack_line(Gpu2D& gpu, bool engine_b,
                         const std::array<std::array<std::uint8_t, 4>, 256>& input,
                         std::array<std::uint32_t, 256>& output);
};

} // namespace dsr::gpu2d_detail
