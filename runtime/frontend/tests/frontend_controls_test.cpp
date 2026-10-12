#include "dsr/frontend.h"
#include <SDL.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
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
    std::filesystem::remove_all(directory);
}
