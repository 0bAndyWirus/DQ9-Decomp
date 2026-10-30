#pragma once

#include "dsr/render_frame.h"
#include "texture_rgba.h"

#include <array>
#include <memory>
#include <vector>

namespace dq9 {

inline constexpr std::size_t kGpuPolygonLimit = 12288;
inline constexpr std::size_t kGpuVertexLimit = 12288;
inline constexpr std::size_t kHardwarePolygonLimit = 2048;
inline constexpr std::size_t kHardwareVertexLimit = 6144;

inline bool enhanced_geometry_enabled(bool full, bool gpu_enabled, bool widescreen) {
    return full || (gpu_enabled && widescreen);
}

inline bool accepts_geometry(const dsr::Gpu3DGeometryView& geometry, bool enhanced) {
    // Hardware polygon overflow discards later polygons in classic mode.
    return !geometry.enhanced_overflow &&
           (enhanced || !geometry.vertex_overflow) &&
           (enhanced || !geometry.polygon_overflow || geometry.polygon_count == kHardwarePolygonLimit) &&
           geometry.vertex_count <= (enhanced ? kGpuVertexLimit : kHardwareVertexLimit) &&
           geometry.polygon_count <= (enhanced ? kGpuPolygonLimit : kHardwarePolygonLimit);
}
inline constexpr std::size_t kGpuNewTextureLimit = 256;

struct TextureKeyHash {
    std::size_t operator()(const dsr::gpu3d_detail::TextureKey& key) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(key.texture_param) << 32U) | key.palette_base;
        return std::hash<std::uint64_t>{}(packed) ^
               (std::hash<std::uint64_t>{}(key.vram_generation) << 1U);
    }
};

// The source and its view are borrowed only while the owner-thread render
// callback is executing. Decoded pixels and identity bytes must be copied out.
inline dsr::gpu3d_detail::TextureSource texture_source(const dsr::Gpu3DTextureView& view) {
    dsr::gpu3d_detail::TextureSource source;
    source.context = &view;
    source.texture_lookup = [](const void* context, int index) -> const std::uint8_t* {
        const auto& slots = static_cast<const dsr::Gpu3DTextureView*>(context)->textures;
        return index >= 0 && static_cast<std::size_t>(index) < slots.size() ? slots[index].bytes : nullptr;
    };
    source.palette_lookup = [](const void* context, int index) -> const std::uint8_t* {
        const auto& slots = static_cast<const dsr::Gpu3DTextureView*>(context)->palettes;
        return index >= 0 && static_cast<std::size_t>(index) < slots.size() ? slots[index].bytes : nullptr;
    };
    source.texture_read = [](const void* context, std::uint32_t address) -> std::uint8_t {
        const auto& input = *static_cast<const dsr::Gpu3DTextureView*>(context);
        return input.texture_read ? input.texture_read(input.read_context, address) : 0;
    };
    source.palette_read = [](const void* context, std::uint32_t address) -> std::uint8_t {
        const auto& input = *static_cast<const dsr::Gpu3DTextureView*>(context);
        return input.palette_read ? input.palette_read(input.read_context, address) : 0;
    };
    return source;
}

// Stable content identity input, independent of VRAM and palette addresses.
// Raw reads use the same wrapping/unmapped semantics as the decoder.
inline std::vector<std::uint8_t> texture_identity_data(
        const dsr::gpu3d_detail::TextureSource& source, std::uint32_t param,
        std::uint32_t palette) {
    std::vector<std::uint8_t> data{'D','Q','9','T','E','X','1',0};
    const auto append32 = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    const auto append16 = [&](std::uint16_t value) {
        data.push_back(static_cast<std::uint8_t>(value));
        data.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    append32(param & 0x3ff00000U);
    const auto format = (param >> 26U) & 7U;
    const std::uint32_t pixels = (8U << ((param >> 20U) & 7U)) *
                                 (8U << ((param >> 23U) & 7U));
    const auto address = (param & 0xffffU) * 8U;
    const auto palette_address = (palette & 0x1fffU) * (format == 2U ? 8U : 16U);
    const auto length = format == 7U ? pixels * 2U :
                        format == 2U || format == 5U ? pixels / 4U :
                        format == 3U ? pixels / 2U : pixels;
    data.reserve(12U + length + (format == 5U ? pixels / 8U + pixels / 2U : 512U));
    for (std::uint32_t i = 0; i < length; ++i) data.push_back(source.texture_byte(address + i));
    if (format == 5U) {
        const auto tiles = pixels / 16U;
        const auto metadata = 0x20000U + (address / 0x20000U == 2U ? 0x10000U : 0U) +
                              (address % 0x20000U) / 2U;
        // Palette offsets are addresses, not texture content. Preserve only the
        // decoding mode; the referenced palette values are serialized below.
        for (std::uint32_t i = 0; i < tiles; ++i)
            append16(source.texture_half(metadata + i * 2U) & 0xc000U);
        for (std::uint32_t i = 0; i < tiles; ++i) {
            const auto value = source.texture_half(metadata + i * 2U);
            const auto mode = value >> 14U;
            const auto count = mode == 0U ? 3U : mode == 2U ? 4U : 2U;
            const auto base = palette_address + (value & 0x3fffU) * 4U;
            for (unsigned j = 0; j < count; ++j) append16(source.palette_half(base + j * 2U));
        }
    } else {
        const auto colors = format == 1U ? 32U : format == 2U ? 4U : format == 3U ? 16U :
                            format == 4U ? 256U : format == 6U ? 8U : 0U;
        for (unsigned i = 0; i < colors; ++i) append16(source.palette_half(palette_address + i * 2U));
    }
    return data;
}

struct DecodedTexture {
    dsr::gpu3d_detail::TextureKey key{};
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    std::vector<std::uint8_t> identity_data;
#ifdef DQ9_GPU_SCENE_TESTS
    std::uint8_t max_partial_alpha = 0;
    void classify_alpha() {
        max_partial_alpha = 0;
        for (std::size_t i = 3; i < rgba.size(); i += 4) {
            if (rgba[i] != 0 && rgba[i] != 255 && (rgba[i] >> 3U) > max_partial_alpha)
                max_partial_alpha = rgba[i] >> 3U;
        }
    }
#endif
};

#ifdef DQ9_GPU_SCENE_TESTS
// DS format1/6 ordering is "translucent" even for binary-alpha textures. Keep
// that ordering/edge flag separate from whether visible fragments must blend.
inline bool requires_fragment_blending(const dsr::Gpu3DPolygon& polygon,
                                      std::uint32_t disp3dcnt, std::uint32_t alpha_ref,
                                      std::uint8_t texture_max_partial_alpha) {
    const auto alpha = (polygon.attr >> 16U) & 31U;
    if (alpha == 0 || ((disp3dcnt & 4U) != 0 && alpha < (alpha_ref & 31U))) return false;
    if (alpha != 31U) return true;
    return (disp3dcnt & 1U) != 0 && ((polygon.attr >> 4U) & 3U) != 1U &&
           texture_max_partial_alpha != 0 &&
           !((disp3dcnt & 4U) != 0 && texture_max_partial_alpha < (alpha_ref & 31U));
}
#endif

enum class GpuFallback { None, Disabled, Warmup, TextureBudget, BitmapClear, TransparentClear, Geometry };

inline const char* fallback_name(GpuFallback reason) {
    switch (reason) {
    case GpuFallback::None: return "none";
    case GpuFallback::Disabled: return "disabled";
    case GpuFallback::Warmup: return "warmup";
    case GpuFallback::TextureBudget: return "texture_budget";
    case GpuFallback::TransparentClear: return "transparent_clear";
    case GpuFallback::BitmapClear: return "bitmap_clear";
    case GpuFallback::Geometry: return "geometry";
    }
    return "geometry";
}

// Allocated lazily in each presentation slot only while GPU 3D is enabled.
// No borrowed Machine pointers and no Godot objects may cross the worker ring.
struct GpuSceneFrame {
    std::vector<dsr::Gpu3DVertex> vertices = std::vector<dsr::Gpu3DVertex>(kHardwareVertexLimit);
    std::vector<dsr::Gpu3DPolygon> polygons = std::vector<dsr::Gpu3DPolygon>(kHardwarePolygonLimit);

    void prepare_geometry(bool enhanced) {
        const auto vertex_limit = enhanced ? kGpuVertexLimit : kHardwareVertexLimit;
        const auto polygon_limit = enhanced ? kGpuPolygonLimit : kHardwarePolygonLimit;
        if (vertices.size() != vertex_limit)
            std::vector<dsr::Gpu3DVertex>(vertex_limit).swap(vertices);
        if (polygons.size() != polygon_limit)
            std::vector<dsr::Gpu3DPolygon>(polygon_limit).swap(polygons);
    }
    std::size_t vertex_count = 0;
    std::size_t polygon_count = 0;
    std::size_t captured_polygon_count = 0;
    std::size_t opaque_count = 0;
    std::array<std::uint8_t, 256 * 192> mask{};
    std::array<std::uint8_t, 256 * 192> opaque_only{};
    // Full-only, RGBA8 display conversion of the raster captured with geometry.
    // Alpha is opaque only where the 3D raster itself is fully opaque.
    std::vector<std::uint8_t> side_software;
    bool capture_side_software(const dsr::Gpu3DRasterView& raster) {
        const auto* pixels = raster.enhanced_output ? raster.enhanced_output : raster.output;
        if (!pixels || raster.output_pixel_count != 256U * 192U) return false;
        side_software.resize(256U * 192U * 4U);
        for (std::size_t i = 0; i < 256U * 192U; ++i) {
            const auto p = pixels[i];
            for (unsigned c = 0; c < 3; ++c) {
                const auto v = (p >> (c * 6U + 1U)) & 31U;
                side_software[i * 4U + c] = static_cast<std::uint8_t>(v * 255U / 31U);
            }
            side_software[i * 4U + 3U] = ((p >> 24U) & 31U) == 31U ? 255U : 0U;
        }
        return true;
    }
    std::uint32_t three_d_pixels = 0, gpu_pixels = 0;
#ifdef DQ9_GPU_SCENE_TESTS
    // Indexed in draw order; hardware eligibility is determined in submission order.
    std::array<bool, kGpuPolygonLimit> enhanced_only{};
    bool hardware_vertex_overflow = false, hardware_polygon_overflow = false;
    bool enhanced_overflow = false;
    // Captured at render_start alongside geometry, never from the later LCD
    // framebuffer. Test builds alone pay for this owned reference copy.
    std::array<std::uint32_t, 256 * 192> software_output{};
    std::array<std::uint8_t, 256 * 192> software_polygon_ids{};
    std::uint64_t captured_snapshots = 0, valid_snapshots = 0;
    std::uint32_t native_translucent_polygons = 0, blending_polygons = 0;
    std::array<std::uint32_t, 5> capture_cause_pixels{};
    // Largest partial-touch polygons: geometry index, ID, alpha, format, mode, pixels.
    std::array<std::array<std::uint32_t, 6>, 8> capture_top_polygons{};
#endif
    std::array<std::uint8_t, 32> fog_table{};
    std::array<std::uint32_t, 32> toon_table{};
    std::array<std::uint32_t, 8> edge_color_table{};
    std::uint32_t fog_color = 0, fog_offset = 0;
    std::uint32_t clear_color = 0, clear_depth = 0;
    std::uint32_t disp3dcnt = 0, alpha_test_ref = 0, swap_mode = 0;
    std::uint64_t vram_generation = 0, epoch = 0, number = 0;
    std::uint64_t source_frame = 0;
    bool screen_is_top = true;
    bool gpu_valid = false;
    bool color_valid = false;
    GpuFallback fallback = GpuFallback::Geometry;
    std::size_t decoded_this_frame = 0;
    // At most 256 not-yet-acknowledged uploads. Shared ownership keeps skipped
    // ring publications from losing decoded textures; acknowledgement is by key.
    std::vector<std::shared_ptr<const DecodedTexture>> new_textures;
};

} // namespace dq9
