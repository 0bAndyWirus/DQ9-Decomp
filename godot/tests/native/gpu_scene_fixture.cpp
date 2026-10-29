#include "dq_gpu_scene.h"
#include "gpu_frame_delay.h"
#include "dsr/machine.h"
#include "gpu_quality_fixture.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector4.hpp>

class DqGpuSceneFixture;
namespace {
DqGpuSceneFixture* live_fixtures = nullptr;
}

// Synthetic alternating-triangle stream using hardware register addresses.
// Enabled by DQ9_GPU_SCENE_TESTS; drives Machine scanlines without an image
// and uses the worker's delay helper.
class DqGpuSceneFixture final : public godot::Node {
    GDCLASS(DqGpuSceneFixture, godot::Node)
public:
    DqGpuSceneFixture() : next_instance_(live_fixtures) {
        if (next_instance_) next_instance_->previous_instance_ = this;
        live_fixtures = this;
    }
    ~DqGpuSceneFixture() override {
        shutdown_machine();
        if (previous_instance_) previous_instance_->next_instance_ = next_instance_;
        else live_fixtures = next_instance_;
        if (next_instance_) next_instance_->previous_instance_ = previous_instance_;
    }
    static void shutdown_instances() {
        for (auto* instance = live_fixtures; instance; instance = instance->next_instance_)
            instance->shutdown_machine();
    }
    void _notification(int what) {
        if (what == NOTIFICATION_PREDELETE) shutdown_machine();
    }
    godot::Node* get_scene() const { return scene_; }
    godot::Dictionary render_probe(const godot::Dictionary& options) {
        if (!machine_) initialize();
        if (!static_cast<bool>(options.get("reuse_generation", false)) || probe_generation_ == 0) ++probe_generation_;
        auto frame = dq9::make_quality_probe(options, probe_generation_);
        scene_->set_scale(static_cast<int>(options.get("scale", 1)));
        scene_->apply_frame(*frame);
        auto stats = scene_->get_stats();
        stats["texture_generation"] = static_cast<std::int64_t>(probe_generation_);
        stats["texture_param"] = static_cast<std::int64_t>(
            (static_cast<unsigned>(static_cast<int>(options.get("format", 0))) << 26U) |
            (1U << 20U) | (static_cast<unsigned>(static_cast<int>(options.get("wrap", 0))) << 16U) |
            (static_cast<unsigned>(static_cast<int>(options.get("texture_slot", 0))) << 14U));
        stats["palette_base"] = static_cast<int>(options.get("palette_base", 0));
        const auto bounds = dq9::DsRasterMesh::projected_bounds(*frame);
        stats["projected_bounds"] = godot::Vector4(bounds[0], bounds[1], bounds[2], bounds[3]);
        const auto max_partial = frame->new_textures.empty() ? 0 : frame->new_textures.front()->max_partial_alpha;
        stats["classified_translucent"] = frame->opaque_count == 0;
        stats["fragment_blending"] = dq9::requires_fragment_blending(frame->polygons[0], frame->disp3dcnt,
            frame->alpha_test_ref, static_cast<std::uint8_t>(max_partial));
        return stats;
    }
    // Exercise the real Classic GX limits, then present the retained geometry
    // and the software raster from the same render_start boundary.
    godot::Dictionary render_classic_overflow() {
        if (!machine_) initialize();
        dsr::Gpu3D gpu;
        gpu.set_render_threads(0);
        gpu.set_enhanced_geometry(false);
        gpu.set_capture_opaque_only(true);
        gpu.io_write(0x04000354U, 0x7fffU, 16);
        const auto send = [&](unsigned code, std::uint32_t value) {
            gpu.io_write(0x04000440U + (code - 0x10U) * 4U, value, 32);
            gpu.run(10000);
        };
        send(0x29, (31U << 16U) | 0xc0U);
        send(0x20, 0x001fU);
        send(0x40, 2U); // Triangle strip: 2053 vertices produce 2051 polygons.
        constexpr std::array<std::array<int, 2>, 3> corners{{
            {{-768, -768}}, {{768, -768}}, {{0, 768}}
        }};
        for (unsigned i = 0; i < 2053; ++i) {
            if (i == 3) send(0x20, 0x03e0U);
            const auto& p = corners[i % corners.size()];
            send(0x23, static_cast<std::uint16_t>(p[0]) |
                 (static_cast<std::uint32_t>(static_cast<std::uint16_t>(p[1])) << 16U));
            // The first red triangle is closer than the subsequent green ones.
            send(0x23, static_cast<std::uint16_t>(i < 3 ? -1024 : 1024));
        }
        const auto ram_count = gpu.io_read(0x04000604U, 32);
        const auto status = gpu.io_read(0x04000060U, 32);
        send(0x41, 0); send(0x50, 0);
        gpu.vblank_start();
        gpu.render_start();
        const auto geometry = gpu.geometry_view();
        const auto order = gpu.draw_order_view();
        const auto raster = gpu.raster_view();
        godot::Dictionary result;
        result["ram_polygon_count"] = static_cast<int>(ram_count & 0xfffU);
        result["ram_vertex_count"] = static_cast<int>(ram_count >> 16U);
        result["ram_overflow"] = (status & (1U << 13U)) != 0;
        result["polygon_count"] = static_cast<int>(geometry.polygon_count);
        result["vertex_count"] = static_cast<int>(geometry.vertex_count);
        result["polygon_overflow"] = geometry.polygon_overflow;
        result["vertex_overflow"] = geometry.vertex_overflow;
        result["enhanced_overflow"] = geometry.enhanced_overflow;
        result["opaque_count"] = static_cast<int>(order.opaque_count);
        result["translucent_count"] = static_cast<int>(order.translucent_count);
        result["capture_ready"] = raster.output && raster.opaque_only_mask &&
            raster.capture_polygon_ids && raster.output_pixel_count == 256U * 192U;
        if (!static_cast<bool>(result["capture_ready"]) ||
            geometry.vertex_count > dq9::kHardwareVertexLimit ||
            geometry.polygon_count > dq9::kHardwarePolygonLimit ||
            order.opaque_count != geometry.polygon_count) return result;

        dq9::GpuSceneFrame frame;
        frame.vertex_count = geometry.vertex_count;
        frame.polygon_count = frame.captured_polygon_count = geometry.polygon_count;
        frame.opaque_count = order.opaque_count;
        std::copy_n(geometry.vertices, frame.vertex_count, frame.vertices.begin());
        for (std::size_t i = 0; i < frame.polygon_count; ++i)
            frame.polygons[i] = geometry.polygons[order.opaque[i]];
        frame.clear_color = raster.clear_color;
        frame.clear_depth = raster.clear_depth;
        frame.disp3dcnt = raster.disp3d;
        frame.alpha_test_ref = raster.alpha_reference;
        std::copy_n(raster.output, frame.software_output.size(), frame.software_output.begin());
        std::copy_n(raster.capture_polygon_ids, frame.software_polygon_ids.size(), frame.software_polygon_ids.begin());
        std::copy_n(raster.opaque_only_mask, frame.opaque_only.size(), frame.opaque_only.begin());
        for (std::size_t i = 0; i < frame.mask.size(); ++i)
            frame.mask[i] = raster.opaque_only_mask[i] && ((raster.output[i] >> 24U) & 31U) == 31U;
        // This fixture tests rendering retained geometry, not the host's
        // fallback decision; production policy is exercised separately.
        frame.gpu_valid = frame.color_valid = true;
        frame.fallback = dq9::GpuFallback::None;
        scene_->set_scale(1);
        scene_->apply_frame(frame);
        result["scene_valid"] = static_cast<bool>(scene_->get_stats()["valid"]);
        return result;
    }

    godot::Dictionary advance() {
        if (!machine_) initialize();
        delay_.begin_step();
        auto& gpu = machine_->gpu3d();
        const auto send = [&](unsigned code, std::uint32_t value) {
            gpu.io_write(0x04000440U + (code - 0x10U) * 4U, value, 32);
            gpu.run(10000);
        };
        send(0x20, 0x7fffU); send(0x29, (31U << 16U) | 0xc0U);
        send(0x40, 1U); // Independent quad, perimeter order.
        const bool right = (step_ & 1U) != 0;
        const int left = right ? 1024 : -3072, bottom = right ? -2048 : 0;
        for (const auto [x, y] : std::array<std::array<int, 2>, 4>{
                 std::array<int, 2>{left, bottom}, {left + 2048, bottom},
                 {left + 2048, bottom + 2048}, {left, bottom + 2048}}) {
            send(0x23, static_cast<std::uint16_t>(x) |
                (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U));
            send(0x23, 0U);
        }
        send(0x41, 0); send(0x50, 0);
        machine_->run_frame();
        ++step_;
        dsr::RenderFrame displayed{};
        dsr::capture_render_frame(*machine_, step_, displayed);
        delay_.publish(slots_[step_ % 3], displayed, 1);
        scene_->apply_frame(*slots_[step_ % 3]);
        auto result = scene_->get_stats();
        godot::PackedByteArray mask;
        mask.resize(256 * 192);
        std::copy_n(displayed.three_d_mask, 256 * 192, mask.ptrw());
        result["software_mask"] = mask;
        result["submitted_right"] = right;
        return result;
    }
protected:
    static void _bind_methods() {
        godot::ClassDB::bind_method(godot::D_METHOD("advance"), &DqGpuSceneFixture::advance);
        godot::ClassDB::bind_method(godot::D_METHOD("get_scene"), &DqGpuSceneFixture::get_scene);
        godot::ClassDB::bind_method(godot::D_METHOD("render_probe", "options"), &DqGpuSceneFixture::render_probe);
        godot::ClassDB::bind_method(godot::D_METHOD("render_classic_overflow"), &DqGpuSceneFixture::render_classic_overflow);
    }
private:
    void shutdown_machine() {
        machine_.reset();
        delay_.reset();
        for (auto& slot : slots_) slot.reset();
    }
    void initialize() {
        machine_ = std::make_unique<dsr::Machine>(dsr::MachineConfig{});
        auto& gpu2d = machine_->gpu2d();
        gpu2d.io_write(0x04000304U, 0x820fU, 16);
        gpu2d.io_write(0x04000000U, (1U << 16U) | (1U << 8U) | (1U << 3U), 32);
        machine_->gpu3d().io_write(0x04000354U, 0x7fffU, 16);
        machine_->gpu3d().set_capture_opaque_only(true);
        machine_->gpu3d().on_render_complete = [&] {
            dsr::RenderFrame captured{};
            dsr::capture_render_frame(*machine_, step_ + 1, captured);
            auto& target = delay_.capture_target();
            target.epoch = 1; target.source_frame = step_ + 1;
            target.vertex_count = captured.geometry.vertex_count;
            target.polygon_count = target.captured_polygon_count = captured.geometry.polygon_count;
            target.opaque_count = captured.draw_order.opaque_count;
            std::copy_n(captured.geometry.vertices, target.vertex_count, target.vertices.begin());
            for (std::size_t i = 0; i < target.polygon_count; ++i)
                target.polygons[i] = captured.geometry.polygons[captured.draw_order.opaque[i]];
            target.clear_color = captured.clear_color; target.clear_depth = captured.clear_depth;
            target.disp3dcnt = captured.disp3dcnt; target.alpha_test_ref = captured.alpha_test_ref;
            target.vram_generation = captured.vram_generation;
            std::copy_n(captured.raster.output, target.software_output.size(), target.software_output.begin());
            std::copy_n(captured.raster.capture_polygon_ids, target.software_polygon_ids.size(), target.software_polygon_ids.begin());
            std::copy_n(captured.opaque_only_mask, target.opaque_only.size(), target.opaque_only.begin());
            target.gpu_valid = target.color_valid = true;
            target.fallback = dq9::GpuFallback::None;
        };
        scene_ = memnew(DqGpuScene);
        scene_->set_scale(1);
        add_child(scene_);
    }
    std::unique_ptr<dsr::Machine> machine_;
    dq9::GpuFrameDelay delay_;
    std::unique_ptr<dq9::GpuSceneFrame> slots_[3];
    DqGpuScene* scene_ = nullptr;
    std::uint64_t step_ = 0;
    std::uint64_t probe_generation_ = 100000;
    DqGpuSceneFixture* previous_instance_ = nullptr;
    DqGpuSceneFixture* next_instance_ = nullptr;
};

void register_gpu_scene_fixture() { godot::ClassDB::register_class<DqGpuSceneFixture>(); }
void shutdown_gpu_scene_fixture_instances() { DqGpuSceneFixture::shutdown_instances(); }
