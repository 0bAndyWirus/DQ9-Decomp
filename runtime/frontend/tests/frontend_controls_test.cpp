#include "dsr/frontend.h"
#include <SDL.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

int main() {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    const auto directory = std::filesystem::temp_directory_path() /
        ("dq9-frontend-controls-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#if defined(_WIN32)
    SDL_setenv("APPDATA", directory.string().c_str(), 1);
    SDL_setenv("LOCALAPPDATA", directory.string().c_str(), 1);
#else
    SDL_setenv("XDG_CONFIG_HOME", directory.string().c_str(), 1);
    SDL_setenv("XDG_DATA_HOME", directory.string().c_str(), 1);
#endif
    auto frontend = dsr::create_sdl_frontend();
    dsr::FrontendConfig config;
    config.debug_tool_enabled = true;
    assert(frontend->init(config));
    dsr::DebugOverlayState overlay;
    overlay.debug_tool_enabled = true;
    frontend->set_debug_overlay(overlay);
    const auto press = [&](SDL_Scancode key, Uint16 modifiers = 0) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.scancode = key;
        event.key.keysym.mod = modifiers;
        assert(SDL_PushEvent(&event) == 1);
        dsr::InputState input;
        frontend->poll(input);
        return input.debug_actions;
    };
    SDL_Window* hotkey_window = SDL_GetWindowFromID(1);
    assert(hotkey_window);
    SDL_Renderer* hotkey_renderer = SDL_GetRenderer(hotkey_window);
    assert(hotkey_renderer);
    const auto push_key = [](SDL_Scancode key, Uint32 type) {
        SDL_Event event{};
        event.type = type;
        event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.scancode = key;
        event.key.keysym.sym = SDL_GetKeyFromScancode(key);
        assert(SDL_PushEvent(&event) == 1);
    };
    dsr::InputState hotkey_input;
    const auto tap = [&](SDL_Scancode key) {
        push_key(key, SDL_KEYDOWN);
        push_key(key, SDL_KEYUP);
        frontend->poll(hotkey_input);
    };
    assert(SDL_RenderGetIntegerScale(hotkey_renderer) == SDL_FALSE);
    tap(SDL_SCANCODE_F3);
    assert(hotkey_input.debug_actions == dsr::kDebugFastForward);
    assert(!hotkey_input.lid_closed);
    tap(SDL_SCANCODE_F4);
    assert(hotkey_input.debug_actions == dsr::kDebugMark);
    assert(SDL_RenderGetIntegerScale(hotkey_renderer) == SDL_FALSE);
    // Two genuine taps can arrive in one poll; both must remain debug-only.
    for (const auto key : {SDL_SCANCODE_F3, SDL_SCANCODE_F4}) {
        push_key(key, SDL_KEYDOWN);
        push_key(key, SDL_KEYUP);
        push_key(key, SDL_KEYDOWN);
        push_key(key, SDL_KEYUP);
        frontend->poll(hotkey_input);
        assert(hotkey_input.debug_actions ==
            (key == SDL_SCANCODE_F3 ? dsr::kDebugFastForward : dsr::kDebugMark));
        assert(!hotkey_input.lid_closed);
        assert(SDL_RenderGetIntegerScale(hotkey_renderer) == SDL_FALSE);
    }
    for (const auto key : {SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F12,
                           SDL_SCANCODE_D, SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_B,
                           SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_M, SDL_SCANCODE_S, SDL_SCANCODE_O})
        assert(press(key) == 0);
    assert(press(SDL_SCANCODE_F12, KMOD_LCTRL | KMOD_LSHIFT) == dsr::kDebugChannelSolo);
    assert(press(SDL_SCANCODE_F7, KMOD_RCTRL | KMOD_RSHIFT) == dsr::kDebugChannelMute);
    assert(press(SDL_SCANCODE_F12, KMOD_LCTRL) == 0);
    assert(press(SDL_SCANCODE_N, KMOD_LCTRL | KMOD_LSHIFT) == dsr::kDebugAnnotate);
    overlay.visible = true;
    frontend->set_debug_overlay(overlay);
    assert(press(SDL_SCANCODE_G) == dsr::kDebugToggleFrameGraph);
    assert(press(SDL_SCANCODE_F7) == 0);
    overlay.audio_mixer = true;
    frontend->set_debug_overlay(overlay);
    assert(press(SDL_SCANCODE_F7) == dsr::kDebugChannelMute);
    assert(press(SDL_SCANCODE_F12) == dsr::kDebugChannelSolo);
    overlay.visible = false;
    overlay.audio_mixer = false;
    overlay.audio_solo = 0;
    overlay.audio_muted[3] = true;
    frontend->set_debug_overlay(overlay);
    SDL_Window* window = SDL_GetWindowFromID(1);
    assert(window);
    assert(std::string(SDL_GetWindowTitle(window)) ==
        config.title + " - AUDIO DEBUG: SOLO CH0 / MUTED CH3");
    std::array<std::uint32_t, 256 * 192> pixels{};
    pixels.fill(0xff102030);
    frontend->present(pixels.data(), pixels.data());
    SDL_Renderer* renderer = SDL_GetRenderer(window);
    int width = 0, height = 0;
    SDL_GetRendererOutputSize(renderer, &width, &height);
    SDL_Rect sample{width - 1, height - 16, 1, 1};
    std::uint32_t warning_pixel = 0;
    assert(SDL_RenderReadPixels(renderer, &sample, SDL_PIXELFORMAT_ARGB8888, &warning_pixel, 4) == 0);
    assert(warning_pixel == 0xffa00000); // warning remains visible with menu/HUD closed
    overlay.audio_solo = -1;
    overlay.audio_muted.fill(false);
    frontend->set_debug_overlay(overlay);
    assert(std::string(SDL_GetWindowTitle(window)) == config.title);
    std::array<std::int16_t, 1600> silence{};
    frontend->clear_audio_queue();
    frontend->queue_audio(silence.data(), 800, 48000);
    dsr::FrontendDiagnostics diagnostics;
    frontend->diagnostics(diagnostics);
    assert(diagnostics.empty_audio_queue_events == 0); // fresh prime is not starvation
    std::vector<std::int16_t> excess(24000 * 2, 0);
    frontend->queue_audio(excess.data(), 24000, 48000);
    frontend->queue_audio(silence.data(), 800, 48000); // emergency clear, then partial prime
    frontend->diagnostics(diagnostics);
    assert(diagnostics.dropped_audio_blocks == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The dummy device would drain this 16.7 ms block if emergency clear forgot
    // to pause/re-prime; an intentional partial prime must stay queued.
    assert(frontend->audio_backlog() > 0);
    frontend.reset();
    // With the debug tool disabled, the original general bindings still work.
    frontend = dsr::create_sdl_frontend();
    config.debug_tool_enabled = false;
    assert(frontend->init(config));
    SDL_Window* ordinary_window = SDL_GetWindowFromID(1);
    assert(ordinary_window);
    SDL_Renderer* ordinary_renderer = SDL_GetRenderer(ordinary_window);
    hotkey_input = {};
    tap(SDL_SCANCODE_F3);
    assert(hotkey_input.debug_actions == 0);
    assert(hotkey_input.lid_closed);
    tap(SDL_SCANCODE_F3);
    assert(!hotkey_input.lid_closed);
    tap(SDL_SCANCODE_F4);
    assert(hotkey_input.debug_actions == 0);
    assert(SDL_RenderGetIntegerScale(ordinary_renderer) == SDL_TRUE);
    tap(SDL_SCANCODE_F4);
    assert(SDL_RenderGetIntegerScale(ordinary_renderer) == SDL_FALSE);
    frontend.reset();
    // Rebound hotkeys (the launcher HOTKEYS page writes these keys): the configured scancode
    // acts and the old default does not. F9 = fullscreen, F10 = integer scaling.
    {
        const auto conf_dir = directory / "dq9-decomp";
        std::filesystem::create_directories(conf_dir);
        std::ofstream(conf_dir / "frontend.conf") << "hotkey_fullscreen=66\nhotkey_integer=67\nkey_screenshot=68\n";
        frontend = dsr::create_sdl_frontend();
        assert(frontend->init(config));
        SDL_Window* rebound_window = SDL_GetWindowFromID(1);
        SDL_Renderer* rebound_renderer = SDL_GetRenderer(rebound_window);
        const auto fullscreen = [&] { return (SDL_GetWindowFlags(rebound_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; };
        hotkey_input = {};
        tap(SDL_SCANCODE_F11);
        assert(!fullscreen());
        tap(SDL_SCANCODE_F9);
        assert(fullscreen());
        tap(SDL_SCANCODE_F9);
        assert(!fullscreen());
        tap(SDL_SCANCODE_F4);
        assert(SDL_RenderGetIntegerScale(rebound_renderer) == SDL_FALSE);
        tap(SDL_SCANCODE_F10);
        assert(SDL_RenderGetIntegerScale(rebound_renderer) == SDL_TRUE);
        tap(SDL_SCANCODE_F12);
        assert(!hotkey_input.screenshot_requested);
        tap(SDL_SCANCODE_F11);
        assert(hotkey_input.screenshot_requested);
        std::filesystem::remove(conf_dir / "frontend.conf");
        frontend.reset();
        frontend = dsr::create_sdl_frontend();
        assert(frontend->init(config));
    }
    {
        // Hotkey feedback: swapping screens (both screens identical here) must draw a notice
        // in the bottom-left corner, and nothing there before the key press.
        SDL_Window* toast_window = SDL_GetWindowFromID(1);
        SDL_Renderer* toast_renderer = SDL_GetRenderer(toast_window);
        std::array<std::uint32_t, 256 * 192> flat{};
        flat.fill(0xff102030);
        int tw = 0, th = 0;
        SDL_GetRendererOutputSize(toast_renderer, &tw, &th);
        const SDL_Rect probe{20, th - 30, 1, 1};
        std::uint32_t before = 0, after = 0;
        frontend->present(flat.data(), flat.data());
        assert(SDL_RenderReadPixels(toast_renderer, &probe, SDL_PIXELFORMAT_ARGB8888, &before, 4) == 0);
        assert(before == 0xff102030);
        tap(SDL_SCANCODE_F8);
        frontend->present(flat.data(), flat.data());
        assert(SDL_RenderReadPixels(toast_renderer, &probe, SDL_PIXELFORMAT_ARGB8888, &after, 4) == 0);
        assert(after != before);
        tap(SDL_SCANCODE_F8);
    }
    // Screenshot hotkey (F12) is a one-shot that works without the debug tool.
    tap(SDL_SCANCODE_F12);
    assert(hotkey_input.screenshot_requested && hotkey_input.debug_actions == 0);
    frontend->poll(hotkey_input);
    assert(!hotkey_input.screenshot_requested);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    {
        // Pad combos through a virtual controller: Select=back, Start, L/R shoulders, R3.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0, 8, 0);
        assert(device >= 0);
        char guid[64]{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device), guid, sizeof(guid));
        const std::string mapping = std::string(guid) +
            ",DQ9 Controls Test,back:b0,start:b1,leftshoulder:b2,rightshoulder:b3,rightstick:b4,a:b5,";
        assert(SDL_GameControllerAddMapping(mapping.c_str()) >= 0);
        SDL_Joystick* joystick = SDL_JoystickOpen(device);
        assert(joystick);
        const auto pad = [&](std::initializer_list<int> down) {
            for (int b = 0; b < 8; ++b) SDL_JoystickSetVirtualButton(joystick, b, 0);
            for (const int b : down) SDL_JoystickSetVirtualButton(joystick, b, 1);
            SDL_JoystickUpdate();
            frontend->poll(hotkey_input);
        };
        frontend->poll(hotkey_input); // open the newly attached controller
        pad({});
        pad({0, 3}); // Select+R held: fast-forward, nothing forwarded to the DS
        assert(hotkey_input.fast_forward && (hotkey_input.buttons & (dsr::kBtnSelect | dsr::kBtnR)) == 0);
        pad({0, 3});
        assert(hotkey_input.fast_forward);
        pad({});
        assert(!hotkey_input.fast_forward);
        pad({0, 1}); // Select+Start: one screenshot per press, buttons withheld for the whole hold
        assert(hotkey_input.screenshot_requested && !hotkey_input.fast_forward);
        assert((hotkey_input.buttons & (dsr::kBtnSelect | dsr::kBtnStart)) == 0);
        pad({0, 1});
        assert(!hotkey_input.screenshot_requested);
        assert((hotkey_input.buttons & (dsr::kBtnSelect | dsr::kBtnStart)) == 0);
        pad({});
        pad({0, 4}); // Select+R3 is the screen swap now: no fast-forward, Select withheld
        assert(!hotkey_input.fast_forward && !hotkey_input.screenshot_requested);
        assert((hotkey_input.buttons & dsr::kBtnSelect) == 0);
        pad({3}); // R alone is an ordinary DS button
        assert(!hotkey_input.fast_forward && (hotkey_input.buttons & dsr::kBtnR) != 0);
        pad({});
        SDL_JoystickClose(joystick);
        SDL_JoystickDetachVirtual(device);
    }
#endif
    frontend.reset();
    std::filesystem::remove_all(directory);
}
