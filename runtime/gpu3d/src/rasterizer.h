#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace dsr::gpu3d_detail {

// Clipped homogeneous coordinates and unprojected texture/color attributes.
struct RasterInputVertex {
    std::array<float, 4> clip{};
    float u = 0.0F;
    float v = 0.0F;
    std::array<float, 3> color{};
};

float raster_plane_distance(const RasterInputVertex& vertex, int plane);
RasterInputVertex interpolate_raster_clip(const RasterInputVertex& a,
                                         const RasterInputVertex& b, float t);
std::size_t clip_raster_polygon(std::array<RasterInputVertex, 16>& input, std::size_t count,
                                std::uint32_t attributes, std::array<RasterInputVertex, 16>& output);

struct RasterScreenVertex {
    float x = 0.0F, y = 0.0F, z = 0.0F, inv_w = 0.0F;
    float u_over_w = 0.0F, v_over_w = 0.0F;
    std::array<float, 3> color_over_w{};
};

RasterScreenVertex project_raster_screen(const std::array<float, 4>& clip, float u, float v,
                                         const std::array<float, 3>& color,
                                         std::uint32_t viewport, int scale);

struct RasterFragment {
    int x = 0, y = 0;
    bool front = false;
    std::array<float, 3> color{};
    float u = 0.0F, v = 0.0F;
    std::uint32_t depth = 0;
};

// Visits covered wire fragments in scan order within [y_begin, y_end).
template<class FragmentSink>
void traverse_wire_triangle(const RasterScreenVertex& a, const RasterScreenVertex& b,
                            const RasterScreenVertex& c, std::uint32_t attributes,
                            bool w_depth, int width, int y_begin, int y_end,
                            FragmentSink&& sink);
bool prepare_raster_dot(const RasterScreenVertex& screen, const RasterInputVertex& vertex,
                        bool w_depth, int width, int height, int y_begin, int y_end,
                        RasterFragment& fragment);

// Fixed-point screen coordinates and perspective-weighted attributes:
// 1/w, z, u/w, v/w, r/w, g/w, b/w.
struct RasterVertex {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::array<float, 7> attribute{};
};

struct RasterBoundary {
    std::int64_t x = 0;
    int y = 0;
    std::array<float, 7> value{};
};

struct RasterSpan {
    int x = 0;
    int width = 0;
    std::array<float, 7> value{};
    std::array<float, 7> delta{};
};
// Sequential advancement is deliberate: multiplying delta by a lane index
// changes floating-point rounding.
inline void advance_raster_values(std::array<float, 7>& value,
                                  const std::array<float, 7>& delta);

#if defined(__x86_64__) || defined(_M_X64)
inline void raster_four_samples(__m128& current, __m128 step, bool more,
                                __m128& a, __m128& b, __m128& c, __m128& d);
#endif


int raster_ceil_fixed(std::int64_t value);
bool prepare_raster_polygon(const RasterInputVertex* vertices, std::size_t count,
                            std::uint32_t viewport, std::uint32_t attributes, int scale,
                            std::array<RasterVertex, 10>& result, bool& front_facing);
// Visits row boundaries in increasing y within [y_begin, y_end); advancement
// outside the band still affects the values supplied inside it.
template<class RowSink>
void traverse_raster_polygon(const std::array<RasterVertex, 10>& vertices, std::size_t count,
                             int y_begin, int y_end, RowSink&& sink);
// Produces a horizontally clipped, nonempty span; returns false if none.
inline bool prepare_raster_span(const RasterBoundary& left, const RasterBoundary& right,
                                int target_width, RasterSpan& span);

} // namespace dsr::gpu3d_detail
