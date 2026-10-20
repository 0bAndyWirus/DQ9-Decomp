#include "dsr/periph.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>

// Test-only syscall interposition makes publication races and denied cleanup deterministic,
// without adding production hooks, relying on timing, or requiring non-root permissions.
namespace save_fault {
bool collide_next_backup = false;
bool deny_restore_commit = false;
bool deny_pending_cleanup = false;
bool deny_stage_publication = false;
std::string restore_target;
std::string collided_target;
std::string retained_stage;

bool has_suffix(const char* path, std::string_view suffix) {
    const std::string_view name(path);
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

struct Scope {
    ~Scope() {
        collide_next_backup = false;
        deny_restore_commit = false;
        deny_pending_cleanup = false;
        deny_stage_publication = false;
        restore_target.clear();
        collided_target.clear();
        retained_stage.clear();
    }
};
} // namespace save_fault

extern "C" int link(const char* source, const char* target) noexcept {
    if (save_fault::deny_stage_publication && save_fault::has_suffix(source, ".pending")) {
        save_fault::retained_stage = source;
        errno = EACCES;
        return -1;
    }
    if (save_fault::collide_next_backup && save_fault::has_suffix(target, ".bak")) {
        save_fault::collide_next_backup = false;
        const int fd = ::open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        assert(fd >= 0);
        std::array<char, 512> bytes{};
        bytes.fill('\x6c');
        const auto amount = ::write(fd, bytes.data(), bytes.size());
        const int closed = ::close(fd);
        assert(amount == static_cast<ssize_t>(bytes.size()));
        assert(closed == 0);
        save_fault::collided_target = target;
    }
    return static_cast<int>(::syscall(SYS_linkat, AT_FDCWD, source, AT_FDCWD, target, 0));
}

extern "C" int rename(const char* source, const char* target) noexcept {
    if (save_fault::deny_restore_commit && save_fault::restore_target == target) {
        errno = EACCES;
        return -1;
    }
    return static_cast<int>(::syscall(SYS_renameat, AT_FDCWD, source, AT_FDCWD, target));
}

extern "C" int unlink(const char* path) noexcept {
    if (save_fault::deny_pending_cleanup && save_fault::has_suffix(path, ".pending")) {
        save_fault::retained_stage = path;
        errno = EACCES;
        return -1;
    }
    return static_cast<int>(::syscall(SYS_unlinkat, AT_FDCWD, path, 0));
}
#endif


namespace {
constexpr std::uint32_t kAuxCnt = 0x040001a0;
constexpr std::uint32_t kAuxData = 0x040001a2;
constexpr std::uint32_t kRomCtrl = 0x040001a4;
constexpr std::uint32_t kCardCmd = 0x040001a8;
constexpr std::uint32_t kCardData = 0x04100010;
constexpr std::uint32_t kSpiCnt = 0x040001c0;
constexpr std::uint32_t kSpiData = 0x040001c2;
constexpr std::uint32_t kRtc = 0x04000138;

struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("dsr-periph-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::filesystem::path path;
};

std::filesystem::path make_rom(const std::filesystem::path& path,
                               bool distinct_card_blocks = false) {
    std::vector<std::uint8_t> bytes(0x20000);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>((i * 13u + 7u) & 0xffu);
    if (distinct_card_blocks) {
        constexpr std::array<std::size_t, 3> offsets{0x8000, 0x8200, 0x8400};
        constexpr std::array<std::uint8_t, 3> markers{0x11, 0x22, 0x33};
        for (std::size_t i = 0; i < offsets.size(); ++i)
            std::fill_n(bytes.begin() + offsets[i], 0x200, markers[i]);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
    return path;
}

std::vector<std::uint8_t> aux_transfer(dsr::GameCard& card,
                                       const std::vector<std::uint8_t>& tx) {
    std::vector<std::uint8_t> rx;
    rx.reserve(tx.size());
    for (std::size_t i = 0; i < tx.size(); ++i) {
        const std::uint16_t control = static_cast<std::uint16_t>(0xa000u |
            (i + 1 < tx.size() ? 0x0040u : 0u));
        card.io_write(kAuxCnt, control, 16);
        card.io_write(kAuxData, tx[i], 8);
        rx.push_back(static_cast<std::uint8_t>(card.io_read(kAuxData, 8)));
    }
    return rx;
}

std::uint16_t crc16(const std::uint8_t* data, std::size_t size, std::uint16_t initial = 0xffff) {
    std::uint16_t crc = initial;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xa001u)
                             : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}

std::uint16_t read_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

void write_card_command(dsr::GameCard& card, std::uint8_t command, std::uint32_t address) {
    const std::array<std::uint8_t, 8> bytes = {
        command, static_cast<std::uint8_t>(address >> 24),
        static_cast<std::uint8_t>(address >> 16), static_cast<std::uint8_t>(address >> 8),
        static_cast<std::uint8_t>(address), 0, 0, 0};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        card.io_write(kCardCmd + static_cast<std::uint32_t>(i), bytes[i], 8);
}

std::uint8_t spi_byte(dsr::Spi& spi, std::uint8_t value) {
    spi.io_write(kSpiData, value, 8);
    return static_cast<std::uint8_t>(spi.io_read(kSpiData, 8));
}

void rtc_gpio(dsr::Rtc& rtc, std::uint8_t value) { rtc.io_write(kRtc, value, 8); }

void rtc_write_byte(dsr::Rtc& rtc, std::uint8_t value, std::uint8_t base) {
    for (unsigned bit = 0; bit < 8; ++bit) {
        const std::uint8_t low = static_cast<std::uint8_t>((base & ~0x02u) |
            (((value >> bit) & 1u) ? 1u : 0u));
        rtc_gpio(rtc, low);
        rtc_gpio(rtc, static_cast<std::uint8_t>(low | 0x02u));
    }
}

std::uint8_t rtc_read_byte(dsr::Rtc& rtc, std::uint8_t base) {
    std::uint8_t value = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        rtc_gpio(rtc, static_cast<std::uint8_t>(base & ~0x02u));
        rtc_gpio(rtc, static_cast<std::uint8_t>(base | 0x02u));
        value |= static_cast<std::uint8_t>((rtc.io_read(kRtc, 8) & 1u) << bit);
    }
    return value;
}

void test_card_rom_and_chip_id(const std::filesystem::path& rom,
                               const std::filesystem::path& save) {
    dsr::GameCard card(rom, save, dsr::BackupType::Flash512K);
    assert(card.chip_id() == 0x00001fc2u);
    assert(card.rom_size() == 0x20000);
    std::array<std::uint8_t, 4> raw{};
    card.read_rom(0x8000, raw.data(), 4);
    assert((raw == std::array<std::uint8_t, 4>{7, 20, 33, 46}));

    int ready = 0;
    int done = 0;
    card.on_data_ready = [&] { ++ready; };
    card.on_transfer_done_irq = [&] { ++done; };
    card.io_write(kAuxCnt, 0xc000, 16); // slot enabled, ROM transfer IRQ
    for (unsigned code = 1; code <= 7; ++code) {
        const unsigned length = code == 7 ? 4u : 0x100u << code;
        write_card_command(card, 0xb7, 0x8000);
        card.io_write(kRomCtrl, 0x80000000u | (code << 24), 32);
        for (unsigned offset = 0; offset < length; offset += 4) {
            card.run(offset == 0 ? 44 : 2); // slow clock, zero gap: (8 * 5 + 4)
            assert((card.io_read(kRomCtrl, 32) & 0x80800000u) == 0x80800000u);
            assert(done == static_cast<int>(code - 1));
            const int previous_ready = ready;
            std::uint32_t expected = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                expected |= (((0x8000u + offset + byte) * 13u + 7u) & 0xffu) << (byte * 8);
            assert(card.io_read(kCardData, 32) == expected);
            assert((card.io_read(kRomCtrl, 32) & (1u << 23)) == 0);
            card.run(1); // No premature next word or duplicate completion.
            assert(ready == previous_ready);
            if (offset + 4 < length) {
                assert((card.io_read(kRomCtrl, 32) & (1u << 31)) != 0);
                assert(done == static_cast<int>(code - 1));
            }
        }
        assert((card.io_read(kRomCtrl, 32) & 0x80800000u) == 0);
        assert(done == static_cast<int>(code));
    }

    write_card_command(card, 0xb8, 0);
    card.io_write(kRomCtrl, 0x87000000u, 32); // 4-byte block
    card.run(44);
    assert(card.io_read(kCardData, 32) == 0x00001fc2u);
    assert(done == 8);
}

void test_card_low_address_alias(const std::filesystem::path& rom,
                                 const std::filesystem::path& save) {
    constexpr std::array<std::uint32_t, 3> low_addresses{0x1000, 0x1200, 0x1400};
    constexpr std::array<std::uint32_t, 3> high_addresses{0x8000, 0x8200, 0x8400};
    constexpr std::array<std::uint8_t, 3> markers{0x11, 0x22, 0x33};
    for (const std::uint32_t address : low_addresses) {
        dsr::GameCard card(rom, save, dsr::BackupType::Flash512K);
        write_card_command(card, 0xb7, address);
        card.io_write(kRomCtrl, 0x82000000u, 32);
        card.run(44);
        assert((card.io_read(kRomCtrl, 32) & (1u << 23)) != 0);
        const std::uint32_t expected = static_cast<std::uint32_t>(markers[0]) * 0x01010101u;
        assert(card.io_read(kCardData, 32) == expected);
    }
    for (std::size_t i = 0; i < high_addresses.size(); ++i) {
        dsr::GameCard card(rom, save, dsr::BackupType::Flash512K);
        write_card_command(card, 0xb7, high_addresses[i]);
        card.io_write(kRomCtrl, 0x82000000u, 32);
        card.run(44);
        assert((card.io_read(kRomCtrl, 32) & (1u << 23)) != 0);
        const std::uint32_t expected = static_cast<std::uint32_t>(markers[i]) * 0x01010101u;
        assert(card.io_read(kCardData, 32) == expected);
    }
}

void test_backup_eeprom(const std::filesystem::path& rom,
                        const std::filesystem::path& save) {
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x01, 0xa5});
        (void)aux_transfer(card, {0x06});
        const auto read = aux_transfer(card, {0x03, 0x01, 0x00});
        assert(read.back() == 0xa5);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x0a, 0x01, 0x3c});
        (void)aux_transfer(card, {0x06});
        const auto high_read = aux_transfer(card, {0x0b, 0x01, 0x00});
        assert(high_read.back() == 0x3c);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x01, 0x0c}); // Protect the entire EEPROM.
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x01, 0x00});
        const auto protected_read = aux_transfer(card, {0x03, 0x01, 0x00});
        assert(protected_read.back() == 0xa5);
        card.flush_save();
        assert(std::filesystem::file_size(save) == 512);
    }
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        const auto read = aux_transfer(card, {0x03, 0x01, 0x00});
        assert(read.back() == 0xa5);
        const auto high_read = aux_transfer(card, {0x0b, 0x01, 0x00});
        assert(high_read.back() == 0x3c);
    }
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}

std::vector<std::filesystem::path> save_backups(const std::filesystem::path& save) {
    std::vector<std::filesystem::path> backups;
    const auto directory = save.parent_path() / "backups";
    if (!std::filesystem::exists(directory)) return backups;
    const auto prefix = save.filename().string() + ".";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!std::filesystem::is_regular_file(entry.symlink_status())) continue;
        const auto name = entry.path().filename().string();
        if (name.size() != prefix.size() + 37 || name.compare(0, prefix.size(), prefix) != 0) continue;
        const auto suffix = name.substr(prefix.size());
        bool matched = true;
        for (std::size_t i = 0; i < suffix.size(); ++i) {
            const char separator = i == 8 ? 'T' : i == 15 ? '.' : i == 25 ? 'Z' :
                                   i == 26 ? '-' : i == 33 ? '.' :
                                   i == 34 ? 'b' : i == 35 ? 'a' : i == 36 ? 'k' : '\0';
            if (separator != '\0' ? suffix[i] != separator : suffix[i] < '0' || suffix[i] > '9')
                matched = false;
        }
        if (matched) backups.push_back(entry.path());
    }
    std::sort(backups.begin(), backups.end());
    return backups;
}

void write_eeprom_byte(dsr::GameCard& card, std::uint8_t value) {
    (void)aux_transfer(card, {0x06});
    (void)aux_transfer(card, {0x02, 0x01, value});
}

std::string read_eeprom_bytes(dsr::GameCard& card) {
    std::vector<std::uint8_t> command(514, 0);
    command[0] = 0x03;
    const auto bytes = aux_transfer(card, command);
    return std::string(bytes.begin() + 2, bytes.end());
}

void assert_restore_fails(dsr::GameCard& card, const std::filesystem::path& source = {}) {
    bool failed = false;
    try {
        card.restore_save(source);
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Failed);
}

// Preserve the pre-session file once, including across reset, not at every autosave.
void test_previous_save_backup(const std::filesystem::path& rom, const std::filesystem::path& save) {
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Idle);
        assert(card.flush_save());
        assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Idle);
        assert(!std::filesystem::exists(save));
        write_eeprom_byte(card, 0x11);
        assert(card.flush_save());
        assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
        assert(card.flush_save());
        assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    }
    const std::string first_session = slurp(save);
    assert(first_session.size() == 512);
    assert(save_backups(save).empty()); // No old file existed before the initial write.
    std::string second_session;
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        assert(card.flush_save());
        assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Idle);
        assert(save_backups(save).empty());
        write_eeprom_byte(card, 0x22);
        assert(card.flush_save());
        const auto backups = save_backups(save);
        assert(backups.size() == 1);
        assert(slurp(backups.front()) == first_session);
        write_eeprom_byte(card, 0x33);
        assert(card.flush_save());
        card.reset();
        write_eeprom_byte(card, 0x44);
        assert(card.flush_save());
        assert(save_backups(save) == backups);
        assert(slurp(backups.front()) == first_session);
        second_session = slurp(save);
        assert(second_session != first_session);
    }
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        write_eeprom_byte(card, 0x55);
        assert(card.flush_save());
        const auto backups = save_backups(save);
        assert(backups.size() == 2);
        assert(slurp(backups.front()) == first_session);
        assert(slurp(backups.back()) == second_session);
    }
    auto legacy_backup = save;
    legacy_backup += ".bak";
    assert(!std::filesystem::exists(legacy_backup));
}

void test_save_backup_rotation(const std::filesystem::path& rom, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    for (const std::size_t limit : {std::size_t{5}, std::size_t{2}, std::size_t{1}}) {
        const auto save = directory / ("keep-" + std::to_string(limit) + ".sav");
        std::vector<std::string> sessions;
        for (std::size_t session = 0; session < 9; ++session) {
            dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
            // Exercise the real default separately from explicit retention settings.
            if (limit != 5) card.set_save_backup_limit(limit);
            write_eeprom_byte(card, static_cast<std::uint8_t>(session + 1));
            assert(card.flush_save());
            const auto backups = save_backups(save);
            const auto count = std::min(session, limit);
            assert(backups.size() == count);
            for (std::size_t i = 0; i < count; ++i)
                assert(slurp(backups[i]) == sessions[session - count + i]);
            sessions.push_back(slurp(save));
        }
    }
}

void test_save_backup_unrelated_retention(const std::filesystem::path& rom,
                                           const std::filesystem::path& directory) {
    const auto save = directory / "retention.sav";
    const auto backups = directory / "backups";
    std::filesystem::create_directories(backups);
    const std::vector<std::filesystem::path> unrelated{
        directory / "retention.sav.bak",
        backups / "notes.txt",
        backups / "other.sav.20990101T010203.000000000Z-000000.bak",
        backups / "retention.sav.20990230T010203.000000000Z-000000.bak",
        backups / "retention.sav.20990101T246000.000000000Z-000000.bak",
        backups / "retention.sav.20990101T010203.000000000Z-000000.bak.keep",
        backups / "retention.sav.20990101T010203.000000000Z-00000.bak",
        backups / "retention.sav.20990101T010203.000000000Z-00000x.bak",
        backups / "retention.sav.extra.20990101T010203.000000000Z-000000.bak",
    };
    for (const auto& path : unrelated) write_bytes(path, "unrelated bytes");
    const auto matching_directory = backups / "retention.sav.20980101T010203.000000000Z-000000.bak";
    std::filesystem::create_directory(matching_directory);
    write_bytes(matching_directory / "keep.txt", "directory contents");
#if !defined(_WIN32)
    const auto matching_symlink = backups / "retention.sav.20970101T010203.000000000Z-000000.bak";
    std::filesystem::create_symlink(unrelated.front(), matching_symlink);
#endif
    std::size_t original_entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(backups)) {
        (void)entry;
        ++original_entries;
    }
    write_bytes(save, std::string(512, '\xff'));
    std::string previous;
    for (std::uint8_t value = 1; value <= 4; ++value) {
        previous = slurp(save);
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        card.set_save_backup_limit(1);
        write_eeprom_byte(card, value);
        assert(card.flush_save());
    }
    std::size_t entries = 0;
    std::size_t retained = 0;
    for (const auto& entry : std::filesystem::directory_iterator(backups)) {
        ++entries;
        if (std::filesystem::is_regular_file(entry.symlink_status()) &&
            std::find(unrelated.begin(), unrelated.end(), entry.path()) == unrelated.end()) {
            ++retained;
            assert(slurp(entry.path()) == previous);
        }
    }
    assert(entries == original_entries + 1);
    assert(retained == 1);
    for (const auto& path : unrelated) assert(slurp(path) == "unrelated bytes");
    assert(slurp(matching_directory / "keep.txt") == "directory contents");
#if !defined(_WIN32)
    assert(std::filesystem::is_symlink(std::filesystem::symlink_status(matching_symlink)));
    assert(slurp(matching_symlink) == "unrelated bytes");
#endif
}

void test_save_backup_listing(const std::filesystem::path& rom,
                               const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto save = directory / "listing.sav";
    const auto backups = directory / "backups";
    const std::string original(512, '\x35');
    write_bytes(save, original);
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        assert(card.save_capacity() == original.size());
        assert(card.save_backups().empty());
        assert(card.flush_save());
    }
    assert(slurp(save) == original);
    assert(!std::filesystem::exists(backups)); // Inspection/destruction of an unstarted card is read-only.
    std::filesystem::create_directory(backups);
    const auto oldest = backups / "listing.sav.20260101T000000.000000001Z-000000.bak";
    const auto middle = backups / "listing.sav.20260101T000000.000000001Z-000002.bak";
    const auto newest = backups / "listing.sav.20260102T000000.000000000Z-000000.bak";
    write_bytes(newest, std::string(512, '\x42'));
    write_bytes(oldest, std::string(512, '\x16'));
    write_bytes(middle, std::string(8192, '\x27')); // A different supported capacity remains visible.
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(oldest, now + std::chrono::hours(24));
    std::filesystem::last_write_time(middle, now);
    std::filesystem::last_write_time(newest, now - std::chrono::hours(24));
    write_bytes(backups / "listing.sav.20260230T000000.000000000Z-000000.bak", "invalid date");
    write_bytes(backups / "other.sav.20260103T000000.000000000Z-000000.bak", "other save");
    write_bytes(backups / "listing.sav.20260103T000000.000000000Z-000000.bak.pending", "staged");
    write_bytes(backups / "listing.sav.20260103T000000.000000000Z-000000.bak.tmp.1", "temporary");
    std::filesystem::create_directory(backups / "listing.sav.20260104T000000.000000000Z-000000.bak");
#if !defined(_WIN32)
    std::filesystem::create_symlink(oldest, backups / "listing.sav.20260105T000000.000000000Z-000000.bak");
#endif
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    const auto listed = card.save_backups();
    assert(listed.size() == 3);
    assert(listed[0].path == newest && listed[0].size == 512);
    assert(listed[1].path == middle && listed[1].size == 8192);
    assert(listed[2].path == oldest && listed[2].size == 512);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Idle);
    assert_restore_fails(card, listed[1].path);
    assert(card.save_capacity() == 512); // Listing/restore never reinterpret the selected chip.
    assert(slurp(save) == original);
    assert(read_eeprom_bytes(card) == original);
    assert(card.save_backups().size() == listed.size());
    assert(slurp(middle) == std::string(8192, '\x27'));
}

void test_save_restore(const std::filesystem::path& rom, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto save = directory / "restore.sav";
    std::string original(512, '\0');
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<char>(i * 37);
    write_bytes(save, original);
    std::vector<std::string> sessions{original};
    for (const std::uint8_t value : {0x11, 0x22}) {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        write_eeprom_byte(card, value);
        assert(card.flush_save());
        sessions.push_back(slurp(save));
    }
    const auto initial_backups = save_backups(save);
    assert(initial_backups.size() == 2);
    // File metadata is not the timestamp: selection follows the managed filename, not source mtime.
    std::filesystem::last_write_time(initial_backups.front(),
                                    std::filesystem::file_time_type::clock::now() + std::chrono::hours(24));
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    card.set_save_backup_limit(1);
    card.restore_save();
    assert(slurp(save) == sessions[1]);
    assert(std::filesystem::file_size(save) == 512);
    assert(read_eeprom_bytes(card) == sessions[1]);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    auto backups = save_backups(save);
    assert(backups.size() == 1);
    assert(slurp(backups.back()) == sessions[2]);
    assert(!std::filesystem::exists(initial_backups.back())); // The read source was safely pruned.
    assert(card.flush_save());
    assert(save_backups(save) == backups); // A restore is already clean, not a pending overwrite.
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    const auto chosen = backups.back();
    card.restore_save(chosen);
    assert(slurp(save) == sessions[2]);
    assert(read_eeprom_bytes(card) == sessions[2]);
    assert(!std::filesystem::exists(chosen));
    backups = save_backups(save);
    assert(backups.size() == 1);
    assert(slurp(backups.back()) == sessions[1]);
    const auto external = directory / "chosen.raw";
    write_bytes(external, original);
    card.restore_save(external);
    assert(slurp(save) == original);
    assert(read_eeprom_bytes(card) == original);
    assert(slurp(external) == original);
    const auto restored_backups = save_backups(save);
    assert(card.flush_save());
    assert(save_backups(save) == restored_backups);
    {
        dsr::GameCard reloaded(rom, save, dsr::BackupType::Eeprom512B);
        assert(read_eeprom_bytes(reloaded) == original);
    }
    const auto missing_save = directory / "new" / "missing.sav";
    dsr::GameCard fresh(rom, missing_save, dsr::BackupType::Eeprom512B);
    fresh.restore_save(external);
    assert(slurp(missing_save) == original);
    assert(read_eeprom_bytes(fresh) == original);
    assert(save_backups(missing_save).empty());
    assert(fresh.flush_save());
}

void test_save_restore_refusal(const std::filesystem::path& rom, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto save = directory / "refuse.sav";
    const std::string original(512, '\x19');
    write_bytes(save, original);
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    assert_restore_fails(card); // No managed backup yet.
    assert(card.flush_save());
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Failed);
    assert(slurp(save) == original);
    assert(save_backups(save).empty());
    write_eeprom_byte(card, 0x44); // Refused restore must retain dirty in-memory data too.
    auto dirty = original;
    dirty[1] = 0x44;
    assert_restore_fails(card, directory / "missing.raw");
    for (const std::size_t size : {std::size_t{511}, std::size_t{513}, std::size_t{552}, std::size_t{8192}}) {
        const auto source = directory / ("wrong-" + std::to_string(size) + ".dsv");
        write_bytes(source, std::string(size, '\x27'));
        assert_restore_fails(card, source); // No truncation, padding, chip change or DSV conversion.
        assert(card.save_capacity() == original.size());
        assert(slurp(source) == std::string(size, '\x27'));
        assert(slurp(save) == original);
        assert(read_eeprom_bytes(card) == dirty);
        assert(save_backups(save).empty());
    }
    bool rejected_zero = false;
    try {
        card.set_save_backup_limit(0);
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    assert(rejected_zero);
    assert(card.flush_save());
    assert(slurp(save) == dirty);
    assert(save_backups(save).size() == 1);
    assert(slurp(save_backups(save).front()) == original);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
}

void test_save_backup_failure_recovery(const std::filesystem::path& rom,
                                        const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto save = directory / "backup-failure.sav";
    const auto blocker = directory / "backups";
    const std::string original(512, '\x35');
    write_bytes(save, original);
    write_bytes(blocker, "not a directory");
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    write_eeprom_byte(card, 0x62);
    assert(!card.flush_save());
    assert(!card.flush_save());
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Failed);
    assert(slurp(save) == original); // Required backup failure cannot overwrite the save.
    assert(slurp(blocker) == "not a directory");
    auto changed = original;
    changed[1] = 0x62;
    assert(read_eeprom_bytes(card) == changed);
    std::filesystem::remove(blocker);
    assert(card.flush_save());
    assert(slurp(save) == changed);
    const auto backups = save_backups(save);
    assert(backups.size() == 1);
    assert(slurp(backups.front()) == original);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    assert(card.flush_save());
    assert(save_backups(save) == backups);
}

void test_readonly_save_recovery(const std::filesystem::path& rom,
                                  const std::filesystem::path& directory) {
#if defined(_WIN32)
    (void)rom;
    (void)directory;
    std::cout << "skipping POSIX read-only save test on Windows\n";
#else
    if (::geteuid() == 0) {
        std::cout << "skipping read-only save test as root\n";
        return;
    }
    std::filesystem::create_directories(directory);
    const auto save = directory / "readonly.sav";
    const auto backup_directory = directory / "backups";
    std::filesystem::create_directory(backup_directory);
    const auto source = directory / "chosen.raw";
    const std::string original(512, '\x31'), restored(512, '\x79');
    write_bytes(save, original);
    write_bytes(source, restored);
    const auto directory_permissions = std::filesystem::status(directory).permissions();
    const auto backup_permissions = std::filesystem::status(backup_directory).permissions();
    const auto readonly = std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec;
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    struct RestorePermissions {
        std::filesystem::path directory;
        std::filesystem::path backups;
        std::filesystem::perms directory_permissions;
        std::filesystem::perms backup_permissions;
        ~RestorePermissions() {
            std::error_code ec;
            std::filesystem::permissions(directory, directory_permissions,
                                         std::filesystem::perm_options::replace, ec);
            std::filesystem::permissions(backups, backup_permissions,
                                         std::filesystem::perm_options::replace, ec);
        }
    } restore_permissions{directory, backup_directory, directory_permissions, backup_permissions};
    write_eeprom_byte(card, 0x42);
    auto changed = original;
    changed[1] = 0x42;
    std::filesystem::permissions(backup_directory, readonly);
    assert(!card.flush_save());
    assert(!card.flush_save());
    assert_restore_fails(card, source);
    assert(slurp(save) == original);
    assert(read_eeprom_bytes(card) == changed);
    assert(save_backups(save).empty());
    std::filesystem::permissions(backup_directory, backup_permissions);
    assert(card.flush_save());
    assert(slurp(save) == changed);
    const auto session_backups = save_backups(save);
    assert(session_backups.size() == 1);
    assert(slurp(session_backups.front()) == original);

    std::filesystem::permissions(directory, readonly);
    write_eeprom_byte(card, 0x53);
    assert(!card.flush_save());
    assert(!card.flush_save());
    assert(slurp(save) == changed);
    assert(save_backups(save) == session_backups);
    std::filesystem::permissions(directory, directory_permissions);
    changed[1] = 0x53;
    assert(card.flush_save());
    assert(slurp(save) == changed);
    assert(save_backups(save) == session_backups);

    // The preservation backup can succeed while the atomic target commit still fails.
    card.set_save_backup_limit(1);
    std::filesystem::permissions(directory, readonly);
    assert_restore_fails(card);
    assert(save_backups(save) == session_backups); // Neither source pruning nor a false "newest".
    assert(slurp(save) == changed);
    assert(read_eeprom_bytes(card) == changed);
    assert(card.flush_save()); // Clean: this must not turn a failed restore into a "Saved" badge.
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Failed);
    std::filesystem::permissions(directory, directory_permissions);
    card.restore_save();
    assert(slurp(save) == original);
    assert(read_eeprom_bytes(card) == original);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    card.restore_save(source);
    assert(slurp(save) == restored);
    assert(std::filesystem::file_size(save) == 512);
    assert(read_eeprom_bytes(card) == restored);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    const auto after_restore = save_backups(save);
    assert(card.flush_save());
    assert(save_backups(save) == after_restore);
    assert(slurp(source) == restored);

    const auto first_commit_save = directory / "first-commit.sav";
    write_bytes(first_commit_save, original);
    dsr::GameCard first_commit(rom, first_commit_save, dsr::BackupType::Eeprom512B);
    write_eeprom_byte(first_commit, 0x24);
    std::filesystem::permissions(directory, readonly);
    assert(!first_commit.flush_save());
    const auto first_commit_backups = save_backups(first_commit_save);
    assert(first_commit_backups.size() == 1);
    assert(slurp(first_commit_backups.front()) == original);
    assert(!first_commit.flush_save());
    assert(save_backups(first_commit_save) == first_commit_backups);
    assert(slurp(first_commit_save) == original);
    std::filesystem::permissions(directory, directory_permissions);
    assert(first_commit.flush_save());
    auto first_commit_bytes = original;
    first_commit_bytes[1] = 0x24;
    assert(slurp(first_commit_save) == first_commit_bytes);
    assert(read_eeprom_bytes(first_commit) == first_commit_bytes);
    assert(save_backups(first_commit_save) == first_commit_backups);
#endif
}

#if defined(__linux__)
void test_save_backup_publish_collision(const std::filesystem::path& rom,
                                         const std::filesystem::path& directory) {
    save_fault::Scope faults;
    std::filesystem::create_directories(directory);
    const auto save = directory / "collision.sav";
    const std::string original(512, '\x31');
    write_bytes(save, original);
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    write_eeprom_byte(card, 0x42);
    save_fault::collide_next_backup = true;
    assert(card.flush_save());
    assert(!save_fault::collided_target.empty()); // The destination appeared at the publish syscall.
    assert(slurp(save_fault::collided_target) == std::string(512, '\x6c'));
    const auto backups = save_backups(save);
    assert(backups.size() == 2);
    std::size_t preserved = 0;
    for (const auto& path : backups) {
        if (path != save_fault::collided_target) {
            ++preserved;
            assert(slurp(path) == original);
        }
    }
    assert(preserved == 1);
    auto changed = original;
    changed[1] = 0x42;
    assert(slurp(save) == changed);
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
}

void test_save_restore_staged_failures(const std::filesystem::path& rom,
                                        const std::filesystem::path& directory) {
    save_fault::Scope faults;
    std::filesystem::create_directories(directory);
    const auto save = directory / "staged.sav";
    const std::string original(512, '\x19');
    write_bytes(save, original);
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    card.set_save_backup_limit(1);
    write_eeprom_byte(card, 0x2a);
    assert(card.flush_save());
    const auto changed = slurp(save);
    const auto initial_backups = save_backups(save);
    assert(initial_backups.size() == 1);
    save_fault::restore_target = save.string();
    save_fault::deny_restore_commit = true;
    save_fault::deny_pending_cleanup = true;
    assert_restore_fails(card);
    assert(!save_fault::retained_stage.empty());
    const std::filesystem::path failed_stage = save_fault::retained_stage;
    assert(slurp(failed_stage) == changed);
    assert(save_backups(save) == initial_backups);
    assert(slurp(save) == changed);
    assert(read_eeprom_bytes(card) == changed);
    assert(card.flush_save());
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Failed);

    save_fault::deny_restore_commit = false;
    save_fault::deny_pending_cleanup = false;
    card.restore_save(); // Denied cleanup must not have made the staged target the newest backup.
    assert(slurp(save) == original);
    assert(read_eeprom_bytes(card) == original);
    assert(slurp(failed_stage) == changed); // Unmanaged stages are never rotated.
    const auto before_promotion_failure = save_backups(save);
    assert(before_promotion_failure.size() == 1);
    assert(slurp(before_promotion_failure.front()) == changed);

    save_fault::retained_stage.clear();
    save_fault::deny_stage_publication = true;
    card.restore_save();
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);
    assert(slurp(save) == changed);
    assert(read_eeprom_bytes(card) == changed);
    assert(save_backups(save) == before_promotion_failure);
    assert(!save_fault::retained_stage.empty());
    const std::filesystem::path unpublished = save_fault::retained_stage;
    assert(slurp(unpublished) == original);
    assert(card.flush_save());
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);

    save_fault::deny_stage_publication = false;
    card.restore_save(unpublished); // Retained staging bytes remain usable as an explicit raw source.
    assert(slurp(save) == original);
    assert(read_eeprom_bytes(card) == original);
    assert(slurp(unpublished) == original);
    assert(save_backups(save).size() == 1);
}
#endif

void test_chip_mismatch_hint(const std::filesystem::path& rom, const std::filesystem::path& save_dir) {
    {   // An address-only read can suggest a mismatch, but is also a cancelled flash read.
        dsr::GameCard flash(rom, save_dir / "hint-flash.sav", dsr::BackupType::Flash512K);
        (void)aux_transfer(flash, {0x03, 0x00, 0x00, 0x00, 0x00});
        assert(!flash.chip_mismatch_suspected());
        (void)aux_transfer(flash, {0x03, 0x00, 0x00, 0x00});
        assert(flash.chip_mismatch_suspected());
    }
    for (const std::uint8_t command : {0x03, 0x0b}) {
        // A cancelled read must not change the chip or consume a previously latched WREN.
        const auto save = save_dir / ("hint-cancelled-" + std::to_string(command) + ".sav");
        dsr::GameCard automatic(rom, save);
        (void)aux_transfer(automatic, {0x06});
        std::vector<std::uint8_t> cancelled{command, 0x00, 0x00, 0x00};
        if (command == 0x0b) cancelled.push_back(0x00); // fast-read dummy clock, no data
        (void)aux_transfer(automatic, cancelled);
        (void)aux_transfer(automatic, {0x02, 0x00, 0x12, 0x34, 0x5a});
        const auto read = aux_transfer(automatic, {0x03, 0x00, 0x12, 0x34, 0x00});
        assert(read.back() == 0x5a);
        assert(automatic.flush_save());
        assert(std::filesystem::file_size(save) == 512u * 1024u);
    }
    {   // Existing save data is unaffected by the informational hint.
        const auto save = save_dir / "hint-existing.sav";
        std::vector<std::uint8_t> existing(512u * 1024u, 0xff);
        existing[1234] = 0x42;
        {
            std::ofstream out(save, std::ios::binary);
            out.write(reinterpret_cast<const char*>(existing.data()), static_cast<std::streamsize>(existing.size()));
        }
        {
            dsr::GameCard kept(rom, save);   // Auto: the 512 KB size selects flash
            (void)aux_transfer(kept, {0x03, 0x00, 0x00, 0x00});
            assert(kept.chip_mismatch_suspected());
            kept.flush_save();
        }
        assert(std::filesystem::file_size(save) == existing.size());
        std::ifstream in(save, std::ios::binary);
        std::vector<std::uint8_t> after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(after == existing);
        // An existing blank save still establishes the chip's size.
        std::fill(existing.begin(), existing.end(), 0xff);
        {
            std::ofstream out(save, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(existing.data()), static_cast<std::streamsize>(existing.size()));
        }
        dsr::GameCard blank(rom, save);
        (void)aux_transfer(blank, {0x03, 0x00, 0x00, 0x00});
        assert(blank.chip_mismatch_suspected());
    }
    {   // A flushed write remains intact after an address-only read.
        const auto save = save_dir / "hint-late.sav";
        std::filesystem::remove(save);
        dsr::GameCard late(rom, save);
        (void)aux_transfer(late, {0x06});
        (void)aux_transfer(late, {0x02, 0x00, 0x00, 0x00, 0x01});   // flash page program: 3-byte address
        assert(late.flush_save());
        (void)aux_transfer(late, {0x03, 0x00, 0x00, 0x00});
        assert(late.chip_mismatch_suspected());
        assert(late.flush_save());
        std::ifstream in(save, std::ios::binary);
        std::vector<std::uint8_t> kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(kept.size() == 512u * 1024u && kept[0] == 0x01);     // the written byte survived
    }
    {
        dsr::GameCard eeprom(rom, save_dir / "hint-eeprom.sav", dsr::BackupType::Eeprom64K);
        (void)aux_transfer(eeprom, {0x03, 0x00, 0x00, 0x00});
        assert(!eeprom.chip_mismatch_suspected());
    }
}

void test_unwritable_save_does_not_throw(const std::filesystem::path& rom,
                                         const std::filesystem::path& save_dir) {
    // Start with a regular file as the parent: failure is independent of process permissions.
    const auto blocker = save_dir / "blocker";
    { std::ofstream(blocker, std::ios::binary) << "x"; }
    const auto save = blocker / "x.sav";
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    assert(card.flush_save());                       // nothing dirty yet: nothing to write
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Idle);
    (void)aux_transfer(card, {0x06});
    (void)aux_transfer(card, {0x02, 0x01, 0x22});
    assert(!card.flush_save());                      // reports failure instead of throwing
    assert(!card.flush_save());                      // still dirty, still failing, still no throw
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Failed);
    std::filesystem::remove(blocker);
    assert(card.flush_save());                      // retry creates the directory and writes retained data
    const auto saved_byte = [&] {
        std::ifstream in(save, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(bytes.size() == 512);
        return bytes[1];
    };
    assert(saved_byte() == 0x22);
    assert(card.flush_save());                      // a clean flush still succeeds after recovery
    assert(card.save_write_status() == dsr::GameCard::SaveWriteStatus::Saved);

    // A later commit failure must also retain new writes and allow another recovery.
    std::filesystem::remove(save);
    std::filesystem::create_directory(save);         // cannot replace a directory with the save file
    (void)aux_transfer(card, {0x06});
    (void)aux_transfer(card, {0x02, 0x01, 0x33});
    assert(!card.flush_save());
    assert(!card.flush_save());
    std::filesystem::remove(save);
    assert(card.flush_save());
    assert(saved_byte() == 0x33);
}

void test_larger_eeproms(const std::filesystem::path& rom,
                         const std::filesystem::path& save_dir) {
    const auto check = [&](dsr::BackupType type, const std::vector<std::uint8_t>& write,
                           const std::vector<std::uint8_t>& read, std::size_t expected_size) {
        const auto save = save_dir / (std::to_string(expected_size) + ".sav");
        dsr::GameCard card(rom, save, type);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, write);
        const auto result = aux_transfer(card, read);
        assert(result.back() == 0x5a);
        card.flush_save();
        assert(std::filesystem::file_size(save) == expected_size);
    };
    check(dsr::BackupType::Eeprom8K, {0x02, 0x01, 0x23, 0x5a},
          {0x03, 0x01, 0x23, 0}, 8u * 1024u);
    check(dsr::BackupType::Eeprom64K, {0x02, 0x12, 0x34, 0x5a},
          {0x03, 0x12, 0x34, 0}, 64u * 1024u);
    check(dsr::BackupType::Eeprom128K, {0x02, 0x01, 0x23, 0x45, 0x5a},
          {0x03, 0x01, 0x23, 0x45, 0}, 128u * 1024u);
}

void test_backup_flash_round_trip(const std::filesystem::path& rom,
                                  const std::filesystem::path& save) {
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Flash512K);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x00, 0x12, 0x34, 0x5a});
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x00, 0x12, 0x40, 0x0d, 0x0a, 0x1a});
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x0a, 0x00, 0x20, 0x00, 0x3c});
        const auto page_read = aux_transfer(card, {0x03, 0x00, 0x20, 0x00, 0});
        assert(page_read.back() == 0x3c);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0xdb, 0x00, 0x20, 0x00});
        const auto page_erased = aux_transfer(card, {0x03, 0x00, 0x20, 0x00, 0});
        assert(page_erased.back() == 0xff);
        const auto id = aux_transfer(card, {0x9f, 0, 0, 0});
        assert(id[1] == 0x20 && id[2] == 0x40 && id[3] == 0x13);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x01, 0x1c}); // Protect the complete flash.
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x00, 0x40, 0x00, 0x66});
        const auto protected_read = aux_transfer(card, {0x03, 0x00, 0x40, 0x00, 0});
        assert(protected_read.back() == 0xff);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x01, 0});
        card.flush_save();
        assert(std::filesystem::file_size(save) == 512u * 1024u);
    }
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Flash512K);
        const auto read = aux_transfer(card, {0x03, 0x00, 0x12, 0x34, 0});
        assert(read.back() == 0x5a);
        const auto binary_bytes = aux_transfer(card, {0x03, 0x00, 0x12, 0x40, 0, 0, 0});
        assert(binary_bytes[4] == 0x0d && binary_bytes[5] == 0x0a && binary_bytes[6] == 0x1a);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0xd8, 0x00, 0x00, 0x00});
        (void)aux_transfer(card, {0x03, 0x00, 0x12, 0x34, 0});
        const auto erased = aux_transfer(card, {0x03, 0x00, 0x00, 0x00, 0});
        assert(erased.back() == 0xff);
    }
}

void test_dsv_import(const std::filesystem::path& rom,
                     const std::filesystem::path& save) {
    constexpr std::size_t raw_size = 512u * 1024u;
    std::vector<std::uint8_t> bytes(raw_size + 40, 0);
    bytes[0x1234] = 0x6d;
    const std::size_t footer = raw_size;
    bytes[footer + 4] = static_cast<std::uint8_t>(raw_size);
    bytes[footer + 5] = static_cast<std::uint8_t>(raw_size >> 8);
    bytes[footer + 6] = static_cast<std::uint8_t>(raw_size >> 16);
    bytes[footer + 7] = static_cast<std::uint8_t>(raw_size >> 24);
    const std::array<std::uint8_t, 16> cookie = {
        '|', '-', 'D', 'E', 'S', 'M', 'U', 'M', 'E', ' ', 'S', 'A', 'V', 'E', '-', '|'};
    std::copy(cookie.begin(), cookie.end(), bytes.begin() + static_cast<std::ptrdiff_t>(footer + 24));
    std::ofstream out(save, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    dsr::GameCard card(rom, save, dsr::BackupType::Flash512K);
    const auto read = aux_transfer(card, {0x03, 0x00, 0x12, 0x34, 0});
    assert(read.back() == 0x6d);
}

void test_explicit_backup_selection(const std::filesystem::path& rom, const std::filesystem::path& save) {
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom64K);
    (void)aux_transfer(card, {0x06});
    (void)aux_transfer(card, {0x02, 0, 0, 0, 0x55});
    card.flush_save();
    assert(std::filesystem::file_size(save) == 64u * 1024u);
}

void test_firmware_and_touch() {
    dsr::FirmwareSettings settings;
    settings.nickname = u"Hero";
    settings.message = u"Hello DS";
    settings.language = 2;
    settings.birthday_month = 3;
    settings.birthday_day = 14;
    dsr::Spi spi(settings);
    const auto* fw = spi.firmware();
    assert(spi.firmware_size() == 256u * 1024u);
    assert(read_le16(fw + 0x20) == 0x7fc0);
    assert(read_le16(fw + 0x2a) == crc16(fw + 0x2c, 0x138, 0));
    for (std::size_t offset : {std::size_t(0x3fe00), std::size_t(0x3ff00)})
        assert(read_le16(fw + offset + 0x72) == crc16(fw + offset, 0x70));
    for (std::size_t offset : {std::size_t(0x3fa00), std::size_t(0x3fb00),
                               std::size_t(0x3fc00), std::size_t(0x3fd00)}) {
        assert(fw[offset + 0xe7] == 0xff);
        assert(read_le16(fw + offset + 0xfe) == crc16(fw + offset, 0xfe, 0));
    }
    assert(fw[0x3fe00 + 2] == 0 && fw[0x3fe00 + 3] == 3 && fw[0x3fe00 + 4] == 14);
    assert(fw[0x3fe00 + 0x06] == 'H' && fw[0x3fe00 + 0x07] == 0);

    spi.set_touch(true, 127, 95);
    spi.io_write(kSpiCnt, 0x8a00, 16); // enabled, touch controller, hold select
    spi_byte(spi, 0xd0); // start, X channel, 12-bit differential mode
    spi.io_write(kSpiCnt, 0x8a00, 16);
    spi.io_write(kSpiData, 0, 8);
    const auto high = static_cast<std::uint8_t>(spi.io_read(kSpiData, 8));
    spi.io_write(kSpiCnt, 0x8200, 16); // release select with final data unit
    spi.io_write(kSpiData, 0, 8);
    const auto low = static_cast<std::uint8_t>(spi.io_read(kSpiData, 8));
    const std::uint16_t raw_x = static_cast<std::uint16_t>((high << 5) | (low >> 3));
    assert(raw_x == 0x0800);
    const auto read_touch_axis = [&](std::uint8_t command) {
        spi.io_write(kSpiCnt, 0x8a00, 16);
        (void)spi_byte(spi, command);
        spi.io_write(kSpiCnt, 0x8a00, 16);
        const std::uint8_t hi = spi_byte(spi, 0);
        spi.io_write(kSpiCnt, 0x8200, 16);
        const std::uint8_t lo = spi_byte(spi, 0);
        return static_cast<std::uint16_t>((hi << 5) | (lo >> 3));
    };
    spi.set_touch(false, 127, 95);
    assert(read_touch_axis(0xd0) == 0);
    assert(read_touch_axis(0x90) == 0x0fff);

    int irqs = 0;
    spi.on_irq = [&] { ++irqs; };
    spi.io_write(kSpiCnt, 0xc100, 16); // enable, firmware, IRQ
    spi.io_write(kSpiData, 0x03, 8);
    (void)spi.io_read(kSpiData, 8);
    assert(irqs == 1);
}

void test_rtc_datetime() {
    dsr::Rtc rtc;
    constexpr std::uint8_t outputs = 0x70;
    const std::uint8_t selected_output = static_cast<std::uint8_t>(outputs | 0x04u);
    rtc_gpio(rtc, static_cast<std::uint8_t>(outputs | 0x02u)); // CS low, clock high
    rtc_gpio(rtc, selected_output); // select high
    rtc_write_byte(rtc, 0xa6, selected_output); // READ DATE/TIME, LSB first
    constexpr std::uint8_t selected_input = 0x64;
    std::array<std::uint8_t, 7> date{};
    for (auto& byte : date) byte = rtc_read_byte(rtc, selected_input);
    rtc_gpio(rtc, 0x62); // de-select, clock high
    const auto valid_bcd = [](std::uint8_t value) {
        return (value & 0x0fu) <= 9 && ((value >> 4) & 0x0fu) <= 9;
    };
    assert(valid_bcd(date[0]) && valid_bcd(date[1]) && valid_bcd(date[2]));
    assert(date[1] >= 1 && date[1] <= 0x12);
    assert(date[2] >= 1 && date[2] <= 0x31);
    assert(date[3] <= 6);
    const auto hour = static_cast<std::uint8_t>(date[4] & 0x3fu);
    assert(valid_bcd(hour) && valid_bcd(date[5]) && valid_bcd(date[6]));
    assert(hour <= 0x23 && date[5] <= 0x59 && date[6] <= 0x59);
    assert(((date[4] & 0x40u) != 0) == (hour >= 0x12));
}
void test_rtc_deterministic_clock() {
    dsr::Rtc rtc;
    rtc.set_deterministic_clock(1704067190, 120); // 2023-12-31 23:59:50 UTC, UTC+02:00.
    rtc.set_emulated_ticks(15u * 33'513'982u);
    const auto read_date = [&] {
        constexpr std::uint8_t outputs = 0x70;
        const std::uint8_t selected_output = static_cast<std::uint8_t>(outputs | 0x04u);
        rtc_gpio(rtc, static_cast<std::uint8_t>(outputs | 0x02u));
        rtc_gpio(rtc, selected_output);
        rtc_write_byte(rtc, 0xa6, selected_output);
        constexpr std::uint8_t selected_input = 0x64;
        std::array<std::uint8_t, 7> date{};
        for (auto& byte : date) byte = rtc_read_byte(rtc, selected_input);
        rtc_gpio(rtc, 0x62);
        return date;
    };
    const auto date = read_date();
    assert((date == std::array<std::uint8_t, 7>{0x24, 0x01, 0x01, 0x01, 0x02, 0x00, 0x05}));
    rtc.set_deterministic_clock(1705000000, -300); // Simulated replay host with a different clock and zone.
    rtc.set_emulated_ticks(15u * 33'513'982u);
    const auto changed_clock = read_date();
    assert(changed_clock != date);
    rtc.set_deterministic_clock(1704067190, 120);
    rtc.set_emulated_ticks(15u * 33'513'982u);
    assert(read_date() == date); // Replay uses the recorded launch base, not the current host clock.
    rtc.set_emulated_ticks(16u * 33'513'982u);
    assert(read_date()[6] == 0x06);
    rtc.set_deterministic_clock(0, 0);
    rtc.set_emulated_ticks(0);
    assert((read_date() == std::array<std::uint8_t, 7>{0x70, 0x01, 0x01, 0x04, 0, 0, 0}));
    rtc.set_deterministic_clock(0, -60);
    assert((read_date() == std::array<std::uint8_t, 7>{0x69, 0x12, 0x31, 0x03, 0x63, 0, 0}));
}


} // namespace

int main() {
    TempDir temp;
    const auto rom = make_rom(temp.path / "synthetic.nds");
    const auto card_alias_rom = make_rom(temp.path / "card-alias.nds", true);
    test_card_rom_and_chip_id(rom, temp.path / "card.sav");
    test_card_low_address_alias(card_alias_rom, temp.path / "card-alias.sav");
    test_backup_eeprom(rom, temp.path / "eeprom.sav");
    test_chip_mismatch_hint(rom, temp.path);
    test_unwritable_save_does_not_throw(rom, temp.path);
    test_larger_eeproms(rom, temp.path);
    test_previous_save_backup(rom, temp.path / "previous.sav");
    test_save_backup_rotation(rom, temp.path / "rotation");
    test_save_backup_unrelated_retention(rom, temp.path / "unrelated");
    test_save_backup_listing(rom, temp.path / "listing");
    test_save_restore(rom, temp.path / "restore");
    test_save_restore_refusal(rom, temp.path / "restore-refusal");
    test_save_backup_failure_recovery(rom, temp.path / "backup-failure");
    test_readonly_save_recovery(rom, temp.path / "readonly");
#if defined(__linux__)
    test_save_backup_publish_collision(rom, temp.path / "publish-collision");
    test_save_restore_staged_failures(rom, temp.path / "staged-failures");
#endif
    test_backup_flash_round_trip(rom, temp.path / "saves" / "nested" / "flash.sav");
    test_dsv_import(rom, temp.path / "import.dsv");
    test_explicit_backup_selection(rom, temp.path / "explicit-backup.sav");
    test_firmware_and_touch();
    test_rtc_deterministic_clock();
    test_rtc_datetime();
    std::cout << "peripheral tests passed\n";
}
