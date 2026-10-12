#pragma once

#include <SDL.h>

#include <filesystem>
#include <string>

namespace dsr::frontend_detail {
void set_controller_hints();
void load_controller_mappings(const std::filesystem::path& config_dir);
std::string controller_guid(SDL_GameController* controller);
}
