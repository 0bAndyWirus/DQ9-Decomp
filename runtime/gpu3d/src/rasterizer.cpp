#include "rasterizer.h"

#include <algorithm>
#include <cmath>

namespace dsr::gpu3d_detail {

float raster_plane_distance(const RasterInputVertex& vertex, int plane) {
    const float coordinate = vertex.clip[static_cast<std::size_t>(plane / 2)];
    return (plane & 1) ? vertex.clip[3] - coordinate : coordinate + vertex.clip[3];
}

RasterInputVertex interpolate_raster_clip(const RasterInputVertex& a,
                                         const RasterInputVertex& b, float t) {
    RasterInputVertex result;
    for (std::size_t i = 0; i < result.clip.size(); ++i)
        result.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
    result.u = a.u + (b.u - a.u) * t;
    result.v = a.v + (b.v - a.v) * t;
    for (std::size_t i = 0; i < result.color.size(); ++i)
        result.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
    return result;
}

std::size_t clip_raster_polygon(std::array<RasterInputVertex, 16>& input, std::size_t count,
                                std::uint32_t attributes, std::array<RasterInputVertex, 16>& output) {
    if (count == 0) return 0;
    // GPU3D_RASTER_SPEC, Geometry: bit 12 controls far-plane intersection.
    if ((attributes & (1U << 12)) == 0) {
        for (std::size_t i = 0; i < count; ++i)
            if (raster_plane_distance(input[i], 5) < 0.0F) return 0;
    }
    for (int plane = 0; plane < 6; ++plane) {
        std::size_t out_count = 0;
        auto previous = input[count - 1];
        float previous_distance = raster_plane_distance(previous, plane);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& current = input[i];
            const float current_distance = raster_plane_distance(current, plane);
            const bool previous_inside = previous_distance >= 0.0F;
            const bool current_inside = current_distance >= 0.0F;
            if (previous_inside != current_inside) {
                const auto& inside = previous_inside ? previous : current;
                const auto& outside = previous_inside ? current : previous;
                const float inside_distance = previous_inside ? previous_distance : current_distance;
                const auto axis = static_cast<std::size_t>(plane / 2);
                const float dw = inside.clip[3] - outside.clip[3];
                const float dc = inside.clip[axis] - outside.clip[axis];
                const float divisor = (plane & 1) ? dw - dc : dw + dc;
                // Evaluate from inside and force the active plane exactly, per spec §Geometry.
                if (std::abs(divisor) > 1e-20F && out_count < output.size()) {
                    auto intersection = interpolate_raster_clip(inside, outside, inside_distance / divisor);
                    intersection.clip[axis] = (plane & 1) ? intersection.clip[3] : -intersection.clip[3];
                    output[out_count++] = intersection;
                }
            }
            if (current_inside && out_count < output.size()) output[out_count++] = current;
            previous = current;
            previous_distance = current_distance;
        }
        count = out_count;
        if (count == 0) return 0;
        std::copy_n(output.begin(), count, input.begin());
    }
    return count;
}

namespace {
RasterScreenVertex project(const std::array<float, 4>& clip, float u, float v,
                           const std::array<float, 3>& color, std::uint32_t viewport,
                           int scale, float w, bool filled) {
    const unsigned x1 = viewport & 255U;
    const unsigned y1 = (viewport >> 8) & 255U;
    const unsigned x2 = (viewport >> 16) & 255U;
    const unsigned y2 = (viewport >> 24) & 255U;
    const int width = static_cast<int>((x2 - x1) & 255U) + 1;
    const int height = static_cast<int>((y2 - y1) & 255U) + 1;
    const int bottom = y1 <= 191U ? static_cast<int>(y1) : static_cast<int>(y1) - 255;
    RasterScreenVertex result;
    result.inv_w = 1.0F / w;
    const float nx = filled ? (clip[0] + w) / w : clip[0] * result.inv_w + 1.0F;
    const float ny = filled ? (clip[1] + w) / w : clip[1] * result.inv_w + 1.0F;
    // Inclusive, wrapping viewport and inverted Y: GPU3D_RASTER_INTERFACE §Coordinates.
    result.x = (static_cast<float>(x1) + nx * 0.5F * static_cast<float>(width)) * static_cast<float>(scale);
    result.y = (192.0F - (static_cast<float>(bottom) + ny * 0.5F * static_cast<float>(height))) * static_cast<float>(scale);
    result.z = clip[2] * result.inv_w;
    // Screen projection weights by the rounded reciprocal; filled projection divides.
    result.u_over_w = filled ? u / w : u * result.inv_w;
    result.v_over_w = filled ? v / w : v * result.inv_w;
    for (std::size_t i = 0; i < result.color_over_w.size(); ++i)
        result.color_over_w[i] = filled ? color[i] / w : color[i] * result.inv_w;
    return result;
}
}

RasterScreenVertex project_raster_screen(const std::array<float, 4>& clip, float u, float v,
                                         const std::array<float, 3>& color,
                                         std::uint32_t viewport, int scale) {
    if (std::abs(clip[3]) < 1e-20F) return {};
    return project(clip, u, v, color, viewport, scale, clip[3], false);
}

int raster_ceil_fixed(std::int64_t value) {
    const std::int64_t quotient = value / 16;
    return static_cast<int>(quotient + (value % 16 > 0 ? 1 : 0));
}

bool prepare_raster_polygon(const RasterInputVertex* vertices, std::size_t count,
                            std::uint32_t viewport, std::uint32_t attributes, int scale,
                            std::array<RasterVertex, 10>& result, bool& front_facing) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto& vertex = vertices[i];
        const float w = vertex.clip[3] == 0.0F ? 1e-8F : vertex.clip[3];
        const auto projected = project(vertex.clip, vertex.u, vertex.v, vertex.color, viewport, scale, w, true);
        auto& out = result[i];
        out.x = static_cast<std::int32_t>(std::clamp(projected.x * 16.0F, -1000000.0F, 1000000.0F));
        out.y = static_cast<std::int32_t>(std::clamp(projected.y * 16.0F, -1000000.0F, 1000000.0F));
        out.attribute = {projected.inv_w, ((vertex.clip[2] + w) / w) * 0.5F,
                         projected.u_over_w, projected.v_over_w,
                         projected.color_over_w[0], projected.color_over_w[1], projected.color_over_w[2]};
    }
    std::int64_t area = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = result[i];
        const auto& b = result[(i + 1) % count];
        area += static_cast<std::int64_t>(a.x) * b.y - static_cast<std::int64_t>(a.y) * b.x;
    }
    // Counterclockwise in the upward viewport is front-facing (spec §Geometry).
    front_facing = area <= 0;
    const bool accepted = area == 0 ? (attributes & 0xc0U) != 0 :
        (attributes & (front_facing ? 0x80U : 0x40U)) != 0;
    if (!accepted) return false;
    // Traversal consumes clockwise chains beginning at the first topmost vertex.
    auto end = result.begin() + count;
    auto top = std::min_element(result.begin(), end,
                                [](const RasterVertex& a, const RasterVertex& b) {
                                    return a.y < b.y;
                                });
    std::rotate(result.begin(), top, end);
    if (front_facing) std::reverse(result.begin() + 1, end);
    return true;
}

bool prepare_raster_dot(const RasterScreenVertex& screen, const RasterInputVertex& vertex,
                        bool w_depth, int width, int height, int y_begin, int y_end,
                        RasterFragment& fragment) {
    const int x = static_cast<int>(std::floor(screen.x));
    const int y = static_cast<int>(std::floor(screen.y));
    if (x < 0 || x >= width || y < 0 || y >= height || y < y_begin || y >= y_end) return false;
    fragment.x = x;
    fragment.y = y;
    fragment.u = vertex.u;
    fragment.v = vertex.v;
    fragment.color = vertex.color;
    // Depth formulas are specified by GPU3D_RASTER_INTERFACE §Operations.
    if (w_depth) {
        const float depth = std::clamp(std::floor(vertex.clip[3] * 4096.0F), 0.0F, 16777215.0F);
        fragment.depth = static_cast<std::uint32_t>(depth);
    } else {
        const float depth = std::floor(std::clamp((screen.z + 1.0F) * 0.5F, 0.0F, 1.0F) * 4194303.0F);
        fragment.depth = static_cast<std::uint32_t>(depth) << 2;
    }
    return true;
}

} // namespace dsr::gpu3d_detail
