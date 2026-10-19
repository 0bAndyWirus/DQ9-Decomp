#include "dsr/frontend.h"
 
#include "frontend_logic.h"
#include "controller_support.h"
#include "ui_toolkit.h"
#include "ui_screenshot_capture.h"


#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <mmsystem.h>
#endif

namespace {
dsr::frontend_detail::Rect debug_menu_rect(int width, int height) {
    const int w = std::min(560, std::max(360, width - 24));
    const int h = std::min(30 + static_cast<int>(dsr::kDebugMenuRowCount) * 22 + 12,
                           std::max(300, height - 24));
    return {(width - w) / 2, (height - h) / 2, w, h};
}
}

namespace dsr {
namespace {
constexpr int kScreenW = 256, kScreenH = 192;
constexpr double kFrameRate = 33513982.0 / 560190.0;
constexpr auto kFramePeriod = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / kFrameRate));
constexpr int kAudioDeviceBufferSamples = 512;
constexpr int kTargetAudioQueueMs = 40;
class SdlFrontend final : public Frontend {
public:
    ~SdlFrontend() override {
        for (SDL_GameController* gamepad : controllers_) SDL_GameControllerClose(gamepad);
        if (audio_metrics_enabled_) emit_audio_metrics(std::chrono::steady_clock::now(), true);
        if (audio_) SDL_CloseAudioDevice(audio_);
        for (auto* texture : textures_) if (texture) SDL_DestroyTexture(texture);
        for (auto* texture : texture_view_textures_) if (texture) SDL_DestroyTexture(texture);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
#if defined(_WIN32)
        if (precise_timer_) timeEndPeriod(1);
#endif
    }
    bool init(const FrontendConfig& requested) override {
        const char* metrics = std::getenv("DSR_AUDIO_METRICS");
        audio_metrics_enabled_ = metrics && *metrics && *metrics != '0';
        config_ = requested;
        paths_ = default_user_paths();
        std::error_code ec;
        std::filesystem::create_directories(paths_.config_dir, ec);
        const auto file = paths_.config_dir / "frontend.conf";
        if (std::ifstream in{file}; in) {
            std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            config_data_ = frontend_detail::parse_config(contents);
            // Caller-selected non-default options take precedence only through the API's config.
            config_data_.frontend.title = requested.title;
        } else {
            config_data_ = frontend_detail::default_config();
            config_data_.frontend = requested;
            if (running_on_steam_deck()) {
                // Deck's 1280x800 panel: prioritize the top LCD at 2x with the
                // bottom touchscreen centered below, preserving both 4:3 images.
                config_data_.frontend.fullscreen = true;
                config_data_.frontend.layout = ScreenLayout::TopFocus;
            }
            std::ofstream out(file);
            if (out) out << frontend_detail::serialize_config(config_data_);
        }
        config_ = config_data_.frontend;
        // The caller owns debug-session creation and prints its ON/OFF state.
        // Persisted launcher choices must not silently enable frontend-only
        // hotkeys when the direct CLI has not created a debug session.
        config_.debug_tool_enabled = requested.debug_tool_enabled;
        config_.frame_time_overlay_enabled = requested.frame_time_overlay_enabled;
        if (requested.gpu3d_scale_explicit) config_.gpu3d_scale = requested.gpu3d_scale;
        config_.gpu3d_scale = std::clamp(config_.gpu3d_scale, 1, 4);
        if (running_on_steam_deck()) config_.fullscreen = true;
        if (config_.debug_tool_enabled) {
            const char* inject = std::getenv("DSR_DEBUG_INJECT");
            if (inject && *inject && *inject != '0') {
                debug_injection_stage_ = 1;
                debug_injection_mode_ = inject;
            }
            if (const char* path = std::getenv("DSR_DEBUG_CAPTURE_BANNER"))
                debug_banner_capture_path_ = path;
            if (const char* path = std::getenv("DSR_DEBUG_CAPTURE_NOTE"))
                debug_note_capture_path_ = path;
        }
        frontend_detail::set_controller_hints();
#if defined(_WIN32)
        SDL_SetMainReady();
        if (!precise_timer_) precise_timer_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) return false;
        frontend_detail::load_controller_mappings(paths_.config_dir);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, config_.linear_filter ? "1" : "0");
        const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                             (config_.fullscreen ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
        const int scale = std::max(1, config_.window_scale);
        const int w = config_.fullscreen && running_on_steam_deck() ? 1280 : 256 * scale;
        const int h = config_.fullscreen && running_on_steam_deck() ? 800 : 384 * scale;
        window_ = SDL_CreateWindow(config_.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
        if (!window_) return false;
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) return false;
        SDL_RenderSetLogicalSize(renderer_, 0, 0);
        if (config_.integer_scaling) SDL_RenderSetIntegerScale(renderer_, SDL_TRUE);
        for (std::size_t i = 0; i < textures_.size(); ++i) {
            if (!create_screen_texture(i, 1)) return false;
        }
        SDL_AudioSpec want{};
        want.freq = config_.audio_rate; want.format = AUDIO_S16SYS; want.channels = 2;
        want.samples = static_cast<Uint16>(kAudioDeviceBufferSamples);
        audio_ = SDL_OpenAudioDevice(nullptr, 0, &want, &audio_spec_, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!audio_) return false;
        output_rate_ = audio_spec_.freq;
        if (audio_metrics_enabled_) {
            const char* driver = SDL_GetCurrentAudioDriver();
            std::fprintf(stderr,
                         "[audio-metrics] device driver=%s rate=%d samples=%u device_buffer_ms=%.2f "
                         "target_queue_ms=%d emergency_clear_ms=%d\n",
                         driver ? driver : "unknown", output_rate_, static_cast<unsigned>(audio_spec_.samples),
                         output_rate_ > 0 ? 1000.0 * audio_spec_.samples / output_rate_ : 0.0,
                         kTargetAudioQueueMs, std::max(config_.audio_buffer_ms, kTargetAudioQueueMs * 2));
        }
        for (int i = 0; i < SDL_NumJoysticks(); ++i) open_controller(i);
        select_controller();
        next_frame_ = std::chrono::steady_clock::now();
        initialized_ = true;
        if (audio_metrics_enabled_) {
            metric_window_started_ = true;
            metric_window_start_ = next_frame_;
            metric_last_frame_ = metric_window_start_;
        }
        return true;
    }
    void poll(InputState& state) override {
        // InputState survives between polls for pen/lid state. Buttons are a
        // fresh keyboard/controller snapshot, not a set of latched presses.
        state.buttons = 0;
        state.editor_events.clear();
        state.menu_actions.clear();
        state.editor_buttons = 0;
        state.fast_forward = false;
        state.screenshot_requested = false;
        state.menu_selection = debug_overlay_.menu_selection;
        if (config_.debug_tool_enabled) state.debug_actions = 0;
        bool annotation_route = debug_overlay_.annotation_open;
        bool menu_route = config_.debug_tool_enabled && debug_overlay_.menu_open;
        const auto push_editor_event = [&](EditorEventType type) -> EditorEvent& {
            state.editor_events.emplace_back();
            state.editor_events.back().type = type;
            return state.editor_events.back();
        };
        const auto push_editor_text = [&](const char* text) {
            const std::size_t length = std::strlen(text);
            for (std::size_t offset = 0; offset < length;) {
                EditorEvent& edit = push_editor_event(EditorEventType::Text);
                std::size_t size = std::min(edit.text.size(), length - offset);
                while (size > 0 && offset + size < length &&
                       (static_cast<unsigned char>(text[offset + size]) & 0xc0U) == 0x80U)
                    --size;
                if (size == 0) break;
                std::memcpy(edit.text.data(), text + offset, size);
                edit.text_size = static_cast<std::uint8_t>(size);
                offset += size;
            }
        };
        if (debug_injection_stage_ == 1 &&
            (debug_injection_mode_ != "mouse" ||
             (debug_overlay_.debug_tool_enabled && debug_present_count_ >= 2))) {
            bool queued = false;
            if (debug_injection_mode_ == "mouse") queued = push_debug_mouse_click();
            else if (debug_injection_mode_ == "shortcut")
                queued = push_debug_key_tap(SDL_SCANCODE_N, static_cast<Uint16>(KMOD_CTRL | KMOD_SHIFT));
            else if (debug_injection_mode_ == "overlay")
                queued = push_debug_key_tap(SDL_SCANCODE_F10);
            else queued = push_debug_key_tap(SDL_SCANCODE_F9);
            if (!queued) std::fprintf(stderr, "[debug-inject] could not queue debug trigger\n");
            debug_injection_stage_ = 2;
        } else if (debug_injection_stage_ == 3) {
            if (!push_debug_test_text())
                std::fprintf(stderr, "[debug-inject] could not queue note text\n");
            debug_injection_stage_ = 4;
        } else if (debug_injection_stage_ == 4) {
            if (!push_debug_key_tap(SDL_SCANCODE_F2))
                std::fprintf(stderr, "[debug-inject] could not queue F2 save\n");
            debug_injection_stage_ = 5;
        } else if (debug_injection_stage_ == 5 && !debug_overlay_.annotation_open &&
                   debug_overlay_.paused) {
            if (push_debug_key_tap(SDL_SCANCODE_SPACE)) debug_injection_stage_ = 0;
            else std::fprintf(stderr, "[debug-inject] could not queue resume\n");
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                state.quit_requested = true; // main loop exits and flushes the save
            }
            if (event.type == SDL_CONTROLLERDEVICEADDED) open_controller(event.cdevice.which);
            if (event.type == SDL_CONTROLLERDEVICEREMOVED) remove_controller(event.cdevice.which);
            if (config_.debug_tool_enabled && annotation_route) {
                if (event.type == SDL_TEXTINPUT) {
                    push_editor_text(event.text.text);
                } else if (event.type == SDL_KEYDOWN) {
                    const int key = event.key.keysym.scancode;
                    const bool repeat = event.key.repeat != 0;
                    const Uint16 modifiers = event.key.keysym.mod;
                    if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) {
                        if ((modifiers & KMOD_CTRL) != 0) {
                            if (!repeat) push_editor_event(EditorEventType::Save);
                        } else {
                            push_editor_event(EditorEventType::Newline);
                        }
                    } else if (key == SDL_SCANCODE_F2 && !repeat) {
                        push_editor_event(EditorEventType::Save);
                    } else if (key == SDL_SCANCODE_ESCAPE && !repeat) {
                        push_editor_event(EditorEventType::Cancel);
                    } else if (key == SDL_SCANCODE_BACKSPACE) {
                        push_editor_event(EditorEventType::Backspace);
                    } else if (key == SDL_SCANCODE_DELETE) {
                        push_editor_event(EditorEventType::Delete);
                    } else if (key == SDL_SCANCODE_LEFT) {
                        push_editor_event(EditorEventType::Left);
                    } else if (key == SDL_SCANCODE_RIGHT) {
                        push_editor_event(EditorEventType::Right);
                    } else if (key == SDL_SCANCODE_UP) {
                        push_editor_event(EditorEventType::Up);
                    } else if (key == SDL_SCANCODE_DOWN) {
                        push_editor_event(EditorEventType::Down);
                    } else if (key == SDL_SCANCODE_HOME) {
                        push_editor_event(EditorEventType::Home);
                    } else if (key == SDL_SCANCODE_END) {
                        push_editor_event(EditorEventType::End);
                    }
                }
                continue;
            }
            if (menu_route) {
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    const int key = event.key.keysym.scancode;
                    if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_F10)
                        state.menu_actions.push_back(static_cast<int>(dsr::DebugMenuItem::Close));
                    else if (key == SDL_SCANCODE_UP) state.menu_actions.push_back(-1);
                    else if (key == SDL_SCANCODE_DOWN) state.menu_actions.push_back(1);
                    else if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER)
                        state.menu_actions.push_back(static_cast<int>(dsr::DebugMenuItem::Count));
                } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    int width = 0, height = 0, window_w = 0, window_h = 0;
                    SDL_GetRendererOutputSize(renderer_, &width, &height);
                    SDL_GetWindowSize(window_, &window_w, &window_h);
                    const int x = window_w > 0 ? static_cast<int>(
                        static_cast<std::int64_t>(event.button.x) * width / window_w) : -1;
                    const int y = window_h > 0 ? static_cast<int>(
                        static_cast<std::int64_t>(event.button.y) * height / window_h) : -1;
                    const auto rect = debug_menu_rect(width, height);
                    if (x >= rect.x && x < rect.x + rect.w && y >= rect.y + 30 &&
                        y < rect.y + rect.h - 12) {
                        const int row = (y - rect.y - 30) / 22;
                        if (row >= 0 && row < static_cast<int>(dsr::kDebugMenuRowCount)) {
                            state.menu_selection = row;
                            state.menu_actions.push_back(
                                row + static_cast<int>(dsr::kDebugMenuRowCount) + 1);
                            state.menu_actions.push_back(static_cast<int>(dsr::DebugMenuItem::Count));
                        }
                    }
                }
                continue;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const int key = event.key.keysym.scancode;
                if (config_.debug_tool_enabled) {
                    const Uint16 modifiers = event.key.keysym.mod;
                    const bool ctrl = (modifiers & KMOD_CTRL) != 0;
                    const bool shift = (modifiers & KMOD_SHIFT) != 0;
                    const auto previous_actions = state.debug_actions;
                    // Route this event independently of earlier taps in this poll.
                    state.debug_actions = 0;
                    if (key == SDL_SCANCODE_F10) {
                        state.debug_actions |= kDebugToggleMenu;
                        menu_route = true;
                    }
                    else if (key == SDL_SCANCODE_PAUSE)
                        state.debug_actions |= kDebugTogglePause;
                    else if (key == SDL_SCANCODE_N &&
                             ctrl && shift) {
                        state.debug_actions |= kDebugAnnotate;
                        annotation_route = true;
                    } else if (key == SDL_SCANCODE_SPACE) state.debug_actions |= kDebugTogglePause;
                    else if (key == SDL_SCANCODE_F8) state.debug_actions |= kDebugSingleStep;
                    else if (key == SDL_SCANCODE_F2) state.debug_actions |= kDebugSlowMotion;
                    else if (key == SDL_SCANCODE_F3) state.debug_actions |= kDebugFastForward;
                    else if (key == SDL_SCANCODE_F9) {
                        state.debug_actions |= kDebugAnnotate;
                        annotation_route = true;
                    } else if (key == SDL_SCANCODE_F4) state.debug_actions |= kDebugMark;
                    else if (key == SDL_SCANCODE_F1) state.debug_actions |= kDebugToggleHelp;
                    else if (key == SDL_SCANCODE_D) state.debug_actions |= kDebugToggleDumps;
                    else if (key == SDL_SCANCODE_T) state.debug_actions |= kDebugToggleTextureInspector;
                    else if (key == SDL_SCANCODE_F5) state.debug_actions |= kDebugChannelUp;
                    else if (key == SDL_SCANCODE_F6) state.debug_actions |= kDebugChannelDown;
                    else if (key == SDL_SCANCODE_F7) state.debug_actions |= kDebugChannelMute;
                    else if (key == SDL_SCANCODE_F12) state.debug_actions |= kDebugChannelSolo;
                    else if (key == SDL_SCANCODE_G) state.debug_actions |= kDebugToggleFrameGraph;
                    else if (key == SDL_SCANCODE_B) state.debug_actions |= kDebugToggleAudioBar;
                    else if (key == SDL_SCANCODE_C) state.debug_actions |= kDebugToggleCpuPc;
                    else if (key == SDL_SCANCODE_V) state.debug_actions |= kDebugToggleTextureSlots;
                    else if (key == SDL_SCANCODE_M) state.debug_actions |= kDebugToggleAudioMixer;
                    else if (key == SDL_SCANCODE_S) state.debug_actions |= kDebugScreenshot;
                    else if (key == SDL_SCANCODE_O) state.debug_actions |= kDebugShowFolder;
                    const auto allowed = frontend_detail::allowed_debug_actions(
                        state.debug_actions, debug_overlay_.visible,
                        debug_overlay_.audio_mixer, ctrl, shift);
                    state.debug_actions = previous_actions | allowed;
                    // A key the debug tool consumed (e.g. F3 fast-forward, F4 mark) must not also fire
                    // the general hotkey bound to it (lid toggle, integer scale).
                    if (allowed != 0) continue;
                }
                if (annotation_route) {
                    state.debug_actions &= kDebugAnnotate;
                    continue;
                }
                if (key == config_data_.key_fullscreen || (key == SDL_SCANCODE_RETURN && (event.key.keysym.mod & KMOD_ALT))) toggle_fullscreen();
                else if (key == config_data_.key_layout) cycle_layout();
                else if (key == config_data_.key_swap) swap_screens();
                else if (key == config_data_.key_filter) toggle_filter();
                else if (key == config_data_.key_integer) {
                    config_.integer_scaling = !config_.integer_scaling;
                    SDL_RenderSetIntegerScale(renderer_, config_.integer_scaling ? SDL_TRUE : SDL_FALSE);
                    show_message(config_.integer_scaling ? "INTEGER SCALING: ON" : "INTEGER SCALING: OFF");
                }
                else if (key == config_data_.key_lid) state.lid_closed = !state.lid_closed;
                else if (key == config_data_.key_screenshot) state.screenshot_requested = true;
                else if (key == config_data_.key_mute) {
                    muted_ = !muted_;
                    show_message(muted_ ? "AUDIO: MUTED" : "AUDIO: " + std::to_string(config_.volume) + "%");
                } else if (key == config_data_.key_volume_down || key == config_data_.key_volume_up) {
                    config_.volume = std::clamp(config_.volume + (key == config_data_.key_volume_up ? 10 : -10), 0, 100);
                    muted_ = false;
                    show_message("VOLUME: " + std::to_string(config_.volume) + "%");
                }
                else if (key == config_data_.key_gpu3d_scale) {
                    config_.gpu3d_scale = config_.gpu3d_scale % 4 + 1;
                    show_message(config_.gpu3d_scale == 1 ? "HI-RES 3D: OFF"
                                                          : "HI-RES 3D: " + std::to_string(config_.gpu3d_scale) + "X");
                }
            }
            if (config_.debug_tool_enabled && event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT) {
                int width = 0, height = 0, window_w = 0, window_h = 0;
                SDL_GetRendererOutputSize(renderer_, &width, &height);
                SDL_GetWindowSize(window_, &window_w, &window_h);
                const int x = window_w > 0 ? static_cast<int>(
                    static_cast<std::int64_t>(event.button.x) * width / window_w) : -1;
                const int y = window_h > 0 ? static_cast<int>(
                    static_cast<std::int64_t>(event.button.y) * height / window_h) : -1;
                const auto control = frontend_detail::debug_control_at(x, y, width, height);
                if (debug_injection_mode_ == "mouse" && event.type == SDL_MOUSEBUTTONDOWN)
                    std::fprintf(stderr, "[debug-inject] mouse control=%d at output=%d,%d size=%d,%d\n",
                                 static_cast<int>(control), x, y, width, height);
                if (control != frontend_detail::DebugControl::None) {
                    if (!annotation_route) {
                        if (control == frontend_detail::DebugControl::Note) {
                            state.debug_actions |= kDebugAnnotate;
                            annotation_route = true;
                        } else if (control == frontend_detail::DebugControl::Pause) {
                            state.debug_actions |= kDebugTogglePause;
                        } else if (control == frontend_detail::DebugControl::Overlay) {
                            state.debug_actions |= kDebugToggleMenu;
                            menu_route = true;
                        }
                    }
                    continue;
                }
            }
            if (config_.mouse_touch_enabled && (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION || event.type == SDL_FINGERUP)) {
                int width = 0, height = 0; SDL_GetRendererOutputSize(renderer_, &width, &height);
                const int x = static_cast<int>(event.tfinger.x * width), y = static_cast<int>(event.tfinger.y * height);
                if (event.type == SDL_FINGERUP) state.pen_down = false;
                else state.pen_down = frontend_detail::map_touch(config_.layout, width, height, x, y, state.pen_x, state.pen_y, config_.integer_scaling);
            }
            if (config_.mouse_touch_enabled &&
                (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONUP)) {
                int width = 0, height = 0; SDL_GetRendererOutputSize(renderer_, &width, &height);
                // A resize and mouse event can arrive in the same poll.
                // Query current dimensions instead of the previous poll's cache.
                int ww = 0, wh = 0;
                SDL_GetWindowSize(window_, &ww, &wh);
                ww = std::max(1, ww);
                wh = std::max(1, wh);
                const int x = event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x;
                const int y = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
                const bool down = (event.type == SDL_MOUSEBUTTONDOWN) || (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK));
                if (event.type == SDL_MOUSEBUTTONUP) state.pen_down = false;
                else if (down) state.pen_down = frontend_detail::map_touch(config_.layout, width, height, x * width / ww, y * height / wh, state.pen_x, state.pen_y, config_.integer_scaling);
            }
        }
        if (debug_injection_stage_ == 2 && (state.debug_actions & kDebugAnnotate))
            debug_injection_stage_ = 3;
        int key_count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&key_count);
        if (!annotation_route && !menu_route) {
            const int ff_key = config_data_.key_fast_forward;
            if (ff_key > 0 && ff_key < key_count && keys[ff_key]) state.fast_forward = true;
            for (std::size_t i = 0; i < config_data_.keycodes.size(); ++i) {
                const int keycode = config_data_.keycodes[i];
                const bool debug_hotkey = config_.debug_tool_enabled &&
                    (keycode == SDL_SCANCODE_F10 || keycode == SDL_SCANCODE_PAUSE ||
                     keycode == SDL_SCANCODE_SPACE || keycode == SDL_SCANCODE_F8 ||
                     keycode == SDL_SCANCODE_F2 || keycode == SDL_SCANCODE_F3 ||
                     keycode == SDL_SCANCODE_F9 || keycode == SDL_SCANCODE_F4 ||
                     keycode == SDL_SCANCODE_F1 ||
                     (debug_overlay_.visible &&
                      (keycode == SDL_SCANCODE_D || keycode == SDL_SCANCODE_T ||
                       keycode == SDL_SCANCODE_G || keycode == SDL_SCANCODE_B ||
                       keycode == SDL_SCANCODE_C || keycode == SDL_SCANCODE_V ||
                       keycode == SDL_SCANCODE_M || keycode == SDL_SCANCODE_S ||
                       keycode == SDL_SCANCODE_O)) ||
                     (((debug_overlay_.visible && debug_overlay_.audio_mixer) ||
                       ((SDL_GetModState() & KMOD_CTRL) && (SDL_GetModState() & KMOD_SHIFT))) &&
                      (keycode == SDL_SCANCODE_F5 || keycode == SDL_SCANCODE_F6 ||
                       keycode == SDL_SCANCODE_F7 || keycode == SDL_SCANCODE_F12)));
                if (keycode >= 0 && keycode < key_count && !debug_hotkey && keys[keycode])
                    state.buttons |= static_cast<std::uint16_t>(1u << i);
            }
        }
        select_controller();
        if (controller_ && SDL_GameControllerGetAttached(controller_)) {
            for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
                if (SDL_GameControllerGetButton(controller_, static_cast<SDL_GameControllerButton>(button)))
                    state.buttons |= frontend_detail::configured_gamepad_mask(config_data_.gamepad_buttons, button);
            const Sint16 dx = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
            const Sint16 dy = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
            if (config_.dpad_from_stick) {
                if (dx < -config_.stick_deadzone) state.buttons |= kBtnLeft; else if (dx > config_.stick_deadzone) state.buttons |= kBtnRight;
                if (dy < -config_.stick_deadzone) state.buttons |= kBtnUp; else if (dy > config_.stick_deadzone) state.buttons |= kBtnDown;
            }
            const bool annotation_open = annotation_route;
            const bool select = (state.buttons & kBtnSelect) != 0;
            const bool left_shoulder =
                SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
            if (!annotation_open && select && left_shoulder && !select_l_held_) {
                cycle_layout();
                state.buttons &= static_cast<std::uint16_t>(~(kBtnSelect | kBtnL));
            }
            select_l_held_ = select && left_shoulder;
            const bool right_shoulder =
                SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
            // Select+R (held) is fast-forward. It replaces the former Select+R swap toggle,
            // which moved to Select+R3 so no pad combo has two meanings.
            if (!annotation_open && select && right_shoulder) {
                state.fast_forward = true;
                state.buttons &= static_cast<std::uint16_t>(~(kBtnSelect | kBtnR));
            }
            const bool r3_down =
                SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
            const bool select_r3 = select && r3_down;
            if (!annotation_open && select_r3 && !select_r3_held_) {
                swap_screens();
                state.buttons &= static_cast<std::uint16_t>(~kBtnSelect);
            }
            select_r3_held_ = select_r3;

            // Select+Start rising edge is a screenshot. The debug tool's separate 1 s
            // Select+Start hold (pause) can still follow in the same hold.
            const auto pad_down = [&](int button) {
                return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX &&
                       SDL_GameControllerGetButton(controller_, static_cast<SDL_GameControllerButton>(button)) != 0;
            };
            const int pad_select = config_data_.gamepad_buttons[2];
            const int pad_start = config_data_.gamepad_buttons[3];
            const bool select_start = pad_select != pad_start && pad_down(pad_select) && pad_down(pad_start);
            if (select_start && !select_start_held_ && !annotation_open) state.screenshot_requested = true;
            select_start_held_ = select_start;
            if (select_start && !annotation_open)
                state.buttons &= static_cast<std::uint16_t>(~(kBtnSelect | kBtnStart));

            if (!annotation_open && r3_down && !r3_pressed_ && !select) stylus_mode_ = !stylus_mode_;
            r3_pressed_ = r3_down;
            if (!annotation_open && stylus_mode_) {
                const Sint16 sx = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX);
                const Sint16 sy = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
                cursor_x_ = std::clamp(cursor_x_ + static_cast<int>(sx / 4096), 0, 255);
                cursor_y_ = std::clamp(cursor_y_ + static_cast<int>(sy / 4096), 0, 191);
                state.pen_x = cursor_x_;
                state.pen_y = cursor_y_;
                state.pen_down =
                    SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSTICK) ||
                    SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
            }
            if (config_.debug_tool_enabled) {
                const bool guide =
                    SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_GUIDE) != 0;
                const bool start =
                    SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_START) != 0;
                const bool menu_chord = guide && start;
                if (menu_chord && !menu_chord_held_ && !annotation_open) {
                    state.debug_actions |= kDebugToggleMenu;
                    menu_route = true;
                }
                menu_chord_held_ = menu_chord;
                std::uint32_t combos = 0;
                if (guide) {
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) combos |= 1u << 0;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) combos |= 1u << 1;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) combos |= 1u << 2;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_X)) combos |= 1u << 3;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_Y)) combos |= 1u << 4;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_UP)) combos |= 1u << 5;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) combos |= 1u << 6;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) combos |= 1u << 7;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_A)) combos |= 1u << 8;
                    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_B)) combos |= 1u << 9;
                    const std::uint32_t pressed = combos & ~controller_debug_combo_held_;
                    if (!annotation_open) {
                        if (pressed & (1u << 0)) state.debug_actions |= kDebugSingleStep;
                        if (pressed & (1u << 1)) state.debug_actions |= kDebugSlowMotion;
                        if (pressed & (1u << 2)) state.debug_actions |= kDebugFastForward;
                        if (pressed & (1u << 3)) state.debug_actions |= kDebugMark;
                        if (pressed & (1u << 4)) state.debug_actions |= kDebugAnnotate;
                        if (pressed & (1u << 5))
                            state.debug_actions |= debug_overlay_.audio_mixer ? kDebugChannelUp : kDebugToggleHelp;
                        if (pressed & (1u << 6))
                            state.debug_actions |= debug_overlay_.audio_mixer ? kDebugChannelDown : kDebugToggleTextureInspector;
                        if (pressed & (1u << 7)) state.debug_actions |= kDebugToggleAudioMixer;
                        if (debug_overlay_.visible && debug_overlay_.audio_mixer) {
                            if (pressed & (1u << 8)) state.debug_actions |= kDebugChannelMute;
                            if (pressed & (1u << 9)) state.debug_actions |= kDebugChannelSolo;
                        }
                    }
                    controller_debug_combo_held_ = combos;
                    if (guide && !annotation_open) state.buttons = 0;
                } else {
                    controller_debug_combo_held_ = 0;
                }
                const int select_button = config_data_.gamepad_buttons[2];
                const int start_button = config_data_.gamepad_buttons[3];
                const auto down = [&](int button) {
                    return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX &&
                           SDL_GameControllerGetButton(controller_, static_cast<SDL_GameControllerButton>(button)) != 0;
                };
                const bool pause_chord = select_button != start_button &&
                                         down(select_button) && down(start_button);
                const Uint32 now = SDL_GetTicks();
                if (pause_chord) {
                    if (!pause_chord_active_) {
                        pause_chord_active_ = true;
                        pause_chord_start_ = now;
                        pause_chord_fired_ = false;
                    }
                    if (!pause_chord_fired_ &&
                        static_cast<Uint32>(now - pause_chord_start_) >= 1000U) {
                        if (!annotation_open) state.debug_actions |= kDebugTogglePause;
                        pause_chord_fired_ = true;
                    }
                    if (!annotation_open)
                        state.buttons &= static_cast<std::uint16_t>(~(kBtnSelect | kBtnStart));
                } else {
                    pause_chord_active_ = false;
                    pause_chord_fired_ = false;
                }
            }
        } else if (config_.debug_tool_enabled) {
            menu_chord_held_ = false;
            controller_debug_combo_held_ = 0;
            pause_chord_active_ = false;
            pause_chord_fired_ = false;
        }
        fast_forward_active_ = state.fast_forward;
        if (menu_route) {
            state.buttons = 0;
            state.pen_down = false;
            if (controller_ && SDL_GameControllerGetAttached(controller_)) {
                std::uint16_t buttons = 0;
                if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons |= 1u;
                if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons |= 2u;
                if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons |= 4u;
                if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= 8u;
                if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_A)) buttons |= 16u;
                if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_B)) buttons |= 32u;
                const std::uint16_t pressed = static_cast<std::uint16_t>(buttons & ~menu_buttons_held_);
                if (pressed & 1u) state.menu_actions.push_back(-1);
                if (pressed & 2u) state.menu_actions.push_back(1);
                if (pressed & 4u) state.menu_actions.push_back(-1);
                if (pressed & 8u) state.menu_actions.push_back(1);
                if (pressed & 16u)
                    state.menu_actions.push_back(static_cast<int>(dsr::DebugMenuItem::Count));
                if (pressed & 32u)
                    state.menu_actions.push_back(static_cast<int>(dsr::DebugMenuItem::Close));
                menu_buttons_held_ = buttons;
            } else menu_buttons_held_ = 0;
        } else menu_buttons_held_ = 0;
        if (annotation_route) {
            constexpr std::uint16_t editor_button_mask =
                kBtnA | kBtnB | kBtnStart | kBtnUp | kBtnDown | kBtnLeft | kBtnRight;
            state.editor_buttons = static_cast<std::uint16_t>(state.buttons & editor_button_mask);
            state.buttons = 0;
            state.pen_down = false;
            state.debug_actions &= debug_overlay_.annotation_open ? 0u : kDebugAnnotate;
        }
    }
    void present(const std::uint32_t* top, const std::uint32_t* bottom) override {
        present_hires(top, 1, bottom, 1);
    }
    int requested_gpu3d_scale() const override { return config_.gpu3d_scale; }
    // (Re)creates screen texture `index` at `scale` times the native size. SDL stretches it
    // into the same layout rectangle, so a larger texture only adds source resolution.
    bool create_screen_texture(std::size_t index, int scale) {
        if (textures_[index]) SDL_DestroyTexture(textures_[index]);
        textures_[index] = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             kScreenW * scale, kScreenH * scale);
        if (!textures_[index]) return false;
        texture_scale_[index] = scale;
        SDL_SetTextureBlendMode(textures_[index], SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(textures_[index], config_.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        return true;
    }
    void present_hires(const std::uint32_t* top, int top_scale, const std::uint32_t* bottom, int bottom_scale) override {
        if (!renderer_) return;
        const std::uint32_t* const sources[2] = {top, bottom};
        const int scales[2] = {std::clamp(top_scale, 1, 4), std::clamp(bottom_scale, 1, 4)};
        for (std::size_t i = 0; i < 2; ++i) {
            if (!sources[i]) continue;
            if (texture_scale_[i] != scales[i] && !create_screen_texture(i, scales[i])) return;
            SDL_UpdateTexture(textures_[i], nullptr, sources[i], kScreenW * scales[i] * static_cast<int>(sizeof(std::uint32_t)));
        }
        int width = 0, height = 0; SDL_GetRendererOutputSize(renderer_, &width, &height);
        const auto rects = frontend_detail::layout_rects(config_.layout, width, height, config_.integer_scaling);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255); SDL_RenderClear(renderer_);
        SDL_Rect top_rect{rects.top.x, rects.top.y, rects.top.w, rects.top.h};
        SDL_Rect bottom_rect{rects.bottom.x, rects.bottom.y, rects.bottom.w, rects.bottom.h};
        if (!swapped_) {
            if (rects.top.w) SDL_RenderCopy(renderer_, textures_[0], nullptr, &top_rect);
            if (rects.bottom.w) SDL_RenderCopy(renderer_, textures_[1], nullptr, &bottom_rect);
        } else {
            if (rects.top.w) SDL_RenderCopy(renderer_, textures_[1], nullptr, &top_rect);
            if (rects.bottom.w) SDL_RenderCopy(renderer_, textures_[0], nullptr, &bottom_rect);
        }
        if (stylus_mode_) {
            const SDL_Rect cursor{rects.bottom.x + cursor_x_ * rects.bottom.w / 256 - 3, rects.bottom.y + cursor_y_ * rects.bottom.h / 192 - 3, 7, 7};
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255); SDL_RenderDrawRect(renderer_, &cursor);
        }
        draw_debug_overlay();
        draw_toast();
        if (!debug_banner_capture_path_.empty() &&
            debug_overlay_.startup_banner_visible && !debug_banner_captured_) {
            debug_banner_captured_ = true;
            if (capture_live_debug_render(debug_banner_capture_path_))
                std::fprintf(stderr, "[debug-render-capture] %s\n", debug_banner_capture_path_.c_str());
            else
                std::fprintf(stderr, "[debug-render-capture] failed: %s\n", debug_banner_capture_path_.c_str());
        }
        if (!debug_note_capture_path_.empty() && debug_overlay_.annotation_open &&
            !debug_overlay_.annotation.empty() && !debug_note_captured_) {
            debug_note_captured_ = true;
            if (capture_live_debug_render(debug_note_capture_path_))
                std::fprintf(stderr, "[debug-render-capture] %s\n", debug_note_capture_path_.c_str());
            else
                std::fprintf(stderr, "[debug-render-capture] failed: %s\n", debug_note_capture_path_.c_str());
        }
        SDL_RenderPresent(renderer_);
        if (debug_injection_stage_ != 0 && debug_injection_mode_ == "mouse" &&
            debug_present_count_ < 2)
            ++debug_present_count_;
    }
    void set_debug_overlay(const DebugOverlayState& state) override {
        if (!config_.debug_tool_enabled && !config_.frame_time_overlay_enabled) return;
        if (state.annotation_open != debug_overlay_.annotation_open) {
            if (state.annotation_open) SDL_StartTextInput();
            else SDL_StopTextInput();
        }
        debug_overlay_ = state;
        const auto warning = frontend_detail::audio_debug_status(state.audio_solo, state.audio_muted);
        if (warning != audio_debug_warning_) {
            audio_debug_warning_ = warning;
            if (window_) SDL_SetWindowTitle(window_, warning.empty() ? config_.title.c_str() :
                (config_.title + " - " + warning).c_str());
        }
    }
    void diagnostics(FrontendDiagnostics& out) const override {
        SDL_version version{};
        SDL_GetVersion(&version);
        out.sdl_version = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                          std::to_string(version.patch);
        out.audio_driver = SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown";
        const char* device = SDL_GetAudioDeviceName(0, 0);
        out.audio_device = device ? device : "default/unknown";
        if (controller_) {
            const char* name = SDL_GameControllerName(controller_);
            out.controller_name = name ? name : "GameController";
            out.controller_guid = frontend_detail::controller_guid(controller_);
        }
        SDL_GetRendererOutputSize(renderer_, &out.width, &out.height);
        out.audio_backlog_frames = audio_backlog();
        out.dropped_audio_blocks = dropped_audio_blocks_;
        out.empty_audio_queue_events = empty_audio_queue_events_;
        out.queued_audio_blocks = queued_audio_blocks_;
    }
    void queue_audio(const std::int16_t* frames, std::size_t count, double source_rate) override {
        if (!audio_ || !frames || count == 0) return;
        std::size_t backlog = audio_backlog();
        if (audio_metrics_enabled_ || config_.debug_tool_enabled) {
            const auto metric_now = std::chrono::steady_clock::now();
            const bool starved = frontend_detail::audio_queue_starved(
                audio_device_started_, metric_has_audio_queue_time_, backlog,
                std::chrono::duration<double>(metric_now - metric_last_audio_queue_time_).count(),
                metric_available_audio_seconds_);
            if (config_.debug_tool_enabled && starved) ++empty_audio_queue_events_;
            if (audio_metrics_enabled_) {
                if (audio_device_started_ && backlog == 0) ++metric_queue_empty_samples_;
                if (starved) ++metric_underruns_;
                if (source_rate > 0.0) metric_produced_audio_ms_ += count * 1000.0 / source_rate;
            }
            metric_last_audio_queue_time_ = metric_now;
            metric_has_audio_queue_time_ = true;
        }
        const std::size_t emergency_ms =
            static_cast<std::size_t>(std::max(config_.audio_buffer_ms, kTargetAudioQueueMs * 2));
        const std::size_t limit = static_cast<std::size_t>(output_rate_) * emergency_ms / 1000;
        if (frontend_detail::audio_queue_needs_emergency_clear(backlog, limit)) {
            if (audio_metrics_enabled_ && output_rate_ > 0) {
                metric_dropped_audio_ms_ += 1000.0 * backlog / output_rate_;
                ++metric_clears_;
            }
            SDL_PauseAudioDevice(audio_, 1);
            SDL_ClearQueuedAudio(audio_);
            audio_device_started_ = frontend_detail::audio_device_started_after_clear();
            metric_has_audio_queue_time_ = false;
            if (config_.debug_tool_enabled) ++dropped_audio_blocks_;
            backlog = 0;
        }
        const std::size_t target = static_cast<std::size_t>(output_rate_) * kTargetAudioQueueMs / 1000;
        const std::size_t projected = backlog + frontend_detail::resampled_frame_count(
            static_cast<double>(count), source_rate, output_rate_);
        const double correction =
            frontend_detail::audio_resample_correction(projected, target, output_rate_);
        metric_audio_correction_ = correction;
        frontend_detail::resample_stereo_cubic_into(frames, count, source_rate, output_rate_,
                                                    source_position_, resampled_audio_, correction);
        // Mute queues silence rather than nothing so pacing and priming behave as when unmuted.
        frontend_detail::apply_volume_gain(resampled_audio_.data(), resampled_audio_.size(),
                                           frontend_detail::volume_gain_q8(config_.volume, muted_));
        const auto bytes = static_cast<Uint32>(resampled_audio_.size() * sizeof(std::int16_t));
        if (bytes) {
            SDL_QueueAudio(audio_, resampled_audio_.data(), bytes);
            if (config_.debug_tool_enabled) ++queued_audio_blocks_;
        }
        if (frontend_detail::audio_priming_should_start(audio_device_started_, audio_backlog(), target)) {
            SDL_PauseAudioDevice(audio_, 0);
            audio_device_started_ = true;
        }
        if ((audio_metrics_enabled_ || config_.debug_tool_enabled) && output_rate_ > 0) {
            metric_available_audio_seconds_ =
                static_cast<double>(audio_backlog() + audio_spec_.samples) / output_rate_;
        }
    }
    std::size_t audio_backlog() const override {
        return audio_ && output_rate_ > 0 ? SDL_GetQueuedAudioSize(audio_) / (2u * sizeof(std::int16_t)) : 0;
    }
    void clear_audio_queue() override {
        if (audio_) {
            if (audio_metrics_enabled_ && output_rate_ > 0) {
                metric_dropped_audio_ms_ += 1000.0 * audio_backlog() / output_rate_;
                ++metric_clears_;
                metric_available_audio_seconds_ =
                    static_cast<double>(audio_spec_.samples) / output_rate_;
            }
            SDL_ClearQueuedAudio(audio_);
            SDL_PauseAudioDevice(audio_, 1);
            audio_device_started_ = frontend_detail::audio_device_started_after_clear();
            metric_has_audio_queue_time_ = false;
        }
    }
    void pace_frame() override {
        if (!initialized_) return;
        const auto now = std::chrono::steady_clock::now();
        if (audio_metrics_enabled_) sample_frame_metrics(now);
        next_frame_ = frontend_detail::advance_frame_deadline(next_frame_, now, kFramePeriod);
        constexpr auto tail = std::chrono::microseconds(500);
        const auto sleep_deadline = next_frame_ - tail;
        if (now < sleep_deadline) std::this_thread::sleep_until(sleep_deadline);
        while (std::chrono::steady_clock::now() < next_frame_) std::this_thread::yield();
    }
    static bool capture_screenshots(const std::filesystem::path& directory) {
        constexpr std::array<std::array<int, 2>, 4> sizes{{{{768, 1152}}, {{1280, 800}}, {{800, 600}}, {{3072, 1920}}}};
        constexpr std::array<std::string_view, 6> names{
            "frame-time-graph", "help", "note-multiline", "note-keyboard-focus", "audio-mixer", "texture-inspector"};
        std::array<std::array<std::uint8_t, 0x20000>, 4> texture_bytes{};
        for (std::size_t slot = 0; slot < texture_bytes.size(); ++slot)
            for (std::size_t byte = 0; byte < texture_bytes[slot].size(); ++byte)
                texture_bytes[slot][byte] = static_cast<std::uint8_t>((byte * 13 + slot * 41) & 0xff);
        for (const auto& size : sizes) {
            const int width = size[0], height = size[1];
            SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
            SdlFrontend view;
            view.renderer_ = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
            if (!view.renderer_) {
                if (surface) SDL_FreeSurface(surface);
                return false;
            }
            view.config_.debug_tool_enabled = true;
            auto cleanup = [&] {
                for (auto* texture : view.texture_view_textures_)
                    if (texture) SDL_DestroyTexture(texture);
                view.texture_view_textures_.fill(nullptr);
                SDL_DestroyRenderer(view.renderer_);
                view.renderer_ = nullptr;
                SDL_FreeSurface(surface);
            };
            bool success = true;
            for (std::size_t index = 0; index < names.size(); ++index) {
                DebugOverlayState state;
                state.visible = true;
                state.paused = true;
                state.frame = 300 + index;
                state.fps = 58.7f;
                state.audio_backlog_ms = 82.4f;
                state.arm9_pc = 0x02100000; // synthetic screenshot fixture
                state.status = "Paused. F8 step; Space resumes.";
                state.frame_time_count = state.frame_times_ms.size();
                state.texture_slots = {texture_bytes[0].data(), texture_bytes[1].data(),
                                       texture_bytes[2].data(), texture_bytes[3].data()};
                for (std::size_t i = 0; i < state.frame_time_count; ++i)
                    state.frame_times_ms[i] = 12.4f + static_cast<float>((i * 7) % 17) * 0.82f;
                if (index == 1) state.help = true;
                if (index == 2 || index == 3) {
                    state.annotation_open = true;
                    state.annotation = "Texture flickers after entering the inn.\n"
                                       "Audio crackles when the map fades out.\n"
                                       "It happens again on the next scene transition.";
                    state.keyboard_selection = index == 2 ? 42 : 58;
                }
                if (index == 4) {
                    state.audio_mixer = true;
                    state.audio_channel_selection = 6;
                    state.audio_muted[2] = true;
                    state.audio_solo = 6;
                }
                if (index == 5) state.texture_inspector = true;
                view.debug_overlay_ = std::move(state);
                SDL_SetRenderDrawColor(view.renderer_, 8, 12, 18, 255);
                SDL_RenderClear(view.renderer_);
                view.draw_debug_overlay();
                SDL_RenderPresent(view.renderer_);
                const auto path = directory / "debug" /
                    (std::to_string(width) + "x" + std::to_string(height)) /
                    (std::string(names[index]) + ".png");
                success = ui_detail::save_renderer_png(view.renderer_, path, width, height);
                if (!success) break;
            }
            cleanup();
            if (!success) return false;
        }
        return true;
    }
private:
    bool push_debug_key_tap(SDL_Scancode scancode, Uint16 modifiers = 0) {
        bool success = true;
        for (const Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
            SDL_Event event{};
            event.type = type;
            event.key.windowID = window_ ? SDL_GetWindowID(window_) : 0;
            event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.key.keysym.scancode = scancode;
            event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
            event.key.keysym.mod = modifiers;
            if (SDL_PushEvent(&event) != 1) success = false;
        }
        return success;
    }
    bool push_debug_mouse_click() {
        if (!window_ || !renderer_) return false;
        int output_w = 0, output_h = 0, window_w = 0, window_h = 0;
        SDL_GetRendererOutputSize(renderer_, &output_w, &output_h);
        SDL_GetWindowSize(window_, &window_w, &window_h);
        const auto rect = frontend_detail::debug_control_rects(output_w, output_h).note;
        if (rect.w <= 0 || rect.h <= 0 || output_w <= 0 || output_h <= 0 ||
            window_w <= 0 || window_h <= 0) return false;
        SDL_Event event{};
        event.type = SDL_MOUSEBUTTONDOWN;
        event.button.windowID = SDL_GetWindowID(window_);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.state = SDL_PRESSED;
        event.button.timestamp = SDL_GetTicks();
        event.button.x = static_cast<Sint32>(
            static_cast<std::int64_t>(rect.x + rect.w / 2) * window_w / output_w);
        event.button.y = static_cast<Sint32>(
            static_cast<std::int64_t>(rect.y + rect.h / 2) * window_h / output_h);
        if (debug_injection_mode_ == "mouse")
            std::fprintf(stderr, "[debug-inject] click output=%dx%d window=%dx%d at=%d,%d\n",
                          output_w, output_h, window_w, window_h, event.button.x, event.button.y);
        if (SDL_PushEvent(&event) != 1) return false;
        event.type = SDL_MOUSEBUTTONUP;
        event.button.state = SDL_RELEASED;
        event.button.timestamp = SDL_GetTicks();
        return SDL_PushEvent(&event) == 1;
    }
    bool push_debug_test_text() {
        SDL_Event event{};
        event.type = SDL_TEXTINPUT;
        event.text.windowID = window_ ? SDL_GetWindowID(window_) : 0;
        constexpr std::string_view note = "SDL note input verified";
        std::copy(note.begin(), note.end(), event.text.text);
        event.text.text[note.size()] = '\0';
        return SDL_PushEvent(&event) == 1;
    }
    bool capture_live_debug_render(const std::string& path) {
        if (!renderer_ || path.empty()) return false;
        const std::filesystem::path file{path};
        std::error_code ec;
        if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
        if (ec) return false;
        int width = 0, height = 0;
        if (SDL_GetRendererOutputSize(renderer_, &width, &height) != 0 || width <= 0 || height <= 0)
            return false;
        return ui_detail::save_renderer_png(renderer_, file, width, height);
    }
    void draw_debug_controls(ui_detail::UiCanvas& canvas, int width, int height,
                             ui_detail::UiColor accent, ui_detail::UiColor muted) {
        const auto controls = frontend_detail::debug_control_rects(width, height);
        const frontend_detail::Rect rects[] = {controls.note, controls.pause, controls.overlay};
        constexpr std::string_view labels[] = {"NOTE", "PAUSE", "MENU"};
        const bool enabled = !debug_overlay_.annotation_open;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto rect = rects[i];
            if (rect.w <= 0 || rect.h <= 0) continue;
            const ui_detail::UiRect box{rect.x, rect.y, rect.w, rect.h};
            canvas.panel(box);
            canvas.outline(box, enabled ? accent : muted);
            const int label_width = canvas.text_width(labels[i], 1);
            const ui_detail::UiColor color = enabled ? ui_detail::UiColor{238, 244, 252, 255} : muted;
            canvas.text(rect.x + (rect.w - label_width) / 2,
                        rect.y + (rect.h - 7) / 2, labels[i], color, 1);
        }
    }
    void emit_audio_metrics(std::chrono::steady_clock::time_point now, bool force) {
        if (!audio_metrics_enabled_ || !metric_window_started_) return;
        const double seconds = std::chrono::duration<double>(now - metric_window_start_).count();
        if (seconds <= 0.0 || (!force && seconds < 1.0)) return;
        std::sort(metric_frame_times_us_.begin(), metric_frame_times_us_.begin() + metric_frame_time_count_);
        const double p50_us = metric_frame_time_count_
            ? metric_frame_times_us_[(metric_frame_time_count_ - 1) / 2] : 0.0;
        const double p95_us = metric_frame_time_count_
            ? metric_frame_times_us_[(metric_frame_time_count_ - 1) * 95 / 100] : 0.0;
        const double queue_avg_bytes = metric_queue_samples_
            ? static_cast<double>(metric_queue_bytes_sum_) / metric_queue_samples_ : 0.0;
        const double queue_ms = output_rate_ > 0 ? 1000.0 * audio_backlog() / output_rate_ : 0.0;
        const double device_ms = output_rate_ > 0
            ? 1000.0 * audio_spec_.samples / output_rate_ : 0.0;
        const double latency_ms = queue_ms + device_ms;
        const double played_audio_ms =
            std::max(0.0, metric_produced_audio_ms_ - metric_dropped_audio_ms_ - latency_ms);
        const double lag_ms = metric_produced_audio_ms_ - played_audio_ms;
        const double frame_min_ms = metric_frame_time_count_ ? metric_frame_time_min_us_ / 1000.0 : 0.0;
        const double frame_mean_ms = metric_frames_ ? metric_frame_time_sum_us_ / metric_frames_ / 1000.0 : 0.0;
        const double frame_max_ms = metric_frame_time_count_ ? metric_frame_time_max_us_ / 1000.0 : 0.0;
        const char* driver = SDL_GetCurrentAudioDriver();
        std::fprintf(stderr,
                     "[audio-metrics] elapsed_s=%.2f fps=%.2f frames=%zu "
                     "frame_ms_min=%.3f frame_ms_mean=%.3f frame_ms_p50=%.3f frame_ms_p95=%.3f frame_ms_max=%.3f "
                     "queue_bytes_min=%zu queue_bytes_avg=%.1f queue_bytes_max=%zu "
                     "queue_empty_samples=%u underruns=%u clears=%u "
                     "queue_ms=%.2f device_ms=%.2f latency_ms=%.2f lag_ms=%.2f "
                     "produced_audio_ms=%.1f played_audio_ms=%.1f dropped_audio_ms=%.1f "
                     "correction_ppm=%.0f driver=%s\n",
                     seconds, metric_frames_ / seconds, metric_frames_, frame_min_ms, frame_mean_ms,
                     p50_us / 1000.0, p95_us / 1000.0, frame_max_ms,
                     metric_queue_min_bytes_, queue_avg_bytes, metric_queue_max_bytes_,
                     metric_queue_empty_samples_, metric_underruns_, metric_clears_,
                     queue_ms, device_ms, latency_ms, lag_ms, metric_produced_audio_ms_, played_audio_ms,
                     metric_dropped_audio_ms_, metric_audio_correction_ * 1'000'000.0,
                     driver ? driver : "unknown");
        metric_window_start_ = now;
        metric_frames_ = 0;
        metric_frame_time_count_ = 0;
        metric_frame_time_sum_us_ = 0.0;
        metric_frame_time_min_us_ = 1.0e30;
        metric_frame_time_max_us_ = 0.0;
        metric_queue_min_bytes_ = 0;
        metric_queue_max_bytes_ = 0;
        metric_queue_bytes_sum_ = 0;
        metric_queue_samples_ = 0;
        metric_queue_empty_samples_ = 0;
        metric_underruns_ = 0;
        metric_clears_ = 0;
    }
    void sample_frame_metrics(std::chrono::steady_clock::time_point now) {
        const double frame_us = std::chrono::duration<double, std::micro>(now - metric_last_frame_).count();
        metric_last_frame_ = now;
        ++metric_frames_;
        metric_frame_time_sum_us_ += frame_us;
        metric_frame_time_min_us_ = std::min(metric_frame_time_min_us_, frame_us);
        metric_frame_time_max_us_ = std::max(metric_frame_time_max_us_, frame_us);
        if (metric_frame_time_count_ < metric_frame_times_us_.size())
            metric_frame_times_us_[metric_frame_time_count_++] = frame_us;
        const std::size_t queued_bytes = SDL_GetQueuedAudioSize(audio_);
        if (metric_queue_samples_ == 0) metric_queue_min_bytes_ = queued_bytes;
        metric_queue_min_bytes_ = std::min(metric_queue_min_bytes_, queued_bytes);
        metric_queue_max_bytes_ = std::max(metric_queue_max_bytes_, queued_bytes);
        metric_queue_bytes_sum_ += queued_bytes;
        ++metric_queue_samples_;
        emit_audio_metrics(now, false);
    }
    void open_controller(int index) {
        if (!SDL_IsGameController(index)) return;
        SDL_GameController* gamepad = SDL_GameControllerOpen(index);
        if (!gamepad) return;
        const SDL_JoystickID instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad));
        for (SDL_GameController* existing : controllers_) {
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(existing)) == instance) {
                SDL_GameControllerClose(gamepad);
                return;
            }
        }
        controllers_.push_back(gamepad);
        select_controller();
    }
    void remove_controller(SDL_JoystickID instance) {
        const auto found = std::find_if(controllers_.begin(), controllers_.end(), [&](SDL_GameController* gamepad) {
            return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad)) == instance;
        });
        if (found != controllers_.end()) {
            if (*found == controller_) controller_ = nullptr;
            SDL_GameControllerClose(*found);
            controllers_.erase(found);
        }
        select_controller();
    }
    void select_controller() {
        controller_ = nullptr;
        for (SDL_GameController* gamepad : controllers_) {
            if (!SDL_GameControllerGetAttached(gamepad)) continue;
            if (config_.controller_guid.empty() ||
                frontend_detail::controller_guid(gamepad) == config_.controller_guid) {
                controller_ = gamepad;
                return;
            }
        }
        if (!controllers_.empty() && SDL_GameControllerGetAttached(controllers_.front()))
            controller_ = controllers_.front();
    }
    void cycle_layout() {
        config_.layout = static_cast<ScreenLayout>((static_cast<int>(config_.layout) + 1) % 6);
        constexpr const char* kNames[6] = {"VERTICAL", "HORIZONTAL", "TOP FOCUS", "BOTTOM FOCUS", "TOP ONLY", "BOTTOM ONLY"};
        show_message(std::string("LAYOUT: ") + kNames[static_cast<int>(config_.layout)]);
    }
    void swap_screens() {
        swapped_ = !swapped_;
        show_message(swapped_ ? "SCREENS SWAPPED" : "SCREENS: DEFAULT ORDER");
    }
    void show_message(const std::string& text) override {
        toast_ = text;
        toast_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    }
    // Transient notice at the bottom-left plus a held-key fast-forward badge at the top-right.
    void draw_toast() {
        const bool toast = !toast_.empty() && std::chrono::steady_clock::now() < toast_until_;
        if (!toast && !fast_forward_active_) return;
        int width = 0, height = 0;
        SDL_GetRendererOutputSize(renderer_, &width, &height);
        ui_detail::UiCanvas canvas(renderer_);
        const ui_detail::UiColor white{238, 244, 252, 255};
        const int scale = height >= 700 ? 2 : 1;
        if (toast) {
            const int text_w = canvas.text_width(toast_, scale);
            const int box_h = 14 * scale + 12;
            canvas.panel({14, height - 14 - box_h, text_w + 28, box_h});
            canvas.text(28, height - 14 - box_h + 6 + scale * 2, toast_, white, scale);
        }
        if (fast_forward_active_) {
            const char* label = "FAST-FORWARD >>";
            const int text_w = canvas.text_width(label, scale);
            const int box_h = 14 * scale + 12;
            canvas.panel({width - 14 - text_w - 28, 14, text_w + 28, box_h});
            canvas.text(width - 14 - text_w - 14, 14 + 6 + scale * 2, label, ui_detail::UiColor{128, 216, 255, 255}, scale);
        }
    }
    void toggle_fullscreen() {
        const bool now = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        SDL_SetWindowFullscreen(window_, now ? 0u : static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP));
    }
    void toggle_filter() {
        config_.linear_filter = !config_.linear_filter;
        show_message(config_.linear_filter ? "FILTER: LINEAR" : "FILTER: NEAREST");
        for (auto* texture : textures_) SDL_SetTextureScaleMode(texture, config_.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    }
    void draw_debug_overlay() {
        if (!debug_overlay_.visible && !debug_overlay_.annotation_open &&
            !debug_overlay_.debug_tool_enabled && !debug_overlay_.startup_banner_visible &&
            audio_debug_warning_.empty()) return;
        int width = 0, height = 0;
        SDL_GetRendererOutputSize(renderer_, &width, &height);
        ui_detail::UiCanvas canvas(renderer_);
        const ui_detail::UiColor white{238, 244, 252, 255};
        const ui_detail::UiColor muted{166, 182, 202, 255};
        const ui_detail::UiColor accent{128, 216, 255, 255};
        if (debug_overlay_.visible && !debug_overlay_.annotation_open) {
            canvas.panel({14, 14, 480, 144});
            char line[192];
            std::snprintf(line, sizeof(line), "DEBUG  FRAME %llu  %s  SPEED %.2fx",
                          static_cast<unsigned long long>(debug_overlay_.frame),
                          debug_overlay_.paused ? "PAUSED" : "RUNNING", debug_overlay_.speed);
            canvas.text(28, 28, line, white, 1);
            if (debug_overlay_.audio_bar)
                std::snprintf(line, sizeof(line), "FPS %.1f  AUDIO QUEUE %.1f ms",
                              debug_overlay_.fps, debug_overlay_.audio_backlog_ms);
            else
                std::snprintf(line, sizeof(line), "FPS %.1f%s%s",
                              debug_overlay_.fps, debug_overlay_.cpu_pc ? "  ARM9 PC " : "",
                              debug_overlay_.cpu_pc ? "00000000" : "");
            if (debug_overlay_.cpu_pc && debug_overlay_.audio_bar)
                std::snprintf(line, sizeof(line), "FPS %.1f  AUDIO QUEUE %.1f ms  ARM9 PC %08X",
                              debug_overlay_.fps, debug_overlay_.audio_backlog_ms, debug_overlay_.arm9_pc);
            else if (debug_overlay_.cpu_pc)
                std::snprintf(line, sizeof(line), "FPS %.1f  ARM9 PC %08X",
                              debug_overlay_.fps, debug_overlay_.arm9_pc);
            canvas.text(28, 48, line, white, 1);
            canvas.text(28, 68, debug_overlay_.status, muted, 1);
            if (debug_overlay_.show_texture_slots) {
                const int mapped_texture_slots = static_cast<int>(std::count_if(
                    debug_overlay_.texture_slots.begin(), debug_overlay_.texture_slots.end(),
                    [](const std::uint8_t* slot) { return slot != nullptr; }));
                std::snprintf(line, sizeof(line), "3D TEXTURE VRAM SLOTS MAPPED %d/4", mapped_texture_slots);
                canvas.text(28, 82, line, muted, 1);
            }
            if (debug_overlay_.audio_bar) {
                const int audio_width = std::clamp(static_cast<int>(debug_overlay_.audio_backlog_ms * 3.0f), 0, 200);
                canvas.fill({300, 72, audio_width, 6}, accent);
            }
            if (debug_overlay_.frame_graph)
                canvas.outline({28, 93, 448, 48}, {92, 115, 148, 255});
            if (debug_overlay_.frame_graph && debug_overlay_.frame_time_count > 1) {
                const float max_ms = 33.4f;
                SDL_SetRenderDrawColor(renderer_, 83, 192, 240, 255);
                for (std::size_t i = 1; i < debug_overlay_.frame_time_count; ++i) {
                    const auto index0 = (i - 1) % debug_overlay_.frame_times_ms.size();
                    const auto index1 = i % debug_overlay_.frame_times_ms.size();
                    const int x0 = 30 + static_cast<int>((i - 1) * 444 / debug_overlay_.frame_time_count);
                    const int x1 = 30 + static_cast<int>(i * 444 / debug_overlay_.frame_time_count);
                    const int y0 = 139 - static_cast<int>(std::clamp(debug_overlay_.frame_times_ms[index0] / max_ms, 0.0f, 1.0f) * 42.0f);
                    const int y1 = 139 - static_cast<int>(std::clamp(debug_overlay_.frame_times_ms[index1] / max_ms, 0.0f, 1.0f) * 42.0f);
                    SDL_RenderDrawLine(renderer_, x0, y0, x1, y1);
                }
            }
        }
        if (debug_overlay_.texture_inspector) draw_texture_inspector(canvas, width, height);
        if (debug_overlay_.audio_mixer) {
            const int y = debug_overlay_.visible ? 174 : 14;
            canvas.panel({14, y, 480, 330});
            canvas.text(28, y + 12, "SPU CHANNELS  A: MUTE  B: SOLO  V: CLOSE", white, 1);
            for (int i = 0; i < 16; ++i) {
                const int column = i / 8, row = i % 8;
                const int x = 28 + column * 225, cy = y + 42 + row * 34;
                const std::string tag = std::string(i == debug_overlay_.audio_channel_selection ? ">" : " ") +
                    "CH" + std::to_string(i) + (debug_overlay_.audio_muted[static_cast<std::size_t>(i)] ? " MUTE" : "") +
                    (debug_overlay_.audio_solo == i ? " SOLO" : "");
                canvas.text(x, cy, tag, i == debug_overlay_.audio_channel_selection ? accent : white, 1);
            }
        }
        if (debug_overlay_.help) {
            const int x = width < 928 ? 14 : std::max(14, width - 430);
            const int y = width < 928 ? 166 : 14;
            canvas.panel({x, y, 416, 246});
            canvas.text(x + 14, y + 12, "DEBUG HOTKEYS", white, 1);
            canvas.text(x + 14, y + 36, "F10 OR MENU BUTTON; SPACE/PAUSE PAUSE; F8 STEP", muted, 1);
            canvas.text(x + 14, y + 58, "F2 SLOW x0.5/x0.25; F3 FAST; F9 OR CTRL+SHIFT+N NOTE", muted, 1);
            canvas.text(x + 14, y + 80, "F4 MARK; D MEMORY DUMPS; T TEXTURE VIEW", muted, 1);
            canvas.text(x + 14, y + 102, "M MIXER (HUD); CTRL+SHIFT+F7/F12 MUTE/SOLO", muted, 1);
            canvas.text(x + 14, y + 124, "ENTER=NEWLINE; CTRL+ENTER/F2=SAVE; ESC=CANCEL", muted, 1);
            canvas.text(x + 14, y + 146, "PAD: GUIDE+DPAD RIGHT STEP; GUIDE+L/R SPEED", muted, 1);
            canvas.text(x + 14, y + 168, "GUIDE+X MARK; GUIDE+Y ANNOTATE; GUIDE+A/B", muted, 1);
            canvas.text(x + 14, y + 190, "MUTE/SOLO; SELECT+START 1S PAUSE CHORD", muted, 1);
        }
        if (debug_overlay_.annotation_open) draw_annotation(canvas, width, height, accent, muted);
        if (debug_overlay_.startup_banner_visible && width > 0 && height > 0) {
            const int x = 8, y = std::max(8, height - 44);
            const int w = std::max(0, std::min(352, width - 16));
            if (w > 0) {
                canvas.panel({x, y, w, 36});
                canvas.text(x + 8, y + 5, "DEBUG TOOL ON", white, 1);
                canvas.text(x + 8, y + 19, "F10 MENU / F9 NOTE / F8 STEP", muted, 1);
            }
        }
        if (debug_overlay_.debug_tool_enabled)
            draw_debug_controls(canvas, width, height, accent, muted);
        if (debug_overlay_.menu_open) draw_debug_menu(canvas, width, height);
        if (!audio_debug_warning_.empty()) {
            const int y = std::max(0, height - 32);
            canvas.fill({0, y, width, 32}, {160, 0, 0, 255});
            canvas.text(8, y + 8, audio_debug_warning_, white, 1);
        }
    }
    void draw_debug_menu(ui_detail::UiCanvas& canvas, int width, int height) {
        const auto rect = debug_menu_rect(width, height);
        canvas.panel({rect.x, rect.y, rect.w, rect.h});
        canvas.text(rect.x + 12, rect.y + 10, "DEBUG MENU - UP/DOWN, ENTER SELECT, ESC CLOSE",
                    {238, 244, 252, 255}, 1);
        const ui_detail::UiColor accent{128, 216, 255, 255};
        const ui_detail::UiColor white{238, 244, 252, 255};
        const ui_detail::UiColor muted{166, 182, 202, 255};
        for (std::size_t i = 0; i < kDebugMenuRowCount; ++i) {
            const auto item = static_cast<DebugMenuItem>(i);
            const int y = rect.y + 30 + static_cast<int>(i) * 22;
            const bool selected = static_cast<int>(i) == debug_overlay_.menu_selection;
            if (selected) canvas.fill({rect.x + 8, y - 2, rect.w - 16, 20},
                                      {44, 105, 157, 255});
            std::string label(debug_menu_label(item));
            if (item == DebugMenuItem::FrameGraph)
                label += debug_overlay_.frame_graph ? "  ON" : "  OFF";
            else if (item == DebugMenuItem::AudioBar)
                label += debug_overlay_.audio_bar ? "  ON" : "  OFF";
            else if (item == DebugMenuItem::CpuPc)
                label += debug_overlay_.cpu_pc ? "  ON" : "  OFF";
            else if (item == DebugMenuItem::TextureSlots)
                label += debug_overlay_.show_texture_slots ? "  ON" : "  OFF";
            else if (item == DebugMenuItem::AudioMixer)
                label += debug_overlay_.audio_mixer ? "  ON" : "  OFF";
            else if (item == DebugMenuItem::TextureInspector)
                label += debug_overlay_.texture_inspector ? "  ON" : "  OFF";
            else if (item == DebugMenuItem::FastForward)
                label += debug_overlay_.speed > 2.0 ? "  ON" : "  OFF";
            canvas.text(rect.x + 16, y, label, selected ? white : muted, 1);
            canvas.text(rect.x + rect.w - 98, y, debug_menu_hotkey(item), selected ? accent : muted, 1);
        }
    }
    void draw_annotation(ui_detail::UiCanvas& canvas, int width, int height,
                         ui_detail::UiColor accent, ui_detail::UiColor muted) {
        const int panel_w = std::min(960, std::max(420, width - 40));
        const int panel_h = std::min(610, std::max(420, height - 40));
        const int x = (width - panel_w) / 2, y = (height - panel_h) / 2;
        canvas.panel({x, y, panel_w, panel_h});
        canvas.text(x + 18, y + 16, "ANNOTATION - PLAIN TEXT", {240, 245, 252, 255}, 1);
        canvas.text(x + 18, y + 38, "PLAIN TEXT; EDIT WITH KEYBOARD", muted, 1);
        const std::string& text = debug_overlay_.annotation;
        int row = 0;
        std::size_t start = 0;
        while (start < text.size() && row < 8) {
            const auto newline = text.find('\n', start);
            const std::size_t end = newline == std::string::npos ? text.size() : newline;
            const std::size_t length = std::min<std::size_t>(80, end - start);
            canvas.text(x + 18, y + 66 + row * 18, std::string_view(text).substr(start, length), {245, 245, 245, 255}, 1);
            ++row;
            if (newline == std::string::npos) break;
            start = newline + 1;
        }
        canvas.text(x + 18, y + 222, "CONTROLLER KEYBOARD (A SELECTS; D-PAD MOVES; START SAVES)", muted, 1);
        constexpr int columns = 12, rows = 8;
        const int cell_w = std::min(54, (panel_w - 36) / columns), cell_h = 31;
        const int grid_x = x + 18, grid_y = y + 245;
        for (int i = 0; i < columns * rows; ++i) {
            const int col = i % columns, r = i / columns;
            const int bx = grid_x + col * cell_w, by = grid_y + r * cell_h;
            const bool selected = i == debug_overlay_.keyboard_selection;
            canvas.fill({bx, by, cell_w - 2, cell_h - 2}, selected ? ui_detail::UiColor{44, 105, 157, 255} : ui_detail::UiColor{25, 35, 50, 255});
            canvas.outline({bx, by, cell_w - 2, cell_h - 2}, selected ? accent : ui_detail::UiColor{82, 102, 132, 255});
            if (i < 95) {
                char ch[2]{static_cast<char>(32 + i), '\0'};
                canvas.text(bx + 8, by + 6, ch, {240, 245, 252, 255}, 1);
            }
        }
        canvas.text(x + 18, y + panel_h - 22,
                    "CTRL+ENTER OR F2 = SAVE; ESC = CANCEL; ENTER = NEWLINE", muted, 1);
    }
    void draw_texture_inspector(ui_detail::UiCanvas& canvas, int width, int height) {
        update_texture_views();
        const int panel_w = std::min(960, std::max(440, width - 32));
        const int panel_h = std::min(560, std::max(300, height - 32));
        const int x = (width - panel_w) / 2, y = (height - panel_h) / 2;
        canvas.panel({x, y, panel_w, panel_h});
        canvas.text(x + 16, y + 12, "TEXTURE VRAM BYTE VIEW - RAW RGBA GROUPING, NOT DECODED TEXELS", {240, 245, 252, 255}, 1);
        const int view_w = (panel_w - 48) / 2, view_h = (panel_h - 76) / 2;
        for (int i = 0; i < 4; ++i) {
            const int col = i % 2, row = i / 2;
            const int vx = x + 16 + col * (view_w + 16), vy = y + 42 + row * (view_h + 12);
            canvas.text(vx, vy, "SLOT " + std::to_string(i) + (debug_overlay_.texture_slots[static_cast<std::size_t>(i)] ? " MAPPED" : " UNMAPPED"), {166, 182, 202, 255}, 1);
            if (texture_view_textures_[static_cast<std::size_t>(i)]) {
                SDL_Rect dest{vx, vy + 18, view_w, view_h - 20};
                SDL_RenderCopy(renderer_, texture_view_textures_[static_cast<std::size_t>(i)], nullptr, &dest);
            }
        }
    }
    void update_texture_views() {
        if (debug_overlay_.frame == texture_view_frame_) return;
        if (texture_view_pixels_.empty()) texture_view_pixels_.resize(256 * 128);
        for (std::size_t i = 0; i < texture_view_textures_.size(); ++i) {
            if (!texture_view_textures_[i]) {
                texture_view_textures_[i] = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                                               SDL_TEXTUREACCESS_STREAMING, 256, 128);
                if (texture_view_textures_[i]) SDL_SetTextureBlendMode(texture_view_textures_[i], SDL_BLENDMODE_NONE);
            }
            const std::uint8_t* source = debug_overlay_.texture_slots[i];
            for (std::size_t pixel = 0; pixel < texture_view_pixels_.size(); ++pixel) {
                const std::size_t byte = pixel * 4;
                const bool readable = source && byte + 2 < 0x20000;
                texture_view_pixels_[pixel] = readable
                    ? 0xff000000u | (static_cast<std::uint32_t>(source[byte]) << 16) |
                      (static_cast<std::uint32_t>(source[byte + 1]) << 8) | source[byte + 2]
                    : 0xff141a24u;
            }
            if (texture_view_textures_[i])
                SDL_UpdateTexture(texture_view_textures_[i], nullptr, texture_view_pixels_.data(), 256 * static_cast<int>(sizeof(std::uint32_t)));
        }
        texture_view_frame_ = debug_overlay_.frame;
    }
    FrontendConfig config_{};
    frontend_detail::ConfigData config_data_{};
    UserPaths paths_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::array<SDL_Texture*, 2> textures_{};
    std::array<int, 2> texture_scale_{1, 1};
    std::string toast_;
    std::chrono::steady_clock::time_point toast_until_{};
    bool fast_forward_active_ = false;
    bool muted_ = false;
    std::array<SDL_Texture*, 4> texture_view_textures_{};
    std::vector<std::uint32_t> texture_view_pixels_;
    DebugOverlayState debug_overlay_{};
    std::string audio_debug_warning_;
    std::uint64_t texture_view_frame_ = ~std::uint64_t{0};
    std::uint64_t dropped_audio_blocks_ = 0, empty_audio_queue_events_ = 0, queued_audio_blocks_ = 0;
    std::uint32_t controller_debug_combo_held_ = 0;
    std::uint16_t menu_buttons_held_ = 0;
    Uint32 pause_chord_start_ = 0;
    bool menu_chord_held_ = false, pause_chord_active_ = false, pause_chord_fired_ = false;
    std::vector<SDL_GameController*> controllers_;
    SDL_GameController* controller_ = nullptr;
    SDL_AudioDeviceID audio_ = 0;
    SDL_AudioSpec audio_spec_{};
    int output_rate_ = 0;
    bool audio_device_started_ = false;
    bool initialized_ = false, swapped_ = false, stylus_mode_ = false, r3_pressed_ = false;
    bool select_l_held_ = false, select_r3_held_ = false, select_start_held_ = false;
    int cursor_x_ = 128, cursor_y_ = 96;
    double source_position_ = 0.0;
    std::vector<std::int16_t> resampled_audio_;
    std::chrono::steady_clock::time_point next_frame_{};
#if defined(_WIN32)
    bool precise_timer_ = false;
#endif
    bool audio_metrics_enabled_ = false, metric_window_started_ = false;
    bool metric_has_audio_queue_time_ = false;
    double metric_produced_audio_ms_ = 0.0, metric_dropped_audio_ms_ = 0.0;
    double metric_available_audio_seconds_ = 0.0;
    double metric_audio_correction_ = 0.0;
    double metric_frame_time_sum_us_ = 0.0, metric_frame_time_min_us_ = 1.0e30;
    double metric_frame_time_max_us_ = 0.0;
    std::size_t metric_frames_ = 0, metric_frame_time_count_ = 0;
    std::size_t metric_queue_min_bytes_ = 0, metric_queue_max_bytes_ = 0;
    std::size_t metric_queue_samples_ = 0;
    std::uint64_t metric_queue_bytes_sum_ = 0;
    unsigned metric_queue_empty_samples_ = 0, metric_underruns_ = 0, metric_clears_ = 0;
    std::array<double, 2048> metric_frame_times_us_{};
    std::chrono::steady_clock::time_point metric_window_start_{};
    std::chrono::steady_clock::time_point metric_last_frame_{};
    std::chrono::steady_clock::time_point metric_last_audio_queue_time_{};
    unsigned debug_injection_stage_ = 0;
    unsigned debug_present_count_ = 0;
    std::string debug_injection_mode_;
    bool debug_banner_captured_ = false, debug_note_captured_ = false;
    std::string debug_banner_capture_path_, debug_note_capture_path_;
};
} // namespace

namespace ui_detail {
bool capture_debug_screenshots(const std::filesystem::path& directory) {
    return SdlFrontend::capture_screenshots(directory);
}
} // namespace ui_detail

UserPaths default_user_paths() {
    frontend_detail::UserPathEnvironment environment;
#if defined(_WIN32)
    environment.windows = true;
    // SDL paths/config strings are UTF-8, but the native environment and known
    // folders are UTF-16. Construct paths directly from the wide roots.
    if (const wchar_t* value = _wgetenv(L"APPDATA")) environment.appdata = value;
    if (const wchar_t* value = _wgetenv(L"LOCALAPPDATA")) environment.local_appdata = value;
    if (const wchar_t* value = _wgetenv(L"USERPROFILE")) environment.user_profile = value;
    PWSTR folder = nullptr;
    if (environment.appdata.empty() &&
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &folder) == S_OK) {
        environment.appdata = folder;
        CoTaskMemFree(folder);
    }
    folder = nullptr;
    if (environment.local_appdata.empty() &&
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder) == S_OK) {
        environment.local_appdata = folder;
        CoTaskMemFree(folder);
    }
#else
    if (const char* value = std::getenv("XDG_CONFIG_HOME")) environment.config_home = value;
    if (const char* value = std::getenv("XDG_DATA_HOME")) environment.data_home = value;
    if (const char* value = std::getenv("HOME")) environment.home = value;
#endif
    return frontend_detail::user_paths_from_environment(environment);
}
bool running_on_steam_deck() {
#if defined(_WIN32)
    return false;
#else
    const char* deck = std::getenv("SteamDeck");
    if (deck && std::string(deck) == "1") return true;
    const char* steamos = std::getenv("SteamOS");
    if (steamos && std::string(steamos) == "1") return true;
    std::ifstream vendor("/sys/devices/virtual/dmi/id/board_vendor");
    std::ifstream product("/sys/devices/virtual/dmi/id/board_name");
    std::string v, p;
    if (vendor && product) { std::getline(vendor, v); std::getline(product, p); if (v.find("Valve") != std::string::npos && (p.find("Jupiter") != std::string::npos || p.find("Galileo") != std::string::npos)) return true; }
    std::ifstream release("/etc/os-release");
    std::string release_line;
    while (std::getline(release, release_line))
        if (release_line == "ID=steamos" || release_line == "ID=\"steamos\"") return true;
    return false;
#endif
}
std::unique_ptr<Frontend> create_sdl_frontend() { return std::make_unique<SdlFrontend>(); }
} // namespace dsr
