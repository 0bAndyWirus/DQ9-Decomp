#pragma once

#include <SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dq9::workflow_test {

struct VirtualController {
    SDL_Joystick* joystick = nullptr;
    int device_index = -1;
    std::string guid;
};

bool attach_virtual_controller(VirtualController& controller, std::string& error);
void close_virtual_controller(VirtualController& controller);
bool push_key(SDL_Scancode key);
bool push_key_event_one(SDL_Scancode key, Uint32 type, bool repeat = false,
                       SDL_Keymod modifiers = KMOD_NONE);
bool push_text_event_one(std::string_view text);
bool push_key_tap(SDL_Scancode key, SDL_Keymod modifiers = KMOD_NONE);
bool push_text(std::string_view text);
bool push_repeated_text_key(SDL_Scancode key, std::string_view text);
bool set_controller_button(VirtualController& controller, SDL_GameControllerButton button, bool down);
bool verify_latest_bundle(const std::filesystem::path& debug_root, std::string_view expected_comment,
                         std::uint64_t expected_frame, std::uint64_t expected_ticks,
                         const std::vector<std::uint32_t>& expected_top,
                         const std::vector<std::uint32_t>& expected_bottom,
                         bool require_workflow_controller, std::string& error);

} // namespace dq9::workflow_test
