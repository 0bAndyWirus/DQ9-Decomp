#include "dsr/periph.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>


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

// The save as it was before a run survives as <save>.bak: written once, before the first overwrite.
void test_previous_save_backup(const std::filesystem::path& rom, const std::filesystem::path& save) {
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x01, 0x11});
        card.flush_save();
    }
    const std::string first_session = slurp(save);
    assert(first_session.size() == 512);
    std::filesystem::path backup = save;
    backup += ".bak";
    assert(!std::filesystem::exists(backup)); // nothing to back up before the first save
    {
        dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x01, 0x22});
        card.flush_save();
        const std::string second_session = slurp(save);
        assert(second_session != first_session);
        assert(slurp(backup) == first_session); // the previous run's save
        (void)aux_transfer(card, {0x06});
        (void)aux_transfer(card, {0x02, 0x01, 0x33});
        card.flush_save();                      // later flushes of the same run keep the first backup
        assert(slurp(backup) == first_session);
        assert(slurp(save) != second_session);
    }
}

void test_chip_mismatch_hint(const std::filesystem::path& rom, const std::filesystem::path& save_dir) {
    {   // Flash reads carry at least one data byte; a 3-byte-address read that ends at once is EEPROM style.
        dsr::GameCard flash(rom, save_dir / "hint-flash.sav", dsr::BackupType::Flash512K);
        (void)aux_transfer(flash, {0x03, 0x00, 0x00, 0x00, 0x00});
        assert(!flash.chip_mismatch_suspected());
        (void)aux_transfer(flash, {0x03, 0x00, 0x00, 0x00});
        assert(flash.chip_mismatch_suspected());
    }
    {   // Auto: the same access switches to a 64 KB EEPROM before anything was written; no notice is needed.
        const auto save = save_dir / "hint-auto.sav";
        std::filesystem::remove(save);
        dsr::GameCard automatic(rom, save);
        (void)aux_transfer(automatic, {0x03, 0x00, 0x00, 0x00});
        assert(!automatic.chip_mismatch_suspected());
        (void)aux_transfer(automatic, {0x06});
        (void)aux_transfer(automatic, {0x02, 0x01, 0x22, 0x5a});               // a 2-byte-address EEPROM write ...
        const auto read = aux_transfer(automatic, {0x03, 0x01, 0x22, 0x00});   // ... reads back
        assert(read.back() == 0x5a);
        automatic.flush_save();
        assert(std::filesystem::file_size(save) == 64u * 1024u);
    }
    {   // Existing save data is never discarded by the inference: only a notice is possible.
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
        // A blank (all 0xff) save of the same size holds nothing to lose: the inference may switch.
        std::fill(existing.begin(), existing.end(), 0xff);
        {
            std::ofstream out(save, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(existing.data()), static_cast<std::streamsize>(existing.size()));
        }
        dsr::GameCard blank(rom, save);
        (void)aux_transfer(blank, {0x03, 0x00, 0x00, 0x00});
        assert(!blank.chip_mismatch_suspected());
    }
    {   // After a write the chip is no longer swapped: a mismatch is only reported.
        dsr::GameCard late(rom, save_dir / "hint-late.sav");
        (void)aux_transfer(late, {0x06});
        (void)aux_transfer(late, {0x02, 0x00, 0x00, 0x00, 0x01});   // flash page program: 3-byte address
        (void)aux_transfer(late, {0x03, 0x00, 0x00, 0x00});
        assert(late.chip_mismatch_suspected());
    }
    {
        dsr::GameCard eeprom(rom, save_dir / "hint-eeprom.sav", dsr::BackupType::Eeprom64K);
        (void)aux_transfer(eeprom, {0x03, 0x00, 0x00, 0x00});
        assert(!eeprom.chip_mismatch_suspected());
    }
}

void test_unwritable_save_does_not_throw(const std::filesystem::path& rom,
                                         const std::filesystem::path& save_dir) {
    // The save's parent "folder" is a regular file, so the file can never be created.
    const auto blocker = save_dir / "blocker";
    { std::ofstream(blocker, std::ios::binary) << "x"; }
    const auto save = blocker / "x.sav";
    dsr::GameCard card(rom, save, dsr::BackupType::Eeprom512B);
    assert(card.flush_save());                       // nothing dirty yet: nothing to write
    (void)aux_transfer(card, {0x06});
    (void)aux_transfer(card, {0x02, 0x01, 0x22});
    assert(!card.flush_save());                      // reports failure instead of throwing
    assert(!card.flush_save());                      // still dirty, still failing, still no throw
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
    test_backup_flash_round_trip(rom, temp.path / "saves" / "nested" / "flash.sav");
    test_dsv_import(rom, temp.path / "import.dsv");
    test_explicit_backup_selection(rom, temp.path / "explicit-backup.sav");
    test_firmware_and_touch();
    test_rtc_deterministic_clock();
    test_rtc_datetime();
    std::cout << "peripheral tests passed\n";
}
