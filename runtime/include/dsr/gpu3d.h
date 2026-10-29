// 3D geometry engine + rendering engine (software rasterizer).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace dsr {

class Gpu2D;
class BandPool;
namespace gpu3d_detail { struct Gpu3DTestAccess; }

struct Gpu3DVertex {
    std::array<float, 4> clip{};
    float u = 0.0F;
    float v = 0.0F;
    std::array<float, 3> color{};
};

struct Gpu3DPolygon {
    std::array<std::uint16_t, 4> index{};
    std::uint8_t count = 0;
    std::uint32_t attr = 0;
    std::uint32_t texture = 0;
    std::uint32_t palette = 0;
    std::uint32_t viewport = 0;
};

// Borrowed view of the geometry most recently activated by SWAP_BUFFERS.
// Pointers remain valid until Gpu3D destruction or an enhanced-geometry
// storage transition; contents change on the next SWAP_BUFFERS. Vertices
// contain post-transform clip coordinates.
struct Gpu3DGeometryView {
    const Gpu3DVertex* vertices = nullptr;
    std::size_t vertex_count = 0;
    const Gpu3DPolygon* polygons = nullptr;
    std::size_t polygon_count = 0;
    std::uint64_t generation = 0;
    std::uint32_t swap_mode = 0;
    bool vertex_overflow = false;
    bool polygon_overflow = false;
    // Enhanced presentation storage, distinct from the hardware RAM flags.
    bool enhanced_overflow = false;
};

// Indices into geometry_view().polygons in the actual software draw order.
// Borrowed until the next render_start, reset, or enhanced-geometry storage
// transition; opaque first, then translucent.
struct Gpu3DDrawOrderView {
    const std::uint16_t* opaque = nullptr;
    std::size_t opaque_count = 0;
    const std::uint16_t* translucent = nullptr;
    std::size_t translucent_count = 0;
};
// The raster state is an end-of-frame register snapshot, not GX command history.
struct Gpu3DRasterView {
    std::uint32_t disp3d = 0;
    std::uint32_t viewport = 0;
    std::uint32_t alpha_reference = 0;
    std::uint32_t clear_color = 0;
    std::uint32_t clear_depth = 0;
    std::uint32_t fog_color = 0;
    std::uint32_t fog_offset = 0;
    // Little-endian RGB555 entries: 32 toon halfwords, 8 edge halfwords.
    const std::uint8_t* toon_table = nullptr;
    std::size_t toon_table_byte_count = 0;
    const std::uint8_t* edge_table = nullptr;
    std::size_t edge_table_byte_count = 0;
    const std::uint8_t* fog_density = nullptr;
    const std::uint32_t* output = nullptr;
    std::size_t output_pixel_count = 0;
    // Full-geometry output corresponding to opaque_only_mask, when different.
    const std::uint32_t* enhanced_output = nullptr;
    // Null unless optional capture has completed a render. A 1 permits opaque
    // or single-layer partial history; unsupported touches dilate by one pixel.
    const std::uint8_t* opaque_only_mask = nullptr;
    // Raw touched causes: polygon alpha, texel alpha, shadow, wire, opaque decal.
    // Bits 0..4 may overlap. Available only with opaque-only capture enabled.
    const std::uint8_t* capture_causes = nullptr;
    // Partial polygon/texel fragment touches per geometry polygon, after the
    // band barrier. Count length is geometry_view().polygon_count.
    const std::uint32_t* capture_polygon_touches = nullptr;
    // GPU test builds only: final opaque destination ID per output pixel,
    // paired with output at the same render boundary, including clear IDs.
    const std::uint8_t* capture_polygon_ids = nullptr;
};

struct Gpu3DTextureSlotView {
    bool mapped = false;
    std::uint8_t physical_bank = 0xff;
    std::uint32_t bank_offset = 0;
    const std::uint8_t* bytes = nullptr;
    std::size_t byte_count = 0;
};

struct Gpu3DTexturePaletteSlotView {
    bool mapped = false;
    std::uint8_t physical_bank = 0xff;
    std::uint32_t bank_offset = 0;
    const std::uint8_t* bytes = nullptr;
    std::size_t byte_count = 0;
};

struct Gpu3DTextureView {
    std::array<Gpu3DTextureSlotView, 4> textures{};
    std::array<Gpu3DTexturePaletteSlotView, 6> palettes{};
    std::uint64_t mapping_generation = 0;
    // Borrowed owner-thread-only reads; valid for the same lifetime as slots.
    // These resolve overlapping banks by OR, just like software rendering.
    const void* read_context = nullptr;
    std::uint8_t (*texture_read)(const void*, std::uint32_t) = nullptr;
    std::uint8_t (*palette_read)(const void*, std::uint32_t) = nullptr;
};

// Register values and resolved texture slots are live when viewed; valid only
// on the Machine owner thread and until its next step or direct mutation.

class Gpu3D {
public:
    Gpu3D();
    ~Gpu3D();
    Gpu3D(const Gpu3D&) = delete;
    Gpu3D& operator=(const Gpu3D&) = delete;

    void reset();
    // Owner-thread-only, between renders. Enhanced presentation can retain
    // geometry beyond the guest-visible hardware limits.
    void set_enhanced_geometry(bool enabled);
    void attach_gpu2d(const Gpu2D* gpu2d);
    // Machine-thread-only, between renders. -1 reselects DSR_RENDER_THREADS/
    // hardware default; nonnegative values clamp to 0..6. Counts 0/1 render
    // one full-frame pass on the caller; counts 2..6 enable band workers.
    void set_render_threads(int threads);
    // Owner-thread-only terminal shutdown, after the final render. Joins the
    // per-machine pool before dependent state/DLLs are destroyed; idempotent.
    void shutdown_render_workers();
    // Owner-thread-only, between renders. Off by default; disabling/resetting
    // invalidates the borrowed hybrid mask. Does not alter software output.
    void set_capture_opaque_only(bool enabled);
    std::uint32_t io_read(std::uint32_t addr, int width);
    void io_write(std::uint32_t addr, std::uint32_t value, int width);
    void run(std::int64_t ticks);
    bool fifo_below_half() const;
    bool irq_condition() const;
    std::function<void()> on_fifo_state_change;
    // Optional owner-thread presentation capture at the actual render boundary.
    // Called after output and sorted draw indices are complete; never from bands.
    std::function<void()> on_render_complete;
    // Scanline192 geometry swap/flush, followed by render start at214.
    // Texture mappings/data and clear-image VRAM are read live at render start.
    void vblank_start();
    void render_start();
    // Packed DS 6-bit RGB + 5-bit alpha row.
    const std::uint32_t* line(int y) const;
    // Presentation-only enhanced raster, or nullptr when identical to line().
    const std::uint32_t* enhanced_line(int y) const;
    // Optional host-only second render at an integer scale, clamped to 1..4.
    // Call on the Machine owner thread between frames; the new scale is drawn
    // at the next render_start(). Scale 1 (the default) disables the extra pass.
    // Native line(), geometry, draw order and capture buffers are unaffected.
    void set_output_scale(int n);
    int output_scale() const;
    // Hi-res row index y in [0, 192 * output_scale()), stride 256 * scale.
    // Packing: r6 bits 0..5, g6 6..11, b6 12..17, alpha5 24..28.
    // Returns nullptr at scale 1, for an out-of-range row, or until a render has
    // completed at the requested scale. Scale changes and reset invalidate all
    // borrowed rows; output_scale() reports the requested scale immediately.
    const std::uint32_t* scaled_line(int y) const;
    Gpu3DGeometryView geometry_view() const;
    Gpu3DDrawOrderView draw_order_view() const;
    Gpu3DRasterView raster_view() const;
    Gpu3DTextureView texture_view(const Gpu2D& gpu2d) const;

    struct Impl;

private:
    friend struct gpu3d_detail::Gpu3DTestAccess;
    friend class Gpu2D;
    BandPool& render_pool();
    // Injected lookup/read callbacks must support concurrent read-only calls;
    // texture data and mappings must stay frozen until render_start returns.
    void set_texture_source(const void* context,
                            const std::uint8_t* (*texture_lookup)(const void*, int),
                            const std::uint8_t* (*palette_lookup)(const void*, int),
                            std::uint8_t (*texture_read)(const void*, std::uint32_t),
                            std::uint8_t (*palette_read)(const void*, std::uint32_t),
                            std::uint64_t (*generation)(const void*));
    Impl* impl_;
};

} // namespace dsr
