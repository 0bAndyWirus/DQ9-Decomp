#include "dsr/session.h"
#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"
#include <algorithm>
namespace dsr {


Session::Session(const MachineConfig& config) : machine_(config) {}

// The 3D engine renders frame N+1 at scanline 214, after frame N's visible lines, so by the
// end of a step its scaled buffer is one frame ahead of the screens and 3D mask the 2D
// engine just produced. Latch the buffer the coming frame will display before running it.
void Session::latch_hires() {
    const Gpu3D& gpu3d = machine_.gpu3d();
    const int scale = gpu3d.output_scale();
    hires_scale_ = 0;
    if (scale <= 1) return;
    const std::size_t row = 256U * static_cast<std::size_t>(scale);
    const std::size_t rows = 192U * static_cast<std::size_t>(scale);
    hires_latched_.resize(row * rows);
    for (std::size_t y = 0; y < rows; ++y) {
        const std::uint32_t* source = gpu3d.scaled_line(static_cast<int>(y));
        if (source == nullptr) return;
        std::copy(source, source + row, hires_latched_.begin() + static_cast<std::ptrdiff_t>(y * row));
    }
    hires_scale_ = scale;
}

void Session::step(const InputState& input) {
    latch_hires();
    machine_.set_input(input);
    machine_.run_frame();
    audio_frames_ = machine_.spu().drain(audio_.data(), audio_.size() / 2);
    ++frame_count_;
}

const std::uint32_t* Session::screen_top() const {
    return machine_.gpu2d().screen_top();
}

const std::uint32_t* Session::screen_bottom() const {
    return machine_.gpu2d().screen_bottom();
}

bool Session::compose_hires(std::uint32_t* out, int scale, bool* on_top) const {
    const Gpu2D& gpu2d = machine_.gpu2d();
    if (scale <= 1 || hires_scale_ != scale) return false;
    constexpr int kWidth = 256, kHeight = 192;
    const std::uint8_t* mask = gpu2d.three_d_mask();
    bool any_3d = false;
    for (int i = 0; i < kWidth * kHeight && !any_3d; ++i) any_3d = mask[i] != 0;
    if (!any_3d) return false;
    const bool top = (gpu2d.frame_view().powcnt1 & (1U << 15U)) != 0U;
    if (on_top != nullptr) *on_top = top;
    const std::uint32_t* native = top ? gpu2d.screen_top() : gpu2d.screen_bottom();
    const std::size_t out_width = static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(scale);
    for (int y = 0; y < kHeight * scale; ++y) {
        const std::uint32_t* scaled = hires_latched_.data() + static_cast<std::size_t>(y) * out_width;
        const int native_y = y / scale;
        std::uint32_t* row = out + static_cast<std::size_t>(y) * out_width;
        for (int x = 0; x < kWidth * scale; ++x) {
            const int native_x = x / scale;
            const std::uint32_t raw = scaled[x];
            // 3D pixels are shown unblended only at opaque coverage; elsewhere (including
            // hi-res samples the scaled render left uncovered) keep the native pixel.
            if (mask[native_y * kWidth + native_x] != 0 && ((raw >> 24U) & 31U) == 31U) {
                row[x] = 0xFF000000U |
                    (static_cast<std::uint32_t>(Gpu2D::expand_color5((raw & 63U) >> 1U)) << 16U) |
                    (static_cast<std::uint32_t>(Gpu2D::expand_color5(((raw >> 6U) & 63U) >> 1U)) << 8U) |
                    static_cast<std::uint32_t>(Gpu2D::expand_color5(((raw >> 12U) & 63U) >> 1U));
            } else {
                row[x] = native[native_y * kWidth + native_x];
            }
        }
    }
    return true;
}

bool Session::compose_both_hires(std::uint32_t* out, int scale) const {
    constexpr int kWidth = 256, kHeight = 192;
    if (scale <= 1) return false;
    const std::size_t width = static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(scale);
    const std::size_t rows = static_cast<std::size_t>(kHeight) * static_cast<std::size_t>(scale);
    std::vector<std::uint32_t> scaled(width * rows);
    bool on_top = true;
    if (!compose_hires(scaled.data(), scale, &on_top)) return false;
    const Gpu2D& gpu2d = machine_.gpu2d();
    std::uint32_t* const top_out = out;
    std::uint32_t* const bottom_out = out + width * rows;
    const auto upscale = [&](const std::uint32_t* native, std::uint32_t* destination) {
        for (std::size_t y = 0; y < rows; ++y)
            for (std::size_t x = 0; x < width; ++x)
                destination[y * width + x] = native[(y / static_cast<std::size_t>(scale)) * kWidth +
                                                    x / static_cast<std::size_t>(scale)];
    };
    if (on_top) {
        std::copy(scaled.begin(), scaled.end(), top_out);
        upscale(gpu2d.screen_bottom(), bottom_out);
    } else {
        upscale(gpu2d.screen_top(), top_out);
        std::copy(scaled.begin(), scaled.end(), bottom_out);
    }
    return true;
}

void Session::flush_save() {
    if (GameCard* card = machine_.game_card()) card->flush_save();
}

} // namespace dsr
