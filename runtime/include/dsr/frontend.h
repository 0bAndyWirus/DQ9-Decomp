// Host platform frontend: window/presentation, input, audio output, frame
// pacing and user paths. Independent of the emulated machine.
//
// Owner: runtime/frontend (SDL2 backend). Presentation choices never feed
// back into simulation state.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <dsr/debug_editor.h>

namespace dsr {

// DS button bits as in KEYINPUT (bits 0-9) plus X/Y from EXTKEYIN.
enum DsButton : std::uint16_t {
    kBtnA = 1u << 0,
    kBtnB = 1u << 1,
    kBtnSelect = 1u << 2,
    kBtnStart = 1u << 3,
    kBtnRight = 1u << 4,
    kBtnLeft = 1u << 5,
    kBtnUp = 1u << 6,
    kBtnDown = 1u << 7,
    kBtnR = 1u << 8,
    kBtnL = 1u << 9,
    kBtnX = 1u << 10,
    kBtnY = 1u << 11,
};

enum DebugAction : std::uint32_t {
    kDebugTogglePause = 1u << 1,
    kDebugSingleStep = 1u << 2,
    kDebugSlowMotion = 1u << 3,
    kDebugFastForward = 1u << 4,
    kDebugAnnotate = 1u << 5,
    kDebugMark = 1u << 6,
    kDebugToggleHelp = 1u << 7,
    kDebugToggleDumps = 1u << 8,
    kDebugToggleTextureInspector = 1u << 9,
    kDebugToggleAudioMixer = 1u << 10,
    kDebugChannelUp = 1u << 13,
    kDebugChannelDown = 1u << 14,
    kDebugChannelMute = 1u << 15,
    kDebugChannelSolo = 1u << 16,
    kDebugToggleMenu = 1u << 17,
    kDebugToggleFrameGraph = 1u << 18,
    kDebugToggleAudioBar = 1u << 19,
    kDebugToggleCpuPc = 1u << 20,
    kDebugToggleTextureSlots = 1u << 21,
    kDebugScreenshot = 1u << 22,
    kDebugShowFolder = 1u << 23,
};
struct InputState {
    std::uint16_t buttons = 0; // DsButton bits, 1 = pressed
    bool pen_down = false;
    int pen_x = 0;
    int pen_y = 0;
    bool lid_closed = false;
    bool quit_requested = false;
    bool fast_forward = false;         // Held fast-forward key or Select+R; works without the debug tool.
    bool window_unfocused = false;     // pause_when_unfocused is on and the window lost input focus
    bool screenshot_requested = false; // One-shot: set by the poll that saw the key or Select+Start tap.
    std::uint32_t debug_actions = 0;
    std::vector<EditorEvent> editor_events;
    std::uint16_t editor_buttons = 0; // Controller-only annotation grid; never guest input.
    std::vector<int> menu_actions;
    int menu_selection = 0;
};

enum class ScreenLayout { Vertical, Horizontal, TopFocus, BottomFocus, SingleTop, SingleBottom };

struct FrontendConfig {
    std::string title = "DQ9-Decomp";
    int window_scale = 3;
    bool fullscreen = false;           // auto-enabled on Steam Deck
    ScreenLayout layout = ScreenLayout::Vertical;
    bool integer_scaling = false;
    bool linear_filter = false;
    int audio_rate = 48000;
    int audio_buffer_ms = 200;
    int stick_deadzone = 12000;
    bool dpad_from_stick = true;
    bool mouse_touch_enabled = true;
    // Optional hi-res 3D: integer scale (1 = native, off; 1-4) of the second 3D render the
    // host composes over the native top-screen frame. `gpu3d_scale_explicit` marks a value
    // chosen on the command line, which wins over the persisted frontend.conf value.
    int gpu3d_scale = 1;
    bool gpu3d_scale_explicit = false;
    bool pause_when_unfocused = false; // main loop idles (and drops queued audio) while the window lacks focus
    int volume = 100; // master output volume, 0-100 percent (host mixer only; never feeds back into emulation)
    std::array<int, 12> keycodes{};
    std::array<int, 12> gamepad_buttons{};
    bool debug_tool_enabled = false;
    bool frame_time_overlay_enabled = false;
    std::string controller_guid;
};

struct LauncherOptions {
    FrontendConfig frontend;
    std::string language = "en";
    std::filesystem::path rom_path;
    std::filesystem::path save_path;
    bool debug_tool = false;
    bool frame_time_overlay = false;
    bool capture_memory_dumps = true;
};

struct DebugOverlayState {
    bool visible = false;
    bool debug_tool_enabled = false;
    bool startup_banner_visible = false;
    bool paused = false;
    bool help = false;
    bool annotation_open = false;
    bool menu_open = false;
    int menu_selection = 0;
    bool frame_graph = true;
    bool audio_bar = true;
    bool cpu_pc = true;
    bool show_texture_slots = true;
    bool texture_inspector = false;
    bool audio_mixer = false;
    bool capture_dumps = false;
    double speed = 1.0;
    float fps = 0.0f;
    float audio_backlog_ms = 0.0f;
    std::uint64_t frame = 0;
    std::uint32_t arm9_pc = 0;
    std::array<float, 120> frame_times_ms{};
    std::size_t frame_time_count = 0;
    int keyboard_selection = 0;
    int audio_channel_selection = 0;
    int audio_solo = -1;
    std::array<bool, 16> audio_muted{};
    std::array<const std::uint8_t*, 4> texture_slots{};
    std::string annotation;
    std::string status;
};

struct FrontendDiagnostics {
    std::string sdl_version;
    std::string audio_driver;
    std::string audio_device;
    std::string controller_name;
    std::string controller_guid;
    int width = 0;
    int height = 0;
    std::size_t audio_backlog_frames = 0;
    std::uint64_t dropped_audio_blocks = 0;
    std::uint64_t empty_audio_queue_events = 0;
    std::uint64_t queued_audio_blocks = 0;
};

// Displays the persisted SDL2 launcher and returns false when canceled.
bool run_sdl_launcher(LauncherOptions& options);

// Keep relative ROM selections usable when a launcher is opened from another
// working directory. Search cwd, config directory, then repository ancestors
// of the executable directory; return the original path if it is unavailable.
std::filesystem::path resolve_rom_path(const std::filesystem::path& path,
                                      const std::filesystem::path& config_dir,
                                      const std::filesystem::path& executable_dir);

// Structural sanity check of a Nintendo DS image header (no game-specific data): size, a trimmed
// or truncated file, ARM9/ARM7 payloads that lie inside the file. Returns an explanation when the
// file cannot be a complete image, empty when it looks usable. `*header_crc_ok` (optional) reports
// whether the header's own CRC-16 matches; a mismatch alone is not an error (patched headers).
std::string check_rom_image(const std::filesystem::path& path, bool* header_crc_ok = nullptr);

struct UserPaths {
    std::filesystem::path config_dir; // e.g. $XDG_CONFIG_HOME/dq9-decomp
    std::filesystem::path data_dir;   // saves, e.g. $XDG_DATA_HOME/dq9-decomp
};

UserPaths default_user_paths();
bool running_on_steam_deck();

class Frontend {
public:
    virtual ~Frontend() = default;

    virtual bool init(const FrontendConfig& config) = 0;

    // Pump OS events and fill the current input state.
    virtual void poll(InputState& state) = 0;

    // Present two 256x192 0xFFRRGGBB frames (top and bottom LCD).
    virtual void set_debug_overlay(const DebugOverlayState&) {}
    virtual void diagnostics(FrontendDiagnostics&) const {}
    // Drawing is performed only by the frontend present stage.
    virtual void present(const std::uint32_t* top, const std::uint32_t* bottom) = 0;
    // Like present(), but each screen is (256*scale) x (192*scale) pixels; scale 1 is native.
    // The window layout is unchanged: the host scales each texture into the same rectangle.
    virtual void present_hires(const std::uint32_t* top, int top_scale,
                               const std::uint32_t* bottom, int bottom_scale) = 0;
    // Hi-res 3D scale (1-4) the user currently wants; polled by the main loop every frame.
    virtual int requested_gpu3d_scale() const = 0;

    // Queue interleaved stereo int16 frames produced at `source_rate` Hz.
    // The frontend resamples to the device rate.
    virtual void queue_audio(const std::int16_t* frames, std::size_t count, double source_rate) = 0;
    // Stereo frames queued but not yet played (for pacing).
    virtual std::size_t audio_backlog() const = 0;
    virtual void clear_audio_queue() {}
    // Short on-screen notice (screenshot saved, layout changed, ...); hosts without a window ignore it.
    virtual void show_message(const std::string& /*text*/) {}

    // Sleep until the next emulated frame deadline (59.8261 Hz).
    virtual void pace_frame() = 0;
};

std::unique_ptr<Frontend> create_sdl_frontend();

} // namespace dsr
