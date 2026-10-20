#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "dsr/frontend.h"
#include "dsr/machine.h"

namespace dq9 {

struct DebugCaptureOptions {
    std::string comment;
    std::string language;
    std::filesystem::path executable;
    bool dump_memory = true;
    bool texture_inspector = false;
    double wall_seconds = 0.0;
    std::uint64_t frame = 0;
};

// Writes both screens, stacked, as <directory>/<stem>.png (creating the directory). Returns the
// written path, or an empty path on failure. `stem` gains a "-N" suffix rather than overwriting.
std::filesystem::path save_screenshot(const std::filesystem::path& directory, dsr::Machine& machine);
// Same naming and collision rules for an already composed image (0xFFRRGGBB, width x height).
std::filesystem::path save_screenshot_pixels(const std::filesystem::path& directory,
                                             const std::vector<std::uint32_t>& pixels, std::uint32_t width,
                                             std::uint32_t height);

class DebugSession {
public:
    DebugSession(const std::filesystem::path& data_dir, const std::filesystem::path& rom,
                 const std::filesystem::path& save, std::string language,
                 std::filesystem::path executable, bool dump_memory,
                 std::int64_t rtc_epoch = 0, std::int32_t rtc_timezone_offset_minutes = 0,
                 std::filesystem::path abi_config = {},
                 std::optional<std::string> backup_type = std::nullopt);
    ~DebugSession();
    DebugSession(const DebugSession&) = delete;
    DebugSession& operator=(const DebugSession&) = delete;

    void record_frame(std::uint64_t frame, const dsr::InputState& input, double frame_ms);
    void update_last_frame_time(double frame_ms);
    void record_audio(const std::int16_t* pcm, std::size_t frames);
    bool capture(const DebugCaptureOptions& options, dsr::Machine& machine, dsr::Frontend& frontend,
                 const dsr::FrontendConfig& config);
    void mark(std::uint64_t frame, std::uint64_t ticks, dsr::Machine& machine);
    bool screenshot_only(dsr::Machine& machine, std::uint64_t frame);
    const std::filesystem::path& directory() const { return session_dir_; }
    double fps() const;
    std::array<float, 120> frame_times() const;
    std::size_t frame_time_count() const { return frame_time_count_; }
    std::size_t audio_sample_count() const { return audio_count_; }
    std::uint64_t frames_recorded() const { return frames_recorded_; }

private:
    struct InputRecord { std::uint64_t frame = 0; dsr::InputState input{}; };
    std::filesystem::path session_dir_;
    std::filesystem::path bundle_root_;
    std::filesystem::path rom_;
    std::filesystem::path save_;
    std::filesystem::path abi_config_;
    std::filesystem::path executable_;
    std::string language_;
    std::optional<std::string> backup_type_; // requested override, including explicit auto
    std::filesystem::path session_input_path_;
    std::ofstream session_input_;
    std::ofstream markers_;
    std::array<InputRecord, 600> input_tail_{};
    std::size_t input_pos_ = 0;
    std::size_t input_count_ = 0;
    std::array<float, 120> frame_times_{};
    std::size_t frame_time_pos_ = 0;
    std::size_t frame_time_count_ = 0;
    std::vector<std::int16_t> audio_ring_;
    std::size_t audio_pos_ = 0;
    std::size_t audio_count_ = 0;
    std::uint64_t frames_recorded_ = 0;
    std::uint64_t bundle_sequence_ = 0;
    bool dump_memory_default_ = true;
    std::string save_reference_;
    std::int64_t rtc_epoch_ = 0;
    std::int32_t rtc_timezone_offset_minutes_ = 0;
};

} // namespace dq9
