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

inline bool accepts_geometry(const dsr::Gpu3DGeometryView& geometry, bool full) {
    return !geometry.enhanced_overflow &&
           (full || (!geometry.vertex_overflow && !geometry.polygon_overflow)) &&
           geometry.vertex_count <= (full ? kGpuVertexLimit : kHardwareVertexLimit) &&
           geometry.polygon_count <= (full ? kGpuPolygonLimit : kHardwarePolygonLimit);
}
inline constexpr std::size_t kGpuNewTextureLimit = 256;

struct TextureKeyHash {
    std::size_t operator()(const dsr::gpu3d_detail::TextureKey& key) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(key.texture_param) << 32U) | key.palette_base;
        return std::hash<std::uint64_t>{}(packed) ^
               (std::hash<std::uint64_t>{}(key.vram_generation) << 1U);
    }
};

struct DecodedTexture {
    dsr::gpu3d_detail::TextureKey key{};
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
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

enum class GpuFallback { None, Disabled, Warmup, TextureBudget, WBuffer, BitmapClear, Geometry };

inline const char* fallback_name(GpuFallback reason) {
    switch (reason) {
    case GpuFallback::None: return "none";
    case GpuFallback::Disabled: return "disabled";
    case GpuFallback::Warmup: return "warmup";
    case GpuFallback::TextureBudget: return "texture_budget";
    case GpuFallback::WBuffer: return "w_buffer";
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

    void prepare_geometry(bool full) {
        const auto vertex_limit = full ? kGpuVertexLimit : kHardwareVertexLimit;
        const auto polygon_limit = full ? kGpuPolygonLimit : kHardwarePolygonLimit;
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
    std::uint32_t three_d_pixels = 0, gpu_pixels = 0;
#ifdef DQ9_GPU_SCENE_TESTS
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
