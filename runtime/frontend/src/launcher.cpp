#include "dsr/frontend.h"
#include "dsr/host_path.h"
#include "frontend_logic.h"
#include "controller_support.h"
#include "ui_toolkit.h"
#include "ui_screenshot_capture.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dsr {
namespace {
using namespace ui_detail;
constexpr UiColor kBg{9, 14, 23, 255};
constexpr UiColor kWhite{239, 244, 251, 255};
constexpr UiColor kMuted{158, 175, 195, 255};
constexpr int kW = 1024;
constexpr int kTabPitch = 186; // five tabs across the 928 px panel
constexpr int kH = 720;
constexpr UiRect kLaunchButton{758, 642, 218, 50};
constexpr std::array<std::string_view, 5> kPages{"LANGUAGE", "CONTROLS", "ROM / SAVE", "OPTIONS", "HOTKEYS"};
constexpr int kHotkeyPage = 4;
constexpr int kOptionCount = 16;
constexpr int kOptionPitch = 29;
constexpr int kHotkeyPitch = 30;
// Keyboard hotkeys handled by the SDL frontend outside the DS button map.
struct HotkeyRow { std::string_view name; int frontend_detail::ConfigData::*field; };
constexpr std::array<HotkeyRow, 12> kHotkeyRows{{
    {"CYCLE SCREEN LAYOUT", &frontend_detail::ConfigData::key_layout},
    {"SWAP SCREENS", &frontend_detail::ConfigData::key_swap},
    {"TOGGLE FULLSCREEN", &frontend_detail::ConfigData::key_fullscreen},
    {"TOGGLE LINEAR FILTER", &frontend_detail::ConfigData::key_filter},
    {"TOGGLE INTEGER SCALING", &frontend_detail::ConfigData::key_integer},
    {"CLOSE / OPEN LID", &frontend_detail::ConfigData::key_lid},
    {"FAST-FORWARD (HOLD)", &frontend_detail::ConfigData::key_fast_forward},
    {"SCREENSHOT", &frontend_detail::ConfigData::key_screenshot},
    {"CYCLE HI-RES 3D SCALE (1X-4X)", &frontend_detail::ConfigData::key_gpu3d_scale},
    {"VOLUME DOWN (10%)", &frontend_detail::ConfigData::key_volume_down},
    {"VOLUME UP (10%)", &frontend_detail::ConfigData::key_volume_up},
    {"MUTE / UNMUTE", &frontend_detail::ConfigData::key_mute},
}};
constexpr int kHotkeyCount = static_cast<int>(kHotkeyRows.size());
constexpr std::array<std::string_view, 12> kDsNames{"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
constexpr std::array<std::string_view, 5> kLanguages{"en", "fr", "de", "es", "it"};

int language_index(std::string_view code) {
    for (std::size_t i = 0; i < kLanguages.size(); ++i) if (kLanguages[i] == code) return static_cast<int>(i);
    return 0;
}
std::string language_name(std::string_view code) {
    if (code == "en") return "English";
    if (code == "fr") return "French";
    if (code == "de") return "German";
    if (code == "es") return "Spanish";
    return "Italian";
}
std::vector<std::pair<std::string, std::string>> connected_controllers() {
    std::vector<std::pair<std::string, std::string>> out;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        char guid[64]{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, static_cast<int>(sizeof(guid)));
        const char* name = SDL_GameControllerNameForIndex(i);
        out.emplace_back(guid, name ? name : "GameController");
    }
    return out;
}
std::filesystem::path launcher_rom_path(const std::filesystem::path& path) {
    char* base = SDL_GetBasePath();
    const auto result = resolve_rom_path(path, default_user_paths().config_dir,
                                         host::path_from_utf8(base ? base : ""));
    SDL_free(base);
    return result;
}
std::filesystem::path detect_rom() {
#if defined(_WIN32)
    const wchar_t* env = _wgetenv(L"DQ9_ROM");
#else
    const char* env = std::getenv("DQ9_ROM");
#endif
    if (env && *env) {
        const auto path = launcher_rom_path(env);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) return path;
    }
    // Resolve a directory via a known repository marker, then inspect its ROM
    // folder. Installed builds still inspect cwd and the config folder.
    const auto marker = launcher_rom_path("tools/run_sdl.sh");
    const auto repo = marker.is_absolute() ? marker.parent_path().parent_path() : std::filesystem::path{};
    for (const auto& relative : {std::filesystem::path("ROM"), std::filesystem::path("rom")}) {
        for (const auto& folder : {relative, default_user_paths().config_dir / relative, repo / relative}) {
            std::error_code ec;
            if (!std::filesystem::is_directory(folder, ec)) continue;
            for (const auto& item : std::filesystem::directory_iterator(folder, ec)) {
                if (ec) break;
                if (item.is_regular_file(ec) && item.path().extension() == ".nds") return item.path();
            }
        }
    }
    return {};
}
std::string key_name(int scancode) {
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES) return "UNBOUND";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name && *name ? name : "UNBOUND";
}
std::string gamepad_name(int button) {
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) return "UNBOUND";
    const char* name = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(button));
    return name && *name ? name : "UNBOUND";
}
void write_config(const UserPaths& paths, const frontend_detail::ConfigData& config) {
    std::error_code ec;
    std::filesystem::create_directories(paths.config_dir, ec);
    std::ofstream out(paths.config_dir / "frontend.conf", std::ios::trunc);
    if (out) out << frontend_detail::serialize_config(config);
}

class Launcher {
public:
    Launcher(SDL_Renderer* renderer, frontend_detail::ConfigData& config)
        : canvas_(renderer), config_(config) { refresh_controllers(); }
    ~Launcher() {
        for (auto* controller : controllers_) SDL_GameControllerClose(controller);
    }

    void render_capture(int page, bool rebinding) {
        page_ = page;
        focus_ = 0;
        pending_bind_ = rebinding ? 0 : -1;
        bind_side_ = 0;
        text_field_ = -1;
        if (page == 2) {
            config_.rom_path = "/tmp/dq9-test-home/games/synthetic-release/very-long-synthetic-image-name.nds";
            config_.save_path = "/tmp/dq9-test-home/saves/very-long-synthetic-session-name.sav";
        }
        draw();
    }

    bool run() {
        bool running = true;
        bool accepted = false;
        SDL_StartTextInput();
        draw();
        const char* inject = std::getenv("DSR_LAUNCHER_INJECT");
        if (inject && *inject && *inject != '0') {
            const auto key_tap = [](SDL_Scancode scancode) {
                bool success = true;
                for (const Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
                    SDL_Event event{};
                    event.type = type;
                    event.key.type = type;
                    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
                    event.key.keysym.scancode = scancode;
                    event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
                    if (SDL_PushEvent(&event) != 1) success = false;
                }
                return success;
            };
            bool queued = true;
            if (!config_.debug_tool) {
                queued = key_tap(SDL_SCANCODE_F4);
                for (int i = 0; i < 6 && queued; ++i) queued = key_tap(SDL_SCANCODE_DOWN);
                if (queued) queued = key_tap(SDL_SCANCODE_RETURN);
            }
            if (queued) queued = key_tap(SDL_SCANCODE_F5);
            if (!queued) std::fprintf(stderr, "[launcher-test] could not queue launcher key sequence\n");
        }
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) { running = false; break; }
                if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED) {
                    refresh_controllers();
                    continue;
                }
                if (event.type == SDL_TEXTINPUT && text_field_ >= 0) {
                    text_buffer(text_field_).insert(event.text.text);
                    continue;
                }
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    if (pending_bind_ >= 0) {
                        if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) pending_bind_ = -1;
                        else if (page_ == kHotkeyPage && pending_bind_ < kHotkeyCount) config_.*kHotkeyRows[static_cast<std::size_t>(pending_bind_)].field = event.key.keysym.scancode;
                        else if (bind_side_ == 0) config_.keycodes[static_cast<std::size_t>(pending_bind_)] = event.key.keysym.scancode;
                        pending_bind_ = -1;
                        continue;
                    }
                    if (text_field_ >= 0 && edit_text_key(event.key.keysym)) continue;
                    if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        if (text_field_ >= 0) { text_field_ = -1; SDL_StopTextInput(); }
                        else running = false;
                    } else if (event.key.keysym.scancode == SDL_SCANCODE_F5) { accepted = true; running = false; }
                    else if (event.key.keysym.scancode >= SDL_SCANCODE_F1 && event.key.keysym.scancode <= SDL_SCANCODE_F4) {
                        page_ = static_cast<int>(event.key.keysym.scancode - SDL_SCANCODE_F1);
                        focus_ = 0;
                    } else if (event.key.keysym.scancode == SDL_SCANCODE_TAB) {
                        page_ = (page_ + 1) % static_cast<int>(kPages.size());
                        focus_ = 0;
                    }
                    else if (event.key.keysym.scancode == SDL_SCANCODE_UP) move_focus(-1);
                    else if (event.key.keysym.scancode == SDL_SCANCODE_DOWN) move_focus(1);
                    else if (event.key.keysym.scancode == SDL_SCANCODE_LEFT) adjust(-1);
                    else if (event.key.keysym.scancode == SDL_SCANCODE_RIGHT) adjust(1);
                    else if (event.key.keysym.scancode == SDL_SCANCODE_RETURN || event.key.keysym.scancode == SDL_SCANCODE_KP_ENTER)
                        activate();
                } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                    if (pending_bind_ >= 0 && bind_side_ == 1) {
                        config_.gamepad_buttons[static_cast<std::size_t>(pending_bind_)] = event.cbutton.button;
                        pending_bind_ = -1;
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                        if (text_field_ >= 0) { text_field_ = -1; SDL_StopTextInput(); }
                        else running = false;
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START) { accepted = true; running = false; }
                    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) move_focus(-1);
                    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) move_focus(1);
                    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) adjust(-1);
                    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) adjust(1);
                    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A) activate();
                } else if (event.type == SDL_CONTROLLERAXISMOTION && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    if (event.caxis.value < -18000) move_focus(-1);
                    else if (event.caxis.value > 18000) move_focus(1);
                } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    mouse_click(event.button.x, event.button.y, accepted, running);
                }
            }
            draw();
            SDL_Delay(12);
        }
        SDL_StopTextInput();
        return accepted;
        
    }
private:
    void refresh_controllers() {
        for (auto* controller : controllers_) SDL_GameControllerClose(controller);
        controllers_.clear();
        devices_ = connected_controllers();
        // Listing devices is not enough: SDL generates GameController events
        // only while a GameController handle is open.
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                if (auto* controller = SDL_GameControllerOpen(i)) controllers_.push_back(controller);
            }
        }
    }
    TextBuffer& text_buffer(int field) { return field == 0 ? rom_text_ : save_text_; }
    void move_focus(int delta) {
        if (text_field_ >= 0) return;
        if (page_ == 0) { adjust(delta); return; } // the language page is a single choice list
        const int count = page_ == 0 ? 1 : page_ == 1 ? 13 : page_ == 2 ? 3 : page_ == kHotkeyPage ? kHotkeyCount + 1 : kOptionCount;
        focus_ = (focus_ + delta + count) % count;
    }
    void adjust(int delta) {
        if (page_ == 0) {
            const int index = (language_index(config_.language) + delta + 5) % 5;
            config_.language = std::string(kLanguages[static_cast<std::size_t>(index)]);
        } else if (page_ == 1 && focus_ < 12) {
            bind_side_ = (bind_side_ + 1) % 2;
        } else if (page_ == 3) {
            switch (focus_) {
            case 0: config_.frontend.layout = static_cast<ScreenLayout>((static_cast<int>(config_.frontend.layout) + delta + 6) % 6); break;
            case 1: config_.frontend.window_scale = std::clamp(config_.frontend.window_scale + delta, 1, 8); break;
            case 5: config_.frontend.audio_buffer_ms = std::clamp(config_.frontend.audio_buffer_ms + delta * 20, 20, 500); break;
            case 9: config_.frontend.stick_deadzone = std::clamp(config_.frontend.stick_deadzone + delta * 1000, 0, 30000); break;
            case 11: cycle_controller(delta); break;
            case 14: config_.frontend.volume = std::clamp(config_.frontend.volume + delta * 10, 0, 100); break;
            case 13: config_.frontend.gpu3d_scale = (config_.frontend.gpu3d_scale - 1 + delta + 4) % 4 + 1; break;
            default: toggle_option(); break;
            }
        } else if (page_ == 2 && focus_ == 2) detect_paths();
    }
    void activate() {
        if (page_ == 0) adjust(1);
        else if (page_ == 1) {
            if (focus_ == 12) reset_controls();
            else { pending_bind_ = focus_; bind_side_ = bind_side_ % 2; }
        } else if (page_ == 2) {
            if (focus_ < 2) {
                text_field_ = focus_;
                text_buffer(text_field_).set(focus_ == 0 ? config_.rom_path : config_.save_path);
                SDL_StartTextInput();
            } else detect_paths();
        } else if (page_ == kHotkeyPage) {
            if (focus_ == kHotkeyCount) reset_hotkeys();
            else { pending_bind_ = focus_; bind_side_ = 0; }
        } else if (page_ == 3) adjust(1);
    }
    void toggle_option() {
        switch (focus_) {
        case 2: config_.frontend.fullscreen = !config_.frontend.fullscreen; break;
        case 3: config_.frontend.integer_scaling = !config_.frontend.integer_scaling; break;
        case 4: config_.frontend.linear_filter = !config_.frontend.linear_filter; break;
        case 6: config_.debug_tool = !config_.debug_tool; break;
        case 7: config_.frame_time_overlay = !config_.frame_time_overlay; break;
        case 8: config_.capture_memory_dumps = !config_.capture_memory_dumps; break;
        case 10: config_.frontend.dpad_from_stick = !config_.frontend.dpad_from_stick; break;
        case 12: config_.frontend.mouse_touch_enabled = !config_.frontend.mouse_touch_enabled; break;
        case 15: config_.frontend.pause_when_unfocused = !config_.frontend.pause_when_unfocused; break;
        default: break;
        }
    }
    void reset_controls() {
        const auto defaults = frontend_detail::default_config();
        config_.keycodes = defaults.keycodes;
        config_.gamepad_buttons = defaults.gamepad_buttons;
        config_.frontend.keycodes = defaults.keycodes;
        config_.frontend.gamepad_buttons = defaults.gamepad_buttons;
    }
    void reset_hotkeys() {
        const auto defaults = frontend_detail::default_config();
        for (const auto& row : kHotkeyRows) config_.*row.field = defaults.*row.field;
    }
    void cycle_controller(int delta) {
        if (devices_.empty()) { config_.frontend.controller_guid.clear(); return; }
        std::size_t current = 0;
        for (std::size_t i = 0; i < devices_.size(); ++i)
            if (devices_[i].first == config_.frontend.controller_guid) current = i;
        const auto n = static_cast<int>(devices_.size());
        const auto next = static_cast<std::size_t>((static_cast<int>(current) + delta + n) % n);
        config_.frontend.controller_guid = devices_[next].first;
    }
    void detect_paths() {
        const auto found = detect_rom();
        if (!found.empty()) { config_.rom_path = host::path_to_utf8(found); rom_text_.set(config_.rom_path); }
        if (config_.save_path.empty()) config_.save_path = host::path_to_utf8(default_user_paths().data_dir / "dq9.sav");
        save_text_.set(config_.save_path);
    }
    bool edit_text_key(const SDL_Keysym& key) {
        if (key.scancode == SDL_SCANCODE_RETURN || key.scancode == SDL_SCANCODE_KP_ENTER) {
            const int field = text_field_;
            const std::string value = text_buffer(field).value();
            if (field == 0) config_.rom_path = value; else config_.save_path = value;
            text_field_ = -1;
            SDL_StopTextInput();
            return true;
        }
        if (key.scancode == SDL_SCANCODE_ESCAPE) { text_field_ = -1; SDL_StopTextInput(); return true; }
        TextBuffer& value = text_buffer(text_field_);
        if (key.scancode == SDL_SCANCODE_BACKSPACE) value.backspace();
        else if (key.scancode == SDL_SCANCODE_DELETE) value.erase_forward();
        else if (key.scancode == SDL_SCANCODE_LEFT) value.move_cursor(-1);
        else if (key.scancode == SDL_SCANCODE_RIGHT) value.move_cursor(1);
        else if (key.scancode == SDL_SCANCODE_HOME) value.home();
        else if (key.scancode == SDL_SCANCODE_END) value.end();
        return true;
    }
    void mouse_click(int x, int y, bool& accepted, bool& running) {
        if (y >= 76 && y < 128) {
            const int tab = (x - 48) / kTabPitch;
            if (x >= 48 && tab >= 0 && tab < static_cast<int>(kPages.size())) {
                page_ = tab;
                focus_ = 0;
                pending_bind_ = -1; // a half-finished rebind must not follow us to another page
            }
        }
        if (y >= kLaunchButton.y && y < kLaunchButton.y + kLaunchButton.h &&
            x >= kLaunchButton.x && x < kLaunchButton.x + kLaunchButton.w) {
            accepted = true; running = false; return;
        }
        if (page_ == 0 && y >= 250 && y < 250 + 5 * 50) {
            config_.language = std::string(kLanguages[static_cast<std::size_t>((y - 250) / 50)]);
            return;
        }
        if (page_ == 1 && y >= 172 && y < 172 + 12 * 34) {
            const int row = (y - 172) / 34;
            focus_ = row;
            bind_side_ = x < 535 ? 0 : 1;
            pending_bind_ = row;
            return;
        }
        if (page_ == 1 && y >= 580 && y < 612) { focus_ = 12; reset_controls(); return; }
        if (page_ == kHotkeyPage && y >= 172 && y < 172 + kHotkeyCount * kHotkeyPitch) {
            focus_ = (y - 172) / kHotkeyPitch;
            pending_bind_ = focus_;
            bind_side_ = 0;
            return;
        }
        if (page_ == kHotkeyPage && y >= 172 + kHotkeyCount * kHotkeyPitch + 12 && y < 172 + kHotkeyCount * kHotkeyPitch + 44) {
            focus_ = kHotkeyCount;
            reset_hotkeys();
            return;
        }
        if (page_ == 2 && y >= 186 && y < 244) { focus_ = 0; activate(); return; }
        if (page_ == 2 && y >= 298 && y < 356) { focus_ = 1; activate(); return; }
        if (page_ == 2 && y >= 405 && y < 465) { focus_ = 2; detect_paths(); return; }
        if (page_ == 3 && y >= 147 && y < 147 + kOptionCount * kOptionPitch) {
            focus_ = (y - 147) / kOptionPitch;
            if (focus_ == 2 || focus_ == 3 || focus_ == 4 || focus_ == 6 || focus_ == 7 ||
                focus_ == 8 || focus_ == 10 || focus_ == 12 || focus_ == 15)
                toggle_option();
            else adjust(focus_ == 14 && x < 684 ? -1 : 1); // volume: left of the value lowers it
        }
    }
    void draw() {
        canvas_.clear(kBg);
        canvas_.text(48, 28, "DQ9-Decomp - LAUNCHER", kWhite, 3);
        canvas_.text(48, 61, "SET UP YOUR SESSION. CHOOSE A PAGE, THEN LAUNCH.", kMuted, 1);
        for (int i = 0; i < static_cast<int>(kPages.size()); ++i)
            canvas_.button({48 + i * kTabPitch, 78, kTabPitch - 12, 44}, kPages[static_cast<std::size_t>(i)], page_ == i);
        canvas_.panel({48, 140, 928, 476});
        if (page_ == 0) draw_language();
        else if (page_ == 1) draw_controls();
        else if (page_ == 2) draw_paths();
        else if (page_ == kHotkeyPage) draw_hotkeys();
        else draw_options();
        canvas_.text(48, 633, "F1-F4 PAGES   TAB NEXT PAGE (5: HOTKEYS)   ARROWS NAVIGATE   ENTER SELECT   ESC CANCEL", kMuted, 1);
        draw_rom_status();
        canvas_.button(kLaunchButton, "LAUNCH (F5)", false);
        const char* capture_path = std::getenv("DSR_LAUNCHER_CAPTURE");
        if (!capture_done_ && capture_path && *capture_path) {
            capture_done_ = true;
            const std::filesystem::path path{capture_path};
            std::error_code ec;
            if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
            int width = 0, height = 0;
            const bool saved = !ec && SDL_GetRendererOutputSize(canvas_.renderer(), &width, &height) == 0 &&
                width > 0 && height > 0 && save_renderer_png(canvas_.renderer(), path, width, height);
            std::fprintf(stderr, "[launcher-render-capture] %s%s\n",
                         saved ? "" : "failed: ", capture_path);
        }
        SDL_RenderPresent(canvas_.renderer());
    }
    void draw_language() {
        canvas_.text(86, 174, "CHOOSE THE LANGUAGE USED BY THE GAME.", kWhite, 2);
        canvas_.text(86, 220, "LANGUAGE", kMuted, 1);
        for (int i = 0; i < 5; ++i) {
            const bool selected = language_index(config_.language) == i;
            canvas_.button({86, 250 + i * 50, 852, 42}, language_name(kLanguages[static_cast<std::size_t>(i)]) +
                           (selected ? "   (selected)" : ""), selected);
        }
        canvas_.text(86, 522, "CLICK A LANGUAGE, OR USE UP/DOWN/LEFT/RIGHT OR THE D-PAD.", kMuted, 1);
        canvas_.text(86, 550, "FIRST RUN USES DEFAULT KEYS, VERTICAL SCREENS, AND AUTOMATIC ROM DETECTION.", kMuted, 1);
    }
    void draw_controls() {
        canvas_.text(72, 151, "DS BUTTON", kMuted, 1);
        canvas_.text(270, 151, "KEYBOARD (CLICK / ENTER TO REBIND)", kMuted, 1);
        canvas_.text(618, 151, "CONTROLLER (CLICK / ENTER TO REBIND)", kMuted, 1);
        for (int i = 0; i < 12; ++i) {
            const int y = 172 + i * 34;
            const bool selected = focus_ == i;
            canvas_.fill({68, y, 872, 29}, selected ? UiColor{33, 70, 103, 255} : UiColor{20, 28, 42, 255});
            canvas_.text(78, y + 7, kDsNames[static_cast<std::size_t>(i)], kWhite, 1);
            const auto kb = key_name(config_.keycodes[static_cast<std::size_t>(i)]);
            const auto pad = gamepad_name(config_.gamepad_buttons[static_cast<std::size_t>(i)]);
            const UiColor key_color = selected && bind_side_ == 0 ? UiColor{134, 216, 255, 255} : kWhite;
            const UiColor pad_color = selected && bind_side_ == 1 ? UiColor{134, 216, 255, 255} : kWhite;
            canvas_.text(270, y + 7, pending_bind_ == i && bind_side_ == 0 ? "PRESS A KEY..." : kb, key_color, 1);
            canvas_.text(618, y + 7, pending_bind_ == i && bind_side_ == 1 ? "PRESS A BUTTON..." : pad, pad_color, 1);
        }
        canvas_.button({68, 580, 260, 32}, "RESET TO DEFAULTS", focus_ == 12);
        canvas_.text(360, 588, "STEAM INPUT CAN PRESENT AS AN XBOX-STYLE PAD.", kMuted, 1);
    }
    void draw_hotkeys() {
        canvas_.text(72, 151, "ACTION", kMuted, 1);
        canvas_.text(470, 151, "KEYBOARD (CLICK / ENTER TO REBIND)", kMuted, 1);
        for (int i = 0; i < kHotkeyCount; ++i) {
            const int y = 172 + i * kHotkeyPitch;
            const bool selected = focus_ == i;
            const auto& row = kHotkeyRows[static_cast<std::size_t>(i)];
            canvas_.fill({68, y, 872, kHotkeyPitch - 3}, selected ? UiColor{33, 70, 103, 255} : UiColor{20, 28, 42, 255});
            canvas_.text(78, y + 7, row.name, kWhite, 1);
            canvas_.text(470, y + 7, pending_bind_ == i ? "PRESS A KEY..." : key_name(config_.*row.field),
                         selected ? UiColor{134, 216, 255, 255} : kWhite, 1);
        }
        canvas_.button({68, 172 + kHotkeyCount * kHotkeyPitch + 12, 260, 32}, "RESET TO DEFAULTS", focus_ == kHotkeyCount);
        canvas_.text(360, 172 + kHotkeyCount * kHotkeyPitch + 20, "PAD: SELECT+L LAYOUT, SELECT+R3 SWAP, SELECT+R FAST-FORWARD, SELECT+START SHOT.", kMuted, 1);
    }
    void draw_paths() {
        canvas_.text(86, 162, "ROM IMAGE (.NDS)", kMuted, 1);
        const std::string rom_display = elide_middle(text_field_ == 0 ? rom_text_.value() : config_.rom_path, 56);
        const std::string save_display = elide_middle(text_field_ == 1 ? save_text_.value() : config_.save_path, 56);
        canvas_.button({86, 186, 852, 58}, rom_display, focus_ == 0, true);
        canvas_.text(86, 274, "SAVE FILE", kMuted, 1);
        canvas_.button({86, 298, 852, 58}, save_display, focus_ == 1, true);
        canvas_.button({86, 405, 360, 54}, "AUTO-DETECT ROM / ENV", focus_ == 2);
        canvas_.text(86, 490, "ROM PATH MAY ALSO COME FROM THE DQ9_ROM ENVIRONMENT VARIABLE.", kMuted, 1);
        canvas_.text(86, 518, "THE DEFAULT SAVE IS <USER DATA DIRECTORY>/dq9.sav.", kMuted, 1);
        if (text_field_ >= 0) canvas_.text(86, 566, "EDIT TEXT; ENTER TO ACCEPT; ESC TO CANCEL.", UiColor{134, 216, 255, 255}, 1);
    }
    // One-line readiness check next to the Launch button, so a missing ROM is visible before
    // launching rather than as an error afterwards. Re-checked only when the path changes.
    void draw_rom_status() {
        const std::string& path = text_field_ == 0 ? rom_text_.value() : config_.rom_path;
        if (path != rom_checked_path_) {
            rom_checked_path_ = path;
            std::error_code ec;
            rom_found_ = !path.empty() &&
                std::filesystem::is_regular_file(launcher_rom_path(host::path_from_utf8(path)), ec);
        }
        if (rom_found_) {
            canvas_.text(48, 668, "ROM FOUND: " + elide_middle(path, 60), UiColor{120, 220, 140, 255}, 1);
        } else {
            canvas_.text(48, 660, rom_checked_path_.empty() ? "NO ROM SET." : "ROM NOT FOUND: " + elide_middle(rom_checked_path_, 56),
                         UiColor{255, 176, 96, 255}, 1);
            canvas_.text(48, 676, "OPEN THE ROM / SAVE PAGE (F3) TO SET ITS PATH OR AUTO-DETECT.", UiColor{255, 176, 96, 255}, 1);
        }
    }
    void draw_options() {
        const std::array<std::string, kOptionCount> values{
            layout_name(config_.frontend.layout),
            std::to_string(config_.frontend.window_scale) + "x",
            config_.frontend.fullscreen ? "ON" : "OFF",
            config_.frontend.integer_scaling ? "ON" : "OFF",
            config_.frontend.linear_filter ? "ON" : "OFF",
            std::to_string(config_.frontend.audio_buffer_ms) + " ms",
            config_.debug_tool ? "ON" : "OFF",
            config_.frame_time_overlay ? "ON" : "OFF",
            config_.capture_memory_dumps ? "ON" : "OFF",
            std::to_string(config_.frontend.stick_deadzone),
            config_.frontend.dpad_from_stick ? "ON" : "OFF",
            selected_controller(),
            config_.frontend.mouse_touch_enabled ? "ON" : "OFF",
            config_.frontend.gpu3d_scale == 1 ? std::string("OFF (native 3D)")
                                              : std::to_string(config_.frontend.gpu3d_scale) + "x (experimental)",
            std::to_string(config_.frontend.volume) + "%",
            config_.frontend.pause_when_unfocused ? "ON" : "OFF"
        };
        constexpr std::array<std::string_view, kOptionCount> labels{
            "Screen layout", "Window scale", "Fullscreen", "Integer scaling", "Linear filter",
            "Audio queue limit", "Debug annotation tool", "Frame-time overlay", "Capture RAM/VRAM dumps",
            "Stick dead zone", "D-pad from left stick", "Controller device", "Mouse/touch input",
            "Hi-res 3D scale", "Master volume", "Pause when window unfocused"
        };
        for (int i = 0; i < kOptionCount; ++i) {
            const int y = 147 + i * kOptionPitch;
            canvas_.fill({72, y, 872, kOptionPitch - 2}, focus_ == i ? UiColor{33, 70, 103, 255} : UiColor{20, 28, 42, 255});
            canvas_.text(86, y + 7, labels[static_cast<std::size_t>(i)], kWhite, 1);
            canvas_.text(684, y + 7, values[static_cast<std::size_t>(i)], UiColor{134, 216, 255, 255}, 1);
        }
    }
    static std::string layout_name(ScreenLayout layout) {
        constexpr std::array<std::string_view, 6> names{"Vertical", "Horizontal", "Top focus", "Bottom focus", "Top only", "Bottom only"};
        return std::string(names[static_cast<std::size_t>(layout)]);
    }
    std::string selected_controller() const {
        if (devices_.empty()) return "None connected";
        for (const auto& item : devices_) if (item.first == config_.frontend.controller_guid) return item.second;
        return devices_.front().second;
    }

    UiCanvas canvas_;
    frontend_detail::ConfigData& config_;
    std::vector<std::pair<std::string, std::string>> devices_;
    std::vector<SDL_GameController*> controllers_;
    int page_ = 0;
    int focus_ = 0;
    int bind_side_ = 0;
    int pending_bind_ = -1;
    TextBuffer rom_text_{4096};
    TextBuffer save_text_{4096};
    int text_field_ = -1;
    bool capture_done_ = false;
    std::string rom_checked_path_;
    bool rom_found_ = false;
};
} // namespace

bool run_sdl_launcher(LauncherOptions& options) {
    const UserPaths paths = default_user_paths();
    std::error_code ec;
    std::filesystem::create_directories(paths.config_dir, ec);
    frontend_detail::ConfigData config = frontend_detail::default_config();
    const auto config_file = paths.config_dir / "frontend.conf";
    if (std::ifstream in{config_file}; in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        config = frontend_detail::parse_config(text, config);
    } else {
        if (running_on_steam_deck()) {
            config.frontend.fullscreen = true;
            config.frontend.layout = ScreenLayout::TopFocus;
        }
    }
    if (!options.rom_path.empty()) config.rom_path = host::path_to_utf8(options.rom_path);
    if (!options.save_path.empty()) config.save_path = host::path_to_utf8(options.save_path);
    if (options.debug_tool) config.debug_tool = true;
    if (config.rom_path.empty()) {
        if (auto detected = detect_rom(); !detected.empty()) config.rom_path = host::path_to_utf8(detected);
    }
    if (config.save_path.empty()) config.save_path = host::path_to_utf8(paths.data_dir / "dq9.sav");
    frontend_detail::set_controller_hints();
#if defined(_WIN32)
    SDL_SetMainReady();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) return false;
    frontend_detail::load_controller_mappings(paths.config_dir);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window* window = SDL_CreateWindow("DQ9-Decomp Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          kW, kH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer && window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    bool accepted = false;
    if (renderer) {
        Launcher launcher(renderer, config);
        accepted = launcher.run();
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    if (!accepted) return false;
    if (config.rom_path.empty()) config.rom_path = host::path_to_utf8(detect_rom());
    if (config.save_path.empty()) config.save_path = host::path_to_utf8(paths.data_dir / "dq9.sav");
    config.rom_path = host::path_to_utf8(launcher_rom_path(host::path_from_utf8(config.rom_path)));
    config.frontend.keycodes = config.keycodes;
    config.frontend.gamepad_buttons = config.gamepad_buttons;
    config.frontend.debug_tool_enabled = config.debug_tool;
    config.frontend.frame_time_overlay_enabled = config.frame_time_overlay;
    write_config(paths, config);
    options.frontend = config.frontend;
    options.language = config.language;
    options.rom_path = host::path_from_utf8(config.rom_path);
    options.save_path = host::path_from_utf8(config.save_path);
    options.debug_tool = config.debug_tool;
    options.frame_time_overlay = config.frame_time_overlay;
    options.capture_memory_dumps = config.capture_memory_dumps;
    return true;
}

namespace ui_detail {
bool capture_launcher_screenshots(const std::filesystem::path& directory) {
#if defined(_WIN32)
    SDL_SetMainReady();
#endif
    if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) return false;
    bool success = true;
    constexpr std::array<std::array<int, 2>, 4> sizes{{{{1024, 720}}, {{1280, 800}}, {{800, 600}}, {{3072, 1920}}}};
    constexpr std::array<std::string_view, 5> pages{"language", "controls", "rom-save", "options", "hotkeys"};
    for (const auto& size : sizes) {
        const int width = size[0], height = size[1];
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!renderer) {
            if (surface) SDL_FreeSurface(surface);
            success = false;
            break;
        }
        SDL_RenderSetLogicalSize(renderer, kW, kH);
        auto config = frontend_detail::default_config();
        Launcher launcher(renderer, config);
        for (int page = 0; page < static_cast<int>(pages.size()); ++page) {
            launcher.render_capture(page, false);
            const auto path = directory / "launcher" /
                (std::to_string(width) + "x" + std::to_string(height)) /
                (std::string(pages[static_cast<std::size_t>(page)]) + ".png");
            success = save_renderer_png(renderer, path, width, height) && success;
            if (page == 1) {
                launcher.render_capture(page, true);
                const auto binding_path = directory / "launcher" /
                    (std::to_string(width) + "x" + std::to_string(height)) / "controls-rebinding.png";
                success = save_renderer_png(renderer, binding_path, width, height) && success;
            }
        }
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
        if (!success) break;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
    return success;
}
} // namespace ui_detail

} // namespace dsr
