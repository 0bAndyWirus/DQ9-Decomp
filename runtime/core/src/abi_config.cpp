#include "dsr/abi_config.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dsr {
namespace {
struct UnsignedField {
    std::string_view name;
    std::uint32_t ThreadAbi::* member;
};
constexpr UnsignedField kUnsignedFields[] = {
    {"save", &ThreadAbi::save},
    {"restore", &ThreadAbi::restore},
    {"continuation", &ThreadAbi::continuation},
    {"irq_switch", &ThreadAbi::irq_switch},
    {"thread_info", &ThreadAbi::thread_info},
    {"alarm_queue", &ThreadAbi::alarm_queue},
    {"irq_callback_lr", &ThreadAbi::irq_callback_lr},
    {"context_cpsr_offset", &ThreadAbi::context_cpsr_offset},
    {"context_regs_offset", &ThreadAbi::context_regs_offset},
    {"context_pc_offset", &ThreadAbi::context_pc_offset},
    {"context_svc_sp_offset", &ThreadAbi::context_svc_sp_offset},
    {"continuation_bias", &ThreadAbi::continuation_bias},
    {"math_state_enabled", &ThreadAbi::math_state_enabled},
    {"context_math_offset", &ThreadAbi::context_math_offset},
    {"math_div_numer_offset", &ThreadAbi::math_div_numer_offset},
    {"math_sqrt_param_offset", &ThreadAbi::math_sqrt_param_offset},
    {"math_div_control_offset", &ThreadAbi::math_div_control_offset},
    {"math_sqrt_control_offset", &ThreadAbi::math_sqrt_control_offset},
    {"info_flag_offset", &ThreadAbi::info_flag_offset},
    {"info_active_offset", &ThreadAbi::info_active_offset},
    {"info_list_offset", &ThreadAbi::info_list_offset},
    {"info_callback_offset", &ThreadAbi::info_callback_offset},
    {"alarm_head_offset", &ThreadAbi::alarm_head_offset},
    {"alarm_tail_offset", &ThreadAbi::alarm_tail_offset},
    {"ready_state", &ThreadAbi::ready_state},
    {"state_offset", &ThreadAbi::state_offset},
    {"list_next_offset", &ThreadAbi::list_next_offset},
    {"alarm_next_offset", &ThreadAbi::alarm_next_offset},
    {"alarm_blocked_queue_offset", &ThreadAbi::alarm_blocked_queue_offset},
    {"alarm_prev_offset", &ThreadAbi::alarm_prev_offset},
    {"stack_top_offset", &ThreadAbi::stack_top_offset},
    {"stack_bottom_offset", &ThreadAbi::stack_bottom_offset},
};
struct SignedField {
    std::string_view name;
    std::int32_t ThreadAbi::* member;
};
constexpr SignedField kSignedFields[] = {
    {"save_module", &ThreadAbi::save_module},
    {"restore_module", &ThreadAbi::restore_module},
    {"irq_module", &ThreadAbi::irq_module},
};
constexpr std::size_t kUnsignedCount = std::size(kUnsignedFields);
constexpr std::size_t kFieldCount = kUnsignedCount + std::size(kSignedFields);

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

[[noreturn]] void fail(std::size_t line, const std::string& message) {
    throw std::runtime_error("ABI config line " + std::to_string(line) + ": " + message);
}

std::uint32_t parse_unsigned(std::string_view text, std::size_t line) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) fail(line, "expected an unsigned 32-bit value");
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(line, "expected an unsigned 32-bit decimal or hexadecimal value");
    return result;
}

std::int32_t parse_signed(std::string_view text, std::size_t line) {
    if (text.empty()) fail(line, "expected a signed 32-bit module number");
    std::int32_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(line, "expected a signed 32-bit decimal module number");
    return result;
}

constexpr std::array kBackupNames{"auto", "eeprom512b", "eeprom8k", "eeprom64k",
                                  "eeprom128k", "flash256k", "flash512k", "flash1m", "flash8m"};
constexpr std::array kBackupTypes{BackupType::Auto, BackupType::Eeprom512B, BackupType::Eeprom8K,
                                  BackupType::Eeprom64K, BackupType::Eeprom128K, BackupType::Flash256K,
                                  BackupType::Flash512K, BackupType::Flash1M, BackupType::Flash8M};

BackupType parse_backup(std::string_view text, std::size_t line) {
    for (std::size_t i = 0; i < kBackupNames.size(); ++i)
        if (text == kBackupNames[i]) return kBackupTypes[i];
    fail(line, "unknown backup_type");
}
} // namespace

BackupType parse_backup_type(std::string_view name) {
    for (std::size_t i = 0; i < kBackupNames.size(); ++i)
        if (name == kBackupNames[i]) return kBackupTypes[i];
    std::string expected;
    for (const char* known : kBackupNames) expected += std::string(expected.empty() ? "" : ", ") + known;
    throw std::invalid_argument("unknown backup type '" + std::string(name) + "' (expected one of: " + expected + ")");
}

GuestAbiConfig load_guest_abi_config(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        if (!std::filesystem::exists(path, error) && !error) {
            std::fprintf(stderr, "[dsr] ABI configuration file not found; thread overrides disabled: %s\n",
                         path.string().c_str());
            return {};
        }
        throw std::runtime_error("cannot open ABI config: " + path.string());
    }
    GuestAbiConfig config;
    std::array<std::array<bool, kFieldCount>, 2> seen{};
    std::array<bool, 3> sections{};
    bool backup_seen = false;
    int section = -1;
    std::size_t line_number = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        std::string_view text = trim(line);
        text = trim(text.substr(0, text.find_first_of("#;")));
        if (text.empty()) continue;
        if (text.front() == '[' && text.back() == ']') {
            const auto name = trim(text.substr(1, text.size() - 2));
            if (name == "arm9") section = 0;
            else if (name == "arm7") section = 1;
            else if (name == "cartridge") section = 2;
            else fail(line_number, "unknown section");
            if (sections[section]) fail(line_number, "duplicate section");
            sections[section] = true;
            if (section == 0) config.arm9.emplace();
            if (section == 1) config.arm7.emplace();
            continue;
        }
        const auto equals = text.find('=');
        if (section < 0 || equals == std::string_view::npos)
            fail(line_number, "expected a section or key=value");
        const auto key = trim(text.substr(0, equals));
        const auto value = trim(text.substr(equals + 1));
        if (section == 2) {
            if (key != "backup_type") fail(line_number, "unknown cartridge field");
            if (backup_seen) fail(line_number, "duplicate backup_type");
            config.backup_type = parse_backup(value, line_number);
            backup_seen = true;
            continue;
        }
        ThreadAbi& abi = section == 0 ? *config.arm9 : *config.arm7;
        std::size_t index = 0;
        for (; index < kUnsignedCount; ++index) {
            if (key != kUnsignedFields[index].name) continue;
            if (seen[section][index]) fail(line_number, "duplicate field");
            abi.*kUnsignedFields[index].member = parse_unsigned(value, line_number);
            break;
        }
        if (index == kUnsignedCount) {
            index = kFieldCount;
            for (std::size_t i = 0; i < std::size(kSignedFields); ++i) {
                if (key != kSignedFields[i].name) continue;
                index = kUnsignedCount + i;
                if (seen[section][index]) fail(line_number, "duplicate field");
                abi.*kSignedFields[i].member = parse_signed(value, line_number);
                break;
            }
            if (index == kFieldCount)
                fail(line_number, "unknown thread field");
        }
        seen[section][index] = true;
    }
    if (input.bad()) throw std::runtime_error("cannot read ABI config: " + path.string());
    for (unsigned cpu = 0; cpu < 2; ++cpu) {
        if (!sections[cpu]) continue;
        for (std::size_t i = 0; i < kFieldCount; ++i) {
            if (!seen[cpu][i]) {
                const auto key = i < kUnsignedCount ? kUnsignedFields[i].name
                                                    : kSignedFields[i - kUnsignedCount].name;
                fail(line_number, "missing " + std::string(cpu == 0 ? "arm9." : "arm7.") + std::string(key));
            }
        }
        const ThreadAbi& abi = cpu == 0 ? *config.arm9 : *config.arm7;
        if (!abi.save || !abi.restore || !abi.irq_switch || !abi.continuation || !abi.thread_info ||
            !abi.alarm_queue || abi.math_state_enabled > 1 ||
            abi.ready_state > std::numeric_limits<std::uint16_t>::max() ||
            (abi.stack_top_offset == 0) != (abi.stack_bottom_offset == 0))
            fail(line_number, "invalid thread addresses, state, or stack-bound settings");
    }
    if (sections[2] && !backup_seen) fail(line_number, "missing cartridge.backup_type");
    return config;
}

std::filesystem::path find_guest_abi_config(const std::filesystem::path& start) {
    std::error_code error;
    std::filesystem::path directory = start;
    for (int depth = 0; depth < 6 && !directory.empty(); ++depth) {
        if (depth == 0 && std::filesystem::is_regular_file(directory / "abi.ini", error)) return directory / "abi.ini";
        const auto generated = directory / "build" / "gen" / "abi.ini";
        if (std::filesystem::is_regular_file(generated, error)) return generated;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return {};
}

std::filesystem::path guest_abi_config_path_from_environment() {
#if defined(_WIN32)
    if (const wchar_t* path = _wgetenv(L"DQ9_ABI_CONFIG")) return path;
#else
    if (const char* path = std::getenv("DQ9_ABI_CONFIG")) return path;
#endif
    return {};
}

} // namespace dsr
