#include "gpu_frame_delay.h"
#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "gpu_scene_frame: %s\n", message);
        std::exit(1);
    }
}

void test_storage() {
    require(!dq9::enhanced_geometry_enabled(false, false, false) &&
            !dq9::enhanced_geometry_enabled(false, true, false) &&
            !dq9::enhanced_geometry_enabled(false, false, true),
            "Off and disabled GPU do not enlarge geometry");
    require(dq9::enhanced_geometry_enabled(false, true, true) &&
            dq9::enhanced_geometry_enabled(true, false, false) &&
            dq9::enhanced_geometry_enabled(true, true, true),
            "Picture and Full use enhanced geometry independently of Full camera");
    dq9::GpuSceneFrame frame;
    require(frame.vertices.size() == 6144 && frame.polygons.size() == 2048,
            "classic storage starts at hardware limits");
    auto* vertices = frame.vertices.data();
    auto* polygons = frame.polygons.data();
    frame.prepare_geometry(false);
    require(frame.vertices.data() == vertices && frame.polygons.data() == polygons,
            "repeated classic preparation retains storage");
    frame.prepare_geometry(true);
    require(frame.vertices.size() == 12288 && frame.polygons.size() == 12288,
            "Picture and Full storage uses enhanced limits");
    vertices = frame.vertices.data();
    polygons = frame.polygons.data();
    frame.prepare_geometry(true);
    require(frame.vertices.data() == vertices && frame.polygons.data() == polygons,
            "repeated enhanced preparation retains storage");
    frame.prepare_geometry(false);
    require(frame.vertices.size() == 6144 && frame.polygons.size() == 2048 &&
            frame.vertices.capacity() == 6144 && frame.polygons.capacity() == 2048,
            "returning to classic releases enhanced storage");
}

void test_geometry_acceptance() {
    dsr::Gpu3DGeometryView view{};
    view.vertex_count = 6144; view.polygon_count = 2048;
    require(dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "hardware limits are inclusive in both modes");
    view.vertex_count = 6145;
    require(!dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "classic rejects excess vertices independently");
    view.vertex_count = 6144; view.polygon_count = 2049;
    require(!dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "classic rejects excess polygons independently");
    view.vertex_count = 12288; view.polygon_count = 12288;
    require(dq9::accepts_geometry(view, true), "Picture and Full limits are inclusive");
    view.vertex_count = 12289;
    require(!dq9::accepts_geometry(view, true), "Full rejects excess vertices");
    view.vertex_count = 12288; view.polygon_count = 12289;
    require(!dq9::accepts_geometry(view, true), "Full rejects excess polygons");
    // Exercise every combination independently of the count checks. Picture and
    // Full ignore hardware flags, but neither accepts enhanced overflow.
    view.vertex_count = 1; view.polygon_count = 2048;
    for (unsigned flags = 0; flags < 8; ++flags) {
        view.vertex_overflow = (flags & 1U) != 0;
        view.polygon_overflow = (flags & 2U) != 0;
        view.enhanced_overflow = (flags & 4U) != 0;
        require(dq9::accepts_geometry(view, false) == ((flags & 5U) == 0),
                "classic accepts polygon overflow only without other overflow");
        require(dq9::accepts_geometry(view, true) == ((flags & 4U) == 0),
                "enhanced modes ignore hardware flags but reject enhanced overflow");
    }
    view = {};
    view.polygon_overflow = true;
    view.vertex_count = 6144; view.polygon_count = 2048;
    require(dq9::accepts_geometry(view, false), "overflow retained hardware limits are inclusive");
    view.polygon_count = 2047;
    require(!dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "overflow with incomplete retained list rejects classic only");
    view.polygon_count = 0;
    require(!dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "overflow with empty retained list rejects classic only");
    view.polygon_count = 2048;
    view.vertex_count = 6145;
    require(!dq9::accepts_geometry(view, false), "overflow cannot admit excess vertices");
    view.vertex_count = 6144; view.polygon_count = 2049;
    require(!dq9::accepts_geometry(view, false), "overflow cannot admit excess polygons");
    view.vertex_count = std::numeric_limits<std::size_t>::max(); view.polygon_count = 1;
    require(!dq9::accepts_geometry(view, false) && !dq9::accepts_geometry(view, true),
            "malformed vertex count rejects both modes");
    view.vertex_count = 1; view.polygon_count = std::numeric_limits<std::size_t>::max();
    require(!dq9::accepts_geometry(view, false) && !dq9::accepts_geometry(view, true),
            "malformed polygon count rejects both modes");
}

void test_ring_recycling() {
    dq9::GpuFrameDelay delay;
    std::array<std::unique_ptr<dq9::GpuSceneFrame>, 3> slots;
    std::array<std::uint8_t, 256 * 192> mask{};
    mask[0] = 1;
    dsr::RenderFrame displayed{};
    displayed.three_d_mask = mask.data();
    displayed.frame_number = 10;
    delay.begin_step();
    auto& first = delay.capture_target();
    auto* first_ptr = &first;
    first.prepare_geometry(true);
    first.vertices[0].u = 42.0F;
    first.opaque_only[0] = 1;
    std::array<std::uint32_t, 256 * 192> hardware{}, enhanced{};
    hardware[0] = (31U << 24U) | 2U;
    enhanced[0] = (31U << 24U) | 62U | (32U << 6U) | (16U << 12U);
    enhanced[1] = (16U << 24U) | 62U;
    dsr::Gpu3DRasterView raster{};
    raster.output = hardware.data();
    raster.enhanced_output = enhanced.data();
    raster.output_pixel_count = hardware.size();
    require(first.capture_side_software(raster), "capture enhanced side raster");
    enhanced[0] = 0;
    first.source_frame = 9;
    first.gpu_valid = true;
    delay.publish(slots[0], displayed, 7);
    require(slots[0]->fallback == dq9::GpuFallback::Warmup && !slots[0]->gpu_valid,
            "first publication is warmup, not the captured frame");
    delay.begin_step();
    auto& second = delay.capture_target();
    auto* second_ptr = &second;
    second.source_frame = 10;
    raster.enhanced_output = nullptr;
    require(second.capture_side_software(raster), "capture hardware raster when enhanced is absent");
    delay.publish(slots[1], displayed, 7);
    require(slots[1].get() == first_ptr && slots[1]->vertices[0].u == 42.0F &&
            slots[1]->vertices.size() == 12288 && slots[1]->mask[0] == 1 &&
            slots[1]->source_frame == 9,
            "previous owned capture is published intact with displayed mask");
    require(slots[1]->side_software[0] == 255 && slots[1]->side_software[1] == 131 &&
            slots[1]->side_software[2] == 65 && slots[1]->side_software[3] == 255 &&
            slots[1]->side_software[7] == 0 && slots[1]->opaque_only[0] == 1,
            "owned enhanced raster, alpha and raw eligibility travel with delayed geometry");
    delay.begin_step();
    auto& third = delay.capture_target();
    require(&third != first_ptr && &third != second_ptr && slots[1]->vertices[0].u == 42.0F,
            "capture does not mutate a frame still owned by a ring slot");
    delay.publish(slots[2], displayed, 7);
    require(slots[2].get() == second_ptr && slots[1].get() == first_ptr,
            "publishing another slot leaves the consuming slot untouched");
    require(slots[2]->side_software[0] == 8 && slots[2]->side_software[3] == 255 &&
            slots[1]->side_software[0] == 255,
            "later hardware raster does not overwrite delayed enhanced raster");
    delay.begin_step();
    delay.capture_target();
    delay.publish(slots[1], displayed, 7);
    delay.begin_step();
    require(&delay.capture_target() == first_ptr, "released destination is recycled as capture target");
    delay.reset();
    delay.begin_step();
    delay.publish(slots[0], displayed, 8);
    require(!slots[0]->gpu_valid && slots[0]->fallback == dq9::GpuFallback::Warmup &&
            slots[0]->epoch == 8 && slots[0]->vertex_count == 0 && slots[0]->side_software.empty(),
            "reset restarts warmup and clears recycled metadata and side payload");
    // A new mode must not reuse the old capture even if it occupied a ring slot.
    delay.begin_step();
    auto& picture = delay.capture_target();
    picture.prepare_geometry(dq9::enhanced_geometry_enabled(false, true, true));
    picture.source_frame = 20;
    picture.epoch = 8;
    picture.gpu_valid = true;
    delay.publish(slots[0], displayed, 8);
    delay.begin_step();
    delay.publish(slots[1], displayed, 8);
    require(slots[1]->gpu_valid && slots[1]->source_frame == 20 &&
            slots[1]->vertices.size() == dq9::kGpuVertexLimit,
            "Picture publishes its own enhanced capture after warmup");
    delay.reset();
    delay.begin_step();
    delay.publish(slots[1], displayed, 9);
    require(!slots[1]->gpu_valid && slots[1]->epoch == 9 && slots[1]->source_frame == 0,
            "return to Off cannot present the previous Picture capture");
}
void test_full_opaque_eligibility_remap() {
    std::array<std::uint8_t, 256 * 192> mask{};
    mask.fill(1);
    dsr::RenderFrame displayed{};
    displayed.three_d_mask = mask.data();
    for (int scale = 1; scale <= 6; ++scale) {
        dq9::GpuFrameDelay delay;
        std::unique_ptr<dq9::GpuSceneFrame> slot;
        delay.begin_step();
        auto& capture = delay.capture_target();
        for (unsigned y = 0; y < 192; ++y)
            for (unsigned x = 0; x < 256; ++x)
                capture.opaque_only[y * 256 + x] = ((x + y) % 7U == 0U);
        delay.publish(slot, displayed, 1, true, scale);
        delay.begin_step();
        delay.publish(slot, displayed, 1, true, scale);
        const unsigned width = (256U * scale * 4U + 1U) / 3U;
        unsigned eligible = 0;
        for (unsigned y = 0; y < 192; ++y) {
            for (unsigned x = 0; x < 256; ++x) {
                const unsigned src = ((2U * x + 1U) * scale + width - 256U * scale) * 128U / width;
                const bool expected = (src + y) % 7U == 0U;
                require(slot->mask[y * 256 + x] == expected, "Full eligibility uses centered source raster");
                eligible += expected;
            }
        }
        require(slot->three_d_pixels == mask.size() && slot->gpu_pixels == eligible,
                "Full eligibility counts match remapped mask");
    }
}

void test_texture_overlap_identity() {
    constexpr std::array<std::uint32_t, 6> controls{
        0x04000240, 0x04000241, 0, 0, 0x04000244, 0x04000245};
    constexpr std::array<std::uint32_t, 6> lcdc{
        0x06800000, 0x06820000, 0, 0, 0x06880000, 0x06890000};
    const auto seed = [&](dsr::Gpu2D& gpu, unsigned bank, std::uint32_t offset,
                          std::uint16_t value) {
        gpu.io_write(controls[bank], 0x80, 8);
        gpu.arm9_write(lcdc[bank] + offset, value, 16);
    };
    const auto map = [&](dsr::Gpu2D& gpu, unsigned bank) {
        gpu.io_write(controls[bank], 0x83, 8);
    };
    dsr::Gpu2D overlap, single, zero;
    dsr::Gpu3D renderer;
    seed(overlap, 0, 0, 0x0100); seed(overlap, 1, 0, 0x0001);
    seed(overlap, 4, 2, 0x000f); seed(overlap, 5, 2, 0x0010);
    for (unsigned bank : {0U, 1U, 4U, 5U}) map(overlap, bank);
    seed(single, 0, 0, 0x0101); seed(single, 4, 2, 0x001f);
    map(single, 0); map(single, 4);
    auto overlap_view = renderer.texture_view(overlap);
    auto single_view = renderer.texture_view(single);
    auto zero_view = renderer.texture_view(zero);
    require(!overlap_view.textures[0].bytes && !overlap_view.palettes[0].bytes,
            "overlap requires fallback reads");
    const auto overlap_source = dq9::texture_source(overlap_view);
    const auto single_source = dq9::texture_source(single_view);
    const auto zero_source = dq9::texture_source(zero_view);
    constexpr std::uint32_t param = 4U << 26U;
    const auto identity = dq9::texture_identity_data(overlap_source, param, 0);
    require(identity == dq9::texture_identity_data(single_source, param, 0),
            "overlapping texture and palette identity equals OR-combined single banks");
    require(identity != dq9::texture_identity_data(zero_source, param, 0),
            "nonzero resolved content must not use the zero-content key");
    std::vector<std::uint8_t> rgba, expected;
    int width = 0, height = 0, expected_width = 0, expected_height = 0;
    require(dsr::gpu3d_detail::decode_texture_rgba8(overlap_source, param, 0, rgba, width, height) &&
            dsr::gpu3d_detail::decode_texture_rgba8(single_source, param, 0, expected, expected_width, expected_height) &&
            rgba == expected && width == expected_width && height == expected_height &&
            rgba[0] == 255 && rgba[3] == 255,
            "overlap decode equals OR-combined single-bank red texel");
    require(overlap_source.texture_byte(0x80000U) == 1 &&
            overlap_source.palette_half(0x20002U) == 0x001f,
            "fallback reads preserve texture wrapping and palette slot mirroring");
    overlap.io_write(controls[0], 0, 8);
    require(identity == dq9::texture_identity_data(single_source, param, 0) && rgba == expected,
            "identity and decoded pixels are owned after borrowed VRAM changes");
}

}

int main() {
    test_storage();
    test_geometry_acceptance();
    test_ring_recycling();
    test_full_opaque_eligibility_remap();
    test_texture_overlap_identity();
    std::puts("GPU_SCENE_FRAME PASS");
}
