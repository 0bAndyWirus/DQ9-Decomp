#pragma once

#include "dsr/frontend.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dsr::frontend_detail {
// Environment/known-folder discovery stays at the platform boundary; resolution
// is pure so Windows drive/UNC roots and fallback policy can be tested anywhere.
struct UserPathEnvironment {
    bool windows = false;
    std::filesystem::path config_home, data_home, home;
    std::filesystem::path appdata, local_appdata, user_profile;
    // Non-empty when a `portable.txt` sits next to the executable: settings and saves then live under
    // <portable_root>/config and <portable_root>/data and nothing is written to the user profile.
    std::filesystem::path portable_root;
};
UserPaths user_paths_from_environment(const UserPathEnvironment& environment);

// Positive correction consumes source faster; negative correction fills an undersized queue.
double audio_resample_correction(std::size_t backlog_frames, std::size_t target_frames, int output_rate);
bool audio_queue_needs_emergency_clear(std::size_t backlog_frames, std::size_t limit_frames);
// The SDL device is released only once the queue holds the target; an explicit queue clear (debug pause,
// emergency clear) must stop the device and require priming again before playback resumes.
bool audio_priming_should_start(bool device_started, std::size_t backlog_frames, std::size_t target_frames);
bool audio_device_started_after_clear();
// SDL queued audio has no backend xrun counter; estimate starvation against queue plus device-buffer time.
bool audio_would_underrun(double elapsed_seconds, double available_seconds);
// Queue emptiness alone is not starvation: priming, intentional pauses and audio
// already handed to the device all legitimately leave SDL's software queue empty.
bool audio_queue_starved(bool device_started, bool has_queue_time, std::size_t backlog_frames,
                         double elapsed_seconds, double available_seconds);
// Discard overdue wall-clock slots rather than emitting a burst of catch-up frames.
std::chrono::steady_clock::time_point advance_frame_deadline(
    std::chrono::steady_clock::time_point previous, std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration frame_period);
// Bare letter shortcuts are HUD-local; destructive mixer shortcuts additionally
// require the visible audio page, or an explicit Ctrl+Shift keyboard chord.
std::uint32_t allowed_debug_actions(std::uint32_t actions, bool overlay_visible,
                                   bool audio_page, bool ctrl, bool shift);
// Output gain as Q8 fixed point (256 = unity) for a 0-100 percent volume; 0 when muted.
int volume_gain_q8(int percent, bool muted);
// Scales interleaved samples in place; unity gain leaves them untouched.
void apply_volume_gain(std::int16_t* samples, std::size_t count, int gain_q8);
// First-run language choice: the first preferred locale ("fr_FR", "de-AT", "es", ...) whose language the
// European release ships (en, fr, de, es, it), else "en".
std::string game_language_for_locales(const std::vector<std::string>& preferred);
// A remembered window size can come from a larger display or a hand-edited file: never open a window
// bigger than the usable desktop area (max_* <= 0 means unknown and leaves the size alone).
int fit_window_extent(int wanted, int max_extent);
std::string audio_debug_status(int solo, const std::array<bool, 16>& muted);

struct Rect { int x; int y; int w; int h; };
struct ScreenRects { Rect top; Rect bottom; int width; int height; };

enum class DebugControl { None, Note, Pause, Overlay };
struct DebugControlRects { Rect note; Rect pause; Rect overlay; };

// Coordinates are renderer-output pixels; callers scale window coordinates before hit testing.
DebugControlRects debug_control_rects(int width, int height);
DebugControl debug_control_at(int x, int y, int width, int height);

ScreenRects layout_rects(ScreenLayout layout, int width, int height, bool integer_scaling = false);

bool map_touch(ScreenLayout layout, int width, int height, int x, int y, int& pen_x, int& pen_y, bool integer_scaling = false);
struct ConfigData {
    FrontendConfig frontend;
    std::array<int, 12> keycodes{};
    std::array<int, 12> gamepad_buttons{};
    int key_layout = 0;
    int key_swap = 0;
    int key_fullscreen = 0;
    int key_filter = 0;
    int key_integer = 0;
    int key_lid = 0;
    int key_fast_forward = 0;
    int key_screenshot = 0;
    int key_gpu3d_scale = 0;
    int key_volume_down = 0;
    int key_volume_up = 0;
    int key_mute = 0;
    int key_pause = 0;
    std::string language = "en";
    std::string rom_path;
    std::string save_path;
    bool debug_tool = false;
    bool capture_memory_dumps = true;
    bool frame_time_overlay = false;
};

ConfigData default_config();
std::string serialize_config(const ConfigData& config);
ConfigData parse_config(std::string_view text, ConfigData base = default_config());
std::uint16_t gamepad_button_mask(int button);
std::uint16_t configured_gamepad_mask(const std::array<int, 12>& mapping, int button);

// Returns the desired number of output frames accumulated for the supplied
// source frame count/rates; fractional source position is retained by caller.
std::size_t resampled_frame_count(double source_frames, double source_rate, int output_rate,
                                  double rate_adjustment = 0.0);
std::vector<std::int16_t> resample_stereo_cubic(const std::int16_t* input, std::size_t frames,
                                                 double source_rate, int output_rate,
                                                 double& source_position, double rate_adjustment = 0.0);
void resample_stereo_cubic_into(const std::int16_t* input, std::size_t frames, double source_rate,
                                int output_rate, double& source_position,
                                std::vector<std::int16_t>& output, double rate_adjustment = 0.0);

} // namespace dsr::frontend_detail
