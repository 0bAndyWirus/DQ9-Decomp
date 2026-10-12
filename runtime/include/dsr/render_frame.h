// Read-only render-state view captured from a Machine after one completed frame.
// All pointers are borrowed from the Machine. Capture and consume on its owner
// thread; views remain valid only until the next step or direct state mutation.
// The 2D records are end-of-frame state, not per-scanline replay data.
#pragma once

#include <cstdint>
#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"

namespace dsr {

class Machine;

struct RenderFrame {
    static constexpr std::uint32_t kSchemaVersion = 2;
    std::uint32_t schema_version = kSchemaVersion;
    std::uint64_t frame_number = 0;
    std::uint64_t ticks = 0;
    std::uint16_t width = 256;
    std::uint16_t height = 192;
    Gpu3DGeometryView geometry{};
    Gpu3DDrawOrderView draw_order{};
    Gpu3DRasterView raster{};
    Gpu3DTextureView texture{};
    Gpu2DFrameView gpu2d{};
    std::uint32_t fog_color = 0;
    std::uint32_t fog_offset = 0;
    std::uint8_t fog_table[32]{};
    // Raw RGB555 register entries, widened to uint32_t (not RGBA colours).
    std::uint32_t toon_table[32]{};
    std::uint32_t edge_color_table[8]{};
    std::uint32_t clear_color = 0;
    std::uint32_t clear_depth = 0;
    std::uint32_t disp3dcnt = 0;
    std::uint32_t alpha_test_ref = 0;
    std::uint64_t vram_generation = 0;
    // Engine A coordinates, independent of POWCNT1 screen swap.
    const std::uint8_t* three_d_mask = nullptr;
    // Optional 3D-raster coordinates; null when capture is disabled/not ready.
    // AND with the matching LCD's three_d_mask before GPU compositing.
    const std::uint8_t* opaque_only_mask = nullptr;
    const std::uint32_t* screen_top = nullptr;
    const std::uint32_t* screen_bottom = nullptr;
    std::size_t screen_pixel_count = 256U * 192U;
};

// Writes fixed-size metadata/tables and borrowed views; does not allocate.
void capture_render_frame(const Machine& machine, std::uint64_t frame_number,
                          RenderFrame& destination) noexcept;

} // namespace dsr
