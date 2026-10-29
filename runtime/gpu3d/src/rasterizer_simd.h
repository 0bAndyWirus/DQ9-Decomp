#pragma once

#include "rasterizer.h"

#if defined(__x86_64__) || defined(_M_X64)
namespace dsr::gpu3d_detail {

// GPU3D_RASTER_INTERFACE.md: each sample uses successive float additions;
// only advance current past the fourth sample when more samples are needed.
inline void raster_four_samples(__m128& current, __m128 step, bool more,
                                __m128& a, __m128& b, __m128& c, __m128& d) {
    a = current;
    b = _mm_add_ps(a, step);
    c = _mm_add_ps(b, step);
    d = _mm_add_ps(c, step);
    if (more) {
        current = _mm_add_ps(d, step);
    }
}

} // namespace dsr::gpu3d_detail
#endif
