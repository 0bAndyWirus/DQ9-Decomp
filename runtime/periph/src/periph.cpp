#include "dsr/periph.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace dsr {
namespace {
constexpr std::uint32_t kAuxSpiCnt = 0x040001A0;
constexpr std::uint32_t kAuxSpiData = 0x040001A2;
constexpr std::uint32_t kRomCtrl = 0x040001A4;
constexpr std::uint32_t kCardCommand = 0x040001A8;
constexpr std::uint32_t kCardData = 0x04100010;
constexpr std::uint32_t kSpiCnt = 0x040001C0;
constexpr std::uint32_t kSpiData = 0x040001C2;
constexpr std::uint32_t kRtcReg = 0x04000138;
constexpr std::uint32_t kCardChipId = 0x00001fc2u;
constexpr std::uint32_t kRomDataReady = 1u << 23;
constexpr std::uint32_t kRomBusy = 1u << 31;
constexpr std::uint16_t kAuxBusy = 1u << 7;
constexpr std::uint16_t kAuxHold = 1u << 6;
constexpr std::uint16_t kAuxSpiMode = 1u << 13;
constexpr std::uint16_t kAuxIrqEnable = 1u << 14;
constexpr std::uint16_t kAuxSlotEnable = 1u << 15;
constexpr std::uint16_t kSpiBusy = 1u << 7;
constexpr std::uint16_t kSpiDeviceMask = 3u << 8;
constexpr std::uint16_t kSpiHold = 1u << 11;
constexpr std::uint16_t kSpiIrqEnable = 1u << 14;
constexpr std::uint16_t kSpiEnable = 1u << 15;
constexpr std::size_t kDsvFooterSize = 40;
constexpr std::array<std::uint8_t, 16> kDsvCookie = {
    '|', '-', 'D', 'E', 'S', 'M', 'U', 'M', 'E', ' ', 'S', 'A', 'V', 'E', '-', '|'};

// IO widths at the public API are in bits (8/16/32), as in the core bus.
int width_bytes(int width_bits) { return std::clamp(width_bits / 8, 1, 4); }

std::uint32_t width_mask(int width) {
    if (width <= 1) return 0xffu;
    if (width == 2) return 0xffffu;
    return 0xffffffffu;
}


std::uint32_t get_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void put_le16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

void put_le32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint16_t crc16_ds(const std::uint8_t* data, std::size_t size,
                       std::uint16_t initial = 0xffff) {
    std::uint16_t crc = initial;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0 ? static_cast<std::uint16_t>((crc >> 1) ^ 0xa001u)
                                  : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

std::uint32_t backup_bytes(BackupType type) {
    switch (type) {
    case BackupType::Eeprom512B: return 512;
    case BackupType::Eeprom8K: return 8u * 1024u;
    case BackupType::Eeprom64K: return 64u * 1024u;
    case BackupType::Eeprom128K: return 128u * 1024u;
    case BackupType::Flash256K: return 256u * 1024u;
    case BackupType::Flash512K: return 512u * 1024u;
    case BackupType::Flash1M: return 1024u * 1024u;
    case BackupType::Flash8M: return 8u * 1024u * 1024u;
    case BackupType::Auto: break;
    }
    return 512u * 1024u;
}

bool backup_is_flash(BackupType type) {
    return type == BackupType::Flash256K || type == BackupType::Flash512K ||
           type == BackupType::Flash1M || type == BackupType::Flash8M;
}

std::size_t backup_page_size(BackupType type) {
    switch (type) {
    case BackupType::Eeprom512B: return 16;
    case BackupType::Eeprom8K: return 32;
    case BackupType::Eeprom64K: return 128;
    case BackupType::Eeprom128K: return 256;
    case BackupType::Flash256K:
    case BackupType::Flash512K:
    case BackupType::Flash1M:
    case BackupType::Flash8M: return 256;
    case BackupType::Auto: break;
    }
    return 256;
}

std::uint8_t bcd(unsigned value) {
    return static_cast<std::uint8_t>(((value / 10u) << 4) | (value % 10u));
}

unsigned clamp_unsigned(int value, unsigned maximum) {
    if (value <= 0) return 0;
    const auto converted = static_cast<unsigned>(value);
    return std::min(converted, maximum);
}

} // namespace

struct GameCard::Impl {
    explicit Impl(const std::filesystem::path& rom_path,
                  const std::filesystem::path& save_path, BackupType requested_type)
        : save_path_(save_path) {
        try {
            open_rom(rom_path);
            backup_type_ = resolve_backup_type(requested_type);
            backup_.assign(backup_bytes(backup_type_), 0xff);
            backup_write_bytes_.reserve(256);
            load_save();
            reset();
        } catch (...) {
            close_rom();
            throw;
        }
    }

    ~Impl() { close_rom(); }

    void open_rom(const std::filesystem::path& rom_path) {
#if defined(_WIN32)
        rom_file_ = CreateFileW(rom_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (rom_file_ == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "open ROM");
        LARGE_INTEGER st{};
        if (!GetFileSizeEx(rom_file_, &st))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "stat ROM");
        if (st.QuadPart <= 0 || static_cast<std::uintmax_t>(st.QuadPart) >
                                    static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
            throw std::runtime_error("ROM is empty or too large to map");
        rom_size_ = static_cast<std::size_t>(st.QuadPart);
        rom_mapping_ = CreateFileMappingW(rom_file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (rom_mapping_ == nullptr)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "map ROM");
        rom_ = static_cast<const std::uint8_t*>(
            MapViewOfFile(rom_mapping_, FILE_MAP_READ, 0, 0, 0));
        if (rom_ == nullptr)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "map ROM");
#else
        rom_fd_ = ::open(rom_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (rom_fd_ < 0) throw std::system_error(errno, std::generic_category(), "open ROM");
        struct stat st {};
        if (::fstat(rom_fd_, &st) != 0)
            throw std::system_error(errno, std::generic_category(), "stat ROM");
        if (st.st_size <= 0 || static_cast<std::uintmax_t>(st.st_size) >
                                   static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
            throw std::runtime_error("ROM is empty or too large to map");
        rom_size_ = static_cast<std::size_t>(st.st_size);
        void* mapped = ::mmap(nullptr, rom_size_, PROT_READ, MAP_PRIVATE, rom_fd_, 0);
        if (mapped == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap ROM");
        rom_ = static_cast<const std::uint8_t*>(mapped);
#endif
    }

    void close_rom() noexcept {
        if (rom_ != nullptr) {
#if defined(_WIN32)
            UnmapViewOfFile(rom_);
#else
            ::munmap(const_cast<std::uint8_t*>(rom_), rom_size_);
#endif
            rom_ = nullptr;
        }
#if defined(_WIN32)
        if (rom_mapping_ != nullptr) {
            CloseHandle(rom_mapping_);
            rom_mapping_ = nullptr;
        }
        if (rom_file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(rom_file_);
            rom_file_ = INVALID_HANDLE_VALUE;
        }
#else
        if (rom_fd_ >= 0) {
            ::close(rom_fd_);
            rom_fd_ = -1;
        }
#endif
        rom_size_ = 0;
    }

    BackupType resolve_backup_type(BackupType requested) const {
        if (requested != BackupType::Auto) return requested;
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(save_path_, ec);
        if (!ec) {
            std::uintmax_t raw_size = file_size;
            if (save_path_.extension() == ".dsv" && raw_size >= kDsvFooterSize) {
                std::ifstream input(save_path_, std::ios::binary);
                if (input) {
                    input.seekg(static_cast<std::streamoff>(raw_size - 16));
                    std::array<std::uint8_t, 16> cookie{};
                    input.read(reinterpret_cast<char*>(cookie.data()),
                               static_cast<std::streamsize>(cookie.size()));
                    if (input && cookie == kDsvCookie) {
                        input.seekg(static_cast<std::streamoff>(raw_size - kDsvFooterSize + 4));
                        std::array<std::uint8_t, 4> field{};
                        input.read(reinterpret_cast<char*>(field.data()),
                                   static_cast<std::streamsize>(field.size()));
                        if (input) {
                            const std::uint32_t size = get_le32(field.data());
                            if (size == 512 || size == 8u * 1024u || size == 64u * 1024u ||
                                size == 128u * 1024u || size == 256u * 1024u ||
                                size == 512u * 1024u || size == 1024u * 1024u ||
                                size == 8u * 1024u * 1024u)
                                raw_size = size;
                        }
                    }
                }
            }
            switch (raw_size) {
            case 512: return BackupType::Eeprom512B;
            case 8u * 1024u: return BackupType::Eeprom8K;
            case 64u * 1024u: return BackupType::Eeprom64K;
            case 128u * 1024u: return BackupType::Eeprom128K;
            case 256u * 1024u: return BackupType::Flash256K;
            case 512u * 1024u: return BackupType::Flash512K;
            case 1024u * 1024u: return BackupType::Flash1M;
            case 8u * 1024u * 1024u: return BackupType::Flash8M;
            }
        }
        return BackupType::Flash512K;
    }


    void reset() {
        finish_backup_transaction();
        aux_control_ = 0;
        aux_data_ = 0xff;
        rom_control_ = 0;
        card_command_.fill(0);
        transfer_active_ = false;
        transfer_data_ready_ = false;
        transfer_ticks_ = 0;
        transfer_pos_ = 0;
        transfer_size_ = 0;
        transfer_rom_address_ = 0;
        transfer_kind_ = TransferKind::None;
        transfer_special_.fill(0);
        backup_wel_ = false;
    }

    enum class TransferKind : std::uint8_t { None, Rom, Special, Fill };

    void raw_rom_read(std::uint32_t offset, std::uint8_t* dst, std::uint32_t size) const {
        if (size == 0) return;
        std::memset(dst, 0xff, size);
        if (static_cast<std::uint64_t>(offset) >= rom_size_) return;
        const std::size_t available = rom_size_ - static_cast<std::size_t>(offset);
        const std::size_t count = std::min<std::size_t>(available, size);
        std::memcpy(dst, rom_ + offset, count);
    }

    std::uint8_t rom_data_byte(std::size_t offset) const {
        if (offset >= rom_size_) return 0xff;
        return rom_[offset];
    }

    std::uint8_t card_data_byte() const {
        if (transfer_kind_ == TransferKind::Special)
            return transfer_special_[transfer_pos_ % transfer_special_.size()];
        if (transfer_kind_ == TransferKind::Rom)
            return rom_data_byte(static_cast<std::size_t>(transfer_rom_address_ +
                                                           static_cast<std::uint32_t>(transfer_pos_)));
        if (transfer_kind_ == TransferKind::Fill) return transfer_special_[0];
        return 0xff;
    }

    std::size_t card_block_size(std::uint32_t ctrl) const {
        const unsigned block = (ctrl >> 24) & 7u;
        if (block == 0) return 0;
        if (block == 7) return 4;
        return static_cast<std::size_t>(0x100u << block);
    }

    void start_card_transfer(std::uint32_t value) {
        rom_control_ = value & ~(kRomDataReady);
        transfer_active_ = true;
        transfer_data_ready_ = false;
        transfer_pos_ = 0;
        transfer_kind_ = TransferKind::None;
        transfer_special_.fill(0);
        transfer_rom_address_ = (static_cast<std::uint32_t>(card_command_[1]) << 24) |
                                (static_cast<std::uint32_t>(card_command_[2]) << 16) |
                                (static_cast<std::uint32_t>(card_command_[3]) << 8) |
                                static_cast<std::uint32_t>(card_command_[4]);
        const std::uint8_t command = card_command_[0];
        std::size_t length = card_block_size(value);
        switch (command) {
        case 0xb7: {
            transfer_kind_ = TransferKind::Rom;
            // Low B7 reads repeat the first protected 0x200-byte block.
            if (transfer_rom_address_ < 0x8000u)
                transfer_rom_address_ = 0x8000u + (transfer_rom_address_ & 0x1ffu);
            break;
        }
        case 0xb8:
        case 0x90:
            transfer_kind_ = TransferKind::Special;
            transfer_special_[0] = static_cast<std::uint8_t>(kCardChipId);
            transfer_special_[1] = static_cast<std::uint8_t>(kCardChipId >> 8);
            transfer_special_[2] = static_cast<std::uint8_t>(kCardChipId >> 16);
            transfer_special_[3] = static_cast<std::uint8_t>(kCardChipId >> 24);
            length = 4;
            break;
        case 0x00:
            transfer_kind_ = TransferKind::Rom;
            transfer_rom_address_ = card_command_[1] == 0 ? 0u :
                ((static_cast<std::uint32_t>(card_command_[1]) << 24) |
                 (static_cast<std::uint32_t>(card_command_[2]) << 16) |
                 (static_cast<std::uint32_t>(card_command_[3]) << 8) |
                 static_cast<std::uint32_t>(card_command_[4]));
            break;
        case 0x9f:
            transfer_kind_ = TransferKind::Fill;
            transfer_special_[0] = 0xff;
            break;
        case 0x3c:
            transfer_kind_ = TransferKind::Fill;
            transfer_special_[0] = 0;
            length = 0;
            break;
        case 0x1f:
        case 0x2f:
        case 0x6f:
        case 0xaf:
            // KEY1 commands need BIOS-derived cipher tables which are not stored
            // in a commercial card image. A deterministic idle word stream lets
            // post-boot probes complete without pretending to decrypt it.
            transfer_kind_ = TransferKind::Fill;
            transfer_special_[0] = 0;
            break;
        default:
            transfer_kind_ = TransferKind::Fill;
            transfer_special_[0] = 0xff;
            break;
        }
        transfer_size_ = length;
        if (length == 0) {
            complete_card_transfer();
            return;
        }
        const std::uint32_t gap = value & 0x1fffu;
        const std::uint32_t clocks = (value & (1u << 27)) != 0 ? 8u : 5u;
        // Oracle's NDS_RescheduleReadSlot1 uses twice this machine's tick rate.
        transfer_ticks_ = static_cast<std::int64_t>((8u + gap) * clocks + 4u);
    }

    void complete_card_transfer() {
        if (!transfer_active_) return;
        transfer_active_ = false;
        transfer_data_ready_ = false;
        rom_control_ &= ~(kRomBusy | kRomDataReady);
        if ((aux_control_ & kAuxIrqEnable) != 0 && on_transfer_done_irq)
            on_transfer_done_irq();
    }

    void advance_card(std::int64_t ticks) {
        if (!transfer_active_ || ticks <= 0) return;
        std::int64_t remaining = ticks;
        while (transfer_active_ && !transfer_data_ready_ && remaining > 0) {
            if (transfer_size_ == 0 || transfer_pos_ >= transfer_size_) {
                if (remaining < transfer_ticks_) {
                    transfer_ticks_ -= remaining;
                    return;
                }
                remaining -= transfer_ticks_;
                complete_card_transfer();
                continue;
            }
            if (remaining < transfer_ticks_) {
                transfer_ticks_ -= remaining;
                return;
            }
            remaining -= transfer_ticks_;
            transfer_ticks_ = 0;
            transfer_data_ready_ = true;
            rom_control_ |= kRomDataReady;
            if (on_data_ready) on_data_ready();
            if (transfer_data_ready_) return;
        }
    }

    std::uint32_t read_card_data(int width) {
        std::uint32_t value = 0;
        const int count = std::clamp(width, 1, 4);
        for (int i = 0; i < count; ++i) {
            if (!transfer_active_ || !transfer_data_ready_ || transfer_pos_ >= transfer_size_)
                break;
            value |= static_cast<std::uint32_t>(card_data_byte()) << (8 * i);
            ++transfer_pos_;
            if (transfer_pos_ >= transfer_size_) {
                complete_card_transfer();
            }
        }
        return value;
    }

    void load_save() {
        std::error_code ec;
        if (!std::filesystem::exists(save_path_, ec) || ec) return;
        const auto size = std::filesystem::file_size(save_path_, ec);
        if (ec || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) return;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        std::ifstream input(save_path_, std::ios::binary);
        if (!input || (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                                      static_cast<std::streamsize>(bytes.size()))))
            return;
        if (bytes.size() == backup_.size()) {
            std::copy(bytes.begin(), bytes.end(), backup_.begin());
            return;
        }
        if (bytes.empty()) return;
        if (save_path_.extension() != ".dsv" || bytes.size() < kDsvFooterSize ||
            !std::equal(kDsvCookie.begin(), kDsvCookie.end(), bytes.end() - 16)) {
            warn_save_ignored(bytes.size());
            return;
        }
        const std::size_t footer = bytes.size() - kDsvFooterSize;
        const std::uint32_t padded_size = get_le32(bytes.data() + footer + 4);
        if (padded_size == backup_.size() && padded_size <= footer)
            std::copy_n(bytes.begin(), backup_.size(), backup_.begin());
        else
            warn_save_ignored(bytes.size());
    }

    void warn_save_ignored(std::size_t found) const {
        std::fprintf(stderr,
                     "[dsr] save file has %zu bytes but the selected cartridge backup type needs %zu: it is not loaded and "
                     "will be replaced on the next save (the previous file is preserved in backups/ beside it). "
                     "Pass --backup-type to match it, or use the chip that created it.\n",
                     found, backup_.size());
    }

    // Managed names are <save filename>.YYYYMMDDTHHMMSS.nnnnnnnnnZ-NNNNNN.bak.
    // Fixed-width UTC timestamps and a collision sequence sort in creation order, even if the
    // host clock moves backwards. Only exact names and regular, non-symlink files are managed.
    static constexpr std::size_t kSaveBackupSuffixSize = 37;

    static bool managed_backup_name(const std::filesystem::path::string_type& name,
                                    const std::filesystem::path::string_type& prefix) {
        if (name.size() != prefix.size() + kSaveBackupSuffixSize ||
            name.compare(0, prefix.size(), prefix) != 0)
            return false;
        const auto start = prefix.size();
        for (std::size_t i = 0; i < kSaveBackupSuffixSize; ++i) {
            const auto c = name[start + i];
            const char separator = i == 8 ? 'T' : i == 15 ? '.' : i == 25 ? 'Z' :
                                   i == 26 ? '-' : i == 33 ? '.' :
                                   i == 34 ? 'b' : i == 35 ? 'a' : i == 36 ? 'k' : '\0';
            if (separator != '\0' ? c != separator : c < '0' || c > '9') return false;
        }
        const auto number = [&](std::size_t offset, std::size_t count) {
            unsigned value = 0;
            for (std::size_t i = 0; i < count; ++i)
                value = value * 10 + static_cast<unsigned>(name[start + offset + i] - '0');
            return value;
        };
        const unsigned year = number(0, 4), month = number(4, 2), day = number(6, 2);
        if (year < 1970 || month < 1 || month > 12 || day < 1 ||
            number(9, 2) > 23 || number(11, 2) > 59 || number(13, 2) > 59)
            return false;
        static constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        return day <= days[month - 1] + (month == 2 && leap ? 1u : 0u);
    }

    std::vector<std::filesystem::path> managed_save_backups() const {
        std::vector<std::filesystem::path> backups;
        const auto directory = save_path_.parent_path() / "backups";
        std::error_code ec;
        const bool exists = std::filesystem::exists(directory, ec);
        if (ec) throw std::filesystem::filesystem_error("inspect save backups", directory, ec);
        if (!exists) return backups;
        auto prefix = save_path_.filename();
        prefix += ".";
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (managed_backup_name(entry.path().filename().native(), prefix.native()) &&
                std::filesystem::is_regular_file(entry.symlink_status()))
                backups.push_back(entry.path());
        }
        std::sort(backups.begin(), backups.end());
        return backups;
    }

    static std::vector<std::uint8_t> read_save_bytes(const std::filesystem::path& source) {
        const auto size = std::filesystem::file_size(source);
        if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
            size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("save file is too large to read");
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("could not open save file for reading");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                         static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("could not read complete save file");
        char extra = 0;
        if (input.get(extra) || !input.eof())
            throw std::runtime_error("save file changed while reading");
        return bytes;
    }

    static std::string save_backup_timestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count();
        const auto time = std::chrono::system_clock::to_time_t(seconds);
        std::tm utc{};
#if defined(_WIN32)
        if (gmtime_s(&utc, &time) != 0)
#else
        if (gmtime_r(&time, &utc) == nullptr)
#endif
            throw std::runtime_error("could not timestamp save backup");
        if (utc.tm_year < 70 || utc.tm_year > 8099)
            throw std::runtime_error("save backup timestamp is out of range");
        char date[16]{};
        if (std::strftime(date, sizeof(date), "%Y%m%dT%H%M%S", &utc) != 15)
            throw std::runtime_error("save backup timestamp is out of range");
        char fraction[12]{};
        std::snprintf(fraction, sizeof(fraction), ".%09uZ", static_cast<unsigned>(nanos));
        return std::string(date) + fraction;
    }

    std::filesystem::path next_save_backup_path(bool staged = false) const {
        const auto directory = save_path_.parent_path() / "backups";
        const auto backups = managed_save_backups();
        std::string timestamp = save_backup_timestamp();
        unsigned sequence = 0;
        if (!backups.empty()) {
            const auto latest = backups.back().filename().native();
            const auto offset = save_path_.filename().native().size() + 1;
            const std::string previous_timestamp(latest.begin() + offset, latest.begin() + offset + 26);
            if (timestamp <= previous_timestamp) {
                timestamp = previous_timestamp;
                for (std::size_t i = 27; i < 33; ++i)
                    sequence = sequence * 10 + static_cast<unsigned>(latest[offset + i] - '0');
                ++sequence;
            }
        }
        // Also avoid occupied names belonging to non-regular entries or interrupted stages.
        for (; sequence < 1'000'000; ++sequence) {
            char suffix[12]{};
            std::snprintf(suffix, sizeof(suffix), "-%06u.bak", sequence);
            auto filename = save_path_.filename();
            filename += ".";
            filename += timestamp;
            filename += suffix;
            const auto managed = directory / filename;
            auto target = managed;
            if (staged) target += ".pending";
            if (std::filesystem::exists(std::filesystem::symlink_status(target)) ||
                (staged && std::filesystem::exists(std::filesystem::symlink_status(managed))))
                continue;
            return target;
        }
        throw std::runtime_error("save backup timestamp sequence is exhausted");
    }

    std::filesystem::path backup_current_save(bool staged = false) {
        std::error_code ec;
        const auto status = std::filesystem::status(save_path_, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            throw std::filesystem::filesystem_error("inspect previous save", save_path_, ec);
        if (!std::filesystem::exists(status)) return {};
        if (!std::filesystem::is_regular_file(status))
            throw std::runtime_error("previous save is not a regular file");
        const auto bytes = read_save_bytes(save_path_);
        std::filesystem::create_directories(save_path_.parent_path() / "backups");
        for (;;) {
            const auto target = next_save_backup_path(staged);
            try {
                atomic_write_save(target, bytes, false);
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::file_exists) continue;
                throw;
            }
            if (!staged) save_backup_rotation_pending_ = true;
            return target;
        }
    }

    // Publish without replacing even a destination created after candidate selection.
    // On failure the source stays intact; after success, a leftover source is harmless cleanup.
    static void publish_new_save_file(const std::filesystem::path& source,
                                      const std::filesystem::path& target) {
#if defined(_WIN32)
        if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "publish save backup");
#else
        if (::link(source.c_str(), target.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "publish save backup");
        if (::unlink(source.c_str()) != 0)
            std::fprintf(stderr, "[dsr] warning: backup published, but its staging file could not be removed: %s\n",
                         source.c_str());
#endif
    }

    void publish_staged_save_backup(const std::filesystem::path& staged) {
        for (;;) {
            const auto target = next_save_backup_path();
            try {
                publish_new_save_file(staged, target);
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::file_exists) continue;
                throw;
            }
            save_backup_rotation_pending_ = true;
            return;
        }
    }

    void rotate_save_backups() {
        if (!save_backup_rotation_pending_) return;
        // Retention is cleanup, not the safety snapshot. Run only after commit: pruning must not
        // lose a restore source on write failure, nor turn committed bytes into a failed save.
        try {
            const auto backups = managed_save_backups();
            const auto remove_count = backups.size() > save_backup_limit_ ? backups.size() - save_backup_limit_ : 0;
            for (std::size_t i = 0; i < remove_count; ++i) {
                if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(backups[i])))
                    throw std::runtime_error("save backup changed during rotation");
                std::filesystem::remove(backups[i]);
            }
            save_backup_rotation_pending_ = false;
            save_backup_rotation_reported_ = false;
        } catch (const std::exception& error) {
            if (!save_backup_rotation_reported_)
                std::fprintf(stderr, "[dsr] warning: save written, but old backups could not be rotated: %s "
                                     "(will retry after a later write)\n", error.what());
            save_backup_rotation_reported_ = true;
        }
    }

    static std::filesystem::path save_temp_path(const std::filesystem::path& target) {
        std::array<std::uint8_t, 16> random{};
#if defined(_WIN32)
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("could not generate random save temp name");
#else
        const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "open save temp entropy");
        std::size_t received = 0;
        while (received < random.size()) {
            const ssize_t amount = ::read(fd, random.data() + received, random.size() - received);
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) {
                const int error = amount < 0 ? errno : EIO;
                ::close(fd);
                throw std::system_error(error, std::generic_category(), "read save temp entropy");
            }
            received += static_cast<std::size_t>(amount);
        }
        if (::close(fd) != 0)
            throw std::system_error(errno, std::generic_category(), "close save temp entropy");
#endif
        constexpr char hex[] = "0123456789abcdef";
        std::array<char, 38> suffix{'.', 't', 'm', 'p', '.'};
        for (std::size_t i = 0; i < random.size(); ++i) {
            suffix[5 + i * 2] = hex[random[i] >> 4];
            suffix[6 + i * 2] = hex[random[i] & 0x0f];
        }
        auto temp = target;
        temp += suffix.data();
        return temp;
    }

    static void atomic_write_save(const std::filesystem::path& target,
                                  const std::vector<std::uint8_t>& bytes,
                                  bool replace_existing = true) {
        const auto temp_path = save_temp_path(target);
#if defined(_WIN32)
        HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "open save temp");
        std::size_t written = 0;
        while (written < bytes.size()) {
            const DWORD requested = static_cast<DWORD>(std::min(
                bytes.size() - written, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD amount = 0;
            const BOOL write_ok = WriteFile(file, bytes.data() + written, requested, &amount, nullptr);
            if (!write_ok || amount == 0) {
                const DWORD error = write_ok ? ERROR_WRITE_FAULT : GetLastError();
                CloseHandle(file);
                DeleteFileW(temp_path.c_str());
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "write save temp");
            }
            written += amount;
        }
        if (!FlushFileBuffers(file)) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            DeleteFileW(temp_path.c_str());
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "sync save temp");
        }
        if (!CloseHandle(file)) {
            const DWORD error = GetLastError();
            DeleteFileW(temp_path.c_str());
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "close save temp");
        }
#else
        const int fd = ::open(temp_path.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "open save temp");
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t amount = ::write(fd, bytes.data() + written, bytes.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) {
                const int error = amount < 0 ? errno : EIO;
                ::close(fd);
                ::unlink(temp_path.c_str());
                throw std::system_error(error, std::generic_category(), "write save temp");
            }
            written += static_cast<std::size_t>(amount);
        }
        if (::fsync(fd) != 0) {
            const int error = errno;
            ::close(fd);
            ::unlink(temp_path.c_str());
            throw std::system_error(error, std::generic_category(), "sync save temp");
        }
        if (::close(fd) != 0) {
            const int error = errno;
            ::unlink(temp_path.c_str());
            throw std::system_error(error, std::generic_category(), "close save temp");
        }
#endif
        try {
            if (!replace_existing) {
                publish_new_save_file(temp_path, target);
            } else {
#if defined(_WIN32)
                if (!MoveFileExW(temp_path.c_str(), target.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                            "rename save temp");
#else
                if (::rename(temp_path.c_str(), target.c_str()) != 0)
                    throw std::system_error(errno, std::generic_category(), "rename save temp");
#endif
            }
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            throw;
        }
    }

    void flush_save() {
        if (!save_dirty_ || save_path_.empty()) return;
        const auto parent = save_path_.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        if (!previous_save_backed_up_ && !backup_current_save().empty()) previous_save_backed_up_ = true;
        atomic_write_save(save_path_, backup_);
        // A missing old save needs no backup. Only a successful commit completes that first write.
        previous_save_backed_up_ = true;
        save_dirty_ = false;
        save_write_status_ = SaveWriteStatus::Saved;
        rotate_save_backups();
    }

    void restore_save(const std::filesystem::path& source) {
        if (save_path_.empty()) throw std::invalid_argument("no save path was selected");
        auto selected = source;
        if (selected.empty()) {
            const auto backups = managed_save_backups();
            if (backups.empty()) throw std::runtime_error("no managed save backup is available");
            selected = backups.back();
        }
        if (std::filesystem::file_size(selected) != backup_.size())
            throw std::runtime_error("restore source raw size does not match the selected backup chip");
        auto restored = read_save_bytes(selected);
        if (restored.size() != backup_.size())
            throw std::runtime_error("restore source changed size while reading");
        const auto parent = save_path_.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        // Restores always preserve the current on-disk target, even after this session has saved.
        // A restore's preservation snapshot must not become "newest" before the target commits.
        // Even denied cleanup after a failed commit can only leave an unmanaged .pending file.
        const auto preserved = backup_current_save(true);
        try {
            atomic_write_save(save_path_, restored);
        } catch (...) {
            if (!preserved.empty()) {
#if defined(_WIN32)
                const bool removed = DeleteFileW(preserved.c_str()) != 0;
#else
                const bool removed = ::unlink(preserved.c_str()) == 0;
#endif
                if (!removed)
                    std::fprintf(stderr, "[dsr] warning: restore did not commit; its preservation snapshot "
                                         "remains as an unmanaged .pending file in backups/\n");
            }
            throw;
        }
        backup_.swap(restored);
        // Discard any unfinished SPI write rather than letting it dirty the newly restored bytes.
        backup_selected_ = false;
        backup_write_bytes_.clear();
        backup_stage_ = BackupStage::Command;
        backup_wel_ = false;
        previous_save_backed_up_ = true;
        save_dirty_ = false;
        save_write_status_ = SaveWriteStatus::Saved;
        save_failure_reported_ = false;
        if (!preserved.empty()) {
            try {
                publish_staged_save_backup(preserved);
            } catch (const std::exception& error) {
                // Bytes are already committed. Preserve the stage for manual recovery rather than
                // reporting a failed restore or exposing it as a partially published managed file.
                std::fprintf(stderr, "[dsr] warning: restore committed, but its preservation snapshot "
                                     "remains as an unmanaged .pending file in backups/: %s\n", error.what());
                return;
            }
        }
        rotate_save_backups();
    }

    std::size_t address_bytes() const {
        if (backup_type_ == BackupType::Eeprom512B) return 1;
        if (backup_type_ == BackupType::Eeprom128K) return 3;
        return backup_is_flash(backup_type_) ? 3 : 2;
    }

    std::uint8_t backup_status_value() const {
        const std::uint8_t upper = backup_type_ == BackupType::Eeprom512B ? 0xf0 : 0;
        return static_cast<std::uint8_t>(upper | (backup_status_ & 0x9cu) |
                                         (backup_wel_ ? 0x02u : 0u));
    }

    void backup_begin() {
        backup_selected_ = true;
        backup_command_ = 0xff;
        backup_stage_ = BackupStage::Command;
        backup_address_ = 0;
        backup_address_count_ = 0;
        backup_write_bytes_.clear();
        backup_id_pos_ = 0;
    }

    void backup_close() {
        if (!backup_selected_) return;
        // An address-only read can suggest EEPROM addressing, but a cancelled flash read is identical.
        // Report the possibility without changing the chip, save buffer or write-enable latch.
        if (!chip_mismatch_hinted_ && backup_is_flash(backup_type_) &&
            (backup_command_ == 0x03 || backup_command_ == 0x0b) &&
            (backup_stage_ == BackupStage::Read || backup_stage_ == BackupStage::Dummy) && backup_data_bytes_ == 0) {
            chip_mismatch_hinted_ = true;
            std::fprintf(stderr,
                         "[dsr] hint: an address-only backup read may indicate an EEPROM/flash mismatch, or a "
                         "cancelled flash read. If the game reports a corrupt save, try --backup-type eeprom64k "
                         "(or eeprom8k), or choose Save chip in the launcher.\n");
        }
        finalize_backup_write();
        backup_selected_ = false;
        backup_stage_ = BackupStage::Command;
        backup_write_bytes_.clear();
    }

    std::uint32_t normalize_backup_address(std::uint32_t address) const {
        return address & static_cast<std::uint32_t>(backup_.size() - 1);
    }
    bool backup_is_protected(std::size_t address) const {
        const unsigned protection = (backup_status_ >> 2) &
                                    (backup_is_flash(backup_type_) ? 7u : 3u);
        if (protection == 0) return false;
        if (backup_is_flash(backup_type_)) {
            const std::size_t protected_size = protection == 7
                ? backup_.size() : backup_.size() >> (7u - protection);
            return address >= backup_.size() - protected_size;
        }
        const std::size_t protected_start =
            protection == 1 ? backup_.size() * 3 / 4 :
            protection == 2 ? backup_.size() / 2 : 0;
        return address >= protected_start;
    }


    void accept_backup_address(std::uint8_t value) {
        if (backup_type_ == BackupType::Eeprom512B &&
            (backup_command_ == 0x0b || backup_command_ == 0x0a)) {
            backup_address_ = 0x100u | value;
        } else {
            backup_address_ = (backup_address_ << 8) | value;
        }
        ++backup_address_count_;
        if (backup_address_count_ >= address_bytes()) {
            backup_address_ = normalize_backup_address(backup_address_);
            backup_stage_ = (backup_command_ == 0x0b && backup_type_ != BackupType::Eeprom512B)
                                ? BackupStage::Dummy :
                            ((backup_command_ == 0x03 || backup_command_ == 0x0b)
                                 ? BackupStage::Read : BackupStage::Write);
        }
    }

    void finalize_backup_write() {
        if (backup_write_bytes_.empty()) {
            if (backup_is_flash(backup_type_) && backup_wel_ &&
                (backup_command_ == 0xdb || backup_command_ == 0xd8)) {
                const std::uint32_t erase_size = backup_command_ == 0xdb ? 0x100u : 0x10000u;
                const std::uint32_t base = backup_address_ & ~(erase_size - 1u);
                for (std::uint32_t i = 0; i < erase_size; ++i) {
                    const std::size_t index = normalize_backup_address(base + i);
                    if (!backup_is_protected(index) && backup_[index] != 0xff) {
                        backup_[index] = 0xff;
                        save_dirty_ = true;
                    }
                }
                backup_wel_ = false;
            }
            return;
        }
        if (!backup_wel_) {
            backup_write_bytes_.clear();
            return;
        }

        const bool flash = backup_is_flash(backup_type_);
        const std::uint8_t command = backup_command_;
        if (!flash) {
            for (std::size_t i = 0; i < backup_write_bytes_.size(); ++i) {
                const std::size_t page = backup_page_size(backup_type_);
                const std::uint32_t page_base = backup_address_ & ~static_cast<std::uint32_t>(page - 1);
                const std::uint32_t page_offset = backup_address_ & static_cast<std::uint32_t>(page - 1);
                const std::uint32_t address = page_base |
                    ((page_offset + static_cast<std::uint32_t>(i)) & static_cast<std::uint32_t>(page - 1));
                const std::size_t index = normalize_backup_address(address);
                if (!backup_is_protected(index) && backup_[index] != backup_write_bytes_[i]) {
                    backup_[index] = backup_write_bytes_[i];
                    save_dirty_ = true;
                }
            }
            backup_wel_ = false;
            backup_write_bytes_.clear();
            return;
        }
        if (command == 0x01) {
            if (!backup_write_bytes_.empty()) backup_status_ = backup_write_bytes_[0] & 0x8c;
        } else if (command == 0x02) {
            const std::size_t page = backup_page_size(backup_type_);
            const std::uint32_t base = backup_address_ & ~static_cast<std::uint32_t>(page - 1);
            const std::uint32_t offset = backup_address_ & static_cast<std::uint32_t>(page - 1);
            for (std::size_t i = 0; i < backup_write_bytes_.size(); ++i) {
                const std::uint32_t address = base |
                    ((offset + static_cast<std::uint32_t>(i)) & static_cast<std::uint32_t>(page - 1));
                const std::size_t index = normalize_backup_address(address);
                if (!backup_is_protected(index) &&
                    backup_[index] != backup_write_bytes_[i]) {
                    backup_[index] = backup_write_bytes_[i];
                    save_dirty_ = true;
                }
            }
        } else if (command == 0x0a) {
            const std::size_t page = backup_page_size(backup_type_);
            const std::uint32_t base = backup_address_ & ~static_cast<std::uint32_t>(page - 1);
            const std::uint32_t offset = backup_address_ & static_cast<std::uint32_t>(page - 1);
            std::array<std::uint8_t, 256> new_page{};
            for (std::size_t i = 0; i < page; ++i)
                new_page[i] = backup_[normalize_backup_address(base + static_cast<std::uint32_t>(i))];
            for (std::size_t i = 0; i < backup_write_bytes_.size(); ++i) {
                const std::size_t page_offset = (offset + i) & (page - 1);
                const std::size_t index = normalize_backup_address(base + static_cast<std::uint32_t>(page_offset));
                if (!backup_is_protected(index)) new_page[page_offset] = backup_write_bytes_[i];
            }
            for (std::size_t i = 0; i < page; ++i) {
                const std::size_t index = normalize_backup_address(base + static_cast<std::uint32_t>(i));
                if (backup_[index] != new_page[i]) {
                    backup_[index] = new_page[i];
                    save_dirty_ = true;
                }
            }
        }
        backup_wel_ = false;
        backup_write_bytes_.clear();
    }

    std::uint8_t backup_tx(std::uint8_t value) {
        if (!backup_selected_) backup_begin();
        if (backup_stage_ == BackupStage::Command) {
            backup_command_ = value;
            backup_address_ = 0;
            backup_address_count_ = 0;
            backup_write_bytes_.clear();
            backup_id_pos_ = 0;
            backup_data_bytes_ = 0;
            switch (value) {
            case 0x06: backup_wel_ = true; backup_stage_ = BackupStage::Ignore; break;
            case 0x04: backup_wel_ = false; backup_stage_ = BackupStage::Ignore; break;
            case 0x05: backup_stage_ = BackupStage::Status; break;
            case 0x01: backup_stage_ = BackupStage::WriteStatus; break;
            case 0x03:
            case 0x0b:
            case 0x02:
            case 0x0a:
            case 0xdb:
            case 0xd8:
                backup_stage_ = BackupStage::Address;
                break;
            case 0xc7:
            case 0x60:
                if (backup_is_flash(backup_type_) && backup_wel_) {
                    for (std::size_t i = 0; i < backup_.size(); ++i) {
                        if (!backup_is_protected(i) && backup_[i] != 0xff) {
                            backup_[i] = 0xff;
                            save_dirty_ = true;
                        }
                    }
                }
                backup_wel_ = false;
                backup_stage_ = BackupStage::Ignore;
                break;
            case 0x9f: backup_stage_ = BackupStage::Id; break;
            default: backup_stage_ = BackupStage::Ignore; break;
            }
            return 0xff;
        }
        switch (backup_stage_) {
        case BackupStage::Address:
            accept_backup_address(value);
            return 0xff;
        case BackupStage::Dummy:
            backup_stage_ = BackupStage::Read;
            return 0xff;
        case BackupStage::Read: {
            ++backup_data_bytes_;
            const std::size_t index = normalize_backup_address(backup_address_++);
            return backup_[index];
        }
        case BackupStage::Write:
            backup_write_bytes_.push_back(value);
            return 0xff;
        case BackupStage::WriteStatus:
            if (backup_wel_) {
                backup_status_ = value & 0x9c;
                backup_wel_ = false;
            }
            backup_stage_ = BackupStage::Ignore;
            return 0xff;
        case BackupStage::Status:
            return backup_status_value();
        case BackupStage::Id: {
            static constexpr std::array<std::uint8_t, 3> kFlashIds[] = {
                {0x20, 0x40, 0x12}, {0x20, 0x40, 0x13},
                {0x20, 0x40, 0x14}, {0xc2, 0x20, 0x17}};
            if (!backup_is_flash(backup_type_)) return 0xff;
            std::size_t which = 0;
            if (backup_type_ == BackupType::Flash512K) which = 1;
            if (backup_type_ == BackupType::Flash1M) which = 2;
            if (backup_type_ == BackupType::Flash8M) which = 3;
            return kFlashIds[which][backup_id_pos_++ % 3];
        }
        case BackupStage::Command:
        case BackupStage::Ignore:
            return 0xff;
        }
        return 0xff;
    }

    void finish_backup_transaction() {
        if (backup_selected_) backup_close();
    }

    enum class BackupStage : std::uint8_t {
        Command, Address, Dummy, Read, Write, WriteStatus, Status, Id, Ignore
    };

    std::filesystem::path save_path_;
#if defined(_WIN32)
    HANDLE rom_file_ = INVALID_HANDLE_VALUE;
    HANDLE rom_mapping_ = nullptr;
#else
    int rom_fd_ = -1;
#endif
    const std::uint8_t* rom_ = nullptr;
    std::size_t rom_size_ = 0;
    BackupType backup_type_ = BackupType::Flash512K;
    std::vector<std::uint8_t> backup_;
    bool save_dirty_ = false;
    bool save_failure_reported_ = false;
    SaveWriteStatus save_write_status_ = SaveWriteStatus::Idle;
    std::size_t save_backup_limit_ = 5;
    bool save_backup_rotation_pending_ = false;
    bool save_backup_rotation_reported_ = false;
    bool chip_mismatch_hinted_ = false;
    std::size_t backup_data_bytes_ = 0;
    bool previous_save_backed_up_ = false;
    std::uint16_t aux_control_ = 0;
    std::uint8_t aux_data_ = 0xff;
    std::uint32_t rom_control_ = 0;
    std::array<std::uint8_t, 8> card_command_{};
    bool transfer_active_ = false;
    bool transfer_data_ready_ = false;
    std::int64_t transfer_ticks_ = 0;
    std::size_t transfer_pos_ = 0;
    std::size_t transfer_size_ = 0;
    std::uint32_t transfer_rom_address_ = 0;
    TransferKind transfer_kind_ = TransferKind::None;
    std::array<std::uint8_t, 4> transfer_special_{};
    bool backup_selected_ = false;
    bool backup_wel_ = false;
    std::uint8_t backup_status_ = 0;
    std::uint8_t backup_command_ = 0xff;
    BackupStage backup_stage_ = BackupStage::Command;
    std::uint32_t backup_address_ = 0;
    std::size_t backup_address_count_ = 0;
    std::vector<std::uint8_t> backup_write_bytes_;
    std::size_t backup_id_pos_ = 0;
    std::function<void()> on_transfer_done_irq;
    std::function<void()> on_data_ready;
};

GameCard::GameCard(const std::filesystem::path& rom_path,
                   const std::filesystem::path& save_path)
    : GameCard(rom_path, save_path, BackupType::Auto) {}

GameCard::GameCard(const std::filesystem::path& rom_path,
                   const std::filesystem::path& save_path, BackupType backup_type)
    : impl_(new Impl(rom_path, save_path, backup_type)) {
    impl_->on_transfer_done_irq = [this] {
        if (on_transfer_done_irq) on_transfer_done_irq();
    };
    impl_->on_data_ready = [this] {
        if (on_data_ready) on_data_ready();
    };
}

GameCard::~GameCard() {
    if (impl_ != nullptr) {
        try {
            impl_->finish_backup_transaction();
            impl_->flush_save();
        } catch (...) {}
        delete impl_;
    }
}

void GameCard::reset() { impl_->reset(); }

std::uint32_t GameCard::io_read(std::uint32_t addr, int width) {
    const int count = width_bytes(width);
    if (addr == kCardData) return impl_->read_card_data(count) & width_mask(count);
    if (addr == kRomCtrl && count == 4) {
        // The guest polls ROMCTRL for BUSY/DATA_READY; skip the bytewise loop for the full word.
        std::uint32_t ctrl = impl_->rom_control_ & ~(kRomBusy | kRomDataReady);
        if (impl_->transfer_active_) ctrl |= kRomBusy;
        if (impl_->transfer_data_ready_) ctrl |= kRomDataReady;
        return ctrl;
    }
    std::uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
        const std::uint32_t at = addr + static_cast<std::uint32_t>(i);
        std::uint8_t byte = 0xff;
        if (at >= kAuxSpiCnt && at < kAuxSpiCnt + 2) {
            byte = static_cast<std::uint8_t>(impl_->aux_control_ >> (8 * (at - kAuxSpiCnt)));
        } else if (at == kAuxSpiData) {
            byte = impl_->aux_data_;
        } else if (at >= kRomCtrl && at < kRomCtrl + 4) {
            std::uint32_t ctrl = impl_->rom_control_ & ~(kRomBusy | kRomDataReady);
            if (impl_->transfer_active_) ctrl |= kRomBusy;
            if (impl_->transfer_data_ready_) ctrl |= kRomDataReady;
            byte = static_cast<std::uint8_t>(ctrl >> (8 * (at - kRomCtrl)));
        } else if (at >= kCardCommand && at < kCardCommand + 8) {
            byte = impl_->card_command_[at - kCardCommand];
        }
        result |= static_cast<std::uint32_t>(byte) << (8 * i);
    }
    return result & width_mask(count);
}

void GameCard::io_write(std::uint32_t addr, std::uint32_t value, int width) {
    const int count = width_bytes(width);
    if (addr == kRomCtrl && count == 4) {
        if ((value & kRomBusy) != 0) impl_->start_card_transfer(value);
        else impl_->rom_control_ = value & ~kRomDataReady;
        return;
    }
    if (addr == kAuxSpiCnt && count == 2) {
        const auto new_control = static_cast<std::uint16_t>(value);
        const bool was_spi = (impl_->aux_control_ & kAuxSpiMode) != 0;
        const bool now_spi = (new_control & kAuxSpiMode) != 0;
        const bool enabled = (new_control & kAuxSlotEnable) != 0;
        if (impl_->backup_selected_ && (!now_spi || !enabled)) impl_->finish_backup_transaction();
        impl_->aux_control_ = static_cast<std::uint16_t>(new_control &
            static_cast<std::uint16_t>(~kAuxBusy));
        if (!was_spi && now_spi) impl_->aux_data_ = 0xff;
        return;
    }
    for (int i = 0; i < count; ++i) {
        const std::uint32_t at = addr + static_cast<std::uint32_t>(i);
        const std::uint8_t byte = static_cast<std::uint8_t>(value >> (8 * i));
        if (at >= kCardCommand && at < kCardCommand + 8)
            impl_->card_command_[at - kCardCommand] = byte;
        else if (at >= kAuxSpiCnt && at < kAuxSpiCnt + 2) {
            const unsigned shift = 8u * (at - kAuxSpiCnt);
            const std::uint16_t mask = static_cast<std::uint16_t>(0xffu << shift);
            const auto new_control = static_cast<std::uint16_t>(
                (impl_->aux_control_ & ~mask) | (static_cast<std::uint16_t>(byte) << shift));
            const bool was_spi = (impl_->aux_control_ & kAuxSpiMode) != 0;
            const bool now_spi = (new_control & kAuxSpiMode) != 0;
            const bool enabled = (new_control & kAuxSlotEnable) != 0;
            if (impl_->backup_selected_ && (!now_spi || !enabled))
                impl_->finish_backup_transaction();
            impl_->aux_control_ = static_cast<std::uint16_t>(new_control &
                static_cast<std::uint16_t>(~kAuxBusy));
            if (!was_spi && now_spi) impl_->aux_data_ = 0xff;
        } else if (at == kAuxSpiData) {
            const bool enabled = (impl_->aux_control_ & kAuxSlotEnable) != 0;
            const bool spi_mode = (impl_->aux_control_ & kAuxSpiMode) != 0;
            if (enabled && spi_mode) {
                impl_->aux_data_ = impl_->backup_tx(byte);
                if ((impl_->aux_control_ & kAuxHold) == 0) impl_->finish_backup_transaction();
            }
        } else if (at >= kRomCtrl && at < kRomCtrl + 4) {
            const std::uint32_t shift = 8u * (at - kRomCtrl);
            const std::uint32_t mask = 0xffu << shift;
            const std::uint32_t combined = (impl_->rom_control_ & ~mask) |
                                           (static_cast<std::uint32_t>(byte) << shift);
            if ((combined & kRomBusy) != 0) impl_->start_card_transfer(combined);
            else impl_->rom_control_ = combined & ~kRomDataReady;
        }
    }
}

void GameCard::run(std::int64_t ticks) { impl_->advance_card(ticks); }

void GameCard::read_rom(std::uint32_t offset, void* dst, std::uint32_t size) const {
    impl_->raw_rom_read(offset, static_cast<std::uint8_t*>(dst), size);
}

std::uint32_t GameCard::chip_id() const { return kCardChipId; }
std::uint64_t GameCard::rom_size() const { return impl_->rom_size_; }
bool GameCard::chip_mismatch_suspected() const { return impl_->chip_mismatch_hinted_; }

GameCard::SaveWriteStatus GameCard::save_write_status() const { return impl_->save_write_status_; }

void GameCard::set_save_backup_limit(std::size_t limit) {
    if (limit == 0) throw std::invalid_argument("save backup limit must be at least one");
    impl_->save_backup_limit_ = limit;
}

std::vector<GameCard::SaveBackup> GameCard::save_backups() const {
    auto paths = impl_->managed_save_backups();
    std::vector<SaveBackup> backups;
    backups.reserve(paths.size());
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        const auto size = std::filesystem::file_size(*it);
        backups.push_back({std::move(*it), size});
    }
    return backups;
}

std::size_t GameCard::save_capacity() const { return impl_->backup_.size(); }

void GameCard::restore_save(const std::filesystem::path& source) {
    try {
        impl_->restore_save(source);
    } catch (...) {
        impl_->save_write_status_ = SaveWriteStatus::Failed;
        throw;
    }
}

bool GameCard::flush_save() {
    try {
        impl_->flush_save();
        impl_->save_failure_reported_ = false;
        return true;
    } catch (const std::exception& error) {
        impl_->save_write_status_ = SaveWriteStatus::Failed;
        // A full disk or a read-only folder must not end the session: the data stays dirty and the next
        // flush (autosave or exit) tries again. Report once per run of failures.
        if (!impl_->save_failure_reported_)
            std::fprintf(stderr, "[dsr] warning: could not write the save file %s: %s (will retry)\n",
                         impl_->save_path_.string().c_str(), error.what());
        impl_->save_failure_reported_ = true;
        return false;
    }
}

struct Spi::Impl {
    // Interior calibration points give the SDK an exact, reversible ADC scale.
    static constexpr int kTouchAdcPerPixel = 16;
    static constexpr int kTouchCalibrationLowPixel = 32;
    static constexpr int kTouchCalibrationHighXPixel = 224;
    static constexpr int kTouchCalibrationHighYPixel = 128;

    explicit Impl(const FirmwareSettings& settings) {
        firmware_.fill(0xff);
        firmware_write_bytes_.reserve(256);
        build_firmware(settings);
        reset();
    }

    void reset() {
        if (selected_) end_device();
        control_ = 0;
        data_ = 0xff;
        selected_ = false;
        selected_device_ = 3;
        transfer_pending_ = false;
        pm_registers_ = {0x0f, 0x00, 0x00, 0x01, 0x4f};
        pm_index_ = 0;
        pm_read_ = false;
        pm_data_phase_ = false;
        firmware_wel_ = false;
        firmware_status_ = 0;
        firmware_command_ = 0xff;
        firmware_stage_ = FirmwareStage::Command;
        firmware_address_ = 0;
        firmware_address_bytes_ = 0;
        firmware_write_bytes_.clear();
        firmware_id_pos_ = 0;
        tsc_channel_ = 0;
        tsc_resolution_8_ = false;
        tsc_power_mode_ = 0;
        tsc_reference_select_ = false;
        tsc_bit_pos_ = 0;
    }

    static void build_user_settings(std::uint8_t* area, const FirmwareSettings& settings,
                                    std::uint16_t counter) {
        std::fill(area, area + 0x100, 0xff);
        std::fill(area, area + 0x70, 0);
        put_le16(area, 5);
        area[2] = static_cast<std::uint8_t>(settings.favorite_color & 0x0f);
        area[3] = static_cast<std::uint8_t>(std::clamp<unsigned>(settings.birthday_month, 1, 12));
        area[4] = static_cast<std::uint8_t>(std::clamp<unsigned>(settings.birthday_day, 1, 31));
        const std::size_t nickname_size = std::min<std::size_t>(settings.nickname.size(), 10);
        for (std::size_t i = 0; i < nickname_size; ++i)
            put_le16(area + 0x06 + i * 2, static_cast<std::uint16_t>(settings.nickname[i]));
        put_le16(area + 0x1a, static_cast<std::uint16_t>(nickname_size));
        const std::size_t message_size = std::min<std::size_t>(settings.message.size(), 26);
        for (std::size_t i = 0; i < message_size; ++i)
            put_le16(area + 0x1c + i * 2, static_cast<std::uint16_t>(settings.message[i]));
        put_le16(area + 0x50, static_cast<std::uint16_t>(message_size));
        area[0x52] = 0;
        area[0x53] = 0;
        put_le16(area + 0x54, 0);
        area[0x56] = 0;
        area[0x57] = 0;
        // Firmware coordinates are one-based; ADC conversion accepts zero-based pixels.
        put_le16(area + 0x58, kTouchCalibrationLowPixel * kTouchAdcPerPixel);
        put_le16(area + 0x5a, kTouchCalibrationLowPixel * kTouchAdcPerPixel);
        area[0x5c] = kTouchCalibrationLowPixel + 1;
        area[0x5d] = kTouchCalibrationLowPixel + 1;
        put_le16(area + 0x5e, kTouchCalibrationHighXPixel * kTouchAdcPerPixel);
        put_le16(area + 0x60, kTouchCalibrationHighYPixel * kTouchAdcPerPixel);
        area[0x62] = kTouchCalibrationHighXPixel + 1;
        area[0x63] = kTouchCalibrationHighYPixel + 1;
        const std::uint16_t flags = static_cast<std::uint16_t>(0xec00u |
            (settings.language & 7u));
        put_le16(area + 0x64, flags);
        area[0x66] = 0;
        area[0x67] = 0;
        put_le32(area + 0x68, 0);
        put_le16(area + 0x70, static_cast<std::uint16_t>(counter & 0x7f));
        put_le16(area + 0x72, crc16_ds(area, 0x70));
    }

    void build_firmware(const FirmwareSettings& settings) {
        std::fill(firmware_.begin(), firmware_.begin() + 0x200, 0);
        firmware_[8] = 'M';
        firmware_[9] = 'A';
        firmware_[10] = 'C';
        firmware_[11] = 1;
        firmware_[0x14] = 0;
        firmware_[0x15] = 0x20; // 256 KiB capacity in 128 KiB units.
        firmware_[0x1d] = 0xff;
        put_le16(firmware_.data() + 0x20, 0x7fc0);
        put_le16(firmware_.data() + 0x22, 0x7ec0);
        put_le16(firmware_.data() + 0x24, 0x7e40);

        // A plausible DS radio calibration block with a locally administered,
        // unicast MAC address and a valid configuration CRC.
        put_le16(firmware_.data() + 0x2c, 0x0138);
        firmware_[0x2f] = 0x05;
        const std::array<std::uint8_t, 6> mac = {0x00, 0x09, 0xbf, 0x12, 0x34, 0x56};
        std::copy(mac.begin(), mac.end(), firmware_.begin() + 0x36);
        put_le16(firmware_.data() + 0x3c, 0x3ffe);
        put_le16(firmware_.data() + 0x3e, 0xffff);
        firmware_[0x40] = 2;
        firmware_[0x41] = 0x18;
        firmware_[0x42] = 0x0c;
        firmware_[0x43] = 1;
        put_le16(firmware_.data() + 0x2a, crc16_ds(firmware_.data() + 0x2c, 0x138, 0));

        for (std::size_t offset : {std::size_t(0x3fa00), std::size_t(0x3fb00),
                                   std::size_t(0x3fc00), std::size_t(0x3fd00)}) {
            auto* slot = firmware_.data() + offset;
            std::fill(slot, slot + 0x100, 0);
            slot[0xe7] = 0xff;
            put_le16(slot + 0xfe, crc16_ds(slot, 0xfe, 0));
        }
        build_user_settings(firmware_.data() + 0x3fe00, settings, 0);
        build_user_settings(firmware_.data() + 0x3ff00, settings, 1);
    }

    void update_touch_command(std::uint8_t control) {
        if ((control & 0x80u) == 0) return;
        tsc_channel_ = static_cast<std::uint8_t>((control >> 4) & 7u);
        tsc_resolution_8_ = (control & 0x08u) != 0;
        tsc_power_mode_ = static_cast<std::uint8_t>(control & 0x03u);
        tsc_reference_select_ = (control & 0x04u) != 0;
        tsc_bit_pos_ = 0;
    }

    std::uint16_t touch_conversion(std::uint8_t channel) const {
        switch (channel & 7u) {
        case 0: return 0x0680; // temperature 0, plausible fixed voltage
        case 1: return touch_down_ ? raw_y_ : 0x0fff;
        case 2: return 0x0000; // NDS battery input is grounded
        case 3: return touch_down_ ? 0x0400 : 0x0000;
        case 4: return touch_down_ ? 0x0700 : 0x0fff;
        case 5: return touch_down_ ? raw_x_ : 0x0000;
        case 6: return 0x0800; // microphone/AUX channel
        case 7: return 0x06a0;
        }
        return 0;
    }

    static std::uint32_t address_bytes_for_spi(std::uint32_t) { return 3; }

    void firmware_begin() {
        firmware_command_ = 0xff;
        firmware_stage_ = FirmwareStage::Command;
        firmware_address_ = 0;
        firmware_address_bytes_ = 0;
        firmware_write_bytes_.clear();
        firmware_id_pos_ = 0;
    }

    void firmware_finalize() {
        if (firmware_command_ == 0x01 && !firmware_write_bytes_.empty() && firmware_wel_) {
            firmware_status_ = firmware_write_bytes_[0] & 0x8c;
            firmware_wel_ = false;
        } else if ((firmware_command_ == 0x02 || firmware_command_ == 0x0a) &&
                   !firmware_write_bytes_.empty() && firmware_wel_) {
            constexpr std::size_t page_size = 256;
            const std::uint32_t page_base = firmware_address_ & ~std::uint32_t(0xff);
            const std::uint32_t page_offset = firmware_address_ & 0xffu;
            if (firmware_command_ == 0x0a) {
                std::array<std::uint8_t, page_size> page{};
                for (std::size_t i = 0; i < page_size; ++i)
                    page[i] = firmware_[(page_base + static_cast<std::uint32_t>(i)) & 0x3ffffu];
                for (std::size_t i = 0; i < firmware_write_bytes_.size(); ++i)
                    page[(page_offset + i) & 0xffu] = firmware_write_bytes_[i];
                for (std::size_t i = 0; i < page_size; ++i)
                    firmware_[(page_base + static_cast<std::uint32_t>(i)) & 0x3ffffu] = page[i];
            } else {
                for (std::size_t i = 0; i < firmware_write_bytes_.size(); ++i) {
                    const std::uint32_t at = (page_base | ((page_offset + static_cast<std::uint32_t>(i)) & 0xffu)) & 0x3ffffu;
                    firmware_[at] &= firmware_write_bytes_[i];
                }
            }
            firmware_wel_ = false;
        } else if (firmware_command_ == 0xdb && firmware_wel_) {
            const std::uint32_t base = firmware_address_ & ~std::uint32_t(0xff);
            for (std::uint32_t i = 0; i < 0x100; ++i) firmware_[(base + i) & 0x3ffffu] = 0xff;
            firmware_wel_ = false;
        } else if (firmware_command_ == 0xd8 && firmware_wel_) {
            const std::uint32_t base = firmware_address_ & ~std::uint32_t(0xffff);
            for (std::uint32_t i = 0; i < 0x10000; ++i) firmware_[(base + i) & 0x3ffffu] = 0xff;
            firmware_wel_ = false;
        } else if ((firmware_command_ == 0xc7 || firmware_command_ == 0x60) && firmware_wel_) {
            firmware_.fill(0xff);
            firmware_wel_ = false;
        }
        firmware_write_bytes_.clear();
    }

    std::uint8_t firmware_tx(std::uint8_t value) {
        if (!selected_) firmware_begin();
        if (firmware_stage_ == FirmwareStage::Command) {
            firmware_command_ = value;
            firmware_address_ = 0;
            firmware_address_bytes_ = 0;
            firmware_write_bytes_.clear();
            firmware_id_pos_ = 0;
            switch (value) {
            case 0x06: firmware_wel_ = true; firmware_stage_ = FirmwareStage::Ignore; break;
            case 0x04: firmware_wel_ = false; firmware_stage_ = FirmwareStage::Ignore; break;
            case 0x05: firmware_stage_ = FirmwareStage::Status; break;
            case 0x01: firmware_stage_ = FirmwareStage::WriteStatus; break;
            case 0x03:
            case 0x0b:
            case 0x02:
            case 0x0a:
            case 0xdb:
            case 0xd8:
                firmware_stage_ = FirmwareStage::Address;
                break;
            case 0xc7:
            case 0x60:
                firmware_stage_ = FirmwareStage::Ignore;
                break;
            case 0x9f: firmware_stage_ = FirmwareStage::Id; break;
            default: firmware_stage_ = FirmwareStage::Ignore; break;
            }
            return 0xff;
        }
        switch (firmware_stage_) {
        case FirmwareStage::Address:
            firmware_address_ = (firmware_address_ << 8) | value;
            ++firmware_address_bytes_;
            if (firmware_address_bytes_ == address_bytes_for_spi(firmware_address_)) {
                firmware_address_ &= 0x3ffffu;
                if (firmware_command_ == 0x0b) firmware_stage_ = FirmwareStage::Dummy;
                else if (firmware_command_ == 0x03) firmware_stage_ = FirmwareStage::Read;
                else firmware_stage_ = FirmwareStage::Write;
            }
            return 0xff;
        case FirmwareStage::Dummy:
            firmware_stage_ = FirmwareStage::Read;
            return 0xff;
        case FirmwareStage::Read: {
            const std::uint8_t result = firmware_[firmware_address_ & 0x3ffffu];
            firmware_address_ = (firmware_address_ + 1u) & 0x3ffffu;
            return result;
        }
        case FirmwareStage::Write:
            firmware_write_bytes_.push_back(value);
            return 0xff;
        case FirmwareStage::WriteStatus:
            if (firmware_wel_) {
                firmware_status_ = value & 0x8c;
                firmware_wel_ = false;
            }
            firmware_stage_ = FirmwareStage::Ignore;
            return 0xff;
        case FirmwareStage::Status:
            return static_cast<std::uint8_t>((firmware_status_ & 0x8c) |
                                             (firmware_wel_ ? 0x02u : 0u));
        case FirmwareStage::Id: {
            static constexpr std::array<std::uint8_t, 3> id = {0x20, 0x40, 0x12};
            return id[firmware_id_pos_++ % id.size()];
        }
        case FirmwareStage::Command:
        case FirmwareStage::Ignore:
            return 0xff;
        }
        return 0xff;
    }

    void firmware_deselect() { firmware_finalize(); }

    std::uint8_t power_tx(std::uint8_t value) {
        if (!selected_) {
            pm_data_phase_ = false;
            pm_read_ = false;
        }
        if (!pm_data_phase_) {
            pm_index_ = value & 0x7fu;
            pm_read_ = (value & 0x80u) != 0;
            pm_data_phase_ = true;
            return 0xff;
        }
        const std::size_t index = pm_index_ < pm_registers_.size() ? pm_index_ : 0;
        if (pm_read_) return pm_registers_[index];
        if (index == 0) {
            pm_registers_[0] = value & 0x7f;
        } else if (index == 2) {
            pm_registers_[2] = value & 0x01;
        } else if (index == 3) {
            pm_registers_[3] = value & 0x03;
        } else if (index == 4) {
            pm_registers_[4] = static_cast<std::uint8_t>((pm_registers_[4] & 0x0c) | (value & 0x07));
        }
        return 0xff;
    }

    std::uint8_t tsc_response_byte() {
        std::uint16_t value = touch_conversion(tsc_channel_);
        if (tsc_power_mode_ == 2) value = 0;
        if (tsc_resolution_8_) {
            const auto value8 = static_cast<std::uint8_t>(value >> 4);
            if (tsc_bit_pos_ == 0) {
                tsc_bit_pos_ = 8;
                return static_cast<std::uint8_t>((value8 >> 1) & 0x7fu);
            }
            if (tsc_bit_pos_ == 8) {
                tsc_bit_pos_ = 16;
                return static_cast<std::uint8_t>((value8 & 1u) << 7);
            }
            return 0;
        }
        if (tsc_bit_pos_ == 0) {
            tsc_bit_pos_ = 8;
            return static_cast<std::uint8_t>((value >> 5) & 0x7fu);
        }
        if (tsc_bit_pos_ == 8) {
            tsc_bit_pos_ = 16;
            return static_cast<std::uint8_t>((value & 0x1fu) << 3);
        }
        return 0;
    }

    std::uint8_t touch_tx(std::uint8_t value) {
        const std::uint8_t result = tsc_response_byte();
        update_touch_command(value);
        return result;
    }

    void start_device() {
        selected_ = true;
        selected_device_ = static_cast<std::uint8_t>((control_ & kSpiDeviceMask) >> 8);
        if (selected_device_ == 1) firmware_begin();
        if (selected_device_ == 0) {
            pm_data_phase_ = false;
            pm_read_ = false;
        }
        if (selected_device_ == 2) tsc_bit_pos_ = 0;
    }

    void end_device() {
        if (!selected_) return;
        if (selected_device_ == 1) firmware_deselect();
        selected_ = false;
    }

    std::uint8_t exchange(std::uint8_t value) {
        const auto device = static_cast<std::uint8_t>((control_ & kSpiDeviceMask) >> 8);
        if (!selected_ || selected_device_ != device) {
            end_device();
            start_device();
        }
        std::uint8_t result = 0xff;
        if (device == 0) result = power_tx(value);
        else if (device == 1) result = firmware_tx(value);
        else if (device == 2) result = touch_tx(value);
        if ((control_ & kSpiHold) == 0) end_device();
        return result;
    }

    void write_control(std::uint16_t value) {
        const std::uint16_t allowed = static_cast<std::uint16_t>(0xcf0fu);
        const auto next = static_cast<std::uint16_t>(value & allowed);
        const auto next_device = static_cast<std::uint8_t>((next & kSpiDeviceMask) >> 8);
        const bool next_enabled = (next & kSpiEnable) != 0;
        if (selected_ && (!next_enabled || next_device != selected_device_)) end_device();
        control_ = next;
    }

    std::uint32_t io_read(std::uint32_t addr, int width) {
        const int count = std::clamp(width, 1, 4);
        std::uint32_t result = 0;
        for (int i = 0; i < count; ++i) {
            const auto at = addr + static_cast<std::uint32_t>(i);
            std::uint8_t byte = 0;
            if (at >= kSpiCnt && at < kSpiCnt + 2) {
                std::uint16_t status = control_;
                if (transfer_pending_) status |= kSpiBusy;
                byte = static_cast<std::uint8_t>(status >> (8 * (at - kSpiCnt)));
                if (at == kSpiCnt) complete_transfer();
            } else if (at == kSpiData) {
                byte = data_;
                complete_transfer();
            }
            result |= static_cast<std::uint32_t>(byte) << (8 * i);
        }
        return result & width_mask(count);
    }

    void complete_transfer() {
        if (!transfer_pending_) return;
        transfer_pending_ = false;
        if ((control_ & kSpiIrqEnable) != 0 && owner_irq_) owner_irq_();
    }

    void io_write(std::uint32_t addr, std::uint32_t value, int width) {
        const int count = std::clamp(width, 1, 4);
        if (addr == kSpiCnt && (count == 2 || count == 1)) {
            std::uint16_t next = control_;
            if (count == 1) next = static_cast<std::uint16_t>((next & 0xff00u) | (value & 0xffu));
            else next = static_cast<std::uint16_t>(value);
            write_control(next);
            return;
        }
        if (addr == kSpiData) {
            if ((control_ & kSpiEnable) == 0) return;
            if (transfer_pending_) complete_transfer();
            data_ = exchange(static_cast<std::uint8_t>(value));
            transfer_pending_ = true;
            return;
        }
        for (int i = 0; i < count; ++i) {
            const auto at = addr + static_cast<std::uint32_t>(i);
            const auto byte = static_cast<std::uint8_t>(value >> (8 * i));
            if (at >= kSpiCnt && at < kSpiCnt + 2) {
                std::uint16_t next = control_;
                const unsigned shift = 8u * (at - kSpiCnt);
                next = static_cast<std::uint16_t>((next & ~(0xffu << shift)) |
                                                  (static_cast<std::uint16_t>(byte) << shift));
                write_control(next);
            } else if (at == kSpiData && (control_ & kSpiEnable) != 0) {
                if (transfer_pending_) complete_transfer();
                data_ = exchange(byte);
                transfer_pending_ = true;
            }
        }
    }

    void set_touch(bool down, int x, int y) {
        touch_down_ = down;
        const int cx = static_cast<int>(clamp_unsigned(x, 255));
        const int cy = static_cast<int>(clamp_unsigned(y, 191));
        // Calibration endpoints are interior points, not ADC saturation limits.
        // An integral scale survives the guest SDK's fixed-point inverse without
        // losing a pixel to two successive truncations.
        raw_x_ = static_cast<std::uint16_t>(cx * kTouchAdcPerPixel);
        raw_y_ = static_cast<std::uint16_t>(cy * kTouchAdcPerPixel);
    }

    const std::uint8_t* firmware() const { return firmware_.data(); }
    std::uint32_t firmware_size() const { return static_cast<std::uint32_t>(firmware_.size()); }

    enum class FirmwareStage : std::uint8_t {
        Command, Address, Dummy, Read, Write, WriteStatus, Status, Id, Ignore
    };

    std::array<std::uint8_t, 256u * 1024u> firmware_{};
    std::uint16_t control_ = 0;
    std::uint8_t data_ = 0xff;
    bool selected_ = false;
    std::uint8_t selected_device_ = 3;
    bool transfer_pending_ = false;
    std::array<std::uint8_t, 5> pm_registers_{};
    std::uint8_t pm_index_ = 0;
    bool pm_read_ = false;
    bool pm_data_phase_ = false;
    bool firmware_wel_ = false;
    std::uint8_t firmware_status_ = 0;
    std::uint8_t firmware_command_ = 0xff;
    FirmwareStage firmware_stage_ = FirmwareStage::Command;
    std::uint32_t firmware_address_ = 0;
    std::uint32_t firmware_address_bytes_ = 0;
    std::vector<std::uint8_t> firmware_write_bytes_;
    std::size_t firmware_id_pos_ = 0;
    bool touch_down_ = false;
    std::uint16_t raw_x_ = 0;
    std::uint16_t raw_y_ = 0;
    std::uint8_t tsc_channel_ = 0;
    bool tsc_resolution_8_ = false;
    std::uint8_t tsc_power_mode_ = 0;
    bool tsc_reference_select_ = false;
    std::uint8_t tsc_bit_pos_ = 0;
    std::function<void()> owner_irq_;
};

Spi::Spi(const FirmwareSettings& settings) : impl_(new Impl(settings)) {
    impl_->owner_irq_ = [this] {
        if (on_irq) on_irq();
    };
}

Spi::~Spi() { delete impl_; }
void Spi::reset() { impl_->reset(); }
std::uint32_t Spi::io_read(std::uint32_t addr, int width) { return impl_->io_read(addr, width_bytes(width)); }
void Spi::io_write(std::uint32_t addr, std::uint32_t value, int width) {
    impl_->io_write(addr, value, width_bytes(width));
}
void Spi::set_touch(bool down, int x, int y) { impl_->set_touch(down, x, y); }
const std::uint8_t* Spi::firmware() const { return impl_->firmware(); }
std::uint32_t Spi::firmware_size() const { return impl_->firmware_size(); }

struct Rtc::Impl {
    enum class Mode : std::uint8_t { Idle, Command, Read, Write };

    void reset() {
        gpio_ = 0;
        selected_ = false;
        clock_ = false;
        mode_ = Mode::Idle;
        bits_ = 0;
        input_byte_ = 0;
        output_bytes_.fill(0);
        output_size_ = 0;
        output_byte_pos_ = 0;
        output_bit_pos_ = 0;
        parameter_count_ = 0;
        parameter_byte_pos_ = 0;
        parameter_bytes_.fill(0);
        command_ = 0;
        status1_ = 0x02; // 24-hour mode.
        status2_ = 0;
        free_register_ = 0;
        alarm1_ = {0, 0, 0};
        alarm2_ = {0, 0, 0};
        int1_ = 0;
        clock_adjust_ = 0;
    }

    std::array<std::uint8_t, 7> now_date_time() const {
        std::time_t now = 0;
        std::int64_t local_epoch = 0;
        if (deterministic_clock_) {
            const std::int64_t elapsed_seconds =
                static_cast<std::int64_t>(emulated_ticks_ / 33'513'982u);
            local_epoch = epoch_seconds_ + elapsed_seconds +
                          static_cast<std::int64_t>(timezone_offset_minutes_) * 60;
            now = static_cast<std::time_t>(local_epoch);
        } else {
            now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        }
        std::tm local{};
#if defined(_WIN32)
        if (deterministic_clock_) {
            // The Windows CRT rejects pre-1970 UTC values. A fixed epoch of
            // zero with a western timezone legitimately crosses that boundary.
            const std::chrono::sys_seconds point{std::chrono::seconds{local_epoch}};
            const auto day = std::chrono::floor<std::chrono::days>(point);
            const std::chrono::year_month_day date{day};
            const std::chrono::hh_mm_ss time{point - day};
            local.tm_year = static_cast<int>(date.year()) - 1900;
            local.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
            local.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
            local.tm_wday = static_cast<int>(std::chrono::weekday{day}.c_encoding());
            local.tm_hour = static_cast<int>(time.hours().count());
            local.tm_min = static_cast<int>(time.minutes().count());
            local.tm_sec = static_cast<int>(time.seconds().count());
        } else if (::localtime_s(&local, &now) != 0) {
            return {0, 1, 1, 0, 0, 0, 0};
        }
#else
        if ((deterministic_clock_ ? ::gmtime_r(&now, &local) : ::localtime_r(&now, &local)) == nullptr)
            return {0, 1, 1, 0, 0, 0, 0};
#endif
        const unsigned year = static_cast<unsigned>((local.tm_year + 1900) % 100);
        const unsigned month = static_cast<unsigned>(local.tm_mon + 1);
        const unsigned day = static_cast<unsigned>(local.tm_mday);
        const unsigned weekday = static_cast<unsigned>(local.tm_wday);
        const unsigned hour24 = static_cast<unsigned>(local.tm_hour);
        const unsigned hour = (status1_ & 0x02u) != 0 ? hour24 : hour24 % 12u;
        const auto hour_value = static_cast<std::uint8_t>(bcd(hour) |
                                                           (hour >= 12 ? 0x40u : 0u));
        return {bcd(year), bcd(month), bcd(day), static_cast<std::uint8_t>(weekday),
                hour_value, bcd(static_cast<unsigned>(local.tm_min)),
                bcd(static_cast<unsigned>(local.tm_sec))};
    }

    std::size_t parameter_length(std::uint8_t command) const {
        switch (command >> 1) {
        case 0: return 1;
        case 1: return 1;
        case 2: return 7;
        case 3: return 3;
        case 4: return ((status2_ & 0x0fu) == 0x04u) ? 3 : 1;
        case 5: return 3;
        case 6: return 1;
        case 7: return 1;
        }
        return 1;
    }

    void setup_command(std::uint8_t wire_command) {
        // Commands accept either bit order; parameters and replies are LSB first.
        // Normalize the mirrored 0x?6 form to the ordinary command's low nibble.
        command_ = (wire_command & 0x0fu) == 0x06u
            ? static_cast<std::uint8_t>(((wire_command & 0x80u) >> 7) |
                                        ((wire_command & 0x40u) >> 5) |
                                        ((wire_command & 0x20u) >> 3) |
                                        ((wire_command & 0x10u) >> 1))
            : static_cast<std::uint8_t>(wire_command & 0x0fu);
        const unsigned index = command_ >> 1;
        parameter_count_ = parameter_length(command_);
        parameter_byte_pos_ = 0;
        output_size_ = 0;
        output_byte_pos_ = 0;
        output_bit_pos_ = 0;
        bits_ = 0;
        input_byte_ = 0;
        if ((command_ & 1u) == 0) {
            mode_ = Mode::Write;
            return;
        }
        mode_ = Mode::Read;
        switch (index) {
        case 0: output_bytes_[0] = static_cast<std::uint8_t>(status1_ & 0x0fu); output_size_ = 1; break;
        case 1: output_bytes_[0] = status2_; output_size_ = 1; break;
        case 2:
            output_bytes_ = now_date_time();
            output_size_ = 7;
            break;
        case 3: {
            const auto date = now_date_time();
            std::copy_n(date.begin() + 4, 3, output_bytes_.begin());
            output_size_ = 3;
            break;
        }
        case 4:
            if ((status2_ & 0x0fu) == 0x04u) {
                std::copy(alarm1_.begin(), alarm1_.end(), output_bytes_.begin());
                output_size_ = 3;
            } else {
                output_bytes_[0] = int1_;
                output_size_ = 1;
            }
            break;
        case 5:
            std::copy(alarm2_.begin(), alarm2_.end(), output_bytes_.begin());
            output_size_ = 3;
            break;
        case 6: output_bytes_[0] = clock_adjust_; output_size_ = 1; break;
        case 7: output_bytes_[0] = free_register_; output_size_ = 1; break;
        }
    }

    void commit_write() {
        const unsigned index = command_ >> 1;
        switch (index) {
        case 0: status1_ = static_cast<std::uint8_t>((status1_ & 0xf0u) | (parameter_bytes_[0] & 0x0fu)); break;
        case 1: status2_ = parameter_bytes_[0]; break;
        case 2:
        case 3: // Clock setting writes are accepted but the host clock is authoritative.
            break;
        case 4:
            if ((status2_ & 0x0fu) == 0x04u)
                std::copy_n(parameter_bytes_.begin(), 3, alarm1_.begin());
            else int1_ = parameter_bytes_[0];
            break;
        case 5: std::copy_n(parameter_bytes_.begin(), 3, alarm2_.begin()); break;
        case 6: clock_adjust_ = parameter_bytes_[0]; break;
        case 7: free_register_ = parameter_bytes_[0]; break;
        }
        mode_ = Mode::Idle;
        bits_ = 0;
        input_byte_ = 0;
    }

    void falling_edge(bool host_output, bool host_bit) {
        if (host_output && (mode_ == Mode::Command || mode_ == Mode::Write)) {
            input_byte_ = static_cast<std::uint8_t>(input_byte_ |
                (static_cast<std::uint8_t>(host_bit) << bits_));
            if (++bits_ == 8) {
                if (mode_ == Mode::Command) {
                    setup_command(input_byte_);
                } else {
                    parameter_bytes_[parameter_byte_pos_++] = input_byte_;
                    if (parameter_byte_pos_ == parameter_count_) commit_write();
                }
                bits_ = 0;
                input_byte_ = 0;
            }
        } else if (!host_output && mode_ == Mode::Read) {
            const auto output_bit = static_cast<std::uint8_t>(
                (output_bytes_[output_byte_pos_] >> output_bit_pos_) & 1u);
            gpio_ = static_cast<std::uint8_t>((gpio_ & ~1u) | output_bit);
            if (++output_bit_pos_ == 8) {
                output_bit_pos_ = 0;
                if (++output_byte_pos_ == output_size_) mode_ = Mode::Idle;
            }
        }
    }

    std::uint8_t read_gpio() const { return gpio_; }

    void write_gpio(std::uint8_t value) {
        const bool previous_select = selected_;
        const bool previous_clock = clock_;
        const bool select = (value & 0x40u) != 0 ? (value & 0x04u) != 0 : selected_;
        const bool next_clock = (value & 0x20u) != 0 ? (value & 0x02u) != 0 : clock_;
        const bool host_output = (value & 0x10u) != 0;
        const auto data_bit = static_cast<std::uint8_t>(host_output ? value & 1u : gpio_ & 1u);
        // Host writes cannot overwrite the device-driven SIO bit in input mode.
        gpio_ = static_cast<std::uint8_t>((value & 0xf8u) | (select ? 4u : 0u) |
                                          (next_clock ? 2u : 0u) | data_bit);
        selected_ = select;
        if (!select || !previous_select) {
            mode_ = select && previous_clock && next_clock ? Mode::Command : Mode::Idle;
            bits_ = 0;
            input_byte_ = 0;
            output_size_ = 0;
            output_byte_pos_ = 0;
            output_bit_pos_ = 0;
        }
        if (select && previous_select && previous_clock && !next_clock)
            falling_edge(host_output, data_bit != 0);
        clock_ = next_clock;
    }

    std::uint8_t gpio_ = 0;
    bool selected_ = false;
    bool clock_ = false;
    Mode mode_ = Mode::Idle;
    unsigned bits_ = 0;
    std::uint8_t input_byte_ = 0;
    std::uint8_t command_ = 0;
    std::size_t parameter_count_ = 0;
    std::size_t parameter_byte_pos_ = 0;
    std::array<std::uint8_t, 7> parameter_bytes_{};
    std::array<std::uint8_t, 7> output_bytes_{};
    std::size_t output_size_ = 0;
    std::size_t output_byte_pos_ = 0;
    unsigned output_bit_pos_ = 0;
    std::uint8_t status1_ = 0x02;
    std::uint8_t status2_ = 0;
    std::uint8_t free_register_ = 0;
    std::array<std::uint8_t, 3> alarm1_{};
    std::array<std::uint8_t, 3> alarm2_{};
    std::uint8_t int1_ = 0;
    std::uint8_t clock_adjust_ = 0;
    bool deterministic_clock_ = false;
    std::int64_t epoch_seconds_ = 0;
    std::int32_t timezone_offset_minutes_ = 0;
    std::uint64_t emulated_ticks_ = 0;
};

Rtc::Rtc() : impl_(new Impl) { impl_->reset(); }
Rtc::~Rtc() { delete impl_; }
void Rtc::reset() { impl_->reset(); }
void Rtc::set_deterministic_clock(std::int64_t epoch_seconds, std::int32_t timezone_offset_minutes) {
    impl_->deterministic_clock_ = true;
    impl_->epoch_seconds_ = epoch_seconds;
    impl_->timezone_offset_minutes_ = timezone_offset_minutes;
    impl_->emulated_ticks_ = 0;
}
void Rtc::set_emulated_ticks(std::uint64_t ticks) { impl_->emulated_ticks_ = ticks; }

std::uint32_t Rtc::io_read(std::uint32_t addr, int width) {
    if (addr < kRtcReg || addr >= kRtcReg + 4) return width_mask(width_bytes(width));
    return impl_->read_gpio() & width_mask(width_bytes(width));
}

void Rtc::io_write(std::uint32_t addr, std::uint32_t value, int width) {
    if (addr < kRtcReg || addr >= kRtcReg + 4) return;
    (void)width;
    impl_->write_gpio(static_cast<std::uint8_t>(value));
}

} // namespace dsr
