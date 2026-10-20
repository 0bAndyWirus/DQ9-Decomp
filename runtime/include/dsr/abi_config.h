// Caller-supplied layout for optional guest thread overrides.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include "dsr/periph.h"

namespace dsr {

// No guest addresses or layout defaults are built into the runtime. A supplied
// layout must implement the save/restore contract described in core/THREADS.md.
struct ThreadAbi {
    std::uint32_t save = 0;
    std::uint32_t restore = 0;
    std::uint32_t continuation = 0;
    std::uint32_t irq_switch = 0;
    std::int32_t save_module = 0;
    std::int32_t restore_module = 0;
    std::int32_t irq_module = 0;
    std::uint32_t thread_info = 0;
    std::uint32_t alarm_queue = 0;
    std::uint32_t irq_callback_lr = 0;
    std::uint32_t context_cpsr_offset = 0;
    std::uint32_t context_regs_offset = 0;
    std::uint32_t context_pc_offset = 0;
    std::uint32_t context_svc_sp_offset = 0;
    std::uint32_t continuation_bias = 0;
    std::uint32_t math_state_enabled = 0;
    std::uint32_t context_math_offset = 0;
    std::uint32_t math_div_numer_offset = 0;
    std::uint32_t math_sqrt_param_offset = 0;
    std::uint32_t math_div_control_offset = 0;
    std::uint32_t math_sqrt_control_offset = 0;
    std::uint32_t info_flag_offset = 0;
    std::uint32_t info_active_offset = 0;
    std::uint32_t info_list_offset = 0;
    std::uint32_t info_callback_offset = 0;
    std::uint32_t alarm_head_offset = 0;
    std::uint32_t alarm_tail_offset = 0;
    std::uint32_t ready_state = 0;
    std::uint32_t state_offset = 0;
    std::uint32_t list_next_offset = 0;
    std::uint32_t alarm_next_offset = 0;
    std::uint32_t alarm_blocked_queue_offset = 0;
    std::uint32_t alarm_prev_offset = 0;
    // A zero pair disables optional stack-bound checks.
    std::uint32_t stack_top_offset = 0;
    std::uint32_t stack_bottom_offset = 0;
};

struct GuestAbiConfig {
    std::optional<ThreadAbi> arm9;
    std::optional<ThreadAbi> arm7;
    BackupType backup_type = BackupType::Auto;
};

// An empty path or missing file returns an unconfigured layout (missing files
// log a warning). Existing unreadable, malformed, or incomplete files throw.
GuestAbiConfig load_guest_abi_config(const std::filesystem::path& path);

// Parses a backup chip name as used by [cartridge] backup_type: auto, eeprom512b, eeprom8k, eeprom64k,
// eeprom128k, flash256k, flash512k, flash1m, flash8m. Throws std::invalid_argument otherwise.
BackupType parse_backup_type(std::string_view name);
std::filesystem::path guest_abi_config_path_from_environment();

// Locates the locally generated configuration when neither a flag nor DQ9_ABI_CONFIG names one: `abi.ini`
// next to `start`, else `build/gen/abi.ini` in `start` or up to five parent folders (an executable inside the
// build tree of the repository that produced it). Empty if nothing is found.
std::filesystem::path find_guest_abi_config(const std::filesystem::path& start);

} // namespace dsr
