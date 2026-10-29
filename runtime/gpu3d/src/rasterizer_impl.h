#pragma once
#include "rasterizer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dsr::gpu3d_detail {
inline void advance_raster_values(std::array<float, 7>& value, const std::array<float, 7>& delta) {
    for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i];
}
inline bool prepare_raster_span(const RasterBoundary& left, const RasterBoundary& right,
                                int target_width, RasterSpan& span) {
    // GPU3D_RASTER_INTERFACE: interpolate across the unclipped span.
    const std::int64_t width = right.x - left.x;
    if (width <= 0 || target_width <= 0) return false;
    const std::int64_t first = std::max<std::int64_t>(left.x, 0);
    const std::int64_t last = std::min<std::int64_t>(right.x, target_width);
    if (last <= first) return false;
    span.x = static_cast<int>(first);
    span.width = static_cast<int>(last - first);
    const float inv = 1.0F / static_cast<float>(width);
    const float skip = static_cast<float>(first - left.x);
    for (std::size_t i = 0; i < span.value.size(); ++i) {
        span.delta[i] = (right.value[i] - left.value[i]) * inv;
        span.value[i] = left.value[i] + span.delta[i] * skip;
    }
    return true;
}
namespace raster_traversal_detail {
inline std::int64_t ceil_ratio(std::int64_t n, std::int64_t d) {
    return n / d + (n % d > 0 ? 1 : 0);
}
struct Chain {
    const std::array<RasterVertex, 10>& vertices;
    std::size_t count;
    int direction;
    std::size_t index = 0, visited = 0;
    const RasterVertex* top = nullptr;
    const RasterVertex* bottom = nullptr;
    int first = 0, end = 0;
    std::array<float, 7> value{}, step{};
    bool next() {
        while (visited < count) {
            const std::size_t next_index = direction > 0 ? (index + 1) % count : (index + count - 1) % count;
            const RasterVertex* a = &vertices[index];
            const RasterVertex* b = &vertices[next_index];
            index = next_index;
            ++visited;
            // GPU3D_RASTER_SPEC: compatibility for small quantized geometry.
            if (b->y < a->y) {
                if (raster_ceil_fixed(a->x) != raster_ceil_fixed(b->x)) return false;
                continue;
            }
            if (b->y == a->y) continue;
            top = a; bottom = b;
            first = raster_ceil_fixed(a->y);
            end = raster_ceil_fixed(b->y);
            const float dy = static_cast<float>(b->y - a->y);
            const float prestep = static_cast<float>(std::int64_t(first) * 16 - a->y) * (1.0F / 16.0F);
            for (std::size_t k = 0; k < value.size(); ++k) {
                step[k] = (b->attribute[k] - a->attribute[k]) * (16.0F / dy);
                value[k] = a->attribute[k] + step[k] * prestep;
            }
            return true;
        }
        return false;
    }
    std::int64_t x_at(int y) const {
        // GPU3D_RASTER_SPEC: ceiling of the signed fixed-point boundary.
        const std::int64_t dy = std::int64_t(bottom->y) - top->y;
        const std::int64_t dx = std::int64_t(bottom->x) - top->x;
        return ceil_ratio(std::int64_t(top->x) * dy + dx * (std::int64_t(y) * 16 - top->y), 16 * dy);
    }
};
} // namespace raster_traversal_detail

template<class RowSink>
void traverse_raster_polygon(const std::array<RasterVertex, 10>& vertices, std::size_t count,
                             int y_begin, int y_end, RowSink&& sink) {
    using raster_traversal_detail::Chain;
    if (count < 3 || count > vertices.size() || y_begin >= y_end) return;
    Chain forward{vertices, count, 1};
    Chain backward{vertices, count, -1};
    if (!forward.next() || !backward.next()) return;
    int y = std::max(forward.first, backward.first);
    // GPU3D_RASTER_INTERFACE: advance before the band to preserve rounding.
    while (y < y_end) {
        while (y >= forward.end) if (!forward.next()) return;
        while (y >= backward.end) if (!backward.next()) return;
        if (y >= y_begin) {
            RasterBoundary l{backward.x_at(y), y, backward.value};
            RasterBoundary r{forward.x_at(y), y, forward.value};
            sink(l, r);
        }
        advance_raster_values(forward.value, forward.step);
        advance_raster_values(backward.value, backward.step);
        ++y;
    }
}

template<class FragmentSink>
void traverse_wire_triangle(const RasterScreenVertex& a, const RasterScreenVertex& b,
                            const RasterScreenVertex& c, std::uint32_t attributes,
                            bool w_depth, int width, int y_begin, int y_end,
                            FragmentSink&& sink) {
    // GPU3D_RASTER_INTERFACE: center-sampled barycentrics and face selection.
    const auto edge = [](const RasterScreenVertex& v, const RasterScreenVertex& w, float x, float y) {
        return (x - v.x) * (w.y - v.y) - (y - v.y) * (w.x - v.x);
    };
    const float area = edge(a, b, c.x, c.y);
    if (std::abs(area) < 1e-12F || width <= 0 || y_begin >= y_end) return;
    const bool front = area > 0.0F;
    if (!(attributes & (front ? (1U << 7) : (1U << 6)))) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(width, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(y_begin, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(y_end, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    const bool wire = ((attributes >> 16) & 31U) == 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float wa = edge(b, c, px, py) / area;
            const float wb = edge(c, a, px, py) / area;
            const float wc = edge(a, b, px, py) / area;
            const float minimum = std::min({wa, wb, wc});
            if (minimum < -1e-6F || (wire && minimum > 0.02F)) continue;
            const float inv_w = wa * a.inv_w + wb * b.inv_w + wc * c.inv_w;
            if (std::abs(inv_w) < 1e-20F) continue;
            // GPU3D_RASTER_INTERFACE: recover unweighted attributes using W.
            const float w = 1.0F / inv_w;
            RasterFragment fragment{};
            fragment.x = x; fragment.y = y; fragment.front = front;
            fragment.u = (wa * a.u_over_w + wb * b.u_over_w + wc * c.u_over_w) * w;
            fragment.v = (wa * a.v_over_w + wb * b.v_over_w + wc * c.v_over_w) * w;
            for (std::size_t k = 0; k < fragment.color.size(); ++k)
                fragment.color[k] = (wa * a.color_over_w[k] + wb * b.color_over_w[k] + wc * c.color_over_w[k]) * w;
            if (w_depth) {
                const float depth = std::clamp(std::floor(w * 4096.0F), 0.0F, 16777215.0F);
                fragment.depth = static_cast<std::uint32_t>(depth);
            } else {
                const float z = wa * a.z + wb * b.z + wc * c.z;
                const float depth = std::floor(std::clamp((z + 1.0F) * 0.5F, 0.0F, 1.0F) * 4194303.0F);
                fragment.depth = static_cast<std::uint32_t>(depth) << 2;
            }
            sink(fragment);
        }
    }
}
} // namespace dsr::gpu3d_detail
