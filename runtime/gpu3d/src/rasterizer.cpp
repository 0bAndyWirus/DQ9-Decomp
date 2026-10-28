#include "rasterizer_impl.h"

#include <algorithm>
#include <cmath>

namespace dsr::gpu3d_detail {
namespace {


RasterVertex to_raster_vertex(const RasterInputVertex& vertex, std::uint32_t viewport, int scale) {
    const float w = vertex.clip[3] != 0.0F ? vertex.clip[3] : 0.00000001F;
    const float x1 = static_cast<float>(viewport & 0xffU);
    const float y1 = static_cast<float>((viewport >> 8) & 0xffU);
    const float width = static_cast<float>(static_cast<std::uint8_t>(
        ((viewport >> 16) & 0xffU) - (viewport & 0xffU))) + 1.0F;
    const float height = static_cast<float>(static_cast<std::uint8_t>(
        ((viewport >> 24) & 0xffU) - ((viewport >> 8) & 0xffU))) + 1.0F;
    const float x01 = (vertex.clip[0] + w) / (2.0F * w);
    const float y01 = (vertex.clip[1] + w) / (2.0F * w);
    const float z01 = (vertex.clip[2] + w) / (2.0F * w);
    const float screen_x = x01 * width + x1;
    const float screen_y = 192.0F - (y01 * height + (y1 > 191.0F ? y1 - 255.0F : y1));
    constexpr float kLimit = 1.0e6F;
    RasterVertex result;
    result.x = static_cast<std::int32_t>(std::clamp(16.0F * (screen_x * scale), -kLimit, kLimit));
    result.y = static_cast<std::int32_t>(std::clamp(16.0F * (screen_y * scale), -kLimit, kLimit));
    result.attribute = {1.0F / w, z01, vertex.u / w, vertex.v / w,
                        vertex.color[0] / w, vertex.color[1] / w, vertex.color[2] / w};
    return result;
}


} // namespace

float raster_plane_distance(const RasterInputVertex& vertex, int plane) {
    const auto x = vertex.clip[0], y = vertex.clip[1], z = vertex.clip[2], w = vertex.clip[3];
    switch (plane) {
    case 0: return x + w;
    case 1: return w - x;
    case 2: return y + w;
    case 3: return w - y;
    case 4: return z + w;
    default: return w - z;
    }
}

RasterInputVertex interpolate_raster_clip(const RasterInputVertex& a, const RasterInputVertex& b, float t) {
    RasterInputVertex result{};
    for (std::size_t i = 0; i < 4; ++i) result.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
    result.u = a.u + (b.u - a.u) * t;
    result.v = a.v + (b.v - a.v) * t;
    for (std::size_t i = 0; i < 3; ++i) result.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
    return result;
}

std::size_t clip_raster_polygon(std::array<RasterInputVertex, 16>& input, std::size_t count,
                                std::uint32_t attributes, std::array<RasterInputVertex, 16>& output) {
    if ((attributes & (1U << 12)) == 0U) {
        bool any_beyond_far = false;
        for (std::size_t i = 0; i < count; ++i)
            if (raster_plane_distance(input[i], 5) < 0.0F) any_beyond_far = true;
        if (any_beyond_far) return 0;
    }
    const int last_plane = (attributes & (1U << 12)) != 0U ? 6 : 5;
    for (int plane = 0; plane < last_plane; ++plane) {
        std::array<RasterInputVertex, 16> next{};
        std::size_t next_count = 0;
        if (count == 0) return 0;
        auto previous = input[count - 1U];
        bool previous_inside = raster_plane_distance(previous, plane) >= 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            const auto current = input[i];
            const bool current_inside = raster_plane_distance(current, plane) >= 0.0F;
            if (current_inside != previous_inside && next_count < next.size()) {
                const auto& inside = previous_inside ? previous : current;
                const auto& outside = previous_inside ? current : previous;
                const auto axis = static_cast<std::size_t>(plane / 2);
                const bool positive = (plane & 1) != 0;
                const float inside_w = positive ? inside.clip[3] : -inside.clip[3];
                const float outside_w = positive ? outside.clip[3] : -outside.clip[3];
                const float divisor = (outside_w - inside_w) -
                                      (outside.clip[axis] - inside.clip[axis]);
                if (std::abs(divisor) > 1.0e-20F) {
                    const float ratio = (inside.clip[axis] - inside_w) / divisor;
                    auto vertex = interpolate_raster_clip(inside, outside, ratio);
                    vertex.clip[axis] = positive ? vertex.clip[3] : -vertex.clip[3];
                    next[next_count++] = vertex;
                }
            }
            if (current_inside && next_count < next.size()) next[next_count++] = current;
            previous = current;
            previous_inside = current_inside;
        }
        input = next;
        count = next_count;
    }
    output = input;
    return count;
}

RasterScreenVertex project_raster_screen(const std::array<float, 4>& clip, float u, float v,
                                         const std::array<float, 3>& color,
                                         std::uint32_t viewport, int scale) {
    RasterScreenVertex result{};
    const auto w = clip[3];
    if (std::abs(w) < 1.0e-20F) return result;
    const auto inv_w = 1.0F / w;
    const float x1 = static_cast<float>(viewport & 0xffU);
    const float y1 = static_cast<float>((viewport >> 8) & 0xffU);
    const float width = static_cast<float>(static_cast<std::uint8_t>(((viewport >> 16) & 0xffU) - (viewport & 0xffU))) + 1.0F;
    const float height = static_cast<float>(static_cast<std::uint8_t>(((viewport >> 24) & 0xffU) - ((viewport >> 8) & 0xffU))) + 1.0F;
    const auto ndc_x = clip[0] * inv_w;
    const auto ndc_y = clip[1] * inv_w;
    const float y_base = y1 > 191.0F ? y1 - 255.0F : y1;
    result.x = x1 + (ndc_x + 1.0F) * width * 0.5F;
    result.y = 192.0F - (y_base + (ndc_y + 1.0F) * height * 0.5F);
    result.x *= static_cast<float>(scale);
    result.y *= static_cast<float>(scale);
    result.z = clip[2] * inv_w;
    result.inv_w = inv_w;
    result.u_over_w = u * inv_w;
    result.v_over_w = v * inv_w;
    for (std::size_t i = 0; i < 3; ++i) result.color_over_w[i] = color[i] * inv_w;
    return result;
}

int raster_ceil_fixed(std::int64_t value) {
    std::int64_t quotient = 0;
    std::int64_t remainder = 0;
    raster_impl::floor_div_mod(value + 15, 16, quotient, remainder);
    return static_cast<int>(quotient);
}

bool prepare_raster_polygon(const RasterInputVertex* vertices, std::size_t count,
                            std::uint32_t viewport, std::uint32_t attributes, int scale,
                            std::array<RasterVertex, 10>& verts, bool& front_facing) {
    if (count < 3U || count > verts.size()) return false;
    for (std::size_t i = 0; i < count; ++i) verts[i] = to_raster_vertex(vertices[i], viewport, scale);
    const auto begin = verts.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(count);
    const std::size_t last = count - 1U;
    double facing = static_cast<double>(verts[0].y + verts[last].y) * (verts[0].x - verts[last].x) +
                    static_cast<double>(verts[1].y + verts[0].y) * (verts[1].x - verts[0].x) +
                    static_cast<double>(verts[2].y + verts[1].y) * (verts[2].x - verts[1].x);
    for (std::size_t j = 2; j < last; ++j)
        facing += static_cast<double>(verts[j + 1U].y + verts[j].y) * (verts[j + 1U].x - verts[j].x);
    const bool back_facing = facing < 0.0;
    if (((attributes & (1U << (back_facing ? 6 : 7)))) == 0U) return false;
    front_facing = !back_facing;
    if (!back_facing) std::reverse(begin, end);
    for (std::size_t guard = 0; guard < count; ++guard) {
        bool above = false;
        for (std::size_t i = 1; i < count; ++i) above = above || verts[0].y > verts[i].y;
        if (!above) break;
        std::rotate(begin, begin + 1, end);
    }
    for (std::size_t guard = 0; guard < count &&
         verts[0].y == verts[1].y && verts[0].x > verts[1].x; ++guard)
        std::rotate(begin, begin + 1, end);
    return true;
}


bool prepare_raster_dot(const RasterScreenVertex& screen, const RasterInputVertex& vertex,
                        bool w_depth, int width, int height, int y_begin, int y_end,
                        RasterFragment& fragment) {
    const auto x = static_cast<int>(std::floor(screen.x));
    const auto y = static_cast<int>(std::floor(screen.y));
    if (x < 0 || x >= width || y < 0 || y >= height || y < y_begin || y >= y_end) return false;
    fragment.x = x;
    fragment.y = y;
    fragment.color = vertex.color;
    fragment.u = vertex.u;
    fragment.v = vertex.v;
    fragment.depth = w_depth
        ? static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(std::floor(vertex.clip[3] * 4096.0F)), 0, 0x00ffffff))
        : static_cast<std::uint32_t>(std::floor(
            std::clamp((screen.z + 1.0F) * 0.5F, 0.0F, 1.0F) * 4194303.0F)) << 2;
    return true;
}

} // namespace dsr::gpu3d_detail
