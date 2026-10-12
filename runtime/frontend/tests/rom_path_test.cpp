#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dsr/frontend.h"
#include "dsr/host_path.h"
#include <cassert>
#include <chrono>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    const std::string utf8 = "ROM/Cr\xc3\xa9" "ateur \xe9\xbe\x8d.nds";
    assert(dsr::host::path_to_utf8(dsr::host::path_from_utf8(utf8)) == utf8);
    assert(dsr::host::path_with_suffix(dsr::host::path_from_utf8(utf8), ".rtc") ==
           dsr::host::path_from_utf8(utf8 + ".rtc"));
    const auto base = fs::temp_directory_path() / ("dq9-rom-path-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto config = base / "config";
    const auto repo = base / "repo";
    const auto binary = repo / "build/game/game";
    const auto cwd = fs::current_path();
    fs::create_directories(config);
    fs::create_directories(repo / "ROM");
    fs::create_directories(repo / "tools");
    fs::create_directories(binary);
    fs::create_directories(base / "other");
    std::ofstream(repo / "tools/run_sdl.sh").put('\n');
    std::ofstream(repo / "ROM/game.nds").put('\n');
    fs::current_path(base / "other");
    assert(dsr::resolve_rom_path("ROM/game.nds", config, binary) == repo / "ROM/game.nds");
    std::ofstream(config / "game.nds").put('\n');
    assert(dsr::resolve_rom_path("game.nds", config, binary) == config / "game.nds");
    std::ofstream("game.nds").put('\n');
    assert(dsr::resolve_rom_path("game.nds", config, binary) == base / "other/game.nds");
    assert(dsr::resolve_rom_path(config / "game.nds", config, binary) == config / "game.nds");
    assert(dsr::resolve_rom_path("missing.nds", config, binary) == "missing.nds");
    assert(dsr::resolve_rom_path(base / "missing.nds", config, binary) == base / "missing.nds");
    assert(dsr::resolve_rom_path("", config, binary).empty());
    // Directories do not count as usable ROM files.
    fs::create_directory(config / "folder.nds");
    assert(dsr::resolve_rom_path("folder.nds", config, binary) == "folder.nds");
    fs::current_path(cwd);
    fs::remove_all(base);
}
