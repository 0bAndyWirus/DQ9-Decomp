#pragma once

#include "gpu_scene_frame.h"

#include <algorithm>
#include <memory>

namespace dq9 {

// LCD rows 0..191 read the 3D buffer rendered at scanline214 of the previous
// hardware frame. Rotate owned snapshots instead of copying geometry twice.
// Only the owner thread calls this; ring destinations must not be consuming.
class GpuFrameDelay {
public:
    void reset() { previous_.reset(); current_.reset(); captured_ = false; }
    void begin_step() { captured_ = false; }
    GpuSceneFrame& capture_target() {
        if (!current_) current_ = std::make_unique<GpuSceneFrame>();
        captured_ = true;
        return *current_;
    }
    void publish(std::unique_ptr<GpuSceneFrame>& destination,
                 const dsr::RenderFrame& displayed, std::uint64_t epoch) {
        auto recycled = std::move(destination);
        destination = std::move(previous_);
        if (!destination) {
            destination = std::move(recycled);
            if (!destination) destination = std::make_unique<GpuSceneFrame>();
            destination->gpu_valid = destination->color_valid = false;
            destination->fallback = GpuFallback::Warmup;
            destination->polygon_count = destination->captured_polygon_count = destination->vertex_count = 0;
            destination->opaque_count = destination->decoded_this_frame = 0;
            destination->source_frame = 0;
            destination->epoch = epoch;
            destination->new_textures.clear();
            destination->opaque_only.fill(0);
        }
        destination->number = displayed.frame_number;
        destination->screen_is_top = (displayed.gpu2d.powcnt1 & (1U << 15U)) != 0;
        destination->three_d_pixels = destination->gpu_pixels = 0;
        for (std::size_t i = 0; i < destination->mask.size(); ++i) {
            const auto selected = displayed.three_d_mask[i];
            destination->mask[i] = selected & destination->opaque_only[i];
            destination->three_d_pixels += selected;
            destination->gpu_pixels += destination->mask[i];
        }
        if (captured_) {
            previous_ = std::move(current_);
            current_ = std::move(recycled);
        } else {
            // No scanline214 callback means there is no new scene epoch to use.
            previous_.reset();
        }
    }

private:
    std::unique_ptr<GpuSceneFrame> previous_, current_;
    bool captured_ = false;
};

} // namespace dq9
