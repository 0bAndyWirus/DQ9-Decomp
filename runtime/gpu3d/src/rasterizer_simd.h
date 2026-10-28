#pragma once

#include <immintrin.h>

namespace dsr::gpu3d_detail {

// Produces four consecutive samples and, when needed, the next sample.
// The additions must remain sequential to match the scalar path's rounding.
inline void raster_four_samples(__m128& current, __m128 step, bool more,
                                __m128& a, __m128& b, __m128& c, __m128& d) {
    a = current;
    b = _mm_add_ps(a, step);
    c = _mm_add_ps(b, step);
    d = _mm_add_ps(c, step);
    if (more) current = _mm_add_ps(d, step);
}

} // namespace dsr::gpu3d_detail
