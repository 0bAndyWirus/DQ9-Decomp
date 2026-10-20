// Reusable synchronous host session for the recompiled DS machine.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
#include "dsr/machine.h"
#include "dsr/spu.h"

namespace dsr {

struct SessionConfig {
    std::filesystem::path rom_path;
    std::filesystem::path save_path;
    std::uint8_t language = 1;
    bool deterministic_rtc = false;
    std::int64_t rtc_epoch_seconds = 0;
    std::int32_t rtc_timezone_offset_minutes = 0;
    std::filesystem::path abi_config_path; // empty: use DQ9_ABI_CONFIG, if set
    std::string backup_type;               // cartridge save chip name (--backup-type); empty or "auto": from the ABI config / save size
};

class Session {
public:
    // ROM-backed convenience constructor; requires generated ARM guest tables.
    // The MachineConfig overload also supports ROM-free synthetic machines.
    explicit Session(const SessionConfig& config);
    explicit Session(const MachineConfig& config);
    ~Session() = default;
    Session& operator=(const Session&) = delete;

    // Apply input, advance exactly one hardware frame, and drain its PCM.
    void step(const InputState& input);
    const std::uint32_t* screen_top() const;
    const std::uint32_t* screen_bottom() const;
    const std::int16_t* audio_data() const { return audio_.data(); }
    std::size_t audio_frames() const { return audio_frames_; }
    static constexpr double audio_rate() {
        return 33'513'982.0 / Spu::kTicksPerSample;
    }
    std::uint64_t frame_count() const { return frame_count_; }
    std::uint64_t ticks() const { return machine_.ticks(); }
    bool flush_save();
    // Optional hi-res 3D overlay (Gpu3D::set_output_scale(N), N > 1). Writes the screen that
    // engine A (the 3D engine) drives, per POWCNT1 bit 15, as 256N x 192N 0xFFRRGGBB: pixels the
    // 2D compositor shows as an unmodified 3D pixel (Gpu2D::three_d_mask) come from the scaled 3D
    // render, every other pixel is nearest-neighbour from the native screen. `*on_top` receives
    // which screen that is. Returns false (nothing written) when the scale is 1, differs from the
    // scale latched at the start of the last step(), or no pixel of the frame is a 3D pixel; present the
    // native screens then.
    bool compose_hires(std::uint32_t* out, int scale, bool* on_top) const;
    // Both screens stacked (top above bottom) at 256N x 384N: the 3D screen from compose_hires, the other
    // one nearest-neighbour from the native frame. Same return contract as compose_hires.
    bool compose_both_hires(std::uint32_t* out, int scale) const;
    Machine& machine() { return machine_; }
    const Machine& machine() const { return machine_; }

private:
    void latch_hires();
    Machine machine_;
    std::vector<std::uint32_t> hires_latched_; // scaled 3D buffer shown by the last step()
    int hires_scale_ = 0;
    std::array<std::int16_t, 8192> audio_{};
    std::size_t audio_frames_ = 0;
    std::uint64_t frame_count_ = 0;
};

} // namespace dsr
