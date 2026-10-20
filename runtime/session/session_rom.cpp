#include "dsr/session.h"
#include "dsr/rt.h"

namespace dsr {

// Keep generated-table references in their own archive member. ROM-free hosts
// using Session(MachineConfig) must link without generated game code on PE too.
Session::Session(const SessionConfig& config)
    : Session([&config] {
          MachineConfig machine;
          machine.rom_path = config.rom_path;
          machine.save_path = config.save_path;
          machine.firmware.language = config.language;
          machine.deterministic_rtc = config.deterministic_rtc;
          machine.rtc_epoch_seconds = config.rtc_epoch_seconds;
          machine.rtc_timezone_offset_minutes = config.rtc_timezone_offset_minutes;
          const auto abi_path = config.abi_config_path.empty()
              ? guest_abi_config_path_from_environment() : config.abi_config_path;
          machine.guest_abi = load_guest_abi_config(abi_path);
          if (!config.backup_type.empty() && config.backup_type != "auto")
              machine.guest_abi.backup_type = parse_backup_type(config.backup_type); // throws on an unknown name
          machine.arm9_table = gen_table_arm9();
          machine.arm7_table = gen_table_arm7();
          machine.arm9_regions = gen_dispatch_regions_arm9();
          machine.arm7_regions = gen_dispatch_regions_arm7();
          return machine;
      }()) {}

} // namespace dsr
