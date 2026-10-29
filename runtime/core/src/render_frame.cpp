#include "dsr/render_frame.h"

#include "dsr/machine.h"

namespace dsr {

void capture_render_frame(const Machine& machine, std::uint64_t frame_number,
                          RenderFrame& destination) noexcept {
    destination.schema_version = RenderFrame::kSchemaVersion;
    destination.frame_number = frame_number;
    destination.ticks = machine.ticks();
    destination.width = 256;
    destination.height = 192;
    destination.geometry = machine.gpu3d().geometry_view();
    destination.draw_order = machine.gpu3d().draw_order_view();
    destination.raster = machine.gpu3d().raster_view();
    destination.texture = machine.gpu3d().texture_view(machine.gpu2d());
    destination.gpu2d = machine.gpu2d().frame_view();
    const auto& raster = destination.raster;
    destination.fog_color = raster.fog_color;
    destination.fog_offset = raster.fog_offset;
    for (std::size_t i = 0; i < 32; ++i) {
        destination.fog_table[i] = raster.fog_density[i];
        destination.toon_table[i] = static_cast<std::uint32_t>(raster.toon_table[i * 2]) |
            (static_cast<std::uint32_t>(raster.toon_table[i * 2 + 1]) << 8);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        destination.edge_color_table[i] = static_cast<std::uint32_t>(raster.edge_table[i * 2]) |
            (static_cast<std::uint32_t>(raster.edge_table[i * 2 + 1]) << 8);
    }
    destination.clear_color = raster.clear_color;
    destination.clear_depth = raster.clear_depth;
    destination.disp3dcnt = raster.disp3d;
    destination.alpha_test_ref = raster.alpha_reference;
    destination.vram_generation = machine.gpu2d().vram_generation();
    destination.three_d_mask = machine.gpu2d().presentation_three_d_mask();
    destination.opaque_only_mask = raster.opaque_only_mask;
    destination.screen_top = machine.gpu2d().screen_top();
    destination.screen_bottom = machine.gpu2d().screen_bottom();
    destination.presentation_top = machine.gpu2d().presentation_top();
    destination.presentation_bottom = machine.gpu2d().presentation_bottom();
    destination.screen_pixel_count = 256U * 192U;
}

} // namespace dsr
