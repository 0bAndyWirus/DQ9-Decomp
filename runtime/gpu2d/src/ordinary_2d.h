#pragma once

#include <cstdint>

namespace dsr::cleanroom {

// RGB5 colors have r, g, b, and alpha members. Factors for brightness are 0..16.
template <typename Color>
inline Color ordinary_blend(Color source, Color target, std::uint32_t eva, std::uint32_t evb);
template <typename Color>
inline Color ordinary_brighten(Color color, std::uint32_t factor);
template <typename Color>
inline Color ordinary_darken(Color color, std::uint32_t factor);

// Result: 0 copy, 1 alpha blend, 2 brighten, 3 darken. Layer IDs are 0..5.
inline unsigned ordinary_effect(unsigned layer, unsigned dst_layer, bool semi_obj,
                                bool effects_allowed, unsigned effect,
                                unsigned target1, unsigned target2);
inline bool ordinary_copy_only(unsigned effect, unsigned target1, unsigned target2);
inline unsigned ordinary_backdrop_effect(unsigned effect, unsigned target1);

} // namespace dsr::cleanroom

#include "ordinary_2d.inc"
