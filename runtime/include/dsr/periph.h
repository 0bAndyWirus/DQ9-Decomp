// Peripherals: game card (ROM + backup memory), SPI bus devices (power
// management, firmware flash, touchscreen controller) and the RTC.
//
// Owner: runtime/periph. The core bus owns EXMEMCNT and routes:
//   GameCard: 0x040001A0-0x040001AF and 0x04100010 on whichever CPU
//             currently owns the slot-1 bus (EXMEMCNT bit 11).
//   Spi:      ARM7 0x040001C0-0x040001C3.
//   Rtc:      ARM7 0x04000138.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace dsr {

enum class BackupType : std::uint8_t {
    Auto,
    Eeprom512B,
    Eeprom8K,
    Eeprom64K,
    Eeprom128K,
    Flash256K,
    Flash512K,
    Flash1M,
    Flash8M
};

class GameCard {
public:
    // rom_path: the user's local .nds image (read-only, never modified).
    // save_path: backup memory file (created on first save).
    GameCard(const std::filesystem::path& rom_path, const std::filesystem::path& save_path);
    // Explicit backup selection is caller-supplied. Auto infers a supported
    // capacity from an existing save, otherwise falling back to flash.
    GameCard(const std::filesystem::path& rom_path,
             const std::filesystem::path& save_path, BackupType backup_type);

    ~GameCard();
    GameCard(const GameCard&) = delete;
    GameCard& operator=(const GameCard&) = delete;

    void reset();

    std::uint32_t io_read(std::uint32_t addr, int width);
    void io_write(std::uint32_t addr, std::uint32_t value, int width);

    // Advance transfer timing by `ticks` bus cycles.
    void run(std::int64_t ticks);

    // Raised when a ROM transfer completes and ROMCTRL/AUXSPICNT request an
    // IRQ (core raises IF bit 19 on the owning CPU).
    std::function<void()> on_transfer_done_irq;
    // Raised when a data word is ready (DS card DMA trigger, DMA mode 5).
    std::function<void()> on_data_ready;

    // Direct ROM access for the boot loader (header, ARM9/ARM7 binaries,
    // overlays) and the card chip ID.
    void read_rom(std::uint32_t offset, void* dst, std::uint32_t size) const;
    std::uint32_t chip_id() const;
    std::uint64_t rom_size() const;

    // Flush backup memory to save_path if dirty. Called periodically and on exit. Returns false (after a
    // one-time stderr warning) if the file could not be written; the data stays dirty for the next call.
    bool flush_save();

    // True after an address-only backup read: this may indicate EEPROM addressing or a cancelled flash read.
    // Informational only; never changes the chip, write-enable latch or save contents.
    bool chip_mismatch_suspected() const;

    struct Impl;

private:
    Impl* impl_;
};

struct FirmwareSettings {
    std::u16string nickname = u"Player";
    std::u16string message;
    // Firmware language: 0 Japanese, 1 English, 2 French, 3 German,
    // 4 Italian, 5 Spanish.
    std::uint8_t language = 1;
    std::uint8_t birthday_month = 1;
    std::uint8_t birthday_day = 1;
    std::uint8_t favorite_color = 0;
};

class Spi {
public:
    explicit Spi(const FirmwareSettings& settings);
    ~Spi();
    Spi(const Spi&) = delete;
    Spi& operator=(const Spi&) = delete;

    void reset();

    std::uint32_t io_read(std::uint32_t addr, int width);
    void io_write(std::uint32_t addr, std::uint32_t value, int width);

    // Touch input in screen pixels (bottom screen, 0..255 x 0..191).
    void set_touch(bool down, int x, int y);

    // Firmware image bytes (synthesized; includes user settings and valid
    // CRCs). The boot loader copies user settings into main RAM.
    const std::uint8_t* firmware() const;
    std::uint32_t firmware_size() const;

    // Raised when an SPI transfer completes with SPICNT IRQ enabled.
    std::function<void()> on_irq;

    struct Impl;

private:
    Impl* impl_;
};

class Rtc {
public:
    Rtc();
    ~Rtc();
    Rtc(const Rtc&) = delete;
    Rtc& operator=(const Rtc&) = delete;

    void reset();
    // Optional reproducible clock for debugger/replay sessions. Time advances
    // only with emulated DS ticks; timezone_offset_minutes is fixed at launch.
    void set_deterministic_clock(std::int64_t epoch_seconds, std::int32_t timezone_offset_minutes);
    void set_emulated_ticks(std::uint64_t ticks);
    std::uint32_t io_read(std::uint32_t addr, int width);
    void io_write(std::uint32_t addr, std::uint32_t value, int width);

    struct Impl;

private:
    Impl* impl_;
};

} // namespace dsr
