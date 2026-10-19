#include "dsr/frontend.h"

#include <array>
#include <fstream>

namespace dsr {
namespace {
std::uint32_t le32(const std::array<std::uint8_t, 0x200>& h, std::size_t at) {
    return h[at] | (static_cast<std::uint32_t>(h[at + 1]) << 8) | (static_cast<std::uint32_t>(h[at + 2]) << 16) |
           (static_cast<std::uint32_t>(h[at + 3]) << 24);
}
std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}
} // namespace

std::string check_rom_image(const std::filesystem::path& path, bool* header_crc_ok) {
    if (header_crc_ok) *header_crc_ok = false;
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) return "the file cannot be read";
    if (file_size < 0x4000) return "the file is too small to be a Nintendo DS image";
    std::array<std::uint8_t, 0x200> header{};
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())))
        return "the file cannot be read";
    if (header_crc_ok) *header_crc_ok = crc16(header.data(), 0x15E) == (header[0x15E] | (header[0x15F] << 8));
    const std::uint64_t arm9_end = static_cast<std::uint64_t>(le32(header, 0x20)) + le32(header, 0x2C);
    const std::uint64_t arm7_end = static_cast<std::uint64_t>(le32(header, 0x30)) + le32(header, 0x3C);
    if (le32(header, 0x2C) == 0 || le32(header, 0x3C) == 0 || arm9_end > file_size || arm7_end > file_size) {
        const std::uint32_t used = le32(header, 0x80);
        if (used > file_size && used < (1u << 30))
            return "the image is truncated (" + std::to_string(file_size) + " of " + std::to_string(used) +
                   " bytes); copy or dump it again";
        return "this does not look like a Nintendo DS image (its header is not valid)";
    }
    const std::uint32_t used = le32(header, 0x80);
    if (used > file_size && used < (1u << 30))
        return "the image is truncated (" + std::to_string(file_size) + " of " + std::to_string(used) +
               " bytes); copy or dump it again";
    return {};
}

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
