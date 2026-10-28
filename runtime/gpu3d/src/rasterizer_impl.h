#pragma once

// Replacement-owned rasterizer implementation. Not part of the read-safe interface.
#include "rasterizer.h"

#include <algorithm>
#include <cmath>

namespace dsr::gpu3d_detail {
namespace raster_impl {
struct RasterEdge : RasterBoundary {
    std::int64_t x_step = 0;
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    std::int64_t error = 0;
    int height = 0;
    std::array<float, 7> step{};
};

inline bool floor_div_mod(std::int64_t numerator, std::int64_t denominator,
                   std::int64_t& quotient, std::int64_t& remainder) {
    if (denominator <= 0) return false;
    quotient = numerator / denominator;
    remainder = numerator % denominator;
    if (remainder < 0) {
        --quotient;
        remainder += denominator;
    }
    return true;
}
inline bool make_edge(const RasterVertex& top, const RasterVertex& bottom, RasterEdge& edge) {
    edge = {};
    edge.y = raster_ceil_fixed(top.y);
    edge.height = raster_ceil_fixed(bottom.y) - edge.y;
    edge.x = raster_ceil_fixed(top.x);
    const auto end_x = raster_ceil_fixed(bottom.x);
    if (edge.height == 0 && end_x == edge.x) {
        edge.x_step = 1;
        edge.value = top.attribute;
        return true;
    }
    const std::int64_t dy = static_cast<std::int64_t>(bottom.y) - top.y;
    const std::int64_t dx = static_cast<std::int64_t>(bottom.x) - top.x;
    if (dy != 0) {
        const std::int64_t denominator = 16 * dy;
        const std::int64_t intersection = static_cast<std::int64_t>(top.x) * dy +
            dx * (16 * static_cast<std::int64_t>(edge.y) - top.y);
        std::int64_t quotient = 0;
        std::int64_t fraction = 0;
        if (!floor_div_mod(intersection, denominator, quotient, edge.error) ||
            !floor_div_mod(dx, dy, edge.x_step, fraction)) return false;
        edge.denominator = denominator;
        edge.numerator = 16 * fraction;
        edge.x = quotient + (edge.error != 0);
    } else {
        edge.x_step = end_x - edge.x;
    }
    const float y_prestep = static_cast<float>(static_cast<std::int64_t>(edge.y) * 16 - top.y) / 16.0F;
    const float per_line = 1.0F / (static_cast<float>(dy != 0 ? dy : 1) / 16.0F);
    for (std::size_t i = 0; i < edge.value.size(); ++i) {
        const float slope = per_line * (bottom.attribute[i] - top.attribute[i]);
        edge.value[i] = top.attribute[i] + y_prestep * slope;
        edge.step[i] = slope;
    }
    return true;
}

inline void step_edge(RasterEdge& edge) {
    const bool was_fractional = edge.error != 0;
    edge.error += edge.numerator;
    const bool carry = edge.error >= edge.denominator;
    if (carry) edge.error -= edge.denominator;
    edge.x += edge.x_step + static_cast<int>(carry) +
              static_cast<int>(edge.error != 0) - static_cast<int>(was_fractional);
    ++edge.y;
    --edge.height;
    for (std::size_t i = 0; i < edge.value.size(); ++i) edge.value[i] += edge.step[i];
}

} // namespace raster_impl

inline void advance_raster_values(std::array<float, 7>& value,
                                  const std::array<float, 7>& delta) {
    for (std::size_t i = 0; i < value.size(); ++i) value[i] += delta[i];
}

template<class RowSink>
void traverse_raster_polygon(const std::array<RasterVertex, 10>& verts, std::size_t count,
                             int y_begin, int y_end, RowSink&& sink) {
    std::size_t left_vertex = count;
    std::size_t right_vertex = 0;
    raster_impl::RasterEdge left;
    raster_impl::RasterEdge right;
    if (!raster_impl::make_edge(verts[0], verts[count - 1U], left) ||
        !raster_impl::make_edge(verts[0], verts[1], right)) return;
    for (std::size_t edges = 0; edges < 2U * count; ++edges) {
        int remaining = std::min(left.height, right.height);
        while (remaining-- > 0) {
            // Traverse outside the band too: sequential additions determine rounding.
            if (left.y >= y_begin && left.y < y_end) sink(left, right);
            raster_impl::step_edge(left);
            raster_impl::step_edge(right);
        }
        const bool finished_left = left.height == 0;
        const bool finished_right = right.height == 0;
        if (finished_left) --left_vertex;
        if (finished_right) ++right_vertex;
        if (left_vertex <= right_vertex + 1U) break;
        if (finished_left && !raster_impl::make_edge(verts[left_vertex], verts[left_vertex - 1U], left)) return;
        if (finished_right && !raster_impl::make_edge(verts[right_vertex], verts[right_vertex + 1U], right)) return;
    }
}

inline bool prepare_raster_span(const RasterBoundary& left, const RasterBoundary& right,
                         int target_width, RasterSpan& span) {
    int x = static_cast<int>(left.x);
    int width = static_cast<int>(right.x) - x;
    if (width <= 0) return false;
    span.value = left.value;
    const float inverse_width = 1.0F / static_cast<float>(width);
    for (std::size_t i = 0; i < span.delta.size(); ++i)
        span.delta[i] = (right.value[i] - left.value[i]) * inverse_width;
    if (x < 0) {
        const float skipped = static_cast<float>(-x);
        for (std::size_t i = 0; i < span.value.size(); ++i) span.value[i] += span.delta[i] * skipped;
        width += x;
        x = 0;
    }
    if (x + width > target_width) width = target_width - x;
    if (width <= 0) return false;
    span.x = x;
    span.width = width;
    return true;
}

template<class FragmentSink>
void traverse_wire_triangle(const RasterScreenVertex& a, const RasterScreenVertex& b,
                            const RasterScreenVertex& c, std::uint32_t attributes,
                            bool w_depth, int width, int y_begin, int y_end,
                            FragmentSink&& sink) {
    const auto edge = [](const RasterScreenVertex& p, const RasterScreenVertex& q, float x, float y) {
        return (q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x);
    };
    const auto area = edge(a, b, c.x, c.y);
    if (std::abs(area) < 1.0e-12F) return;
    const bool front = area < 0.0F;
    const bool surface_enabled = front ? (attributes & (1U << 7)) != 0U
                                       : (attributes & (1U << 6)) != 0U;
    if (!surface_enabled) return;
    const auto min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const auto max_x = std::min(width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const auto min_y = std::max(y_begin, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const auto max_y = std::min(y_end - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    if (max_x < min_x || max_y < min_y) return;
    const auto alpha_attr = static_cast<std::uint8_t>((attributes >> 16) & 31U);
    const bool wireframe = alpha_attr == 0U;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const auto e0 = edge(b, c, px, py);
            const auto e1 = edge(c, a, px, py);
            const auto e2 = edge(a, b, px, py);
            float w0 = e0 / area;
            float w1 = e1 / area;
            float w2 = e2 / area;
            if (w0 < -1.0e-6F || w1 < -1.0e-6F || w2 < -1.0e-6F) continue;
            const auto edge_weight = std::min({w0, w1, w2});
            if (wireframe && edge_weight > 0.02F) continue;
            const auto inv_w = w0 * a.inv_w + w1 * b.inv_w + w2 * c.inv_w;
            if (std::abs(inv_w) < 1.0e-20F) continue;
            const auto inv = 1.0F / inv_w;
            RasterFragment fragment;
            fragment.x = x;
            fragment.y = y;
            fragment.front = front;
            fragment.u = (w0 * a.u_over_w + w1 * b.u_over_w + w2 * c.u_over_w) * inv;
            fragment.v = (w0 * a.v_over_w + w1 * b.v_over_w + w2 * c.v_over_w) * inv;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                fragment.color[channel] = (w0 * a.color_over_w[channel] +
                    w1 * b.color_over_w[channel] + w2 * c.color_over_w[channel]) * inv;
            }
            const auto ndc_z = w0 * a.z + w1 * b.z + w2 * c.z;
            const auto z_depth = static_cast<std::uint32_t>(std::floor(
                std::clamp((ndc_z + 1.0F) * 0.5F, 0.0F, 1.0F) * 4194303.0F)) << 2;
            fragment.depth = w_depth
                ? static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(std::floor(inv * 4096.0F)), 0, 0x00ffffff))
                : z_depth;
            sink(fragment);
        }
    }
}

} // namespace dsr::gpu3d_detail
