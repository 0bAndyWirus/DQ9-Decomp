#include "dsr/frontend.h"

namespace dsr {
std::filesystem::path resolve_rom_path(const std::filesystem::path& path,
                                      const std::filesystem::path& config_dir,
                                      const std::filesystem::path& executable_dir) {
    if (path.empty()) return path;
    const auto available = [](const std::filesystem::path& candidate) {
        std::error_code ec;
        return std::filesystem::is_regular_file(candidate, ec);
    };
    const auto absolute = [](const std::filesystem::path& candidate) {
        std::error_code ec;
        const auto result = std::filesystem::absolute(candidate, ec);
        return ec ? candidate : result.lexically_normal();
    };
    if (available(path)) return absolute(path);
    if (path.is_absolute()) return path;
    if (available(config_dir / path)) return absolute(config_dir / path);
    auto directory = absolute(executable_dir);
    while (!directory.empty()) {
        // The source launcher wrapper identifies a checkout, not an installed
        // binary directory. Worktree binaries may also find their parent repo.
        if (available(directory / "tools/run_sdl.sh") && available(directory / path))
            return absolute(directory / path);
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return path;
}
} // namespace dsr
