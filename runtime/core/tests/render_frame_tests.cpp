#include "dsr/machine.h"
#include "dsr/render_frame.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool check(bool condition, std::string_view message) {
    if (condition) return true;
    std::cerr << "[render-frame-test] FAIL: " << message << '\n';
    return false;
}

void gpu_port(dsr::Gpu3D& gpu, std::uint8_t code, std::uint32_t value = 0) {
    gpu.io_write(0x04000440U + (static_cast<std::uint32_t>(code) - 0x10U) * 4U,
                 value, 32);
}

void send(dsr::Gpu3D& gpu, std::uint8_t code, std::uint32_t value = 0) {
    gpu_port(gpu, code, value);
    gpu.run(10000);
}

void submit_triangle(dsr::Gpu3D& gpu) {
    send(gpu, 0x20, 0x7fffU);
    send(gpu, 0x29, (31U << 16U) | 0xc0U);
    send(gpu, 0x40, 0U);
    for (const auto [x, y] : std::array<std::array<std::int16_t, 2>, 3>{
             std::array<std::int16_t, 2>{-2048, -2048},
             std::array<std::int16_t, 2>{2048, -2048},
             std::array<std::int16_t, 2>{0, 2048}}) {
        send(gpu, 0x23, static_cast<std::uint16_t>(x) |
                         (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U));
        send(gpu, 0x23, 0U);
    }
    send(gpu, 0x41);
    send(gpu, 0x50, 3U);
    gpu.vblank_start();
    gpu.render_start();
}

bool test_gpu3d_geometry_generation_and_observation() {
    dsr::Gpu3D gpu;
    const auto before = gpu.geometry_view();
    if (!check(before.generation == 0 && before.vertex_count == 0 && before.polygon_count == 0,
               "reset geometry is empty generation zero")) return false;
    submit_triangle(gpu);
    const auto geometry = gpu.geometry_view();
    if (!check(geometry.generation == 1 && geometry.vertex_count == 3 &&
               geometry.polygon_count == 1 && geometry.swap_mode == 0,
               "swap publishes transformed geometry with the preceding frame mode")) return false;
    const auto repeated = gpu.geometry_view();
    if (!check(geometry.vertices != nullptr && geometry.vertices == repeated.vertices &&
               geometry.polygons != nullptr && geometry.polygons == repeated.polygons &&
               geometry.vertices[0].clip[3] != 0.0F && geometry.polygons[0].count == 3,
               "geometry pointers borrow stable active arrays without copying")) return false;
    const auto generation = geometry.generation;
    gpu.io_write(0x04000350U, 0x001fU, 32);
    const auto observed = gpu.geometry_view();
    const auto raster = gpu.raster_view();
    return check(observed.generation == generation && raster.clear_color == 0x001fU,
                 "accessors observe without changing geometry generation") &&
           check(raster.output != nullptr && raster.output_pixel_count == 256U * 192U,
                 "raster view exposes the software reference output");
}

bool test_gpu3d_draw_order_matches_sort_modes() {
    dsr::Gpu3D gpu;
    const auto submit_list = [&](bool manual) {
        // Top triangles (positive Y) sort before bottom ones. Submission
        // alternates alpha classes so the opaque/translucent split is exercised.
        for (int i = 0; i < 4; ++i) {
            send(gpu, 0x29, ((i & 1 ? 16U : 31U) << 16U) | 0xc0U);
            send(gpu, 0x40, 0U);
            const int y = i < 2 ? -2048 : 1024;
            for (const auto [x, dy] : std::array<std::array<int, 2>, 3>{
                     std::array<int, 2>{-512, 0}, {512, 0}, {0, 512}}) {
                send(gpu, 0x23, static_cast<std::uint16_t>(x) |
                    (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y + dy)) << 16U));
                send(gpu, 0x23, 0U);
            }
            send(gpu, 0x41);
        }
        send(gpu, 0x50, manual ? 1U : 0U);
        gpu.vblank_start();
        gpu.render_start();
    };
    submit_list(true);
    auto order = gpu.draw_order_view();
    bool ok = check(order.opaque_count == 2 && order.translucent_count == 2 &&
                    order.opaque[0] == 2 && order.opaque[1] == 0 &&
                    order.translucent[0] == 3 && order.translucent[1] == 1,
                    "automatic draw order splits classes and sorts by screen Y");
    // SWAP's manual flag takes effect for the following geometry frame.
    submit_list(false);
    order = gpu.draw_order_view();
    ok &= check(order.opaque[0] == 2 && order.opaque[1] == 0 &&
                order.translucent[0] == 1 && order.translucent[1] == 3,
                "manual sorting preserves translucent submission order only");
    gpu.reset();
    order = gpu.draw_order_view();
    return check(order.opaque_count == 0 && order.translucent_count == 0,
                 "reset clears borrowed draw order counts") && ok;
}

bool test_gpu3d_overflow_flags_survive_swap() {
    dsr::Gpu3D gpu;
    send(gpu, 0x40, 2U);
    for (std::size_t vertex = 0; vertex < 2053U; ++vertex) {
        gpu_port(gpu, 0x24, 0U);
        gpu.run(8);
    }
    send(gpu, 0x50, 0U);
    gpu.vblank_start();
    const auto geometry = gpu.geometry_view();
    return check(geometry.polygon_overflow && geometry.polygon_count == 2048,
                 "polygon overflow is attached to the active swapped geometry");
}

bool test_gpu2d_end_frame_mapping_views() {
    dsr::Gpu2D gpu;
    gpu.io_write(0x04000240U, 0x80U, 8); // A to LCDC for physical bank write access.
    gpu.arm9_write(0x06800000U, 0x12345678U, 32);
    gpu.io_write(0x04000240U, 0x9bU, 8); // A to 3D texture slot 3, bank enabled.
    gpu.arm9_write(0x05000000U, 0x00007fffU, 16);
    gpu.arm9_write(0x07000000U, 0x00001234U, 16);
    gpu.io_write(0x04000000U, 0x01010101U, 32);
    gpu.io_write(0x04000028U, 0x00002000U, 32);
    gpu.io_write(0x04000244U, 0x83U, 8); // E to texture-palette slot zero.
    gpu.io_write(0x0400002cU, 0x00003000U, 32);
    const auto view = gpu.frame_view();
    bool ok = true;
    ok &= check(view.engines[0].register_count == 28 && view.engines[0].registers[0] == 0x01010101U,
                "engine register view exposes fixed register block");
    ok &= check(view.engines[0].affine_x[0] == 0x2000 && view.engines[0].affine_y[0] == 0x3000,
                "affine state reports current reference accumulators");
    ok &= check(view.banks[0].bytes != nullptr && view.banks[0].byte_count == 0x20000U &&
                view.banks[0].bytes[0] == 0x78U,
                "physical bank view exposes exact bank storage");
    ok &= check(view.palettes[0] == 0xffU && view.oam[0] == 0x34U,
                "palette and OAM views expose physical bytes");
    ok &= check(view.texture_slots[3].mapped && view.texture_slots[3].physical_bank == 0 &&
                view.texture_slots[3].bytes == view.banks[0].bytes &&
                view.texture_slots[3].byte_count == 0x20000U,
                "texture slot view retains mapping and aliases its physical bank");
    ok &= check(view.mapping_generation > 0 && view.vramcnt_count == 9,
                "mapping generation and all VRAMCNT values are captured");
    dsr::Gpu3D gpu3d;
    const auto texture_view = gpu3d.texture_view(gpu);
    ok &= check(texture_view.mapping_generation == view.mapping_generation &&
                texture_view.textures[3].physical_bank == 0 &&
                texture_view.textures[3].bytes == view.texture_slots[3].bytes &&
                texture_view.palettes[0].mapped &&
                texture_view.palettes[0].physical_bank == 4 &&
                texture_view.palettes[0].bank_offset == 0 &&
                texture_view.palettes[0].bytes == view.texture_palette_slots[0].bytes,
                "3D texture view carries texture and palette mapping metadata");
    ok &= check(view.texture_palette_slots[0].mapped &&
                view.texture_palette_slots[0].physical_bank == 4 &&
                view.texture_palette_slots[0].bank_offset == 0 &&
                view.texture_palette_slots[0].byte_count == 0x4000U &&
                view.texture_palette_slots[0].bytes == view.banks[4].bytes,
                "palette slot view records physical bank and content span");
    return ok;
}

bool test_render_frame_machine_capture_is_observational() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    const auto* top = machine.gpu2d().screen_top();
    const auto* bottom = machine.gpu2d().screen_bottom();
    const auto top_pixel = top[0];
    const auto bottom_pixel = bottom[0];
    const auto ticks = machine.ticks();
    dsr::RenderFrame frame{};
    dsr::capture_render_frame(machine, 42, frame);
    bool ok = true;
    ok &= check(frame.schema_version == dsr::RenderFrame::kSchemaVersion &&
                frame.frame_number == 42 && frame.ticks == ticks,
                "capture fills stable frame header");
    ok &= check(frame.gpu2d.engines[0].registers == machine.gpu2d().frame_view().engines[0].registers &&
                frame.screen_top == top && frame.screen_bottom == bottom,
                "RenderFrame carries borrowed owner storage, not a copy");
    ok &= check(machine.ticks() == ticks && top[0] == top_pixel && bottom[0] == bottom_pixel,
                "capture changes neither timing nor software screens");
    return ok;
}

bool test_render_frame_gpu_state_snapshot() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    auto& gpu = machine.gpu3d();
    submit_triangle(gpu);
    gpu.io_write(0x04000060U, 0x81U, 16);
    gpu.io_write(0x04000340U, 9U, 32);
    gpu.io_write(0x04000350U, 0x001f1234U, 32);
    gpu.io_write(0x04000354U, 0x2345U, 32);
    gpu.io_write(0x04000358U, 0x001f4321U, 32);
    gpu.io_write(0x0400035cU, 0x123U, 32);
    for (std::uint32_t i = 0; i < 32; ++i) {
        gpu.io_write(0x04000360U + i, i + 1U, 8);
        gpu.io_write(0x04000380U + i * 2U, 0x100U + i, 16);
    }
    for (std::uint32_t i = 0; i < 8; ++i) {
        gpu.io_write(0x04000330U + i * 2U, 0x200U + i, 16);
    }
    machine.gpu2d().io_write(0x04000240U, 0x83U, 8);
    dsr::RenderFrame frame{};
    dsr::capture_render_frame(machine, 7, frame);
    bool ok = check(frame.schema_version == 2 && frame.geometry.polygon_count == 1,
                    "schema two captures state after scripted GX stream");
    ok &= check(frame.draw_order.opaque_count == 1 && frame.draw_order.opaque[0] == 0 &&
                frame.draw_order.translucent_count == 0,
                "RenderFrame exposes the software polygon draw order");
    ok &= check(frame.fog_color == 0x001f4321U && frame.fog_offset == 0x123U &&
                frame.clear_color == 0x001f1234U && frame.clear_depth == 0x2345U &&
                frame.disp3dcnt == 0x81U && frame.alpha_test_ref == 9U &&
                frame.vram_generation == machine.gpu2d().vram_generation(),
                "GPU registers and invalidation token captured exactly");
    for (std::uint32_t i = 0; i < 32; ++i) {
        ok &= check(frame.fog_table[i] == i + 1 && frame.toon_table[i] == 0x100U + i,
                    "all fog and RGB555 toon entries captured");
    }
    for (std::uint32_t i = 0; i < 8; ++i) {
        ok &= check(frame.edge_color_table[i] == 0x200U + i,
                    "all RGB555 edge entries captured");
    }
    ok &= check(frame.raster.toon_table_byte_count == 64 &&
                frame.raster.edge_table_byte_count == 16,
                "borrowed table spans have exact halfword byte counts");
    gpu.io_write(0x04000380U, 0x777U, 16);
    return check(frame.toon_table[0] == 0x100U,
                 "fixed-size table snapshot does not alias subsequent writes") && ok;
}

bool test_render_frame_three_d_compositor_mask() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    auto& gpu = machine.gpu2d();
    auto& renderer = machine.gpu3d();
    gpu.io_write(0x04000304U, 0x8207U, 16);
    gpu.io_write(0x04000240U, 0x81U, 8); // BG1 tile storage.
    gpu.io_write(0x0400000aU, 0x0005U, 16); // Character block 1, priority 1.
    gpu.arm9_write(0x05000002U, 0x03e0U, 16);
    for (std::uint32_t offset = 0; offset < 32; offset += 2) {
        gpu.arm9_write(0x06004000U + offset, 0x1111U, 16);
    }
    constexpr auto display = (1U << 16) | (1U << 8) | (1U << 9) | (1U << 3);
    gpu.io_write(0x04000000U, display, 32);
    renderer.io_write(0x04000350U, (31U << 16) | 31U, 32);
    renderer.vblank_start();
    renderer.render_start();
    dsr::RenderFrame frame{};
    const auto capture_line = [&] {
        gpu.render_line(0);
        dsr::capture_render_frame(machine, 0, frame);
    };
    capture_line();
    bool ok = check(frame.three_d_mask == gpu.three_d_mask() &&
                    frame.three_d_mask[0] == 1 && frame.screen_top[0] == 0xffff0000U,
                    "opaque BG0 3D over BG1 sets mask without changing output");
    gpu.io_write(0x04000050U, 1U << 9, 16); // BG1 second blend target.
    renderer.io_write(0x04000350U, (16U << 16) | 31U, 32);
    renderer.render_start();
    capture_line();
    ok &= check(frame.three_d_mask[0] == 0 && frame.screen_top[0] != 0xffff0000U,
                "alpha-blended 3D stays software");
    renderer.io_write(0x04000350U, (31U << 16) | 31U, 32);
    renderer.render_start();
    capture_line();
    ok &= check(frame.three_d_mask[0] == 0,
                "opaque 3D with compositor blend enabled still has zero mask");
    gpu.io_write(0x04000050U, 0, 16);
    gpu.io_write(0x04000241U, 0x82U, 8); // OBJ storage.
    for (std::uint32_t offset = 0; offset < 32; offset += 2) {
        gpu.arm9_write(0x06400000U + offset, 0x1111U, 16);
    }
    for (std::uint32_t object = 1; object < 128; ++object) {
        gpu.arm9_write(0x07000000U + object * 8, 0x0200U, 16);
    }
    gpu.arm9_write(0x05000202U, 0x7c00U, 16);
    gpu.io_write(0x04000000U, display | (1U << 12), 32);
    capture_line();
    ok &= check(frame.three_d_mask[0] == 0 && frame.three_d_mask[8] == 1 &&
                frame.screen_top[0] == 0xff0000ffU,
                "higher priority sprite clears mask only where it covers 3D");
    gpu.io_write(0x04000000U, display | (1U << 13), 32);
    gpu.io_write(0x04000040U, 1, 16); // WIN0 x [0,1).
    gpu.io_write(0x04000044U, 1, 16); // WIN0 y [0,1).
    gpu.io_write(0x04000048U, 2, 16); // BG1 only inside.
    gpu.io_write(0x0400004aU, 3, 16); // Both BGs outside.
    capture_line();
    ok &= check(frame.three_d_mask[0] == 0 && frame.three_d_mask[1] == 1 &&
                frame.screen_top[0] == 0xff00ff00U,
                "window-masked BG0 clears mask, outside stays 3D");
    gpu.io_write(0x04000000U, 0, 32);
    capture_line();
    ok &= check(frame.three_d_mask[0] == 0 && frame.three_d_mask[1] == 0,
                "forced display blank clears a previously rendered mask");
    gpu.reset();
    ok &= check(gpu.three_d_mask()[1] == 0, "reset clears the whole engine A mask");
    return ok;
}

bool test_three_d_mask_excludes_display_transforms() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    auto& gpu = machine.gpu2d();
    auto& renderer = machine.gpu3d();
    gpu.io_write(0x04000304U, 0x8207U, 16);
    constexpr auto display = (1U << 16) | (1U << 8) | (1U << 3);
    gpu.io_write(0x04000000U, display, 32);
    renderer.io_write(0x04000350U, (31U << 16) | 31U, 32);
    renderer.vblank_start();
    renderer.render_start();
    dsr::RenderFrame frame{};
    const auto render_frame = [&] {
        for (int y = 0; y < 192; ++y) gpu.render_line(y);
        dsr::capture_render_frame(machine, 0, frame);
    };
    const auto mask_is = [&](std::uint8_t expected) {
        for (std::size_t i = 0; i < 256U * 192U; ++i) {
            if (frame.three_d_mask[i] != expected) return false;
        }
        return true;
    };
    render_frame();
    bool ok = check(mask_is(1) && frame.screen_top[0] == 0xffff0000U,
                    "unmodified opaque 3D has mask one across the frame");
    for (std::uint32_t mode : {1U, 2U}) {
        gpu.io_write(0x0400006cU, (mode << 14) | 8U, 16);
        render_frame();
        ok &= check(mask_is(0) && frame.screen_top[0] != 0xffff0000U,
                    "fade to white/black excludes every transformed pixel");
        gpu.io_write(0x0400006cU, mode << 14, 16);
        render_frame();
        ok &= check(mask_is(1) && frame.screen_top[0] == 0xffff0000U,
                    "zero-factor master brightness leaves the opaque mask eligible");
    }
    gpu.io_write(0x0400006cU, (3U << 14) | 8U, 16);
    render_frame();
    ok &= check(mask_is(1), "ignored master brightness mode does not exclude 3D");
    gpu.io_write(0x0400006cU, 0, 16);
    gpu.io_write(0x0400106cU, (2U << 14) | 8U, 16);
    render_frame();
    ok &= check(mask_is(1), "engine B brightness does not exclude engine A 3D");
    for (std::uint32_t effect : {2U, 3U}) {
        gpu.io_write(0x04000050U, (effect << 6) | 1U, 16);
        gpu.io_write(0x04000054U, 8U, 16);
        render_frame();
        ok &= check(mask_is(0) && frame.screen_top[0] != 0xffff0000U,
                    "BLDCNT brighten/darken on BG0 excludes transformed pixels");
        gpu.io_write(0x04000054U, 0, 16);
        render_frame();
        ok &= check(mask_is(1), "zero-factor BLDCNT brightness remains eligible");
    }
    gpu.io_write(0x04000050U, (2U << 6) | 1U, 16);
    gpu.io_write(0x04000054U, 8U, 16);
    gpu.io_write(0x04000000U, display | (1U << 13), 32);
    gpu.io_write(0x04000040U, 1U, 16); // WIN0 x [0,1).
    gpu.io_write(0x04000044U, 1U, 16); // WIN0 y [0,1).
    gpu.io_write(0x04000048U, 1U, 16); // BG0 on, effects off inside.
    gpu.io_write(0x0400004aU, 0x21U, 16); // BG0 and effects on outside.
    render_frame();
    ok &= check(frame.three_d_mask[0] == 1 && frame.three_d_mask[1] == 0 &&
                frame.screen_top[0] == 0xffff0000U && frame.screen_top[1] != 0xffff0000U,
                "window-disabled brightness excludes only actually transformed pixels");
    gpu.io_write(0x04000050U, 0, 16);
    gpu.io_write(0x04000000U, display, 32);
    render_frame();
    gpu.io_write(0x04000010U, 5U, 16);
    gpu.render_line(23);
    bool scrolled_row_zero = true;
    for (std::size_t x = 0; x < 256; ++x) {
        scrolled_row_zero &= frame.three_d_mask[23U * 256U + x] == 0;
    }
    ok &= check(scrolled_row_zero && frame.three_d_mask[22U * 256U] == 1 &&
                frame.three_d_mask[24U * 256U] == 1,
                "BG0HOFS five excludes only the lines rendered with scrolling");
    render_frame();
    ok &= check(mask_is(0), "scrolled 3D stays software across the frame");
    gpu.io_write(0x04000010U, 0x200U, 16); // Ignored upper bit; effective offset zero.
    render_frame();
    ok &= check(mask_is(1), "zero effective BG0HOFS restores opaque eligibility");
    return ok;
}

bool test_three_d_mask_copy_only_frame() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    auto& gpu = machine.gpu2d();
    auto& renderer = machine.gpu3d();
    gpu.io_write(0x04000304U, 0x8207U, 16);
    gpu.io_write(0x04000240U, 0x81U, 8);
    gpu.io_write(0x0400000aU, 0x0005U, 16); // BG1 text, character block 1, priority 1.
    gpu.arm9_write(0x05000002U, 0x03e0U, 16);
    for (std::uint32_t offset = 0; offset < 32; offset += 2) {
        gpu.arm9_write(0x06004000U + offset, 0x1111U, 16);
    }
    // No windows, objects, mosaic, or blend targets: this must use copy_only.
    constexpr auto display = (1U << 16) | (1U << 8) | (1U << 9) | (1U << 3);
    gpu.io_write(0x04000000U, display, 32);
    gpu.io_write(0x04000050U, 0, 16);
    renderer.io_write(0x04000350U, 0, 32); // Transparent outside the opaque triangle.
    submit_triangle(renderer);
    dsr::RenderFrame frame{};
    const auto render_frame = [&] {
        for (int y = 0; y < 192; ++y) gpu.render_line(y);
        dsr::capture_render_frame(machine, 0, frame);
    };
    render_frame();
    const auto raster = renderer.raster_view();
    std::size_t three_d_pixels = 0;
    std::size_t two_d_pixels = 0;
    bool exact_mask = true;
    bool exact_colors = true;
    for (std::size_t i = 0; i < 256U * 192U; ++i) {
        const bool three_d_wins = ((raster.output[i] >> 24U) & 31U) == 31U;
        three_d_pixels += three_d_wins;
        two_d_pixels += !three_d_wins;
        exact_mask &= frame.three_d_mask[i] == static_cast<std::uint8_t>(three_d_wins);
        exact_colors &= frame.screen_top[i] == (three_d_wins ? 0xffffffffU : 0xff00ff00U);
    }
    bool ok = check(three_d_pixels != 0 && two_d_pixels != 0,
                    "copy-only frame contains both 3D triangle and BG1 text winners");
    ok &= check(exact_mask && exact_colors,
                "copy-only mask is one exactly on opaque 3D-winning pixels across the frame");
    // Cover the triangle with BG1, verifying stale mask bits are overwritten.
    gpu.io_write(0x04000008U, 1U, 16); // BG0 priority 1.
    gpu.io_write(0x0400000aU, 4U, 16); // BG1 priority 0.
    render_frame();
    bool all_two_d = true;
    for (std::size_t i = 0; i < 256U * 192U; ++i) {
        all_two_d &= frame.three_d_mask[i] == 0 && frame.screen_top[i] == 0xff00ff00U;
    }
    ok &= check(all_two_d, "copy-only BG1 winner clears every previous 3D mask bit");
    std::cout << "[render-frame-test] copy-only winners: 3D=" << three_d_pixels
              << ", 2D=" << two_d_pixels << '\n';
    return ok;
}
bool test_render_frame_opaque_only_capture() {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Machine machine(config);
    auto& gpu = machine.gpu3d();
    dsr::RenderFrame frame{};
    dsr::capture_render_frame(machine, 0, frame);
    bool ok = check(frame.schema_version == 2 && frame.opaque_only_mask == nullptr,
                    "schema2 optional opaque-only view is null by default");
    gpu.set_capture_opaque_only(true);
    submit_triangle(gpu);
    dsr::capture_render_frame(machine, 1, frame);
    ok &= check(frame.opaque_only_mask == gpu.raster_view().opaque_only_mask &&
                frame.opaque_only_mask != nullptr,
                "RenderFrame borrows the completed optional 3D-raster mask");
    for (std::size_t i = 0; i < 256U * 192U; ++i)
        ok &= check(frame.opaque_only_mask[i] == 1, "opaque geometry and clear stay GPU-eligible");
    gpu.set_capture_opaque_only(false);
    dsr::capture_render_frame(machine, 2, frame);
    ok &= check(frame.opaque_only_mask == nullptr, "recapture clears stale mask pointers when disabled");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_render_frame_opaque_only_capture();
    ok &= test_gpu3d_geometry_generation_and_observation();
    ok &= test_gpu3d_draw_order_matches_sort_modes();
    ok &= test_gpu3d_overflow_flags_survive_swap();
    ok &= test_gpu2d_end_frame_mapping_views();
    ok &= test_render_frame_machine_capture_is_observational();
    ok &= test_render_frame_gpu_state_snapshot();
    ok &= test_render_frame_three_d_compositor_mask();
    ok &= test_three_d_mask_excludes_display_transforms();
    ok &= test_three_d_mask_copy_only_frame();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
