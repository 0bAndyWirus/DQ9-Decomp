#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dsr/frontend.h"
#include <SDL.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("dq9-input-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#if defined(_WIN32)
    SDL_setenv("APPDATA", directory.string().c_str(), 1);
    SDL_setenv("LOCALAPPDATA", directory.string().c_str(), 1);
#else
    SDL_setenv("XDG_CONFIG_HOME", directory.string().c_str(), 1);
    SDL_setenv("XDG_DATA_HOME", directory.string().c_str(), 1);
#endif
    std::filesystem::create_directories(directory / "dq9-remastered");
    std::ofstream(directory / "dq9-remastered/frontend.conf") <<
        "debug_tool=1\nframe_time_overlay=1\n";
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    auto frontend = dsr::create_sdl_frontend();
    dsr::FrontendConfig config;
    config.window_scale = 1;
    assert(frontend->init(config));
    dsr::InputState state;
    state.buttons = 0xfff;
    state.lid_closed = true;
    frontend->poll(state);
    assert(state.buttons == 0); // previously latched all twelve forever
    // A direct CLI launch with debug OFF must not inherit frontend-only debug
    // shortcuts from a prior launcher session (the game owns debug sessions).
    for (const auto scancode : {SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F8, SDL_SCANCODE_PAUSE}) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.scancode = scancode;
        assert(SDL_PushEvent(&event) == 1);
        frontend->poll(state);
        assert(state.debug_actions == 0);
    }
    assert(state.lid_closed);
    SDL_Window* window = SDL_GetWindowFromID(1);
    assert(window);
    for (const auto modifiers : {KMOD_NONE, KMOD_ALT}) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.scancode = modifiers == KMOD_NONE ? SDL_SCANCODE_F11 : SDL_SCANCODE_RETURN;
        event.key.keysym.mod = modifiers;
        assert(SDL_PushEvent(&event) == 1);
        frontend->poll(state);
        assert((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0);
        assert(SDL_PushEvent(&event) == 1);
        frontend->poll(state);
        assert((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0);
    }
    SDL_SetWindowSize(window, 3072, 1920);
    // No intervening poll: any cached dimensions still describe the old window.
    SDL_Event mouse{};
    mouse.type = SDL_MOUSEBUTTONDOWN;
    mouse.button.button = SDL_BUTTON_LEFT;
    mouse.button.x = 1536;
    mouse.button.y = 1440;
    assert(SDL_PushEvent(&mouse) == 1);
    frontend->poll(state);
    assert(state.pen_down && state.pen_x == 128 && state.pen_y == 96);
    mouse.type = SDL_MOUSEBUTTONUP;
    assert(SDL_PushEvent(&mouse) == 1);
    frontend->poll(state);
    assert(!state.pen_down);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0,
                                               SDL_CONTROLLER_BUTTON_MAX, 1);
    assert(device >= 0);
    char guid[64]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device), guid, sizeof(guid));
    const std::string mapping = std::string(guid) +
        ",DQ9 Input Test,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
        "leftshoulder:b9,rightshoulder:b10,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,";
    assert(SDL_GameControllerAddMapping(mapping.c_str()) >= 0);
    SDL_Joystick* joystick = SDL_JoystickOpen(device);
    assert(joystick);
    frontend->poll(state);
    // Defaults swap Nintendo A/B and X/Y relative to SDL's labels.
    const int buttons[]{1, 0, 4, 6, 10, 9, 3, 2};
    const std::uint16_t expected[]{dsr::kBtnA, dsr::kBtnB, dsr::kBtnSelect, dsr::kBtnStart,
                                 dsr::kBtnR, dsr::kBtnL, dsr::kBtnX, dsr::kBtnY};
    for (unsigned i = 0; i < 8; ++i) {
        assert(SDL_JoystickSetVirtualButton(joystick, buttons[i], 1) == 0);
        frontend->poll(state);
        assert(state.buttons == expected[i]);
        assert(SDL_JoystickSetVirtualButton(joystick, buttons[i], 0) == 0);
        frontend->poll(state);
        assert(state.buttons == 0);
    }
    const Uint8 hats[]{SDL_HAT_RIGHT, SDL_HAT_LEFT, SDL_HAT_UP, SDL_HAT_DOWN};
    const std::uint16_t directions[]{dsr::kBtnRight, dsr::kBtnLeft, dsr::kBtnUp, dsr::kBtnDown};
    for (unsigned i = 0; i < 4; ++i) {
        assert(SDL_JoystickSetVirtualHat(joystick, 0, hats[i]) == 0);
        frontend->poll(state);
        if (state.buttons != directions[i]) {
            char* actual_mapping = SDL_GameControllerMappingForDeviceIndex(device);
            std::fprintf(stderr, "hat=%u raw=%u DS=%u expected=%u mapping=%s\n",
                         hats[i], SDL_JoystickGetHat(joystick, 0), state.buttons,
                         directions[i], actual_mapping ? actual_mapping : "none");
            SDL_free(actual_mapping);
        }
        assert(state.buttons == directions[i]);
        assert(SDL_JoystickSetVirtualHat(joystick, 0, SDL_HAT_CENTERED) == 0);
        frontend->poll(state);
        assert(state.buttons == 0);
    }
    SDL_JoystickClose(joystick);
    SDL_JoystickDetachVirtual(device);
#endif
    frontend.reset();
    std::filesystem::remove_all(directory);
}
