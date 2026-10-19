#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dsr/frontend.h"
#include "dsr/host_path.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <vector>

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
    // Structural image check: synthetic headers only, no game data.
    {
        const auto write_image = [&](const fs::path& file, std::size_t size, std::uint32_t arm9_off, std::uint32_t arm9_size,
                                     std::uint32_t arm7_off, std::uint32_t arm7_size, std::uint32_t used, bool fix_crc) {
            std::vector<unsigned char> bytes(size, 0);
            const auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<unsigned char>(v >> (8 * i)); };
            put32(0x20, arm9_off); put32(0x2C, arm9_size); put32(0x30, arm7_off); put32(0x3C, arm7_size); put32(0x80, used);
            if (fix_crc) {
                std::uint16_t crc = 0xFFFF;
                for (std::size_t i = 0; i < 0x15E; ++i) {
                    crc ^= bytes[i];
                    for (int b = 0; b < 8; ++b) crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
                }
                bytes[0x15E] = static_cast<unsigned char>(crc); bytes[0x15F] = static_cast<unsigned char>(crc >> 8);
            }
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        bool crc_ok = false;
        write_image(base / "good.nds", 0x10000, 0x4000, 0x4000, 0x8000, 0x2000, 0x10000, true);
        assert(dsr::check_rom_image(base / "good.nds", &crc_ok).empty() && crc_ok);
        write_image(base / "patched.nds", 0x10000, 0x4000, 0x4000, 0x8000, 0x2000, 0x10000, false);
        assert(dsr::check_rom_image(base / "patched.nds", &crc_ok).empty() && !crc_ok); // header CRC alone is only a warning
        write_image(base / "cut.nds", 0x6000, 0x4000, 0x4000, 0x8000, 0x2000, 0x10000, true);
        assert(dsr::check_rom_image(base / "cut.nds").find("truncated") != std::string::npos);
        write_image(base / "junk.nds", 0x10000, 0x7457f159u, 0x1234, 0x99999999u, 0x5555, 0x42, false);
        assert(dsr::check_rom_image(base / "junk.nds").find("does not look like") != std::string::npos);
        write_image(base / "tiny.nds", 0x100, 0, 0, 0, 0, 0, false);
        assert(dsr::check_rom_image(base / "tiny.nds").find("too small") != std::string::npos);
        assert(!dsr::check_rom_image(base / "absent.nds").empty());
    }
    fs::current_path(cwd);
    fs::remove_all(base);
}
