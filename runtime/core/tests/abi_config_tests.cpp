#include "dsr/abi_config.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace dsr::tests {
namespace {
// Invented entries and reordered fields: not a cartridge-derived layout.
const std::string kSyntheticFields = R"ini(
save = 0x02110000
restore = 0x02110100
continuation = 0x02110200
irq_switch = 0x02120100
save_module = -1
restore_module = -1
irq_module = -2
thread_info = 0x02130000
alarm_queue = 0x02131000
irq_callback_lr = 0x02120200
context_cpsr_offset = 32
context_regs_offset = 48
context_pc_offset = 120
context_svc_sp_offset = 128
continuation_bias = 8
math_state_enabled = 1
context_math_offset = 208
math_div_numer_offset = 16
math_sqrt_param_offset = 4
math_div_control_offset = 0
math_sqrt_control_offset = 2
info_flag_offset = 24
info_active_offset = 36
info_list_offset = 4
info_callback_offset = 44
alarm_head_offset = 8
alarm_tail_offset = 20
ready_state = 7
state_offset = 136
list_next_offset = 140
alarm_next_offset = 168
alarm_blocked_queue_offset = 160
alarm_prev_offset = 164
stack_top_offset = 176
stack_bottom_offset = 180
)ini";

struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("dq9-abi-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "synthetic.ini";
    Fixture() { std::filesystem::create_directories(directory); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    GuestAbiConfig load(const std::string& text) {
        std::ofstream file(path);
        file << text;
        file.close();
        assert(file.good());
        return load_guest_abi_config(path);
    }
    void reject(const std::string& text) {
        bool threw = false;
        try { (void)load(text); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw);
    }
};
} // namespace

void run_abi_config_tests() {
    const auto disabled = load_guest_abi_config({});
    assert(!disabled.arm9 && !disabled.arm7 && disabled.backup_type == BackupType::Auto);
    Fixture fixture;
    const auto empty_file = fixture.load("# no CPU layout supplied\n");
    assert(!empty_file.arm9 && !empty_file.arm7);
    const auto config = fixture.load(" ; synthetic config\n[arm9]\n" + kSyntheticFields +
                                    "[arm7]\n" + kSyntheticFields +
                                    "[cartridge]\n backup_type = eeprom64k ; explicit device\n");
    assert(config.arm9 && config.arm7 && config.backup_type == BackupType::Eeprom64K);
    // The command-line override uses the same names as [cartridge] backup_type.
    assert(parse_backup_type("auto") == BackupType::Auto && parse_backup_type("eeprom512b") == BackupType::Eeprom512B);
    assert(parse_backup_type("eeprom8k") == BackupType::Eeprom8K && parse_backup_type("eeprom64k") == BackupType::Eeprom64K);
    assert(parse_backup_type("eeprom128k") == BackupType::Eeprom128K && parse_backup_type("flash256k") == BackupType::Flash256K);
    assert(parse_backup_type("flash512k") == BackupType::Flash512K && parse_backup_type("flash1m") == BackupType::Flash1M);
    assert(parse_backup_type("flash8m") == BackupType::Flash8M);
    for (const char* bad : {"", "EEPROM64K", "eeprom", "flash"}) {
        bool rejected = false;
        try { (void)parse_backup_type(bad); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    for (const ThreadAbi* a : {&*config.arm9, &*config.arm7}) {
        assert(a->save == 0x02110000 && a->restore == 0x02110100 && a->continuation == 0x02110200);
        assert(a->irq_switch == 0x02120100 && a->irq_callback_lr == 0x02120200);
        assert(a->save_module == -1 && a->restore_module == -1 && a->irq_module == -2);
        assert(a->thread_info == 0x02130000 && a->alarm_queue == 0x02131000);
        assert(a->context_cpsr_offset == 32 && a->context_regs_offset == 48);
        assert(a->context_pc_offset == 120 && a->context_svc_sp_offset == 128 && a->continuation_bias == 8);
        assert(a->math_state_enabled == 1 && a->context_math_offset == 208);
        assert(a->math_div_numer_offset == 16 && a->math_sqrt_param_offset == 4);
        assert(a->math_div_control_offset == 0 && a->math_sqrt_control_offset == 2);
        assert(a->info_flag_offset == 24 && a->info_active_offset == 36);
        assert(a->info_list_offset == 4 && a->info_callback_offset == 44);
        assert(a->alarm_head_offset == 8 && a->alarm_tail_offset == 20);
        assert(a->ready_state == 7 && a->state_offset == 136 && a->list_next_offset == 140);
        assert(a->alarm_next_offset == 168 && a->alarm_blocked_queue_offset == 160 && a->alarm_prev_offset == 164);
        assert(a->stack_top_offset == 176 && a->stack_bottom_offset == 180);
    }
    const auto one_cpu = fixture.load("[arm7]\n" + kSyntheticFields);
    assert(!one_cpu.arm9 && one_cpu.arm7);
    const auto cartridge_only = fixture.load("[cartridge]\nbackup_type=flash1m\n");
    assert(!cartridge_only.arm9 && !cartridge_only.arm7 && cartridge_only.backup_type == BackupType::Flash1M);
    fixture.reject("[arm9]\nsave=1\n");
    fixture.reject("[arm9]\n" + kSyntheticFields + "save=1\n");
    fixture.reject("[arm9]\n" + kSyntheticFields + "unknown=1\n");
    fixture.reject("[arm9]\n" + kSyntheticFields + "[arm9]\n" + kSyntheticFields);
    fixture.reject("[unknown]\nsave=1\n");
    fixture.reject("save=1\n");
    fixture.reject("[arm9]\nsave=-1\n");
    fixture.reject("[arm9]\nsave=\n");
    fixture.reject("[arm9]\nsave=0x\n");
    fixture.reject("[arm9]\nsave_module=\n");
    fixture.reject("[arm9]\nsave=0x100000000\n");
    fixture.reject("[arm9]\nsave=0x123junk\n");
    fixture.reject("[arm9]\nsave_module=2147483648\n");
    fixture.reject("[cartridge]\n");
    fixture.reject("[cartridge]\nbackup_type=unknown\n");
    fixture.reject("[cartridge]\nbackup_type=auto\nbackup_type=auto\n");
    std::string missing = kSyntheticFields;
    const auto field = missing.find("info_list_offset = 4\n");
    missing.erase(field, std::string("info_list_offset = 4\n").size());
    fixture.reject("[arm9]\n" + missing);
    std::string invalid = kSyntheticFields;
    invalid.replace(invalid.find("math_state_enabled = 1"), std::string("math_state_enabled = 1").size(),
                    "math_state_enabled = 2");
    fixture.reject("[arm9]\n" + invalid);
    const auto missing_file = load_guest_abi_config(fixture.directory / "missing.ini");
    assert(!missing_file.arm9 && !missing_file.arm7 && missing_file.backup_type == BackupType::Auto);
#if defined(_WIN32)
    const wchar_t* previous = _wgetenv(L"DQ9_ABI_CONFIG");
    const std::wstring saved = previous ? previous : L"";
    assert(_wputenv_s(L"DQ9_ABI_CONFIG", fixture.path.c_str()) == 0);
    assert(guest_abi_config_path_from_environment() == fixture.path);
    assert(_wputenv_s(L"DQ9_ABI_CONFIG", L"") == 0);
    assert(guest_abi_config_path_from_environment().empty());
    assert(_wputenv_s(L"DQ9_ABI_CONFIG", saved.c_str()) == 0);
#else
    const char* previous = std::getenv("DQ9_ABI_CONFIG");
    const bool had_previous = previous != nullptr;
    const std::string saved = previous ? previous : "";
    assert(setenv("DQ9_ABI_CONFIG", fixture.path.c_str(), 1) == 0);
    assert(guest_abi_config_path_from_environment() == fixture.path);
    assert(unsetenv("DQ9_ABI_CONFIG") == 0);
    assert(guest_abi_config_path_from_environment().empty());
    assert(had_previous ? setenv("DQ9_ABI_CONFIG", saved.c_str(), 1) == 0 : unsetenv("DQ9_ABI_CONFIG") == 0);
#endif
}
} // namespace dsr::tests
