#include "gpu_frame_delay.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "gpu_scene_frame: %s\n", message);
        std::exit(1);
    }
}

void test_storage() {
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
            "Full storage uses enhanced limits");
    vertices = frame.vertices.data();
    polygons = frame.polygons.data();
    frame.prepare_geometry(true);
    require(frame.vertices.data() == vertices && frame.polygons.data() == polygons,
            "repeated Full preparation retains storage");
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
    require(dq9::accepts_geometry(view, true), "Full limits are inclusive");
    view.vertex_count = 12289;
    require(!dq9::accepts_geometry(view, true), "Full rejects excess vertices");
    view.vertex_count = 12288; view.polygon_count = 12289;
    require(!dq9::accepts_geometry(view, true), "Full rejects excess polygons");
    view.vertex_count = 1; view.polygon_count = 1;
    view.vertex_overflow = true;
    require(!dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "hardware vertex overflow rejects classic only");
    view.vertex_overflow = false; view.polygon_overflow = true;
    require(!dq9::accepts_geometry(view, false) && dq9::accepts_geometry(view, true),
            "hardware polygon overflow rejects classic only");
    view.polygon_overflow = false; view.enhanced_overflow = true;
    require(!dq9::accepts_geometry(view, false) && !dq9::accepts_geometry(view, true),
            "enhanced overflow rejects both modes even below limits");
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
    first.source_frame = 9;
    first.gpu_valid = true;
    delay.publish(slots[0], displayed, 7);
    require(slots[0]->fallback == dq9::GpuFallback::Warmup && !slots[0]->gpu_valid,
            "first publication is warmup, not the captured frame");
    delay.begin_step();
    auto& second = delay.capture_target();
    auto* second_ptr = &second;
    second.source_frame = 10;
    delay.publish(slots[1], displayed, 7);
    require(slots[1].get() == first_ptr && slots[1]->vertices[0].u == 42.0F &&
            slots[1]->vertices.size() == 12288 && slots[1]->mask[0] == 1 &&
            slots[1]->source_frame == 9,
            "previous owned capture is published intact with displayed mask");
    delay.begin_step();
    auto& third = delay.capture_target();
    require(&third != first_ptr && &third != second_ptr && slots[1]->vertices[0].u == 42.0F,
            "capture does not mutate a frame still owned by a ring slot");
    delay.publish(slots[2], displayed, 7);
    require(slots[2].get() == second_ptr && slots[1].get() == first_ptr,
            "publishing another slot leaves the consuming slot untouched");
    delay.begin_step();
    delay.capture_target();
    delay.publish(slots[1], displayed, 7);
    delay.begin_step();
    require(&delay.capture_target() == first_ptr, "released destination is recycled as capture target");
    delay.reset();
    delay.begin_step();
    delay.publish(slots[0], displayed, 8);
    require(!slots[0]->gpu_valid && slots[0]->fallback == dq9::GpuFallback::Warmup &&
            slots[0]->epoch == 8 && slots[0]->vertex_count == 0,
            "reset restarts warmup and clears recycled metadata");
}
}

int main() {
    test_storage();
    test_geometry_acceptance();
    test_ring_recycling();
    std::puts("GPU_SCENE_FRAME PASS");
}
