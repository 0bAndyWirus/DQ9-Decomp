#pragma once

#include "gpu_scene_frame.h"
#include "gpu3d_test.h"
#include <godot_cpp/variant/dictionary.hpp>

namespace dq9 {

// Synthetic geometry, palette and texture probes, authored independently of
// any image. They use the real GX pipeline/decoder/rasterizer rather than a
// second barycentric approximation of the software renderer.
inline std::unique_ptr<GpuSceneFrame> make_quality_probe(const godot::Dictionary& options,
                                                        std::uint64_t generation) {
    struct Source {
        std::array<std::array<std::uint8_t, 0x20000>, 2> texture{};
        std::array<std::uint8_t, 0x4000> palette{};
        static const std::uint8_t* tex(const void* p, int slot) {
            const auto& s = *static_cast<const Source*>(p);
            return slot >= 0 && slot < 2 ? s.texture[slot].data() : nullptr;
        }
        static const std::uint8_t* pal(const void* p, int slot) {
            return slot == 0 ? static_cast<const Source*>(p)->palette.data() : nullptr;
        }
    };
    auto source = std::make_unique<Source>();
    const auto integer = [&](const char* name, int value) { return static_cast<int>(options.get(name, value)); };
    const auto flag = [&](const char* name) { return static_cast<bool>(options.get(name, false)); };
    const bool cutout_layers = flag("cutout_layers");
    const unsigned format = cutout_layers ? 7U : integer("format", 0);
    const unsigned mode = integer("mode", 0);
    const unsigned alpha = integer("alpha", 31);
    const unsigned wrap = cutout_layers ? 1U : integer("wrap", 0);
    const std::uint32_t texture = (format << 26U) | (1U << 20U) | (wrap << 16U);
    for (unsigned i = 0; i < 32; ++i) {
        const auto color = static_cast<std::uint16_t>((i * 5U % 32U) | ((i * 11U % 32U) << 5U) |
                                                     ((31U - i) << 10U));
        source->palette[i * 2U] = static_cast<std::uint8_t>(color);
        source->palette[i * 2U + 1U] = static_cast<std::uint8_t>(color >> 8U);
    }
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            const unsigned offset = y * 16U + x;
            const unsigned index = x + y * 3U;
            auto& bytes = source->texture[0];
            switch (format) {
            case 1: bytes[offset] = static_cast<std::uint8_t>((index % 32U) |
                ((flag("partial_texture") && (x & 1U) == 0 ? 3U : 7U) << 5U)); break;
            case 2: bytes[offset / 4U] |= static_cast<std::uint8_t>((index % 4U) << ((offset % 4U) * 2U)); break;
            case 3: bytes[offset / 2U] |= static_cast<std::uint8_t>((index % 16U) << ((offset % 2U) * 4U)); break;
            case 4: bytes[offset] = static_cast<std::uint8_t>(index % 32U); break;
            case 5: {
                const unsigned tile = (y / 4U) * 4U + x / 4U;
                bytes[tile * 4U + y % 4U] |= static_cast<std::uint8_t>((index % 4U) << ((x % 4U) * 2U));
                const auto metadata = static_cast<std::uint16_t>((tile % 4U) << 14U);
                source->texture[1][tile * 2U] = static_cast<std::uint8_t>(metadata);
                source->texture[1][tile * 2U + 1U] = static_cast<std::uint8_t>(metadata >> 8U);
                break;
            }
            case 6: bytes[offset] = static_cast<std::uint8_t>((index % 8U) | (31U << 3U)); break;
            case 7: {
                const auto color = cutout_layers
                    ? static_cast<std::uint16_t>((x & 1U) ? 0x0000U : 0x83e0U)
                    : static_cast<std::uint16_t>(0x8000U | x * 2U | ((y * 4U) << 5U) |
                                                 ((31U - x) << 10U));
                bytes[offset * 2U] = static_cast<std::uint8_t>(color);
                bytes[offset * 2U + 1U] = static_cast<std::uint8_t>(color >> 8U);
                break;
            }
            default: break;
            }
        }
    }
    dsr::Gpu3D gpu;
    gpu.set_capture_opaque_only(true);
    const dsr::gpu3d_detail::TextureSource texture_source{source.get(), &Source::tex, &Source::pal};
    dsr::gpu3d_detail::Gpu3DTestAccess::attach_texture_source(gpu, texture_source);
    const auto send = [&](unsigned code, std::uint32_t value) {
        gpu.io_write(0x04000440U + (code - 0x10U) * 4U, value, 32);
        gpu.run(10000);
    };
    gpu.io_write(0x04000060U, 1U | (flag("highlight") ? 2U : 0U) |
                 ((flag("blend") || cutout_layers) ? 8U : 0U) |
                 (flag("edges") ? 32U : 0U) | (flag("antialias") ? 16U : 0U) |
                 (flag("fog") ? 128U : 0U) | (flag("alpha_only") ? 64U : 0U) |
                 (integer("alpha_ref", 0) != 0 ? 4U : 0U), 32);
    gpu.io_write(0x04000354U, static_cast<std::uint32_t>(integer("clear_depth", 0x7fff)), 16);
    gpu.io_write(0x04000340U, static_cast<std::uint32_t>(integer("alpha_ref", 0)), 32);
    gpu.io_write(0x04000350U, 8U | (12U << 5U) | (20U << 10U) |
                 (static_cast<std::uint32_t>(integer("clear_alpha", 0)) << 16U) |
                 (flag("clear_fog") ? 32768U : 0U), 32);
    for (unsigned i = 0; i < 32; ++i)
        gpu.io_write(0x04000380U + i * 2U, (31U - i) | ((i / 2U) << 5U) | (i << 10U), 16);
    gpu.io_write(0x04000358U, 20U | (24U << 5U) | (31U << 10U) |
                 (static_cast<std::uint32_t>(integer("fog_alpha", 31)) << 16U), 32);
    for (unsigned i = 0; i < 32; ++i)
        gpu.io_write(0x04000360U + i, static_cast<std::uint32_t>(integer("fog_density", 127)), 8);
    for (unsigned i = 0; i < 8; ++i)
        gpu.io_write(0x04000330U + i * 2U, 4U | (12U << 5U) | (20U << 10U), 16);
    if (flag("perspective")) {
        send(0x10, 0); // Projection W = 1 + Z/2; all vertices remain inside clip.
        for (unsigned i = 0; i < 16; ++i)
            send(0x16, i == 11 ? 2048U : i % 5U == 0 ? 4096U : 0U);
    }
    if (cutout_layers) {
        // The cutout spans 64 native pixels but 128 texels: each native
        // sample sees an opaque texel while the 3x render exposes holes.
        const auto quad = [&](int left, int right, int z, unsigned color,
                              unsigned polygon_alpha, unsigned id, bool textured) {
            send(0x2a, textured ? texture : 0U);
            send(0x29, (polygon_alpha << 16U) | (id << 24U) | 0xc0U);
            send(0x20, color);
            send(0x40, 1U);
            for (const auto [x, y, u, v] : std::array<std::array<int, 4>, 4>{{
                     {left, -1024, 0, 0}, {right, -1024, 128, 0},
                     {right, 1024, 128, 8}, {left, 1024, 0, 8}}}) {
                send(0x22, static_cast<std::uint16_t>(u * 16) |
                     (static_cast<std::uint32_t>(static_cast<std::uint16_t>(v * 16)) << 16U));
                send(0x23, static_cast<std::uint16_t>(x) |
                     (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U));
                send(0x23, static_cast<std::uint16_t>(z));
            }
            send(0x41, 0);
        };
        quad(-2048, 2048, 2048, 0x0000, 31, 1, false);
        if (!flag("cutout_no_foreground"))
            quad(-1024, 1024, -1024, 0x7fff, 31, 2, true);
        // The left-hand opaque region supplies mask-covered control pixels.
        quad(-2048, -1280, -1024, 0x03e0, 31, 3, false);
        quad(-1024, 1024, 1024, 0x001f, 15, 4, false);
        quad(-1024, 1024, 0, 0x7c00, 15, 5, false);
    } else {
    send(0x2a, texture);
    send(0x29, (alpha << 16U) | (mode << 4U) | (8U << 24U) | 0xc0U |
         (flag("polygon_fog") ? 32768U : 0U));
    send(0x40, flag("quad") ? 1 : 0);
    const std::array<std::array<int, 3>, 5> positions{{{-2816, -2304, -1024},
        {2944, -1664, 1024}, {2176, 2688, 1024}, {-2048, 2176, -1024}, {-137, 277, 0}}};
    const std::array<std::uint16_t, 5> colors{{0x3e88, 0x2d53, 0x65c9, 0x56c3, 0x7fff}};
    constexpr std::array<unsigned, 12> fan{{0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4}};
    const unsigned count = flag("shared_edges") ? fan.size() : flag("quad") ? 4U : 3U;
    for (unsigned emitted = 0; emitted < count; ++emitted) {
        const auto i = flag("shared_edges") ? fan[emitted] : emitted;
        send(0x20, flag("varying_color") ? colors[i] : static_cast<unsigned>(integer("color", 0x3e88)));
        const int u = (i == 1 || i == 2) ? (wrap ? 36 : 16) : (wrap ? -12 : 0);
        const int v = i >= 2 ? (wrap ? 20 : 8) : (wrap ? -6 : 0);
        send(0x22, static_cast<std::uint16_t>(u * 16) |
             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(v * 16)) << 16U));
        const int x = flag("wide_quad") ? positions[i][0] * 4 / 3 : positions[i][0];
        send(0x23, static_cast<std::uint16_t>(x) |
             (static_cast<std::uint32_t>(static_cast<std::uint16_t>(positions[i][1])) << 16U));
        send(0x23, flag("perspective") ? static_cast<std::uint16_t>(positions[i][2]) : 0U);
    }
    }
    send(0x41, 0); send(0x50, 0);
    gpu.vblank_start(); gpu.render_start();
    const auto geometry = gpu.geometry_view();
    const auto raster = gpu.raster_view();
    auto frame = std::make_unique<GpuSceneFrame>();
    frame->epoch = 1; frame->number = 2; frame->source_frame = 1;
    frame->vram_generation = generation;
    frame->vertex_count = geometry.vertex_count;
    frame->polygon_count = frame->captured_polygon_count = geometry.polygon_count;
    frame->opaque_count = gpu.draw_order_view().opaque_count;
    std::copy_n(geometry.vertices, frame->vertex_count, frame->vertices.begin());
    std::copy_n(geometry.polygons, frame->polygon_count, frame->polygons.begin());
    std::copy_n(raster.output, frame->software_output.size(), frame->software_output.begin());
    std::copy_n(raster.capture_polygon_ids, frame->software_polygon_ids.size(), frame->software_polygon_ids.begin());
    std::copy_n(raster.opaque_only_mask, frame->opaque_only.size(), frame->opaque_only.begin());
    for (std::size_t i = 0; i < frame->mask.size(); ++i) {
        const auto selected = static_cast<std::uint8_t>(((raster.output[i] >> 24U) & 31U) == 31U);
        frame->mask[i] = selected & frame->opaque_only[i];
        frame->three_d_pixels += selected;
        frame->gpu_pixels += frame->mask[i];
    }
    frame->clear_color = raster.clear_color; frame->clear_depth = raster.clear_depth;
    frame->disp3dcnt = raster.disp3d; frame->alpha_test_ref = raster.alpha_reference;
    frame->fog_color = raster.fog_color; frame->fog_offset = raster.fog_offset;
    std::copy_n(raster.fog_density, 32, frame->fog_table.begin());
    for (unsigned i = 0; i < 8; ++i)
        frame->edge_color_table[i] = raster.edge_table[i * 2U] | (raster.edge_table[i * 2U + 1U] << 8U);
    for (unsigned i = 0; i < 32; ++i)
        frame->toon_table[i] = raster.toon_table[i * 2U] | (raster.toon_table[i * 2U + 1U] << 8U);
    if (format != 0) {
        auto decoded = std::make_shared<DecodedTexture>();
        decoded->key = {texture, 0, generation};
        dsr::gpu3d_detail::decode_texture_rgba8(texture_source, texture, 0,
                                              decoded->rgba, decoded->width, decoded->height);
        decoded->classify_alpha();
        frame->new_textures.push_back(std::move(decoded));
    }
    frame->color_valid = frame->gpu_valid = true; frame->fallback = GpuFallback::None;
    return frame;
}

} // namespace dq9
