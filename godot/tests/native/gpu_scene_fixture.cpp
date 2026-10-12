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
        auto frame = dq9::make_quality_probe(options, ++probe_generation_);
        scene_->set_scale(static_cast<int>(options.get("scale", 1)));
        scene_->apply_frame(*frame);
        auto stats = scene_->get_stats();
        const auto bounds = dq9::DsRasterMesh::projected_bounds(*frame);
        stats["projected_bounds"] = godot::Vector4(bounds[0], bounds[1], bounds[2], bounds[3]);
        const auto max_partial = frame->new_textures.empty() ? 0 : frame->new_textures.front()->max_partial_alpha;
        stats["classified_translucent"] = frame->opaque_count == 0;
        stats["fragment_blending"] = dq9::requires_fragment_blending(frame->polygons[0], frame->disp3dcnt,
            frame->alpha_test_ref, static_cast<std::uint8_t>(max_partial));
        return stats;
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
