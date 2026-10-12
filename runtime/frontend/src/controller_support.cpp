#include "controller_support.h"

namespace dsr::frontend_detail {

void set_controller_hints() {
#ifdef SDL_HINT_JOYSTICK_HIDAPI
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_STEAM
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
#endif
}

void load_controller_mappings(const std::filesystem::path& config_dir) {
    const auto mappings = config_dir / "gamecontrollerdb.txt";
    if (std::filesystem::exists(mappings)) SDL_GameControllerAddMappingsFromFile(mappings.string().c_str());
}

std::string controller_guid(SDL_GameController* controller) {
    if (!controller) return {};
    char guid[64]{};
    const SDL_JoystickGUID value = SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller));
    SDL_JoystickGetGUIDString(value, guid, static_cast<int>(sizeof(guid)));
    return guid;
}

} // namespace dsr::frontend_detail
