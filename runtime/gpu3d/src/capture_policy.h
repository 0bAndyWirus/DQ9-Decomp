#pragma once
#include <cstdint>

namespace dsr::gpu3d_detail {

// Software history permits one partial layer over opaque destination data.
// This is a coverage decision, never a comparison-error-derived mask.
inline bool capture_single_layer(std::uint8_t alpha, std::uint32_t attr,
                                 std::uint32_t disp3d, std::uint8_t destination_alpha,
                                 bool destination_fog, bool& partial_seen) {
    const bool shadow = ((attr >> 4U) & 3U) == 3U;
    const bool wire = ((attr >> 16U) & 31U) == 0U;
    if (shadow || wire) return false;
    if (alpha == 0U) return true;
    if (alpha == 31U) return !partial_seen;
    const bool overlap = partial_seen;
    partial_seen = true;
    return !overlap && destination_alpha == 31U && (disp3d & 8U) != 0U &&
           (attr & ((1U << 11U) | (1U << 14U))) == 0U &&
           ((disp3d & 128U) == 0U || !destination_fog || (attr & 32768U) != 0U);
}

} // namespace dsr::gpu3d_detail
