#pragma once

#include "dsr/gpu2d.h"

namespace dsr::gpu2d_detail {

// Physical write hook for testing texture invalidation independently of the
// CPU mapping: texture-only mappings deliberately expose no CPU write window.
struct Gpu2DTestAccess {
    static void write_bank_byte(Gpu2D& gpu, std::uint8_t bank,
                                std::uint32_t offset, std::uint8_t value);
};

} // namespace dsr::gpu2d_detail
