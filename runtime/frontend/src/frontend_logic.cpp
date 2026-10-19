#include "frontend_logic.h"
#include "ui_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

namespace dsr::frontend_detail {
namespace {
Rect fit_rect(int x, int y, int w, int h, int area_w, int area_h) {
    if (w <= 0 || h <= 0 || area_w <= 0 || area_h <= 0) return {x, y, 0, 0};
    const double scale = std::min(static_cast<double>(area_w) / w, static_cast<double>(area_h) / h);
    const int out_w = std::max(1, static_cast<int>(std::floor(w * scale)));
    const int out_h = std::max(1, static_cast<int>(std::floor(h * scale)));
    return {x + (area_w - out_w) / 2, y + (area_h - out_h) / 2, out_w, out_h};
}
// SDL scancodes are 0..511; anything else keeps the previous binding.
int scancode_or(int value, int fallback) { return value >= 0 && value < 512 ? value : fallback; }

void parse_line(const std::string& line, ConfigData& cfg) {
    const auto split = line.find('=');
    if (split == std::string::npos) return;
    const std::string key = line.substr(0, split);
    const std::string value = line.substr(split + 1);
    try {
        // Named key_* settings must precede the numeric key_<N> branch below.
        if (key == "key_fast_forward") cfg.key_fast_forward = scancode_or(std::stoi(value), cfg.key_fast_forward);
        else if (key == "key_screenshot") cfg.key_screenshot = scancode_or(std::stoi(value), cfg.key_screenshot);
        else if (key == "layout") {
            const int n = std::stoi(value);
            if (n >= 0 && n <= 5) cfg.frontend.layout = static_cast<ScreenLayout>(n);
        } else if (key == "scale") cfg.frontend.window_scale = std::clamp(std::stoi(value), 1, 8);
        else if (key == "fullscreen") cfg.frontend.fullscreen = std::stoi(value) != 0;
        else if (key == "integer_scaling") cfg.frontend.integer_scaling = std::stoi(value) != 0;
        else if (key == "linear_filter") cfg.frontend.linear_filter = std::stoi(value) != 0;
        else if (key == "audio_rate") cfg.frontend.audio_rate = std::clamp(std::stoi(value), 8000, 192000);
        else if (key == "audio_buffer_ms") cfg.frontend.audio_buffer_ms = std::clamp(std::stoi(value), 20, 500);
        else if (key == "stick_deadzone") cfg.frontend.stick_deadzone = std::clamp(std::stoi(value), 0, 30000);
        else if (key == "dpad_from_stick") cfg.frontend.dpad_from_stick = std::stoi(value) != 0;
        else if (key == "mouse_touch_enabled") cfg.frontend.mouse_touch_enabled = std::stoi(value) != 0;
        else if (key == "controller_guid") cfg.frontend.controller_guid = ui_detail::config_unescape(value);
        else if (key == "language") cfg.language = ui_detail::config_unescape(value);
        else if (key == "rom_path") cfg.rom_path = ui_detail::config_unescape(value);
        else if (key == "save_path") cfg.save_path = ui_detail::config_unescape(value);
        else if (key == "debug_tool") {
            cfg.debug_tool = std::stoi(value) != 0;
            cfg.frontend.debug_tool_enabled = cfg.debug_tool;
        } else if (key == "frame_time_overlay") {
            cfg.frame_time_overlay = std::stoi(value) != 0;
            cfg.frontend.frame_time_overlay_enabled = cfg.frame_time_overlay;
        }
        else if (key == "capture_memory_dumps") cfg.capture_memory_dumps = std::stoi(value) != 0;
        else if (key.rfind("key_", 0) == 0) {
            const auto idx = static_cast<std::size_t>(std::stoul(key.substr(4)));
            if (idx < cfg.keycodes.size()) {
                cfg.keycodes[idx] = std::stoi(value);
                cfg.frontend.keycodes[idx] = cfg.keycodes[idx];
            }
        } else if (key.rfind("gamepad_", 0) == 0) {
            const auto idx = static_cast<std::size_t>(std::stoul(key.substr(8)));
            if (idx < cfg.gamepad_buttons.size()) {
                cfg.gamepad_buttons[idx] = std::stoi(value);
                cfg.frontend.gamepad_buttons[idx] = cfg.gamepad_buttons[idx];
            }
        } else if (key == "hotkey_layout") cfg.key_layout = std::stoi(value);
        else if (key == "hotkey_swap") cfg.key_swap = std::stoi(value);
        else if (key == "hotkey_fullscreen") cfg.key_fullscreen = std::stoi(value);
        else if (key == "hotkey_filter") cfg.key_filter = std::stoi(value);
        else if (key == "hotkey_integer") cfg.key_integer = std::stoi(value);
        else if (key == "hotkey_lid") cfg.key_lid = std::stoi(value);
    } catch (const std::exception&) {
        // Ignore malformed individual settings and retain the documented default.
    }
}
} // namespace

UserPaths user_paths_from_environment(const UserPathEnvironment& environment) {
    UserPaths paths;
    if (environment.windows) {
        paths.config_dir = environment.appdata;
        if (paths.config_dir.empty()) {
            paths.config_dir = environment.user_profile.empty() ? std::filesystem::path(".") :
                environment.user_profile / "AppData" / "Roaming";
        }
        paths.data_dir = environment.local_appdata.empty() ? paths.config_dir : environment.local_appdata;
    } else {
        paths.config_dir = !environment.config_home.empty() ? environment.config_home :
            (!environment.home.empty() ? environment.home / ".config" : std::filesystem::path("."));
        paths.data_dir = !environment.data_home.empty() ? environment.data_home :
            (!environment.home.empty() ? environment.home / ".local" / "share" : std::filesystem::path("."));
    }
    paths.config_dir /= "dq9-decomp";
    paths.data_dir /= "dq9-decomp";
    return paths;
}

ScreenRects layout_rects(ScreenLayout layout, int width, int height, bool integer_scaling) {
    ScreenRects r{{0, 0, 0, 0}, {0, 0, 0, 0}, width, height};
    if (width <= 0 || height <= 0) return r;
    int virtual_w = 256, virtual_h = 384;
    switch (layout) {
    case ScreenLayout::Vertical: break;
    case ScreenLayout::Horizontal: virtual_w = 512; virtual_h = 192; break;
    case ScreenLayout::TopFocus: case ScreenLayout::BottomFocus: virtual_w = 512; virtual_h = 384; break;
    case ScreenLayout::SingleTop: case ScreenLayout::SingleBottom: virtual_w = 256; virtual_h = 192; break;
    }
    const double fit_scale = std::min(static_cast<double>(width) / virtual_w, static_cast<double>(height) / virtual_h);
    const double scale = integer_scaling && fit_scale >= 1.0 ? std::floor(fit_scale) : fit_scale;
    const int canvas_w = std::max(1, static_cast<int>(std::floor(virtual_w * scale)));
    const int canvas_h = std::max(1, static_cast<int>(std::floor(virtual_h * scale)));
    const int ox = (width - canvas_w) / 2, oy = (height - canvas_h) / 2;
    const int unit = std::max(1, static_cast<int>(std::floor(192.0 * scale)));
    const int gap = layout == ScreenLayout::TopFocus || layout == ScreenLayout::BottomFocus
        ? (integer_scaling && unit < 384 ? 0 : unit / 8) : 0;
    auto rect = [&](int x, int y, int w, int h) { return Rect{ox + x, oy + y, w, h}; };
    const int sw = std::max(1, static_cast<int>(std::floor(256.0 * scale)));
    const int sh = unit;
    if (layout == ScreenLayout::Vertical) {
        r.top = rect(0, 0, sw, sh); r.bottom = rect(0, sh, sw, sh);
    } else if (layout == ScreenLayout::Horizontal) {
        r.top = rect(0, 0, sw, sh); r.bottom = rect(sw, 0, sw, sh);
    } else if (layout == ScreenLayout::TopFocus) {
        r.top = fit_rect(ox, oy, 512, 384, canvas_w, canvas_h);
        r.bottom = rect((canvas_w - sw) / 2, sh * 2 + gap, sw, sh);
    } else if (layout == ScreenLayout::BottomFocus) {
        r.bottom = fit_rect(ox, oy, 512, 384, canvas_w, canvas_h);
        r.top = rect((canvas_w - sw) / 2, sh * 2 + gap, sw, sh);
    } else if (layout == ScreenLayout::SingleTop) r.top = rect((canvas_w - sw) / 2, (canvas_h - sh) / 2, sw, sh);
    else r.bottom = rect((canvas_w - sw) / 2, (canvas_h - sh) / 2, sw, sh);
    // Focus layouts arrange a 2x screen main display plus a centered inset.
    if (layout == ScreenLayout::TopFocus || layout == ScreenLayout::BottomFocus) {
        const int main_h = integer_scaling
            ? std::max(192, ((canvas_h - gap) * 2 / 3 / 192) * 192)
            : std::max(1, (canvas_h - gap) * 2 / 3);
        const int main_w = integer_scaling ? main_h * 4 / 3 : std::min(canvas_w, main_h * 4 / 3);
        const int main_x = ox + (canvas_w - main_w) / 2;
        const int main_y = layout == ScreenLayout::TopFocus ? oy : oy + canvas_h - main_h;
        const int available_inset = std::max(1, canvas_h - main_h - gap);
        const int inset_h = integer_scaling && available_inset >= 192 ? (available_inset / 192) * 192 : available_inset;
        const int inset_w = inset_h * 4 / 3;
        const Rect main{main_x, main_y, main_w, main_h};
        const Rect inset{ox + (canvas_w - inset_w) / 2,
                         layout == ScreenLayout::TopFocus ? oy + main_h + gap : oy,
                         inset_w, inset_h};
        if (layout == ScreenLayout::TopFocus) { r.top = main; r.bottom = inset; }
        else { r.bottom = main; r.top = inset; }
    }
    return r;
}

bool map_touch(ScreenLayout layout, int width, int height, int x, int y, int& pen_x, int& pen_y, bool integer_scaling) {
    const ScreenRects rects = layout_rects(layout, width, height, integer_scaling);
    const Rect r = rects.bottom;
    if (r.w <= 0 || r.h <= 0 || x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) return false;
    pen_x = std::clamp((x - r.x) * 256 / r.w, 0, 255);
    pen_y = std::clamp((y - r.y) * 192 / r.h, 0, 191);
    return true;
}

DebugControlRects debug_control_rects(int width, int height) {
    const Rect empty{0, 0, 0, 0};
    constexpr int margin = 8;
    constexpr int gap = 4;
    constexpr int button_height = 32;
    if (width < 240 || height < button_height + margin * 2) return {empty, empty, empty};

    const int button_width = std::min(120, width / 10 * 3);
    const int row_width = button_width * 3 + gap * 2;
    const int left = width - margin - row_width;
    return {{left, margin, button_width, button_height},
            {left + button_width + gap, margin, button_width, button_height},
            {left + (button_width + gap) * 2, margin, button_width, button_height}};
}

DebugControl debug_control_at(int x, int y, int width, int height) {
    const DebugControlRects rects = debug_control_rects(width, height);
    const auto contains = [x, y](const Rect& rect) {
        return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y &&
               x < rect.x + rect.w && y < rect.y + rect.h;
    };
    if (contains(rects.note)) return DebugControl::Note;
    if (contains(rects.pause)) return DebugControl::Pause;
    if (contains(rects.overlay)) return DebugControl::Overlay;
    return DebugControl::None;
}

ConfigData default_config() {
    ConfigData c;
    // SDL scancodes: Z/X = A/B, Backspace/Return = Select/Start, arrows,
    // Q/E = L/R, A/S = X/Y; F6 layout, F7 swap, F11 fullscreen, F5 filter.
    c.keycodes = {29, 27, 42, 40, 79, 80, 82, 81, 8, 20, 4, 22};
    c.frontend.keycodes = c.keycodes;
    c.gamepad_buttons = {1, 0, 4, 6, 14, 13, 11, 12, 10, 9, 3, 2};
    c.frontend.gamepad_buttons = c.gamepad_buttons;
    c.key_layout = 63; c.key_swap = 64; c.key_fullscreen = 68;
    c.key_filter = 62; c.key_integer = 61; c.key_lid = 60;
    c.key_fast_forward = 43; c.key_screenshot = 69; // Tab (held), F12
    return c;
}
std::string serialize_config(const ConfigData& c) {
    std::ostringstream out;
    out << "# SDL scancodes: key_0..11 map A,B,Select,Start,Right,Left,Up,Down,R,L,X,Y.\n"
        << "# GameController button IDs: gamepad_0..11 use the same DS button order.\n"
        << "layout=" << static_cast<int>(c.frontend.layout) << '\n'
        << "scale=" << c.frontend.window_scale << '\n'
        << "fullscreen=" << (c.frontend.fullscreen ? 1 : 0) << '\n'
        << "integer_scaling=" << (c.frontend.integer_scaling ? 1 : 0) << '\n'
        << "linear_filter=" << (c.frontend.linear_filter ? 1 : 0) << '\n'
        << "audio_rate=" << c.frontend.audio_rate << '\n'
        << "audio_buffer_ms=" << c.frontend.audio_buffer_ms << '\n'
        << "stick_deadzone=" << c.frontend.stick_deadzone << '\n'
        << "dpad_from_stick=" << (c.frontend.dpad_from_stick ? 1 : 0) << '\n'
        << "mouse_touch_enabled=" << (c.frontend.mouse_touch_enabled ? 1 : 0) << '\n'
        << "controller_guid=" << ui_detail::config_escape(c.frontend.controller_guid) << '\n'
        << "language=" << ui_detail::config_escape(c.language) << '\n'
        << "rom_path=" << ui_detail::config_escape(c.rom_path) << '\n'
        << "save_path=" << ui_detail::config_escape(c.save_path) << '\n'
        << "debug_tool=" << (c.debug_tool ? 1 : 0) << '\n'
        << "frame_time_overlay=" << (c.frame_time_overlay ? 1 : 0) << '\n'
        << "capture_memory_dumps=" << (c.capture_memory_dumps ? 1 : 0) << '\n';
    for (std::size_t i = 0; i < c.keycodes.size(); ++i) out << "key_" << i << '=' << c.keycodes[i] << '\n';
    for (std::size_t i = 0; i < c.gamepad_buttons.size(); ++i)
        out << "gamepad_" << i << '=' << c.gamepad_buttons[i] << '\n';
    out << "hotkey_layout=" << c.key_layout << '\n' << "hotkey_swap=" << c.key_swap << '\n'
        << "hotkey_fullscreen=" << c.key_fullscreen << '\n' << "hotkey_filter=" << c.key_filter << '\n'
        << "hotkey_integer=" << c.key_integer << '\n' << "hotkey_lid=" << c.key_lid << '\n'
        << "key_fast_forward=" << c.key_fast_forward << '\n'
        << "key_screenshot=" << c.key_screenshot << '\n';
    return out.str();
}
ConfigData parse_config(std::string_view text, ConfigData base) {
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) if (!line.empty() && line[0] != '#') parse_line(line, base);
    return base;
}
std::uint16_t gamepad_button_mask(int button) {
    // SDL_CONTROLLER_BUTTON_* values, with face labels mapped by position.
    switch (button) {
    case 0: return kBtnB; // SDL south -> DS B
    case 1: return kBtnA; // SDL east -> DS A
    case 2: return kBtnY; // SDL west -> DS Y
    case 3: return kBtnX; // SDL north -> DS X
    case 4: return kBtnSelect;
    case 6: return kBtnStart;
    case 9: return kBtnL;
    case 10: return kBtnR;
    case 11: return kBtnUp;
    case 12: return kBtnDown;
    case 13: return kBtnLeft;
    case 14: return kBtnRight;
    default: return 0;
    }
}
std::uint16_t configured_gamepad_mask(const std::array<int, 12>& mapping, int button) {
    if (button < 0) return 0;
    std::uint16_t mask = 0;
    for (std::size_t i = 0; i < mapping.size(); ++i)
        if (mapping[i] == button) mask |= static_cast<std::uint16_t>(1u << i);
    return mask;
}
double audio_resample_correction(std::size_t backlog_frames, std::size_t target_frames, int output_rate) {
    if (target_frames == 0 || output_rate <= 0) return 0.0;
    const double error_seconds =
        (static_cast<double>(backlog_frames) - static_cast<double>(target_frames)) / output_rate;
    return std::clamp(error_seconds * 0.15, -0.01, 0.01);
}
bool audio_queue_needs_emergency_clear(std::size_t backlog_frames, std::size_t limit_frames) {
    return backlog_frames > limit_frames;
}
bool audio_priming_should_start(bool device_started, std::size_t backlog_frames, std::size_t target_frames) {
    return !device_started && backlog_frames >= target_frames;
}
bool audio_device_started_after_clear() { return false; }
bool audio_would_underrun(double elapsed_seconds, double available_seconds) {
    return elapsed_seconds > available_seconds;
}
bool audio_queue_starved(bool device_started, bool has_queue_time, std::size_t backlog_frames,
                         double elapsed_seconds, double available_seconds) {
    return device_started && has_queue_time && backlog_frames == 0 &&
           audio_would_underrun(elapsed_seconds, available_seconds);
}
std::uint32_t allowed_debug_actions(std::uint32_t actions, bool overlay_visible,
                                   bool audio_page, bool ctrl, bool shift) {
    constexpr std::uint32_t hud = kDebugToggleDumps | kDebugToggleTextureInspector |
        kDebugToggleAudioMixer | kDebugToggleFrameGraph | kDebugToggleAudioBar |
        kDebugToggleCpuPc | kDebugToggleTextureSlots | kDebugScreenshot | kDebugShowFolder;
    constexpr std::uint32_t mixer =
        kDebugChannelUp | kDebugChannelDown | kDebugChannelMute | kDebugChannelSolo;
    if (!overlay_visible) actions &= ~hud;
    if (!(overlay_visible && audio_page) && !(ctrl && shift)) actions &= ~mixer;
    return actions;
}
std::string audio_debug_status(int solo, const std::array<bool, 16>& muted) {
    std::string status;
    if (solo >= 0) status = "SOLO CH" + std::to_string(solo);
    for (std::size_t i = 0; i < muted.size(); ++i) {
        if (!muted[i]) continue;
        if (!status.empty()) status += " / ";
        status += "MUTED CH" + std::to_string(i);
    }
    return status.empty() ? status : "AUDIO DEBUG: " + status;
}
std::chrono::steady_clock::time_point advance_frame_deadline(
    std::chrono::steady_clock::time_point previous, std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration frame_period) {
    const auto deadline = previous + frame_period;
    return now >= deadline ? now : deadline;
}

std::size_t resampled_frame_count(double source_frames, double source_rate, int output_rate,
                                  double rate_adjustment) {
    if (!(source_frames > 0.0) || !(source_rate > 0.0) || output_rate <= 0 ||
        !(1.0 + rate_adjustment > 0.0)) return 0;
    return static_cast<std::size_t>(
        std::floor(source_frames * output_rate / (source_rate * (1.0 + rate_adjustment))));
}
void resample_stereo_cubic_into(const std::int16_t* input, std::size_t frames, double source_rate,
                                int output_rate, double& source_position,
                                std::vector<std::int16_t>& out, double rate_adjustment) {
    out.clear();
    if (!input || frames == 0 || !(source_rate > 0.0) || output_rate <= 0 ||
        !(1.0 + rate_adjustment > 0.0)) return;
    const double step = source_rate / output_rate * (1.0 + rate_adjustment);
    out.reserve(std::max(out.capacity(),
        (resampled_frame_count(static_cast<double>(frames), source_rate, output_rate, rate_adjustment) + 2) * 2));
    while (source_position < static_cast<double>(frames)) {
        const auto i = static_cast<std::ptrdiff_t>(source_position);
        const double t = source_position - static_cast<double>(i);
        const auto sample = [&](int channel, std::ptrdiff_t offset) {
            const auto ix = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i + offset, 0, static_cast<std::ptrdiff_t>(frames - 1)));
            return static_cast<double>(input[ix * 2 + static_cast<std::size_t>(channel)]);
        };
        for (int channel = 0; channel < 2; ++channel) {
            const double p0 = sample(channel, -1), p1 = sample(channel, 0), p2 = sample(channel, 1), p3 = sample(channel, 2);
            const double value = p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
            out.push_back(static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0)));
        }
        source_position += step;
    }
    source_position -= static_cast<double>(frames);
}
std::vector<std::int16_t> resample_stereo_cubic(const std::int16_t* input, std::size_t frames,
                                                 double source_rate, int output_rate,
                                                 double& source_position, double rate_adjustment) {
    std::vector<std::int16_t> out;
    resample_stereo_cubic_into(input, frames, source_rate, output_rate, source_position, out,
                               rate_adjustment);
    return out;
}

} // namespace dsr::frontend_detail
