// Reusable synchronous host session for the recompiled DS machine.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
    void flush_save();
    Machine& machine() { return machine_; }
    const Machine& machine() const { return machine_; }

private:
    Machine machine_;
    std::array<std::int16_t, 8192> audio_{};
    std::size_t audio_frames_ = 0;
    std::uint64_t frame_count_ = 0;
};

} // namespace dsr
