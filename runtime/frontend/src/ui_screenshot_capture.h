#pragma once

#include <filesystem>

namespace dsr::ui_detail {

bool capture_launcher_screenshots(const std::filesystem::path& directory);
bool capture_debug_screenshots(const std::filesystem::path& directory);

} // namespace dsr::ui_detail
