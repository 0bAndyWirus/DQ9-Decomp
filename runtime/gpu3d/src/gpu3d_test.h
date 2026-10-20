#pragma once

#include "dsr/gpu3d.h"
#include "texture.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dsr::gpu3d_detail {

struct Gpu3DTestAccess {
    static void attach_texture_source(Gpu3D& gpu, const TextureSource& source) {
        gpu.set_texture_source(source.context, source.texture_lookup, source.palette_lookup,
                               source.texture_read, source.palette_read, source.generation);
    }

    static std::array<std::int32_t, 3> light_half_vector(Gpu3D& gpu, unsigned index);
    static std::array<std::int64_t, 2> transformed_texcoord(Gpu3D& gpu);
    static std::uint8_t rounded_color(float value, bool six_bit);
    static std::size_t clip_positions(Gpu3D& gpu,
        const std::array<std::array<float, 4>, 3>& positions,
        std::array<std::array<float, 4>, 16>& clipped);
    static std::array<Texel, 64> sample_texels(Gpu3D& gpu, const Gpu3DPolygon& polygon,
        const std::array<std::array<float, 2>, 64>& coordinates);
    static std::array<std::uint32_t, 256> draw_span(Gpu3D& gpu, const Gpu3DPolygon& polygon,
        int x_left, int x_right, const std::array<float, 7>& left,
        const std::array<float, 7>& right);
    static std::array<std::uint32_t, 4> clear_pixel(Gpu3D& gpu, unsigned index);
    // Read named fields only (not FramePixel padding), after the render barrier.
    static std::array<std::uint32_t, 4> framebuffer_pixel(const Gpu3D& gpu, unsigned index);
    static std::array<std::uint8_t, 4> shade_pixel(Gpu3D& gpu, const Gpu3DPolygon& polygon,
        const std::array<float, 3>& color, float u, float v);
    using SpanPixel = std::array<std::uint32_t, 4>;
    using SpanState = std::array<SpanPixel, 256>;
    static SpanState draw_span_state(Gpu3D& gpu, const Gpu3DPolygon& polygon,
        int x_left, int x_right, const std::array<float, 7>& left,
        const std::array<float, 7>& right, const SpanState& initial,
        bool front_facing, bool translucent_polygon, bool w_depth, bool batch);
};

} // namespace dsr::gpu3d_detail
