#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dsr/frontend.h"
#include "dsr/host_path.h"
#include <SDL.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

#if SDL_VERSION_ATLEAST(2, 0, 14)
namespace {
struct Sequence {
    SDL_Joystick* joystick;
    std::string dropped_path;
    unsigned phase = 0;
};
void write_image(const std::filesystem::path& path) {
    // Entirely synthetic structural NDS image; no game code or assets.
    std::vector<unsigned char> bytes(0x10000, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put32(0x20, 0x4000);
    put32(0x2C, 0x4000);
    put32(0x30, 0x8000);
    put32(0x3C, 0x2000);
    put32(0x80, 0x10000);
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < 0x15E; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001)
                            : static_cast<std::uint16_t>(crc >> 1);
    }
    bytes[0x15E] = static_cast<unsigned char>(crc);
    bytes[0x15F] = static_cast<unsigned char>(crc >> 8);
    std::ofstream image(path, std::ios::binary);
    image.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(image.good());
}
void key(SDL_Scancode scancode) {
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.keysym.scancode = scancode;
    event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
    assert(SDL_PushEvent(&event) == 1);
    event.type = SDL_KEYUP;
    assert(SDL_PushEvent(&event) == 1);
}
Uint32 sequence(Uint32 interval, void* data) {
    auto& state = *static_cast<Sequence*>(data);
    switch (state.phase++) {
    case 0:
        key(SDL_SCANCODE_F1); // The launcher initially shows the ROM picker.
        assert(SDL_JoystickSetVirtualHat(state.joystick, 0, SDL_HAT_RIGHT) == 0);
        break;
    case 1: assert(SDL_JoystickSetVirtualHat(state.joystick, 0, SDL_HAT_CENTERED) == 0); break;
    case 2:
        key(SDL_SCANCODE_F2);
        key(SDL_SCANCODE_RIGHT);
        key(SDL_SCANCODE_RETURN); // Rebind A using an actual controller event.
        break;
    case 3: assert(SDL_JoystickSetVirtualButton(state.joystick, 9, 1) == 0); break;
    case 4:
        assert(SDL_JoystickSetVirtualButton(state.joystick, 9, 0) == 0);
        for (int i = 0; i < 12; ++i) key(SDL_SCANCODE_DOWN);
        key(SDL_SCANCODE_F4);
        key(SDL_SCANCODE_DOWN);
        key(SDL_SCANCODE_RIGHT); // Options row 1: window scale 3x -> 4x clears the remembered window size
        for (int i = 0; i < 5; ++i) key(SDL_SCANCODE_DOWN);
        key(SDL_SCANCODE_RETURN); // Debug tool, not a stale Controls row.
        {
            SDL_Event mouse{};
            mouse.type = SDL_MOUSEBUTTONDOWN;
            mouse.button.button = SDL_BUTTON_LEFT;
            mouse.button.x = 1010;
            mouse.button.y = 710; // Outside the visible Launch button.
            assert(SDL_PushEvent(&mouse) == 1);
        }
        break;
    case 5: {
        // Start rebinding DS row 11 on CONTROLS, then switch to the HOTKEYS tab with the mouse:
        // the pending rebind must be dropped, not applied to a hotkey row that does not exist.
        key(SDL_SCANCODE_F2);
        for (int i = 0; i < 11; ++i) key(SDL_SCANCODE_DOWN);
        key(SDL_SCANCODE_RETURN);
        SDL_Event mouse{};
        mouse.type = SDL_MOUSEBUTTONDOWN;
        mouse.button.button = SDL_BUTTON_LEFT;
        mouse.button.x = 48 + 4 * 186 + 60;
        mouse.button.y = 100;
        assert(SDL_PushEvent(&mouse) == 1);
        break;
    }
    case 6: {
        key(SDL_SCANCODE_Q); // an ordinary key press, not a rebind target
        // A dropped file becomes the ROM path (SDL hands ownership of the string to the receiver).
        SDL_Event drop{};
        drop.type = SDL_DROPFILE;
        const char* dropped = state.dropped_path.c_str();
        drop.drop.file = static_cast<char*>(SDL_malloc(std::strlen(dropped) + 1));
        std::strcpy(drop.drop.file, dropped);
        assert(SDL_PushEvent(&drop) == 1);
        break;
    }
    case 7: assert(SDL_JoystickSetVirtualButton(state.joystick, 6, 1) == 0); break;
    default: {
        SDL_Event event{};
        event.type = SDL_QUIT; // Bounded failure rather than a stuck CTest.
        assert(SDL_PushEvent(&event) == 1);
        return 0;
    }
    }
    return interval;
}
struct RejectionSequence { unsigned phase = 0; };
Uint32 reject_sequence(Uint32 interval, void* data) {
    auto& state = *static_cast<RejectionSequence*>(data);
    if (state.phase++ == 0) {
        key(SDL_SCANCODE_F5);
        return interval;
    }
    SDL_Event event{};
    event.type = SDL_QUIT; // Give Start a separate event-loop turn before quitting.
    assert(SDL_PushEvent(&event) == 1);
    return 0;
}
void assert_rejected(dsr::LauncherOptions options) {
    assert(SDL_Init(SDL_INIT_TIMER) == 0);
    RejectionSequence state;
    const SDL_TimerID timer = SDL_AddTimer(150, reject_sequence, &state);
    assert(timer != 0);
    const bool accepted = dsr::run_sdl_launcher(options);
    SDL_RemoveTimer(timer);
    assert(!accepted);
    assert(state.phase == 2); // Rejection, not an early SDL initialization failure.
}
}
#endif
int main() {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const auto directory = std::filesystem::temp_directory_path() / ("dq9-launcher-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#if defined(_WIN32)
    SDL_SetMainReady();
    SDL_setenv("APPDATA", directory.string().c_str(), 1);
    SDL_setenv("LOCALAPPDATA", directory.string().c_str(), 1);
#else
    SDL_setenv("XDG_CONFIG_HOME", directory.string().c_str(), 1);
    SDL_setenv("XDG_DATA_HOME", directory.string().c_str(), 1);
#endif
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) == 0);
    const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0,
                                                SDL_CONTROLLER_BUTTON_MAX, 1);
    assert(device >= 0);
    char guid[64]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device), guid, sizeof(guid));
    const std::string mapping = std::string(guid) + ",DQ9 Launcher Test,start:b6,leftshoulder:b9,dpright:h0.2,";
    assert(SDL_GameControllerAddMapping(mapping.c_str()) >= 0);
    SDL_Joystick* joystick = SDL_JoystickOpen(device);
    assert(joystick);
    std::filesystem::create_directories(directory);
    const auto image_path = directory / "Game Image.nds";
    write_image(image_path);
    Sequence state{joystick, dsr::host::path_to_utf8(image_path)};
    assert(SDL_AddTimer(150, sequence, &state) != 0);
    {
        const auto conf_dir = std::filesystem::path(directory) / "dq9-decomp";
        std::filesystem::create_directories(conf_dir);
        std::ofstream(conf_dir / "frontend.conf") << "window_width=800\nwindow_height=900\nscale=3\n";
    }
    dsr::LauncherOptions options;
    options.rom_path = "unavailable-test-rom.nds";
    const bool accepted = dsr::run_sdl_launcher(options);
    assert(accepted); // Start arrives only if launcher opens a controller handle.
    assert(options.language == "fr");
    assert(options.debug_tool); // F4 must reset focus after Controls row 12.
    assert(options.frontend.window_scale == 4);
    assert(options.frontend.window_width == 0 && options.frontend.window_height == 0);
    assert(state.phase >= 8); // Outside-button clicks must not launch early.
    assert(options.rom_path == image_path);
    assert(options.frontend.keycodes[11] == 22); // the abandoned rebind of row 11 changed nothing
    assert(options.frontend.gamepad_buttons[0] == SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    dsr::LauncherOptions missing;
    missing.rom_path = directory / "missing.nds";
    assert_rejected(missing);
    const auto invalid_path = directory / "invalid.nds";
    std::ofstream(invalid_path, std::ios::binary) << "not an NDS image";
    dsr::LauncherOptions invalid;
    invalid.rom_path = invalid_path;
    assert_rejected(invalid);
    dsr::LauncherOptions startup_error;
    startup_error.rom_path = image_path;
    startup_error.startup_error = "Synthetic build configuration error";
    assert_rejected(startup_error);
    std::filesystem::remove_all(directory);
#endif
}
