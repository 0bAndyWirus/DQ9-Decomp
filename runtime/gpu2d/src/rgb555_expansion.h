#pragma once

#include <cstdint>

namespace dsr::cleanroom {

// Expand a channel in [0, 31] to [0, 255], truncating the exact ratio.
constexpr std::uint32_t color5_to_8(std::uint32_t value);

#if defined(DSR_X86_PACK_LINE)
// Eight 16-bit lanes contain channels in [0, 31] and factors in [0, 16].
// Mode 0 leaves brightness unchanged, 1 brightens, 2 darkens; return eight
// expanded 16-bit lanes. Brightness and expansion both truncate.
template<unsigned Mode>
inline __m128i expand_output_channels(__m128i channels, __m128i factor);
#endif

} // namespace dsr::cleanroom

#include "rgb555_expansion.inc"
