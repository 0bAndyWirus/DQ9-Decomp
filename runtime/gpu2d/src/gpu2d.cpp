#include "dsr/gpu2d.h"

#include "dsr/gpu3d.h"
#include "dsr/full_presentation.h"
#include "gpu2d_test.h"
#include "composition_order.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <bit>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#if (defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))) || \
    (defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && defined(__ELF__))
#include <immintrin.h>
#define DSR_X86_PACK_LINE 1
#endif

#include "blend_3d.h"
#include "ordinary_2d.h"
#include "rgb555_expansion.h"

namespace dsr {
namespace {

constexpr std::uint32_t kVramBase = 0x06000000U;
constexpr std::uint32_t kVramLimit = 0x07000000U;
constexpr std::uint32_t kPageSize = 0x4000U;
constexpr std::size_t kPageCount = (kVramLimit - kVramBase) / kPageSize;
constexpr std::size_t kBankCapacity = 128U * 1024U;
constexpr std::array<std::uint32_t, 9> kBankSizes{
    128U * 1024U, 128U * 1024U, 128U * 1024U, 128U * 1024U,
    64U * 1024U, 16U * 1024U, 16U * 1024U, 32U * 1024U, 16U * 1024U};
constexpr std::array<std::uint32_t, 9> kLcdcBases{
    0x06800000U, 0x06820000U, 0x06840000U, 0x06860000U,
    0x06880000U, 0x06890000U, 0x06894000U, 0x06898000U, 0x068A0000U};
constexpr std::uint32_t kPaletteBase = 0x05000000U;
constexpr std::uint32_t kOamBase = 0x07000000U;
constexpr std::uint32_t kEngineA = 0x04000000U;
constexpr std::uint32_t kEngineB = 0x04001000U;
constexpr std::uint32_t kVramCntBase = 0x04000240U;
constexpr std::uint32_t kPowerCnt = 0x04000304U;
constexpr std::uint32_t kDisplayFifo = 0x04000068U;
constexpr std::uint32_t kScreenWidth = 256U;
constexpr std::uint32_t kScreenHeight = 192U;
constexpr std::uint32_t kPixelsPerFrame = kScreenWidth * kScreenHeight;

constexpr std::uint32_t load_le(const std::uint8_t* p, int width) {
    if (!std::is_constant_evaluated() && std::endian::native == std::endian::little) {
        // One unaligned load instead of a byte-assembly loop the optimizer may not merge.
        if (width == 16) { std::uint16_t v; std::memcpy(&v, p, sizeof v); return v; }
        if (width == 32) { std::uint32_t v; std::memcpy(&v, p, sizeof v); return v; }
    }
    std::uint32_t value = 0;
    for (int i = 0; i < width / 8; ++i) {
        value |= static_cast<std::uint32_t>(p[i]) << (i * 8);
    }
    return value;
}

constexpr void store_le(std::uint8_t* p, std::uint32_t value, int width) {
    for (int i = 0; i < width / 8; ++i) {
        p[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}


constexpr std::uint8_t clamp_factor(std::uint32_t value) {
    return static_cast<std::uint8_t>(std::min(value, 16U));
}

constexpr std::int32_t sign_extend_28(std::uint32_t value) {
    value &= 0x0FFFFFFFU;
    if ((value & 0x08000000U) != 0U) {
        value |= 0xF0000000U;
    }
    return static_cast<std::int32_t>(value);
}

// 5-bit channels (see above); `alpha` is the 5-bit 3D polygon alpha (31 = opaque).
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t alpha = 31;
};

struct Pixel {
    Color color{};
    // 3D layer pixels keep the 3D engine's full 6-bit colour for alpha blending.
    std::array<std::uint8_t, 3> rgb6{};
    std::uint8_t priority = 3;
    std::uint8_t layer = 5;
    bool opaque = false;
    bool semi = false;
    bool three_d = false;
};

// Reuse an immutable fill value. A temporary Pixel{} makes GCC reconstruct
// overlapping byte fields inside each fill iteration, stalling store forwarding.
constexpr Pixel kTransparentPixel{};

constexpr std::uint32_t vram_address(std::uint32_t address, bool arm7 = false) {
    if (arm7) {
        return kVramBase + ((address - kVramBase) & 0x3FFFFU);
    }
    if (address < 0x06800000U) {
        const std::uint32_t region = (address - kVramBase) / 0x200000U;
        constexpr std::array<std::uint32_t, 4> masks{0x7FFFFU, 0x1FFFFU, 0x3FFFFU, 0x1FFFFU};
        return kVramBase + region * 0x200000U + (address & masks[region]);
    }
    return address;
}

struct VramMap {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
    std::uint8_t bank = 0;
};

struct ExtPaletteRegion {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
    std::uint8_t bank = 0;
    bool object = false;
};

struct Engine {
    std::array<std::uint32_t, 0x70U / 4U> regs{};
    std::array<std::int64_t, 2> affine_x{};
    std::array<std::int64_t, 2> affine_y{};
    int last_affine_line = -1;
    std::array<std::array<Pixel, kScreenWidth>, 4> bg_mosaic{};
    std::array<Pixel, kScreenWidth> obj_mosaic{};
#if !defined(DSR_X86_PACK_LINE)
    std::array<std::uint32_t, 32> output_r{}, output_g{}, output_b{};
    std::uint16_t output_master = 0U;
    bool output_valid = false;
#endif
};

constexpr std::uint32_t engine_reg(const Engine& e, std::uint32_t offset) {
    return e.regs[offset / 4U];
}

constexpr std::uint16_t engine_half(const Engine& e, std::uint32_t offset) {
    const std::uint32_t value = engine_reg(e, offset & ~3U);
    return static_cast<std::uint16_t>(value >> ((offset & 2U) * 8U));
}


constexpr std::uint16_t read_half(const std::array<std::uint8_t, 0x800U>& bytes,
                                  std::uint32_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}


constexpr Color direct_color(std::uint16_t raw) {
    return {static_cast<std::uint8_t>(raw & 31U), static_cast<std::uint8_t>((raw >> 5U) & 31U),
            static_cast<std::uint8_t>((raw >> 10U) & 31U), 31};
}

constexpr bool in_range(std::uint32_t address, std::uint32_t base, std::uint32_t size) {
    return address >= base && address - base < size;
}

constexpr std::uint32_t bank_offset(std::uint8_t bank, std::uint32_t offset) {
    return static_cast<std::uint32_t>(bank) * static_cast<std::uint32_t>(kBankCapacity) + offset;
}

} // namespace

struct Gpu2D::Impl {
    std::array<std::uint8_t, 9U * kBankCapacity> vram{};
    std::array<std::uint8_t, 0x800U> palettes{};
    std::array<Color, 0x800U / 2U> palette_colors{};
    std::array<std::uint8_t, 0x800U> oam{};
    std::array<std::uint8_t, 9U> vramcnt{}; // Banks A-I; 0x04000247 is WRAMCNT.
    Engine engine_a{};
    Engine engine_b{};
    std::uint16_t powcnt1 = 0;
    Gpu3D* gpu3d = nullptr;
    std::array<VramMap, 40> arm9_maps{};
    std::size_t arm9_map_count = 0;
    std::array<VramMap, 2> arm7_maps{};
    std::size_t arm7_map_count = 0;
    std::array<std::uint8_t*, kPageCount> arm9_pages{};
    std::array<std::uint8_t*, kPageCount> arm7_pages{};
    std::array<std::uint8_t, kPageCount> arm9_page_maps{};
    std::array<std::uint8_t, kPageCount> arm7_page_maps{};
    std::array<std::uint32_t, 4> texture_banks{};
    std::array<std::uint32_t, 6> texture_palette_banks{};
    std::array<std::uint8_t, 4> texture_counts{};
    std::array<std::uint8_t, 6> texture_palette_counts{};
    std::array<std::uint16_t, 4> texture_bank_masks{};
    std::array<std::uint16_t, 6> texture_palette_bank_masks{};
    std::uint64_t mapping_generation = 0;
    std::uint64_t vram_generation = 0;
    std::array<bool, 9> texture_bank{};
    std::array<std::uint8_t, kPixelsPerFrame> three_d_mask_buffer{};
    struct Presentation {
        std::array<std::uint8_t, kPixelsPerFrame> mask;
        std::array<std::uint32_t, kPixelsPerFrame> top;
        std::array<std::uint32_t, kPixelsPerFrame> bottom;
        std::array<Color, kScreenWidth> a_line;
        std::array<Pixel, kScreenWidth> bg0;
    };
    std::unique_ptr<Presentation> presentation;
    bool presentation_full = true;
    unsigned presentation_scale = 1;
    std::array<std::uint8_t, kScreenWidth> unused_mask_line{};
    std::array<ExtPaletteRegion, 16> ext_regions_a{};
    std::array<ExtPaletteRegion, 16> ext_regions_b{};
    std::size_t ext_regions_a_count = 0;
    std::size_t ext_regions_b_count = 0;
    std::array<std::uint32_t, kPixelsPerFrame> screen_top_buffer{};
    std::array<std::uint32_t, kPixelsPerFrame> screen_bottom_buffer{};
    bool presentation_a_line_valid = false;
    std::array<Color, kScreenWidth> engine_a_line{};
    std::array<Color, kScreenWidth> engine_b_line{};
    std::array<Color, kScreenWidth> capture_display_line{};
    // Engines render sequentially into one scratch set. Reusing it keeps the
    // working set small and avoids clearing backgrounds that are disabled.
    std::array<std::array<Pixel, kScreenWidth>, 4> bg_line{};
    std::array<Pixel, kScreenWidth> obj_line{};
    std::array<bool, kScreenWidth> obj_window_buffer{};
    bool obj_buffers_dirty = false; // obj_line / obj_window_buffer differ from their cleared state
    std::array<std::uint8_t, kScreenWidth> window_line_buffer{};
    std::array<std::uint16_t, kScreenWidth> fifo_line{};
    std::array<std::uint16_t, kPixelsPerFrame> fifo{};
    std::size_t fifo_read = 0;
    std::size_t fifo_count = 0;
    bool capture_pending = false;
    bool capture_active = false;

    void add_arm9_map(std::uint8_t bank, std::uint32_t address, std::uint32_t size,
                      std::uint32_t offset = 0U) {
        if (size == 0U || arm9_map_count >= arm9_maps.size()) {
            return;
        }
        arm9_maps[arm9_map_count++] = {address, size, offset, bank};
    }

    void add_arm7_map(std::uint8_t bank, std::uint32_t address, std::uint32_t size,
                      std::uint32_t offset = 0U) {
        if (size == 0U || arm7_map_count >= arm7_maps.size()) {
            return;
        }
        arm7_maps[arm7_map_count++] = {address, size, offset, bank};
    }

    void add_ext_region(bool engine_a_ext, std::uint32_t base, std::uint32_t size,
                        std::uint8_t bank, std::uint32_t offset = 0U, bool object = false) {
        auto& regions = engine_a_ext ? ext_regions_a : ext_regions_b;
        auto& count = engine_a_ext ? ext_regions_a_count : ext_regions_b_count;
        if (count < regions.size()) {
            regions[count++] = {base, size, offset, bank, object};
        }
    }

    void add_texture(int slot, std::uint8_t bank) {
        if (slot < 0 || slot >= static_cast<int>(texture_banks.size())) {
            return;
        }
        auto& count = texture_counts[static_cast<std::size_t>(slot)];
        texture_bank_masks[static_cast<std::size_t>(slot)] |= static_cast<std::uint16_t>(1U << bank);
        if (count == 0U) {
            texture_banks[static_cast<std::size_t>(slot)] = bank;
        }
        if (count < 255U) {
            ++count;
        }
    }

    void add_texture_palette(int slot, std::uint8_t bank, std::uint32_t offset = 0U) {
        if (slot < 0 || slot >= static_cast<int>(texture_palette_banks.size())) {
            return;
        }
        auto& count = texture_palette_counts[static_cast<std::size_t>(slot)];
        texture_palette_bank_masks[static_cast<std::size_t>(slot)] |= static_cast<std::uint16_t>(1U << bank);
        if (count == 0U) {
            texture_palette_banks[static_cast<std::size_t>(slot)] =
                static_cast<std::uint32_t>(bank) * static_cast<std::uint32_t>(kBankCapacity) + offset;
        }
        if (count < 255U) {
            ++count;
        }
    }

    void rebuild_mappings() {
        arm9_map_count = 0;
        arm7_map_count = 0;
        ext_regions_a_count = 0;
        ext_regions_b_count = 0;
        texture_counts.fill(0U);
        texture_palette_counts.fill(0U);
        texture_bank_masks.fill(0U);
        texture_palette_bank_masks.fill(0U);
        texture_bank.fill(false);

        for (std::uint8_t bank = 0; bank < 9U; ++bank) {
            const std::uint8_t control = vramcnt[bank];
            if ((control & 0x80U) == 0U) {
                continue;
            }
            const std::uint8_t mst = static_cast<std::uint8_t>(
                control & ((bank <= 1U || bank >= 7U) ? 3U : 7U));
            const std::uint8_t ofs = static_cast<std::uint8_t>((control >> 3U) & 3U);
            const std::uint32_t size = kBankSizes[bank];
            texture_bank[bank] = bank < 7U && mst == 3U;
            if (mst == 0U) {
                add_arm9_map(bank, kLcdcBases[bank], size);
                continue;
            }
            if (bank <= 1U) {
                switch (mst) {
                case 1U:
                    add_arm9_map(bank, 0x06000000U + static_cast<std::uint32_t>(ofs) * 0x20000U, size);
                    break;
                case 2U:
                    if (bank <= 1U && (ofs & 2U) == 0U) {
                        add_arm9_map(bank, 0x06400000U + static_cast<std::uint32_t>(ofs & 1U) * 0x20000U, size);
                    }
                    break;
                case 3U:
                    add_texture(ofs, bank);
                    break;
                case 4U:
                    if (bank == 0U || bank == 1U) {
                        // Bank A/B cannot map to extended palettes; the value is reserved.
                    }
                    break;
                default:
                    break;
                }
                continue;
            }
            if (bank == 2U || bank == 3U) {
                switch (mst) {
                case 1U:
                    add_arm9_map(bank, 0x06000000U + static_cast<std::uint32_t>(ofs) * 0x20000U, size);
                    break;
                case 2U:
                    if ((ofs & 2U) == 0U) {
                        add_arm7_map(bank, 0x06000000U + static_cast<std::uint32_t>(ofs & 1U) * 0x20000U, size);
                    }
                    break;
                case 3U:
                    add_texture(ofs, bank);
                    break;
                case 4U:
                    add_arm9_map(bank, bank == 2U ? 0x06200000U : 0x06600000U, size);
                    break;
                case 5U:
                    break;
                default:
                    break;
                }
                continue;
            }
            if (bank == 4U) {
                switch (mst) {
                case 1U:
                    add_arm9_map(bank, 0x06000000U, size);
                    break;
                case 2U:
                    add_arm9_map(bank, 0x06400000U, size);
                    break;
                case 3U:
                    for (int slot = 0; slot < 4; ++slot) {
                        add_texture_palette(slot, bank, static_cast<std::uint32_t>(slot) * 0x4000U);
                    }
                    break;
                case 4U:
                    for (int slot = 0; slot < 4; ++slot) {
                        add_ext_region(true, static_cast<std::uint32_t>(slot) * 0x2000U,
                                       0x2000U, bank, static_cast<std::uint32_t>(slot) * 0x2000U);
                    }
                    break;
                default:
                    break;
                }
                continue;
            }
            if (bank == 5U || bank == 6U) {
                const std::uint32_t small_base =
                    static_cast<std::uint32_t>(ofs & 1U) * 0x4000U +
                    static_cast<std::uint32_t>((ofs >> 1U) & 1U) * 0x10000U;
                switch (mst) {
                case 1U:
                    add_arm9_map(bank, 0x06000000U + small_base, size);
                    add_arm9_map(bank, 0x06008000U + small_base, size);
                    break;
                case 2U:
                    add_arm9_map(bank, 0x06400000U + small_base, size);
                    add_arm9_map(bank, 0x06408000U + small_base, size);
                    break;
                case 3U:
                    add_texture_palette(static_cast<int>((ofs & 1U) + ((ofs >> 1U) & 1U) * 4U), bank);
                    break;
                case 4U: {
                    if (ofs >= 2U) {
                        break;
                    }
                    const std::uint32_t first_slot = static_cast<std::uint32_t>(ofs & 1U) * 2U;
                    for (std::uint32_t sub = 0; sub < 2U; ++sub) {
                        add_ext_region(true, (first_slot + sub) * 0x2000U, 0x2000U,
                                       bank, sub * 0x2000U);
                    }
                    break;
                }
                case 5U:
                    add_ext_region(true, 0U, 0x2000U, bank, 0U, true);
                    break;
                default:
                    break;
                }
                continue;
            }
            if (bank == 7U) {
                switch (mst) {
                case 1U:
                    add_arm9_map(bank, 0x06200000U, size);
                    add_arm9_map(bank, 0x06210000U, size);
                    break;
                case 2U:
                    for (std::uint32_t slot = 0; slot < 4U; ++slot) {
                        add_ext_region(false, slot * 0x2000U, 0x2000U, bank, slot * 0x2000U);
                    }
                    break;
                default:
                    break;
                }
                continue;
            }
            if (bank == 8U) {
                switch (mst) {
                case 1U:
                    add_arm9_map(bank, 0x06208000U, size);
                    add_arm9_map(bank, 0x0620C000U, size);
                    add_arm9_map(bank, 0x06218000U, size);
                    add_arm9_map(bank, 0x0621C000U, size);
                    break;
                case 2U:
                    add_arm9_map(bank, 0x06600000U, size);
                    add_arm9_map(bank, 0x06604000U, size);
                    break;
                case 3U:
                    add_ext_region(false, 0U, 0x2000U, bank, 0U, true);
                    break;
                default:
                    break;
                }
            }
        }
        ++mapping_generation;
        rebuild_pages(arm9_maps, arm9_map_count, arm9_page_maps, arm9_pages);
        rebuild_pages(arm7_maps, arm7_map_count, arm7_page_maps, arm7_pages);
        mirror_pages(arm9_page_maps, arm9_pages, false);
        mirror_pages(arm7_page_maps, arm7_pages, true);
    }

    template <std::size_t N>
    void rebuild_pages(const std::array<VramMap, N>& maps, std::size_t count,
                       std::array<std::uint8_t, kPageCount>& page_maps,
                       std::array<std::uint8_t*, kPageCount>& pages) {
        page_maps.fill(0U);
        pages.fill(nullptr);
        for (std::size_t i = 0; i < count; ++i) {
            const VramMap& mapping = maps[i];
            const std::uint32_t first = std::max(mapping.address, kVramBase);
            const std::uint32_t end = std::min(mapping.address + mapping.size, kVramLimit);
            if (first >= end) {
                continue;
            }
            const std::size_t first_page = (first - kVramBase) / kPageSize;
            const std::size_t end_page = (end - kVramBase) / kPageSize;
            for (std::size_t page = first_page; page < end_page; ++page) {
                auto& map_count = page_maps[page];
                if (map_count == 0U) {
                    const std::uint32_t page_address = kVramBase + static_cast<std::uint32_t>(page) * kPageSize;
                    const std::uint32_t offset = mapping.offset + page_address - mapping.address;
                    pages[page] = vram.data() + bank_offset(mapping.bank, offset);
                } else {
                    pages[page] = nullptr;
                }
                if (map_count < 255U) {
                    ++map_count;
                }
            }
        }
    }
    void mirror_pages(std::array<std::uint8_t, kPageCount>& counts,
                      std::array<std::uint8_t*, kPageCount>& pages, bool arm7) {
        const std::uint32_t end = arm7 ? kVramLimit : 0x06800000U;
        for (std::uint32_t address = kVramBase; address < end; address += kPageSize) {
            const auto page = (address - kVramBase) / kPageSize;
            const auto source = (vram_address(address, arm7) - kVramBase) / kPageSize;
            counts[page] = counts[source];
            pages[page] = pages[source];
        }
    }


    std::uint8_t read_mapped_byte(const VramMap* maps, std::size_t count,
                                  std::uint32_t address) const {
        std::uint8_t value = 0U;
        for (std::size_t i = 0; i < count; ++i) {
            const VramMap& mapping = maps[i];
            if (in_range(address, mapping.address, mapping.size)) {
                const std::uint32_t offset = mapping.offset + address - mapping.address;
                value = static_cast<std::uint8_t>(value | vram[bank_offset(mapping.bank, offset)]);
            }
        }
        return value;
    }

    void write_bank_byte(std::uint8_t bank, std::uint32_t offset, std::uint8_t value) {
        vram[bank_offset(bank, offset)] = value;
        vram_generation += static_cast<std::uint64_t>(texture_bank[bank]);
    }

    void write_mapped_byte(VramMap* maps, std::size_t count, std::uint32_t address,
                           std::uint8_t value) {
        for (std::size_t i = 0; i < count; ++i) {
            VramMap& mapping = maps[i];
            if (in_range(address, mapping.address, mapping.size)) {
                const std::uint32_t offset = mapping.offset + address - mapping.address;
                write_bank_byte(mapping.bank, offset, value);
            }
        }
    }

    std::uint8_t read_vram_byte(std::uint32_t address, bool arm7) const {
        address = vram_address(address, arm7);
        if (address >= kVramBase && address < kVramLimit) {
            const std::uint32_t offset = address - kVramBase;
            const std::size_t page = offset / kPageSize;
            const auto* page_base = arm7 ? arm7_pages[page] : arm9_pages[page];
            if (page_base != nullptr) {
                return page_base[offset & (kPageSize - 1U)];
            }
        }
        return arm7 ? read_mapped_byte(arm7_maps.data(), arm7_map_count, address)
                    : read_mapped_byte(arm9_maps.data(), arm9_map_count, address);
    }

    std::uint16_t read_vram_half(std::uint32_t address, bool arm7) const {
        return static_cast<std::uint16_t>(read_vram_byte(address, arm7)) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(read_vram_byte(address + 1U, arm7)) << 8U);
    }

    std::uint8_t read_lcdc_byte(std::uint8_t bank, std::uint32_t offset) const {
        return (bank < 9U && offset < kBankSizes[bank]) ? vram[bank_offset(bank, offset)] : 0U;
    }

    void write_lcdc_half(std::uint8_t bank, std::uint32_t offset, std::uint16_t value) {
        if (bank < 4U && (vramcnt[bank] & 0x87U) == 0x80U) {
            const std::uint32_t target = offset % kBankSizes[bank];
            write_bank_byte(bank, target, static_cast<std::uint8_t>(value));
            write_bank_byte(bank, (target + 1U) % kBankSizes[bank], static_cast<std::uint8_t>(value >> 8U));
        }
    }

    std::uint16_t read_lcdc_half(std::uint8_t bank, std::uint32_t offset) const {
        if (bank >= 4U || (vramcnt[bank] & 0x87U) != 0x80U) {
            return 0U;
        }
        const std::uint32_t target = offset % kBankSizes[bank];
        const std::uint32_t next = (target + 1U) % kBankSizes[bank];
        return static_cast<std::uint16_t>(read_lcdc_byte(bank, target)) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(read_lcdc_byte(bank, next)) << 8U);
    }

    // Whole-line form of read_lcdc_half(bank, y * 512 + x * 2) -> direct_color. Banks 0-3 are
    // 128 KiB, so a 192-line 512-byte stride never wraps and the per-pixel modulo is redundant.
    void fill_lcdc_line(std::array<Color, kScreenWidth>& output, std::uint8_t bank, int y) const {
        static_assert(kBankSizes[0] >= 192U * 512U && kBankSizes[1] >= 192U * 512U &&
                      kBankSizes[2] >= 192U * 512U && kBankSizes[3] >= 192U * 512U);
        if (bank >= 4U || (vramcnt[bank] & 0x87U) != 0x80U) {
            output.fill(direct_color(0U));
            return;
        }
        const std::uint8_t* row = vram.data() + bank_offset(bank, static_cast<std::uint32_t>(y) * 512U);
        for (std::size_t x = 0; x < kScreenWidth; ++x) {
            output[x] = direct_color(static_cast<std::uint16_t>(row[x * 2U] | (row[x * 2U + 1U] << 8U)));
        }
    }

    std::uint16_t read_ext(bool engine_a_ext, bool object, std::uint32_t slot,
                           std::uint32_t offset) const {
        const auto& regions = engine_a_ext ? ext_regions_a : ext_regions_b;
        const std::size_t count = engine_a_ext ? ext_regions_a_count : ext_regions_b_count;
        std::uint8_t low = 0U;
        std::uint8_t high = 0U;
        const std::uint32_t target = object ? 0U : slot * 0x2000U;
        for (std::size_t i = 0; i < count; ++i) {
            const ExtPaletteRegion& region = regions[i];
            if (region.object != object || (!object && region.base != target)) {
                continue;
            }
            if (offset + 1U >= region.size) {
                continue;
            }
            const std::uint32_t bank_off = region.offset + offset;
            const std::uint8_t lo = vram[bank_offset(region.bank, bank_off)];
            const std::uint8_t hi = vram[bank_offset(region.bank, bank_off + 1U)];
            low = static_cast<std::uint8_t>(low | lo);
            high = static_cast<std::uint8_t>(high | hi);
        }
        return static_cast<std::uint16_t>(low) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U);
    }

    // A single extended-palette mapping is common. Resolve it once per layer,
    // retaining the OR-combined slow path for overlapping banks.
    const std::uint8_t* palette_pointer(bool is_a, bool object, int bg) const {
        const auto& regions = is_a ? ext_regions_a : ext_regions_b;
        const auto count = is_a ? ext_regions_a_count : ext_regions_b_count;
        const auto slot = object ? 0U : static_cast<std::uint32_t>(bg_ext_slot(
            is_a ? engine_a : engine_b, bg)) * 0x2000U;
        const std::uint8_t* result = nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& region = regions[i];
            if (region.object == object && (object || region.base == slot)) {
                if (result != nullptr || region.size < 0x2000U) {
                    return nullptr;
                }
                result = vram.data() + bank_offset(region.bank, region.offset);
            }
        }
        return result;
    }

    int bg_ext_slot(const Engine& engine, int bg) const {
        if (bg == 0) {
            return (engine_half(engine, 0x08U) & 0x2000U) != 0U ? 2 : 0;
        }
        if (bg == 1) {
            return (engine_half(engine, 0x0AU) & 0x2000U) != 0U ? 3 : 1;
        }
        return bg;
    }


    static bool window_range(std::uint8_t coordinate, std::uint8_t start, std::uint8_t end) {
        if (start <= end) {
            return coordinate >= start && coordinate < end;
        }
        return coordinate >= start || coordinate < end;
    }

    std::uint8_t window_line(const Engine& engine, std::uint32_t dispcnt, int y) {
        auto& masks = window_line_buffer;
        if ((dispcnt & 0xE000U) == 0U) {
            masks.fill(0x3FU);
            return 0x3FU;
        }
        const auto h0 = engine_half(engine, 0x40U);
        const auto h1 = engine_half(engine, 0x42U);
        const auto v0 = engine_half(engine, 0x44U);
        const auto v1 = engine_half(engine, 0x46U);
        const bool active0 = (dispcnt & (1U << 13U)) != 0U &&
            window_range(static_cast<std::uint8_t>(y), v0 >> 8U, v0);
        const bool active1 = (dispcnt & (1U << 14U)) != 0U &&
            window_range(static_cast<std::uint8_t>(y), v1 >> 8U, v1);
        const auto in = engine_half(engine, 0x48U);
        const auto out = engine_half(engine, 0x4AU);
        const bool object_enabled = (dispcnt & (1U << 15U)) != 0U;
        std::uint8_t visible = 0U;
        for (unsigned x = 0; x < kScreenWidth; ++x) {
            const auto mask = static_cast<std::uint8_t>((active0 && window_range(x, h0 >> 8U, h0)
                ? in : active1 && window_range(x, h1 >> 8U, h1) ? in >> 8U
                : object_enabled && obj_window_buffer[x] ? out >> 8U : out) & 0x3FU);
            masks[x] = mask;
            visible |= mask;
        }
        return visible;
    }

    struct BackgroundVram {
        const Impl& gpu;
        const std::uint8_t* const* pages;
        std::uint32_t base;
        std::uint32_t mask;

        std::uint8_t byte(std::uint32_t address) const {
            address &= mask;
            const auto* page = pages[address / kPageSize];
            return page != nullptr ? page[address & (kPageSize - 1U)]
                : gpu.read_mapped_byte(gpu.arm9_maps.data(), gpu.arm9_map_count, base + address);
        }

        std::uint16_t half(std::uint32_t address) const {
            address &= mask;
            const auto* page = pages[address / kPageSize];
            if (page != nullptr && (address & (kPageSize - 1U)) != kPageSize - 1U) {
                return static_cast<std::uint16_t>(load_le(page + (address & (kPageSize - 1U)), 16));
            }
            return static_cast<std::uint16_t>(byte(address) | (byte(address + 1U) << 8U));
        }

        // Text rows are naturally 4/8-byte aligned, so one row never crosses a
        // page or BG mirror boundary. Preserve bytewise OR reads for overlaps.
        template <bool Color256>
        std::uint64_t text_row(std::uint32_t address) const {
            address &= mask;
            const auto* page = pages[address / kPageSize];
            if (page != nullptr) {
                const auto* row = page + (address & (kPageSize - 1U));
                const auto low = static_cast<std::uint64_t>(load_le(row, 32));
                if constexpr (Color256) {
                    return low | (static_cast<std::uint64_t>(load_le(row + 4U, 32)) << 32U);
                }
                return low;
            }
            std::uint64_t result = 0U;
            for (unsigned i = 0; i < (Color256 ? 8U : 4U); ++i) {
                result |= static_cast<std::uint64_t>(byte(address + i)) << (i * 8U);
            }
            return result;
        }
    };

    static Color indexed_color(const std::uint8_t* palette, std::uint32_t index) {
        return direct_color(static_cast<std::uint16_t>(load_le(palette + index * 2U, 16)));
    }

    template <bool Color256, bool Extended>
    void add_text_bg_line(std::array<Pixel, kScreenWidth>& line, const Engine& engine,
                          bool is_a, int bg, int screen_y, std::uint16_t cnt,
                          std::uint32_t char_base, std::uint32_t screen_base) {
        const unsigned size = cnt >> 14U;
        const unsigned width_mask = (size & 1U) != 0U ? 511U : 255U;
        const unsigned height_mask = (size & 2U) != 0U ? 511U : 255U;
        const unsigned scroll_x = engine_half(engine, 0x10U + bg * 4U) & 0x1FFU;
        const unsigned source_y = (screen_y + engine_half(engine, 0x12U + bg * 4U)) & height_mask;
        const unsigned map_row = screen_base + ((source_y >> 3U) & 31U) * 64U +
            (source_y >> 8U) * ((size & 1U) != 0U ? 0x1000U : 0x800U);
        const auto* palette = Extended ? palette_pointer(is_a, false, bg) : nullptr;
        const auto* regular_palette = palette_colors.data() + (is_a ? 0U : 0x200U);
        const BackgroundVram source{*this, arm9_pages.data() + (is_a ? 0U : 128U),
                                   is_a ? 0x06000000U : 0x06200000U,
                                   is_a ? 0x7FFFFU : 0x1FFFFU};
        constexpr unsigned bits_per_pixel = Color256 ? 8U : 4U;
        constexpr unsigned row_bytes = Color256 ? 8U : 4U;
        constexpr unsigned index_mask = Color256 ? 255U : 15U;
        for (unsigned x = 0; x < kScreenWidth;) {
            const unsigned source_x = (x + scroll_x) & width_mask;
            const unsigned first = source_x & 7U;
            const unsigned count = std::min(8U - first, kScreenWidth - x);
            const auto entry = source.half(map_row + (source_x >> 8U) * 0x800U +
                                          ((source_x >> 3U) & 31U) * 2U);
            const unsigned local_y = (source_y & 7U) ^ ((entry & 0x800U) != 0U ? 7U : 0U);
            const auto row = source.text_row<Color256>(
                char_base + (entry & 0x3FFU) * (row_bytes * 8U) + local_y * row_bytes);
            if (row == 0U) {
                // A fully transparent tile row (every index 0) writes nothing.
                x += count;
                continue;
            }
            const unsigned flip_x = (entry & 0x400U) != 0U ? 7U : 0U;
            const unsigned palette_base = Color256 && !Extended ? 0U
                : (entry >> 12U) * (Color256 ? 256U : 16U);
            for (unsigned i = 0; i < count; ++i) {
                const unsigned local_x = (first + i) ^ flip_x;
                const unsigned index = (row >> (local_x * bits_per_pixel)) & index_mask;
                if (index == 0U) {
                    continue;
                }
                // The line was cleared once before sampling. Write the fields
                // directly, avoiding a partially rebuilt 12-byte Pixel temporary.
                Pixel& pixel = line[x + i];
                if constexpr (Extended) {
                    pixel.color = palette != nullptr ? indexed_color(palette, palette_base + index)
                        : direct_color(read_ext(is_a, false,
                            static_cast<std::uint32_t>(bg_ext_slot(engine, bg)),
                            (palette_base + index) * 2U));
                } else {
                    pixel.color = regular_palette[palette_base + index];
                }
                pixel.priority = static_cast<std::uint8_t>(cnt & 3U);
                pixel.layer = static_cast<std::uint8_t>(bg);
                pixel.opaque = true;
            }
            x += count;
        }
    }

    void add_bg_pixel(std::array<Pixel, kScreenWidth>& line, const Engine& engine,
                      bool is_a, int bg, int screen_y, std::uint32_t dispcnt,
                      const std::uint32_t* line3d, bool full_presentation = false) {
        if ((dispcnt & (1U << (8U + static_cast<unsigned>(bg)))) == 0U) {
            return;
        }
        const std::uint32_t mode = dispcnt & 7U;
        const std::uint16_t cnt = engine_half(engine, 0x08U + static_cast<std::uint32_t>(bg) * 2U);
        const bool is_large = is_a && mode == 6U && bg == 2;
        const bool is_extended = (mode == 3U && bg == 3) ||
                                 (mode == 4U && bg == 3) ||
                                 (mode == 5U && (bg == 2 || bg == 3));
        const bool is_affine = (mode == 1U && bg == 3) ||
                               (mode == 2U && (bg == 2 || bg == 3)) ||
                               (mode == 4U && bg == 2);
        const bool text = !is_large && !is_extended && !is_affine;
        const bool three_d = is_a && bg == 0 && (dispcnt & (1U << 3U)) != 0U;
        if (!three_d && (mode == 7U || (mode == 6U && !is_large))) {
            return;
        }
        if (three_d) {
            if (line3d == nullptr) {
                return;
            }
            // BG0HOFS scrolls the 3D layer horizontally; the 3D frame is 256 wide, so
            // sampling positions past it (of the 512-wide wrap) stay transparent.
            const std::uint32_t hofs = engine_half(engine, 0x10U) & 0x1FFU;
            for (int x = 0; x < static_cast<int>(kScreenWidth); ++x) {
                const std::uint32_t source_x = (static_cast<std::uint32_t>(x) + hofs) & 0x1FFU;
                if (source_x >= kScreenWidth) {
                    continue;
                }
                const std::uint32_t raw = line3d[full_presentation
                    ? full_presentation_source_x(source_x, presentation_scale) : source_x];
                const std::uint8_t alpha = static_cast<std::uint8_t>((raw >> 24U) & 31U);
                if (alpha == 0U) {
                    continue;
                }
                Pixel pixel;
                pixel.rgb6 = {static_cast<std::uint8_t>(raw & 63U),
                              static_cast<std::uint8_t>((raw >> 6U) & 63U),
                              static_cast<std::uint8_t>((raw >> 12U) & 63U)};
                pixel.color = {static_cast<std::uint8_t>(pixel.rgb6[0] >> 1U),
                               static_cast<std::uint8_t>(pixel.rgb6[1] >> 1U),
                               static_cast<std::uint8_t>(pixel.rgb6[2] >> 1U), alpha};
                pixel.priority = static_cast<std::uint8_t>(cnt & 3U);
                pixel.layer = static_cast<std::uint8_t>(bg);
                pixel.opaque = true;
                pixel.three_d = true;
                line[static_cast<std::size_t>(x)] = pixel;
            }
            return;
        }

        const std::uint32_t char_base =
            ((cnt >> 2U) & 0xFU) * 0x4000U +
            (is_a ? ((dispcnt >> 24U) & 7U) * 0x10000U : 0U);
        const std::uint32_t screen_base =
            ((cnt >> 8U) & 0x1FU) * 0x800U +
            (is_a ? ((dispcnt >> 27U) & 7U) * 0x10000U : 0U);
        if (text) {
            if ((cnt & 0x80U) == 0U) {
                add_text_bg_line<false, false>(line, engine, is_a, bg, screen_y, cnt,
                                               char_base, screen_base);
            } else if ((dispcnt & (1U << 30U)) == 0U) {
                add_text_bg_line<true, false>(line, engine, is_a, bg, screen_y, cnt,
                                              char_base, screen_base);
            } else {
                add_text_bg_line<true, true>(line, engine, is_a, bg, screen_y, cnt,
                                             char_base, screen_base);
            }
            return;
        }
        const bool wrap = (cnt & (1U << 13U)) != 0U;
        const std::uint32_t size_code = (cnt >> 14U) & 3U;
        const std::array<int, 4> affine_sizes{128, 256, 512, 1024};
        const std::array<std::array<int, 2>, 4> bitmap_sizes{{{{128, 128}}, {{256, 256}}, {{512, 256}}, {{512, 512}}}};
        const std::array<std::array<int, 2>, 4> large_sizes{{{{512, 1024}}, {{1024, 512}}, {{0, 0}}, {{0, 0}}}};
        const int affine_index = bg == 2 ? 0 : 1;
        const std::uint32_t affine_reg_base = bg == 2 ? 0x20U : 0x30U;
        const std::int16_t pa = static_cast<std::int16_t>(engine_half(engine, affine_reg_base));
        const std::int16_t pc = static_cast<std::int16_t>(engine_half(engine, affine_reg_base + 4U));
        const auto* regular_palette = palette_colors.data() + (is_a ? 0U : 0x200U);
        const auto* extended_palette = palette_pointer(is_a, false, bg);
        const auto palette_lookup = [&](std::uint32_t index, bool extended) {
            if (!extended) {
                return regular_palette[index & 0xFFU];
            }
            return extended_palette != nullptr ? indexed_color(extended_palette, index)
                : direct_color(read_ext(is_a, false,
                    static_cast<std::uint32_t>(bg_ext_slot(engine, bg)), index * 2U));
        };
        const BackgroundVram bg_vram{*this, arm9_pages.data() + (is_a ? 0U : 128U),
                                     is_a ? 0x06000000U : 0x06200000U,
                                     is_a ? 0x7FFFFU : 0x1FFFFU};
        for (int x = 0; x < static_cast<int>(kScreenWidth); ++x) {
            Color color{};
            bool transparent = false;
            const bool bitmap = is_extended && (cnt & (1U << 7U)) != 0U;
            const int width = is_large ? large_sizes[size_code][0]
                : bitmap ? bitmap_sizes[size_code][0] : affine_sizes[size_code];
            const int height = is_large ? large_sizes[size_code][1]
                : bitmap ? bitmap_sizes[size_code][1] : affine_sizes[size_code];
            if (width <= 0 || height <= 0) {
                continue;
            }
            const std::int64_t ref_x = engine.affine_x[static_cast<std::size_t>(affine_index)];
            const std::int64_t ref_y = engine.affine_y[static_cast<std::size_t>(affine_index)];
            const std::int64_t fixed_x = ref_x + static_cast<std::int64_t>(pa) * x;
            const std::int64_t fixed_y = ref_y + static_cast<std::int64_t>(pc) * x;
            int source_x = static_cast<int>(fixed_x >> 8);
            int source_y = static_cast<int>(fixed_y >> 8);
            if (source_x < 0 || source_y < 0 || source_x >= width || source_y >= height) {
                if (!wrap) {
                    continue;
                }
                source_x &= width - 1;
                source_y &= height - 1;
            }
            const std::uint32_t bitmap_base = ((cnt >> 8U) & 0x1FU) * 0x4000U;
            const std::uint32_t map_base = screen_base;
            if (is_large) {
                const std::uint32_t index = static_cast<std::uint32_t>(source_y * width + source_x);
                const std::uint8_t pixel = bg_vram.byte(index);
                transparent = pixel == 0U;
                if (!transparent) {
                    color = palette_lookup(pixel, false);
                }
            } else if (is_extended && (cnt & (1U << 7U)) != 0U) {
                const bool direct = (cnt & (1U << 2U)) != 0U;
                const std::uint32_t index = static_cast<std::uint32_t>(source_y * width + source_x);
                if (direct) {
                    const std::uint16_t raw = bg_vram.half(bitmap_base + index * 2U);
                    transparent = (raw & 0x8000U) == 0U;
                    if (!transparent) {
                        color = direct_color(raw);
                    }
                } else {
                    const std::uint8_t pixel = bg_vram.byte(bitmap_base + index);
                    transparent = pixel == 0U;
                    if (!transparent) {
                        color = palette_lookup(pixel, false);
                    }
                }
            } else if (is_extended) {
                const std::uint32_t map_width = static_cast<std::uint32_t>(width / 8);
                const std::uint32_t map_index = static_cast<std::uint32_t>(source_y / 8) * map_width +
                                                static_cast<std::uint32_t>(source_x / 8);
                const std::uint16_t entry = bg_vram.half(map_base + map_index * 2U);
                const std::uint32_t tile_number = entry & 0x3FFU;
                int tile_x = source_x & 7;
                int tile_y = source_y & 7;
                if ((entry & 0x0400U) != 0U) {
                    tile_x = 7 - tile_x;
                }
                if ((entry & 0x0800U) != 0U) {
                    tile_y = 7 - tile_y;
                }
                const std::uint8_t pixel = bg_vram.byte(char_base + tile_number * 64U +
                    static_cast<std::uint32_t>(tile_y * 8 + tile_x));
                transparent = pixel == 0U;
                if (!transparent) {
                    const bool ext = (dispcnt & (1U << 30U)) != 0U;
                    const std::uint32_t palette_index = ((entry >> 12U) & 0xFU) * 256U + pixel;
                    color = palette_lookup(palette_index, ext);
                }
            } else {
                const std::uint32_t map_width = static_cast<std::uint32_t>(width / 8);
                const std::uint32_t map_index = static_cast<std::uint32_t>(source_y / 8) * map_width +
                                                static_cast<std::uint32_t>(source_x / 8);
                const std::uint8_t tile_number = bg_vram.byte(map_base + map_index);
                const std::uint8_t pixel = bg_vram.byte(char_base + static_cast<std::uint32_t>(tile_number) * 64U +
                    static_cast<std::uint32_t>((source_y & 7) * 8 + (source_x & 7)));
                transparent = pixel == 0U;
                if (!transparent) {
                    color = palette_lookup(pixel, false);
                }
            }
            if (transparent) {
                continue;
            }
            Pixel pixel;
            pixel.color = color;
            pixel.priority = static_cast<std::uint8_t>(cnt & 3U);
            pixel.layer = static_cast<std::uint8_t>(bg);
            pixel.opaque = true;
            line[static_cast<std::size_t>(x)] = pixel;
        }
    }

    std::uint8_t read_obj_byte(std::uint32_t offset, bool is_a) const {
        const std::uint32_t base = is_a ? 0x06400000U : 0x06600000U;
        return read_vram_byte(base + (offset % 0x40000U), false);
    }

    std::uint16_t read_obj_half(std::uint32_t offset, bool is_a) const {
        const std::uint32_t base = is_a ? 0x06400000U : 0x06600000U;
        return read_vram_half(base + (offset % 0x40000U), false);
    }

    void add_obj_pixels(std::array<Pixel, kScreenWidth>& line, std::array<bool, kScreenWidth>& obj_window,
                        const Engine& engine, bool is_a, int screen_y, std::uint32_t dispcnt) {
        if ((dispcnt & (1U << 12U)) == 0U) {
            return;
        }
        const std::uint32_t mapping = dispcnt;
        const std::uint32_t mosaic = engine_reg(engine, 0x4CU);
        const int mosaic_h = static_cast<int>(((mosaic >> 8U) & 0xFU) + 1U);
        const int mosaic_v = static_cast<int>(((mosaic >> 12U) & 0xFU) + 1U);
        constexpr std::array<std::array<std::array<int, 2>, 4>, 3> sizes{{
            {{{8, 8}, {16, 16}, {32, 32}, {64, 64}}},
            {{{16, 8}, {32, 8}, {32, 16}, {64, 32}}},
            {{{8, 16}, {8, 32}, {16, 32}, {32, 64}}}}};
        const std::uint32_t object_oam_base = is_a ? 0U : 0x400U;
        const auto* regular_palette = palette_colors.data() + (is_a ? 0x100U : 0x300U);
        const auto* extended_palette = palette_pointer(is_a, true, 0);
        for (int object = 0; object < 128; ++object) {
            const std::uint32_t attr_base = object_oam_base +
                static_cast<std::uint32_t>(object) * 8U;
            const std::uint16_t attr0 = read_half(oam, attr_base);
            const std::uint16_t attr1 = read_half(oam, attr_base + 2U);
            const std::uint16_t attr2 = read_half(oam, attr_base + 4U);
            const int mode = (attr0 >> 10U) & 3U;
            const bool affine = (attr0 & (1U << 8U)) != 0U;
            const bool double_size = affine && (attr0 & (1U << 9U)) != 0U;
            if ((!affine && (attr0 & (1U << 9U)) != 0U) ||
                (mode == 3 && (attr2 & 0xF000U) == 0U)) {
                continue;
            }
            const int shape = (attr0 >> 14U) & 3U;
            if (shape == 3) {
                continue;
            }
            const int size_code = (attr1 >> 14U) & 3U;
            const int width = sizes[static_cast<std::size_t>(shape)][static_cast<std::size_t>(size_code)][0];
            const int height = sizes[static_cast<std::size_t>(shape)][static_cast<std::size_t>(size_code)][1];
            const int bound_width = double_size ? width * 2 : width;
            const int bound_height = double_size ? height * 2 : height;
            int object_x = attr1 & 0x1FFU;
            int object_y = attr0 & 0xFFU;
            if (object_x >= 256) {
                object_x -= 512;
            }
            if (object_y >= 192) {
                object_y -= 256;
            }
            // Most objects miss this line; reject them before the affine matrix reads and the
            // bitmap/tile setup below. Same wrapped-row test as the per-row loop.
            if (!((screen_y - object_y >= 0 && screen_y - object_y < bound_height) ||
                  (screen_y - 256 - object_y >= 0 && screen_y - 256 - object_y < bound_height) ||
                  (screen_y + 256 - object_y >= 0 && screen_y + 256 - object_y < bound_height))) {
                continue;
            }
            const bool bitmap = mode == 3;
            const bool color256 = (attr0 & (1U << 13U)) != 0U;
            const std::uint32_t tile_number = attr2 & 0x3FFU;
            const int tiles_wide = width / 8;
            const std::uint32_t character_number = tile_number;
            const bool bitmap_1d = (mapping & (1U << 6U)) != 0U;
            const std::uint32_t bitmap_1d_boundary =
                is_a && (mapping & (1U << 22U)) != 0U ? 256U : 128U;
            const int bitmap_width = (mapping & (1U << 5U)) != 0U ? 256 : 128;
            const std::uint32_t bitmap_mask_x =
                static_cast<std::uint32_t>(bitmap_width / 8 - 1);
            const std::uint32_t extended_boundary = 32U << ((mapping >> 20U) & 3U);
            std::int16_t pa = 0;
            std::int16_t pb = 0;
            std::int16_t pc = 0;
            std::int16_t pd = 0;
            if (affine) {
                const std::uint32_t matrix =
                    static_cast<std::uint32_t>((attr1 >> 9U) & 0x1FU) * 32U + 6U;
                pa = static_cast<std::int16_t>(read_half(oam, object_oam_base + matrix));
                pb = static_cast<std::int16_t>(read_half(oam, object_oam_base + matrix + 8U));
                pc = static_cast<std::int16_t>(read_half(oam, object_oam_base + matrix + 16U));
                pd = static_cast<std::int16_t>(read_half(oam, object_oam_base + matrix + 24U));
            }
            for (int wrapped_y : {screen_y, screen_y - 256, screen_y + 256}) {
                const int dest_y = wrapped_y - object_y;
                if (dest_y < 0 || dest_y >= bound_height) {
                    continue;
                }
                for (int dest_x = 0; dest_x < bound_width; ++dest_x) {
                    const int screen_x = object_x + dest_x;
                    if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) {
                        continue;
                    }
                    int source_x = dest_x;
                    int source_y = dest_y;
                    if (affine) {
                        const std::int64_t dx =
                            static_cast<std::int64_t>(dest_x - bound_width / 2) * pa +
                            static_cast<std::int64_t>(dest_y - bound_height / 2) * pb;
                        const std::int64_t dy =
                            static_cast<std::int64_t>(dest_x - bound_width / 2) * pc +
                            static_cast<std::int64_t>(dest_y - bound_height / 2) * pd;
                        source_x = static_cast<int>((dx >> 8) + width / 2);
                        source_y = static_cast<int>((dy >> 8) + height / 2);
                    } else {
                        if ((attr1 & (1U << 12U)) != 0U) {
                            source_x = width - 1 - source_x;
                        }
                        if ((attr1 & (1U << 13U)) != 0U) {
                            source_y = height - 1 - source_y;
                        }
                    }
                    if (source_x < 0 || source_y < 0 || source_x >= width || source_y >= height) {
                        continue;
                    }
                    const int tile_x = source_x / 8;
                    const int tile_y = source_y / 8;
                    std::uint32_t address = 0U;
                    if (bitmap) {
                        if (bitmap_1d) {
                            address = tile_number * bitmap_1d_boundary +
                                static_cast<std::uint32_t>((source_y * width + source_x) * 2);
                        } else {
                            address = (tile_number & bitmap_mask_x) * 16U +
                                (tile_number & ~bitmap_mask_x) * 128U +
                                static_cast<std::uint32_t>((source_y * bitmap_width + source_x) * 2);
                        }
                    } else if ((mapping & (1U << 4U)) != 0U) {
                        address = character_number * extended_boundary +
                            static_cast<std::uint32_t>(tile_y * tiles_wide) * (color256 ? 64U : 32U) +
                            static_cast<std::uint32_t>(tile_x) * (color256 ? 64U : 32U) +
                            static_cast<std::uint32_t>((source_y & 7) * (color256 ? 8 : 4) +
                                (color256 ? source_x & 7 : (source_x & 7) / 2));
                    } else {
                        address = character_number * 32U + static_cast<std::uint32_t>(tile_y) * 1024U +
                            static_cast<std::uint32_t>(tile_x) * (color256 ? 64U : 32U) +
                            static_cast<std::uint32_t>((source_y & 7) * (color256 ? 8 : 4) +
                                (color256 ? source_x & 7 : (source_x & 7) / 2));
                    }
                    Color color{};
                    bool transparent = false;
                    bool semitransparent = mode == 1;
                    std::uint8_t object_alpha = 31U;
                    if (bitmap) {
                        const std::uint16_t raw = read_obj_half(address, is_a);
                        transparent = (raw & 0x8000U) == 0U;
                        if (!transparent) {
                            color = direct_color(raw);
                            const std::uint32_t alpha = (attr2 >> 12U) & 0xFU;
                            semitransparent = true;
                            object_alpha = static_cast<std::uint8_t>(alpha + 1U);
                        }
                    } else {
                        std::uint8_t color_index = read_obj_byte(address, is_a);
                        if (!color256 && (source_x & 1) != 0) {
                            color_index = static_cast<std::uint8_t>(color_index >> 4U);
                        } else if (!color256) {
                            color_index = static_cast<std::uint8_t>(color_index & 0xFU);
                        }
                        transparent = color_index == 0U;
                        if (!transparent && mode == 2) {
                            obj_window[static_cast<std::size_t>(screen_x)] = true;
                            continue;
                        }
                        if (!transparent) {
                            const bool ext = color256 && ((dispcnt & (1U << 31U)) != 0U);
                            const std::uint32_t palette_index = color256
                                ? (((attr2 >> 12U) & 0xFU) * 256U + color_index)
                                : (((attr2 >> 12U) & 0xFU) * 16U + color_index);
                            color = !ext ? regular_palette[palette_index & 0xFFU]
                                : extended_palette != nullptr ? indexed_color(extended_palette, palette_index)
                                : direct_color(read_ext(is_a, true, 0U, palette_index * 2U));
                        }
                    }
                    if (transparent) {
                        continue;
                    }
                    color.alpha = object_alpha;
                    const std::size_t x_index = static_cast<std::size_t>(screen_x);
                    const std::uint8_t priority = static_cast<std::uint8_t>((attr2 >> 10U) & 3U);
                    Pixel& existing = line[x_index];
                    const int new_key = static_cast<int>(priority) * 128 + object;
                    const int old_key = existing.opaque
                        ? static_cast<int>(existing.priority) * 128 + (existing.layer & 0x7FU)
                        : std::numeric_limits<int>::max();
                    if (!existing.opaque || new_key < old_key) {
                        existing.color = color;
                        existing.priority = priority;
                        existing.layer = static_cast<std::uint8_t>(0x80U | static_cast<unsigned>(object));
                        existing.opaque = true;
                        existing.semi = semitransparent;
                    }
                }
            }
        }
        // Mosaic is applied to the winning OBJ in screen space, not to sprite-local
        // coordinates (which would shift the mosaic grid for positioned/affine OBJ).
        for (std::size_t x = 0; x < kScreenWidth; ++x) {
            Pixel& pixel = line[x];
            if (!pixel.opaque) {
                continue;
            }
            const auto attr = read_half(oam, object_oam_base + (pixel.layer & 0x7FU) * 8U);
            if ((attr & (1U << 12U)) == 0U) {
                continue;
            }
            Engine& state = is_a ? engine_a : engine_b;
            const auto anchor = x / mosaic_h * mosaic_h;
            if (screen_y % mosaic_v != 0 || x != anchor) {
                const Pixel& cached = state.obj_mosaic[anchor];
                pixel.color = cached.color;
                pixel.opaque = cached.opaque;
                pixel.semi = cached.semi;
            }
            state.obj_mosaic[x] = pixel;
        }
    }


    Color render_engine_line(Engine& engine, bool is_a, int y,
                             std::array<Color, kScreenWidth>& output) {
        const std::uint32_t dispcnt = engine_reg(engine, 0U);
        // POWCNT1 bit 1 powers engine A, bit 9 engine B (GBATEK DS Power Control).
        const bool engine_enabled = (powcnt1 & (1U << (is_a ? 1U : 9U))) != 0U;
        const std::uint32_t display_mode = (dispcnt >> 16U) & 3U;
        const bool needs_capture = is_a && capture_active;
        auto* mask_line = is_a ? three_d_mask_buffer.data() + y * kScreenWidth
                              : unused_mask_line.data();
        const bool normal_display = engine_enabled && (dispcnt & (1U << 7U)) == 0U &&
                                    display_mode == 1U;
        const auto master = engine_half(engine, 0x6CU);
        const auto master_mode = (master >> 14U) & 3U;
        const bool master_changes_color = (master_mode == 1U || master_mode == 2U) &&
                                          (master & 31U) != 0U;
        // The GPU overlay is unscrolled and has no per-line 2D brightness pass.
        const bool raw_three_d_display = normal_display && !master_changes_color &&
                                         (engine_half(engine, 0x10U) & 0x1FFU) == 0U;
        if (!normal_display) {
            std::fill_n(mask_line, kScreenWidth, 0U);
        }
        if ((!engine_enabled || (dispcnt & (1U << 7U)) != 0U) && !needs_capture) {
            output.fill({31U, 31U, 31U, 31U});
            return output[0];
        }
        if (!needs_capture && (display_mode == 0U || (!is_a && (display_mode & 1U) == 0U))) {
            output.fill({31U, 31U, 31U, 31U});
            return output[0];
        }
        if (!needs_capture && is_a && display_mode >= 2U) {
            const auto bank = static_cast<std::uint8_t>((dispcnt >> 18U) & 3U);
            if (display_mode == 2U) {
                fill_lcdc_line(output, bank, y);
            } else {
                for (std::size_t x = 0; x < kScreenWidth; ++x) {
                    output[x] = direct_color(next_fifo_pixel());
                }
            }
            return output[0];
        }

        auto& bg_pixels = bg_line;
        auto& obj_pixels = obj_line;
        auto& obj_window = obj_window_buffer;
        // add_obj_pixels returns before touching either buffer while OBJ is disabled, so a
        // buffer that is already clean needs no per-line clear.
        const bool objects_enabled = (dispcnt & (1U << 12U)) != 0U;
        if (obj_buffers_dirty) {
            obj_pixels.fill(kTransparentPixel);
            obj_window.fill(false);
        }
        obj_buffers_dirty = objects_enabled;
        add_obj_pixels(obj_pixels, obj_window, engine, is_a, y, dispcnt);
        // Decode the window registers once for the line, not for every layer.
        auto& masks = window_line_buffer;
        const std::uint8_t visible_layers = window_line(engine, dispcnt, y);
        auto* enhanced_bg0 = presentation ? &presentation->bg0 : nullptr;
        const std::uint32_t* presentation_line3d = nullptr;
        const std::uint32_t* line3d = nullptr;
        if (is_a && gpu3d != nullptr && (powcnt1 & (1U << 2U)) != 0U) {
            line3d = gpu3d->line(y);
            if (normal_display && presentation && (dispcnt & (1U << 3U)) != 0U) {
                presentation_line3d = gpu3d->enhanced_line(y);
                if (!presentation_line3d) presentation_line3d = line3d;
            }
        }
        for (int bg = 0; bg < 4; ++bg) {
            if ((dispcnt & (1U << (8U + static_cast<unsigned>(bg)))) == 0U) {
                // Preserve the old disabled-layer mosaic history (transparent on
                // anchor lines, retained between them).
                if ((engine_half(engine, 0x08U + bg * 2U) & (1U << 6U)) == 0U) {
                    continue;
                }
            }
            bg_pixels[bg].fill(kTransparentPixel);
            const bool mosaic_bg = (engine_half(engine, 0x08U + bg * 2U) & (1U << 6U)) != 0U;
            if ((visible_layers & (1U << bg)) == 0U && !mosaic_bg) {
                continue;
            }
            add_bg_pixel(bg_pixels[static_cast<std::size_t>(bg)], engine, is_a, bg, y,
                         dispcnt, line3d);
            if ((engine_half(engine, 0x08U + bg * 2U) & (1U << 6U)) != 0U &&
                !(is_a && bg == 0 && (dispcnt & (1U << 3U)) != 0U)) {
                const auto mosaic = engine_reg(engine, 0x4CU);
                const auto width = (mosaic & 15U) + 1U;
                const auto height = ((mosaic >> 4U) & 15U) + 1U;
                auto& pixels = bg_pixels[bg];
                auto& cached = engine.bg_mosaic[bg];
                for (std::size_t x = 0; x < kScreenWidth; ++x) {
                    const auto anchor = x / width * width;
                    if (y % height != 0 || x != anchor) {
                        pixels[x] = cached[anchor];
                    }
                    cached[x] = pixels[x];
                }
            }
        }
        if (presentation_line3d != nullptr) {
            enhanced_bg0->fill(kTransparentPixel);
            add_bg_pixel(*enhanced_bg0, engine, is_a, 0, y, dispcnt, presentation_line3d, presentation_full);
            presentation_a_line_valid = true;
        }
        std::array<std::array<std::uint8_t, 4>, 4> bg_order{};
        std::array<std::uint8_t, 4> bg_order_count{};
        for (int bg = 3; bg >= 0; --bg) {
            if ((dispcnt & (1U << (8U + static_cast<unsigned>(bg)))) == 0U) {
                continue;
            }
            const std::uint32_t cnt_offset = 0x08U + static_cast<std::uint32_t>(bg) * 2U;
            const std::uint32_t priority = engine_half(engine, cnt_offset) & 3U;
            bg_order[priority][bg_order_count[priority]++] = static_cast<std::uint8_t>(bg);
        }
        const std::uint16_t blend_control = engine_half(engine, 0x50U);
        const std::uint16_t blend_alpha = engine_half(engine, 0x52U);
        const std::uint32_t effect = (blend_control >> 6U) & 3U;
        const std::uint32_t target1 = blend_control & 0x3FU;
        const std::uint32_t target2 = (blend_control >> 8U) & 0x3FU;
        const std::uint32_t eva = blend_alpha & 31U;
        const std::uint32_t evb = (blend_alpha >> 8U) & 31U;
        const std::uint32_t evy = clamp_factor(engine_half(engine, 0x54U) & 31U);
        // A brightness effect on the backdrop applies to the backdrop colour itself, which
        // is then also what blending sees underneath.
        Color backdrop = palette_colors[is_a ? 0U : 0x200U];
        switch (cleanroom::ordinary_backdrop_effect(effect, target1)) {
        case 2U: backdrop = cleanroom::ordinary_brighten(backdrop, evy); break;
        case 3U: backdrop = cleanroom::ordinary_darken(backdrop, evy); break;
        default: break;
        }
        // Layer ids in the compositing order below: 0-3 backgrounds, 4 objects, 5 backdrop.
        constexpr std::uint8_t kObjLayer = 4U;
        constexpr std::uint8_t kBackdropLayer = 5U;
        // No second targets means even semi-transparent OBJ and 3D cannot
        // force blending. Select the first opaque layer, rather than painting
        // all the occluded layers beneath it.
        const bool copy_only = cleanroom::ordinary_copy_only(effect, target1, target2);
        std::array<std::uint8_t, 4> front_order{};
        unsigned front_count = 0U;
        for (unsigned priority = 0; priority < 4; ++priority) {
            for (unsigned i = bg_order_count[priority]; i > 0; --i) {
                front_order[front_count++] = bg_order[priority][i - 1U];
            }
        }
        const auto compose = [&](auto presentation) {
        auto& composed_output = presentation ? this->presentation->a_line : output;
        auto* composed_mask = presentation
            ? this->presentation->mask.data() + y * kScreenWidth : mask_line;
        if (copy_only) {
            std::array<const Pixel*, 4> front_pixels{};
            std::array<std::uint8_t, 4> front_bits{};
            for (unsigned i = 0; i < front_count; ++i) {
                front_pixels[i] = presentation && front_order[i] == 0
                    ? enhanced_bg0->data() : bg_pixels[front_order[i]].data();
                front_bits[i] = static_cast<std::uint8_t>(1U << front_order[i]);
            }
            const Pixel* const object_pixels = obj_pixels.data();
            // Without windows every layer is visible everywhere (masks are all 0x3F), and with
            // OBJ disabled the object line is clear; both tests can be compiled away.
            const auto compose_line = [&](auto windowed, auto with_objects) {
                for (std::size_t xi = 0; xi < kScreenWidth; ++xi) {
                    const std::uint8_t mask = windowed ? masks[xi] : static_cast<std::uint8_t>(0x3FU);
                    const Pixel& object = object_pixels[xi];
                    const bool has_object = with_objects && object.opaque && (mask & (1U << 4U)) != 0U;
                    const Pixel* chosen_source = has_object ? &object : nullptr;
                    for (unsigned i = 0; i < front_count; ++i) {
                        const Pixel& pixel = front_pixels[i][xi];
                        if (pixel.opaque && (!windowed || (mask & front_bits[i]) != 0U)) {
                            chosen_source = has_object && object.priority <= pixel.priority ? &object : &pixel;
                            break;
                        }
                    }
                    composed_output[xi] = chosen_source != nullptr ? chosen_source->color : backdrop;
                    composed_mask[xi] = static_cast<std::uint8_t>(raw_three_d_display && chosen_source != nullptr &&
                        chosen_source->three_d && chosen_source->color.alpha == 31U);
                }
            };
            const bool windowed = (dispcnt & 0xE000U) != 0U;
            using Yes = std::true_type;
            using No = std::false_type;
            if (windowed) {
                if (objects_enabled) compose_line(Yes{}, Yes{}); else compose_line(Yes{}, No{});
            } else {
                if (objects_enabled) compose_line(No{}, Yes{}); else compose_line(No{}, No{});
            }
        }
        // The copy-only case above has already produced the whole line.
        const int blended_end = copy_only ? 0 : static_cast<int>(kScreenWidth);
        std::array<gpu2d_detail::CompositionOrder, 5> composition_orders{};
        if (!copy_only) {
            std::array<std::uint8_t, 4> priorities{};
            for (unsigned bg = 0; bg < 4; ++bg) {
                priorities[bg] = static_cast<std::uint8_t>(engine_half(engine, 0x08U + bg * 2U) & 3U);
            }
            for (unsigned priority = 0; priority < composition_orders.size(); ++priority) {
                composition_orders[priority] =
                    gpu2d_detail::composition_order((dispcnt >> 8U) & 15U, priorities, priority);
            }
        }
        for (int x = 0; x < blended_end; ++x) {
            const auto xi = static_cast<std::size_t>(x);
            const std::uint8_t mask = masks[xi];
            const bool effects_allowed = (mask & (1U << 5U)) != 0U;
            Color dst = backdrop;
            std::uint8_t dst_layer = kBackdropLayer;
            bool dst_three_d = false;
            // Layers are painted from the lowest priority up; each pixel is composited over
            // whatever the painted layers below left in `dst`.
            const auto composite = [&](const Pixel& source, std::uint8_t layer) {
                const bool dst_is_target = dst_layer != layer && (target2 & (1U << dst_layer)) != 0U;
                // The 3D forced-blend rule remains separate from ordinary 2D selection.
                const bool forced_3d = source.three_d && dst_is_target;
                const unsigned selected = forced_3d ? 1U : cleanroom::ordinary_effect(
                    layer, dst_layer, !source.three_d && source.semi, effects_allowed,
                    effect, target1, target2);
                const bool blends = selected == 1U;
                const unsigned brightness = selected >= 2U ? selected : 0U;
                if (blends) {
                    if (source.three_d) {
                        const auto blended = cleanroom::blend_3d(
                            source.rgb6, source.color.alpha, {dst.r, dst.g, dst.b});
                        dst = {blended[0], blended[1], blended[2], 31U};
                    } else if (source.semi && source.color.alpha != 31U) {
                        dst = cleanroom::ordinary_blend(source.color, dst, source.color.alpha, 16U - source.color.alpha);
                    } else {
                        dst = cleanroom::ordinary_blend(source.color, dst, eva, evb);
                    }
                } else if (brightness == 2U) {
                    dst = cleanroom::ordinary_brighten(source.color, evy);
                } else if (brightness == 3U) {
                    dst = cleanroom::ordinary_darken(source.color, evy);
                } else {
                    dst = source.color;
                }
                dst_layer = layer;
                dst_three_d = source.three_d && source.color.alpha == 31U && !blends &&
                              (brightness == 0U || evy == 0U);
            };
            const Pixel& object = obj_pixels[xi];
            const bool object_visible = object.opaque && (mask & (1U << 4U)) != 0U;
            const auto& order = composition_orders[object_visible ? object.priority : 4U];
            for (unsigned i = 0; i < order.count; ++i) {
                const auto layer = order.layers[i];
                const Pixel& pixel = layer == kObjLayer ? object :
                    (presentation && layer == 0 ? (*enhanced_bg0)[xi] : bg_pixels[layer][xi]);
                if (pixel.opaque && (mask & (1U << layer)) != 0U) {
                    composite(pixel, layer);
                }
            }
            composed_output[xi] = dst;
            composed_mask[xi] = static_cast<std::uint8_t>(dst_three_d && raw_three_d_display);
        }
        };
        compose(std::false_type{});
        if (presentation_line3d != nullptr) compose(std::true_type{});
        if (is_a) {
            capture_display_line = output;
        }
        if (display_mode == 0U || (!is_a && (display_mode & 1U) == 0U) ||
            (dispcnt & (1U << 7U)) != 0U || !engine_enabled) {
            output.fill({31U, 31U, 31U, 31U});
        } else if (is_a && display_mode >= 2U) {
            const auto bank = static_cast<std::uint8_t>((dispcnt >> 18U) & 3U);
            if (display_mode == 2U) {
                fill_lcdc_line(output, bank, y);
            } else {
                for (std::size_t x = 0; x < kScreenWidth; ++x) {
                    output[x] = direct_color(next_fifo_pixel());
                }
            }
        }
        return output[0];
    }

    std::uint16_t next_fifo_pixel() {
        if (fifo_count == 0U) {
            return 0U;
        }
        const std::uint16_t pixel = fifo[fifo_read];
        fifo_read = (fifo_read + 1U) % fifo.size();
        --fifo_count;
        return pixel;
    }

#if defined(DSR_X86_PACK_LINE)

    template<unsigned Mode>
    static void pack_line_x86(const std::array<Color, kScreenWidth>& line,
                              std::uint32_t* output, std::uint8_t brightness) {
        static_assert(sizeof(Color) == 4U);
        const auto zero = _mm_setzero_si128();
        const auto factor = _mm_set1_epi16(brightness);
        const auto rb_mask = _mm_set1_epi32(0x00FF00FF);
        const auto g_mask = _mm_set1_epi32(0x0000FF00);
        const auto alpha = _mm_set1_epi32(-16777216);
        for (std::size_t x = 0; x < kScreenWidth; x += 4U) {
            const auto colors = _mm_loadu_si128(reinterpret_cast<const __m128i*>(line.data() + x));
            const auto lo = cleanroom::expand_output_channels<Mode>(_mm_unpacklo_epi8(colors, zero), factor);
            const auto hi = cleanroom::expand_output_channels<Mode>(_mm_unpackhi_epi8(colors, zero), factor);
            const auto bytes = _mm_packus_epi16(lo, hi);
            // Color bytes are RGBA; little-endian host ARGB needs BGRA.
            const auto rb = _mm_and_si128(_mm_or_si128(
                _mm_slli_epi32(bytes, 16), _mm_srli_epi32(bytes, 16)), rb_mask);
            const auto packed = _mm_or_si128(alpha,
                _mm_or_si128(rb, _mm_and_si128(bytes, g_mask)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(output + x), packed);
        }
    }
#endif

    // Dispatch brightness once per line; portable targets cache exact RGB555
    // expansion tables instead of repeating brightness arithmetic per pixel.
    static void pack_line(Engine& engine, const std::array<Color, kScreenWidth>& line,
                          std::uint32_t* output, std::uint16_t master) {
#if defined(DSR_X86_PACK_LINE)
        (void)engine;
        const auto factor = clamp_factor(master & 31U);
        switch ((master >> 14U) & 3U) {
        case 1U: pack_line_x86<1U>(line, output, factor); break;
        case 2U: pack_line_x86<2U>(line, output, factor); break;
        default: pack_line_x86<0U>(line, output, factor); break;
        }
#else
        if (!engine.output_valid || engine.output_master != master) {
            const auto mode = (master >> 14U) & 3U;
            const auto factor = clamp_factor(master & 31U);
            for (unsigned i = 0; i < 32; ++i) {
                Color color{static_cast<std::uint8_t>(i), 0U, 0U, 31U};
                if (mode == 1U) {
                    color = cleanroom::ordinary_brighten(color, factor);
                } else if (mode == 2U) {
                    color = cleanroom::ordinary_darken(color, factor);
                }
                const auto expanded = cleanroom::color5_to_8(color.r);
                engine.output_r[i] = 0xFF000000U | (expanded << 16U);
                engine.output_g[i] = expanded << 8U;
                engine.output_b[i] = expanded;
            }
            engine.output_master = master;
            engine.output_valid = true;
        }
        for (std::size_t x = 0; x < kScreenWidth; ++x) {
            const auto color = line[x];
            output[x] = engine.output_r[color.r] | engine.output_g[color.g] | engine.output_b[color.b];
        }
#endif
    }

    void capture_line(int y) {
        const std::uint32_t control = engine_reg(engine_a, 0x64U);
        if (!capture_active || (control & 0x80000000U) == 0U) {
            return;
        }
        const std::uint32_t size = (control >> 20U) & 3U;
        const std::array<std::uint32_t, 4> heights{128U, 64U, 128U, 192U};
        const std::uint32_t height = heights[size];
        const std::uint32_t width = size == 0U ? 128U : 256U;
        if (static_cast<std::uint32_t>(y) >= height) {
            return;
        }
        const std::uint8_t dst_bank = static_cast<std::uint8_t>((control >> 16U) & 3U);
        if ((vramcnt[dst_bank] & 0x87U) != 0x80U) {
            return;
        }
        const std::uint32_t source_mode = (control >> 29U) & 3U;
        const bool source_a_3d = (control & (1U << 24U)) != 0U;
        const bool source_b_fifo = (control & (1U << 25U)) != 0U;
        const std::uint32_t read_bank = (engine_reg(engine_a, 0U) >> 18U) & 3U;
        const std::uint32_t read_offset = (control >> 26U) & 3U;
        const std::uint32_t write_offset = (control >> 18U) & 3U;
        const std::uint32_t eva = control & 31U;
        const std::uint32_t evb = (control >> 8U) & 31U;
        const std::uint32_t* line3d = nullptr;
        if (source_a_3d && gpu3d != nullptr) {
            line3d = gpu3d->line(y);
        }
        if (source_b_fifo && source_mode != 0U) {
            // Capture drains a whole FIFO scanline, even for a 128-pixel capture.
            for (auto& pixel : fifo_line) {
                pixel = next_fifo_pixel();
            }
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            // Sources are 15-bit colours plus an opaque flag; the display line is always
            // opaque, a 3D pixel is opaque when its alpha is non-zero (partial alpha is
            // not weighed into the capture).
            Color source_a = capture_display_line[x];
            bool a_opaque = true;
            if (source_a_3d) {
                const std::uint32_t raw = line3d != nullptr ? line3d[x] : 0U;
                source_a = {static_cast<std::uint8_t>((raw & 63U) >> 1U),
                            static_cast<std::uint8_t>(((raw >> 6U) & 63U) >> 1U),
                            static_cast<std::uint8_t>(((raw >> 12U) & 63U) >> 1U), 31U};
                a_opaque = ((raw >> 24U) & 31U) != 0U;
            }
            Color source_b{};
            bool b_opaque = true;
            if (source_b_fifo) {
                source_b = direct_color(fifo_line[x]);
                b_opaque = (fifo_line[x] & 0x8000U) != 0U;
            } else {
                const bool vram_display = ((engine_reg(engine_a, 0U) >> 16U) & 3U) == 2U;
                const std::uint32_t source_offset = ((vram_display ? 0U : read_offset) * 0x8000U +
                    static_cast<std::uint32_t>(y) * 512U + x * 2U) % 0x20000U;
                const std::uint16_t raw = read_lcdc_half(static_cast<std::uint8_t>(read_bank), source_offset);
                source_b = direct_color(raw);
                b_opaque = (raw & 0x8000U) != 0U;
            }
            Color captured = source_a;
            bool alpha = a_opaque;
            if (source_mode == 1U) {
                captured = source_b;
                alpha = source_b_fifo ? b_opaque : true;
            } else if (source_mode >= 2U) {
                const std::uint32_t weight_a = a_opaque ? std::min(eva, 16U) : 0U;
                const std::uint32_t weight_b = b_opaque ? std::min(evb, 16U) : 0U;
                const auto channel = [weight_a, weight_b](std::uint8_t first, std::uint8_t second) {
                    return static_cast<std::uint8_t>(std::min((static_cast<std::uint32_t>(first) * weight_a +
                        static_cast<std::uint32_t>(second) * weight_b) >> 4U, 31U));
                };
                captured = {channel(source_a.r, source_b.r), channel(source_a.g, source_b.g),
                            channel(source_a.b, source_b.b), 31U};
                alpha = a_opaque || b_opaque;
            }
            const std::uint16_t raw = static_cast<std::uint16_t>(captured.r |
                (captured.g << 5U) | (captured.b << 10U) | (alpha ? 0x8000U : 0U));
            const std::uint32_t target = write_offset * 0x8000U +
                static_cast<std::uint32_t>(y) * width * 2U + x * 2U;
            write_lcdc_half(dst_bank, target, raw);
        }
    }

    void advance_affine(Engine& engine, int y) {
        if (y == 0 || engine.last_affine_line < 0 || y <= engine.last_affine_line) {
            engine.affine_x[0] = sign_extend_28(engine_reg(engine, 0x28U));
            engine.affine_y[0] = sign_extend_28(engine_reg(engine, 0x2CU));
            engine.affine_x[1] = sign_extend_28(engine_reg(engine, 0x38U));
            engine.affine_y[1] = sign_extend_28(engine_reg(engine, 0x3CU));
        } else {
            for (int line = engine.last_affine_line + 1; line < y; ++line) {
                engine.affine_x[0] += static_cast<std::int16_t>(engine_half(engine, 0x22U));
                engine.affine_y[0] += static_cast<std::int16_t>(engine_half(engine, 0x26U));
                engine.affine_x[1] += static_cast<std::int16_t>(engine_half(engine, 0x32U));
                engine.affine_y[1] += static_cast<std::int16_t>(engine_half(engine, 0x36U));
            }
        }
        engine.last_affine_line = y;
    }

    void render_line(int y) {
        if (y < 0 || y >= static_cast<int>(kScreenHeight)) {
            return;
        }
        if (y == 0 && capture_pending) {
            capture_active = true;
            capture_pending = false;
        }
        advance_affine(engine_a, y);
        advance_affine(engine_b, y);
        if (presentation) presentation_a_line_valid = false;
        render_engine_line(engine_a, true, y, engine_a_line);
        if (presentation && !presentation_a_line_valid) {
            presentation->a_line = engine_a_line;
            std::copy_n(three_d_mask_buffer.data() + y * kScreenWidth, kScreenWidth,
                        presentation->mask.data() + y * kScreenWidth);
        }
        render_engine_line(engine_b, false, y, engine_b_line);
        // Advance at the end of the rendered line. HBlank writes of PB/PD affect
        // subsequent increments; reference-point writes replace the next line's origin.
        for (Engine* engine : {&engine_a, &engine_b}) {
            const auto control = engine_reg(*engine, 0U);
            const auto mode = control & 7U;
            for (unsigned bg = 0; bg < 2; ++bg) {
                const bool affine = bg == 0 ? (mode == 2U || mode == 4U || mode == 5U || mode == 6U)
                                            : (mode >= 1U && mode <= 5U);
                if (affine && (control & (1U << (10U + bg))) != 0U &&
                    (((control >> 16U) & 1U) != 0U || (engine == &engine_a && capture_active))) {
                    const auto base = 0x20U + bg * 0x10U;
                    engine->affine_x[bg] += static_cast<std::int16_t>(engine_half(*engine, base + 2U));
                    engine->affine_y[bg] += static_cast<std::int16_t>(engine_half(*engine, base + 6U));
                }
            }
        }
        const std::uint16_t master_a = engine_half(engine_a, 0x6CU);
        const std::uint16_t master_b = engine_half(engine_b, 0x6CU);
        // POWCNT1 bit 15: 0 sends engine A to the lower screen, 1 to the upper.
        const bool a_on_top = (powcnt1 & (1U << 15U)) != 0U;
        auto& a_screen = a_on_top ? screen_top_buffer : screen_bottom_buffer;
        auto& b_screen = a_on_top ? screen_bottom_buffer : screen_top_buffer;
        const std::size_t row = static_cast<std::size_t>(y) * kScreenWidth;
        pack_line(engine_a, engine_a_line, a_screen.data() + row, master_a);
        pack_line(engine_b, engine_b_line, b_screen.data() + row, master_b);
        if (presentation) {
            auto& presentation_a = a_on_top ? presentation->top : presentation->bottom;
            auto& presentation_b = a_on_top ? presentation->bottom : presentation->top;
            pack_line(engine_a, presentation->a_line, presentation_a.data() + row, master_a);
            std::copy_n(b_screen.data() + row, kScreenWidth, presentation_b.data() + row);
        }
        capture_line(y);
    }

    void affine_register_written(Engine& engine, std::uint32_t offset) {
        if (offset >= 0x28U && offset <= 0x2FU) {
            const auto index = (offset - 0x28U) / 4U;
            (index == 0 ? engine.affine_x[0] : engine.affine_y[0]) =
                sign_extend_28(engine_reg(engine, 0x28U + index * 4U));
        } else if (offset >= 0x38U && offset <= 0x3FU) {
            const auto index = (offset - 0x38U) / 4U;
            (index == 0 ? engine.affine_x[1] : engine.affine_y[1]) =
                sign_extend_28(engine_reg(engine, 0x38U + index * 4U));
        }
    }

    std::uint32_t io_read(std::uint32_t address, int width) const {
        if ((address >= kEngineA && address < kEngineA + 0x70U) ||
            (address >= kEngineB && address < kEngineB + 0x70U)) {
            const bool is_a = address < kEngineB;
            const std::uint32_t offset = address - (is_a ? kEngineA : kEngineB);
            const Engine& engine = is_a ? engine_a : engine_b;
            if (is_a && offset >= 0x64U && offset < 0x68U) {
                return (engine_reg(engine_a, 0x64U) >> ((offset & 3U) * 8U)) &
                    (width == 32 ? 0xFFFFFFFFU : ((1U << width) - 1U));
            }
            const std::uint32_t raw = engine_reg(engine, offset & ~3U);
            return (raw >> ((offset & 3U) * 8U)) &
                (width == 32 ? 0xFFFFFFFFU : ((1U << width) - 1U));
        }
        if (address >= kVramCntBase && address < kVramCntBase + 10U) {
            std::uint32_t value = 0U;
            for (int i = 0; i < width / 8; ++i) {
                const std::uint32_t at = address + static_cast<std::uint32_t>(i);
                if (at >= kVramCntBase && at < kVramCntBase + 7U) {
                    value |= static_cast<std::uint32_t>(vramcnt[at - kVramCntBase]) << (i * 8);
                } else if (at == 0x04000248U || at == 0x04000249U) {
                    value |= static_cast<std::uint32_t>(vramcnt[at - 0x04000241U]) << (i * 8);
                }
            }
            return value;
        }
        if (address >= kPowerCnt && address < kPowerCnt + 2U) {
            return (powcnt1 >> ((address - kPowerCnt) * 8U)) &
                (width == 32 ? 0xFFFFFFFFU : ((1U << width) - 1U));
        }
        return 0U;
    }

    void write_engine(Engine& engine, bool is_a, std::uint32_t offset,
                      std::uint32_t value, int width) {
        if (offset >= 0x70U) {
            return;
        }
        const std::uint32_t shift = (offset & 3U) * 8U;
        const std::uint32_t mask = width == 32 ? 0xFFFFFFFFU : ((1U << width) - 1U) << shift;
        std::uint32_t& reg = engine.regs[(offset & ~3U) / 4U];
        reg = (reg & ~mask) | ((value << shift) & mask);
        affine_register_written(engine, offset);
        if (is_a && offset < 0x68U && offset + static_cast<std::uint32_t>(width / 8) > 0x64U) {
            if ((engine_reg(engine, 0x64U) & 0x80000000U) != 0U) {
                capture_pending = true;
            } else {
                capture_pending = false;
                capture_active = false;
            }
        }
    }

    void io_write(std::uint32_t address, std::uint32_t value, int width,
                  const std::function<void()>& remap_callback) {
        if ((address >= kEngineA && address < kEngineA + 0x70U) ||
            (address >= kEngineB && address < kEngineB + 0x70U)) {
            const bool is_a = address < kEngineB;
            const std::uint32_t offset = address - (is_a ? kEngineA : kEngineB);
            write_engine(is_a ? engine_a : engine_b, is_a, offset, value, width);
            if (address == kDisplayFifo && width == 32) {
                push_fifo(value);
            }
            return;
        }
        if (address < kVramCntBase + 10U && address >= kVramCntBase) {
            for (int i = 0; i < width / 8; ++i) {
                const std::uint32_t at = address + static_cast<std::uint32_t>(i);
                std::size_t index = 10U;
                if (at >= kVramCntBase && at < kVramCntBase + 7U) {
                    index = at - kVramCntBase;
                } else if (at == 0x04000248U || at == 0x04000249U) {
                    index = at - 0x04000241U; // H -> 7, I -> 8 (skip WRAMCNT at 0x247)
                }
                if (index < vramcnt.size()) {
                    const std::uint8_t raw = static_cast<std::uint8_t>(value >> (i * 8));
                    const std::uint8_t mask = (index <= 1U || index >= 7U) ? 0x9BU : 0x9FU;
                    vramcnt[index] = static_cast<std::uint8_t>(raw & mask);
                    ++vram_generation;
                }
            }
            rebuild_mappings();
            if (remap_callback) {
                remap_callback();
            }
            return;
        }
        if (address >= kPowerCnt && address < kPowerCnt + 2U) {
            const std::uint32_t shift = (address - kPowerCnt) * 8U;
            const std::uint32_t mask = width == 32 ? 0xFFFFFFFFU : ((1U << width) - 1U) << shift;
            powcnt1 = static_cast<std::uint16_t>(
                ((powcnt1 & ~mask) | ((value << shift) & mask)) & 0x820FU);
        }
    }

    void push_fifo(std::uint32_t value) {
        const std::size_t capacity = fifo.size();
        if (fifo_count + 2U > capacity) {
            return;
        }
        const std::size_t write_index = (fifo_read + fifo_count) % capacity;
        fifo[write_index] = static_cast<std::uint16_t>(value);
        fifo[(write_index + 1U) % capacity] = static_cast<std::uint16_t>(value >> 16U);
        fifo_count += 2U;
    }

    void reset() {
        vram.fill(0U);
        palettes.fill(0U);
        palette_colors.fill(direct_color(0U));
        oam.fill(0U);
        vramcnt.fill(0U);
        engine_a = {};
        engine_b = {};
        powcnt1 = 0U;
        fifo_read = 0U;
        fifo_count = 0U;
        capture_pending = false;
        capture_active = false;
        screen_top_buffer.fill(0xFFFFFFFFU);
        screen_bottom_buffer.fill(0xFFFFFFFFU);
        if (presentation) {
            presentation->top.fill(0xFFFFFFFFU);
            presentation->bottom.fill(0xFFFFFFFFU);
            presentation->mask.fill(0);
        }
        engine_a_line.fill({31U, 31U, 31U, 31U});
        engine_b_line.fill({31U, 31U, 31U, 31U});
        rebuild_mappings();
        mapping_generation = 0;
        vram_generation = 0;
        three_d_mask_buffer.fill(0);
    }
};

void gpu2d_detail::Gpu2DTestAccess::write_bank_byte(
        Gpu2D& gpu, std::uint8_t bank, std::uint32_t offset, std::uint8_t value) {
    gpu.impl_->write_bank_byte(bank, offset, value);
}

void gpu2d_detail::Gpu2DTestAccess::pack_line(
        Gpu2D& gpu, bool engine_b,
        const std::array<std::array<std::uint8_t, 4>, 256>& input,
        std::array<std::uint32_t, 256>& output) {
    std::array<Color, kScreenWidth> line{};
    for (std::size_t x = 0; x < line.size(); ++x) {
        line[x] = {input[x][0], input[x][1], input[x][2], input[x][3]};
    }
    auto& engine = engine_b ? gpu.impl_->engine_b : gpu.impl_->engine_a;
    Gpu2D::Impl::pack_line(engine, line, output.data(),
        static_cast<std::uint16_t>(engine_reg(engine, 0x6CU)));
}

std::array<std::uint8_t, 4> gpu2d_detail::Gpu2DTestAccess::palette_color(
        const Gpu2D& gpu, std::size_t index) {
    const Color color = gpu.impl_->palette_colors[index];
    return {color.r, color.g, color.b, color.alpha};
}

Gpu2D::Gpu2D() : impl_(new Impl) {
    reset();
}

Gpu2D::~Gpu2D() {
    delete impl_;
}

void Gpu2D::reset() {
    impl_->reset();
}

std::uint32_t Gpu2D::io_read(std::uint32_t addr, int width) {
    return impl_->io_read(addr, width);
}

void Gpu2D::io_write(std::uint32_t addr, std::uint32_t value, int width) {
    impl_->io_write(addr, value, width, on_vram_remap);
}

std::uint8_t* Gpu2D::arm9_vram_page(std::uint32_t addr) {
    if (addr < kVramBase || addr >= kVramLimit) {
        return nullptr;
    }
    return impl_->arm9_pages[(addr - kVramBase) / kPageSize];
}

std::uint8_t* Gpu2D::arm7_vram_page(std::uint32_t addr) {
    if (addr < kVramBase || addr >= kVramLimit) {
        return nullptr;
    }
    return impl_->arm7_pages[(addr - kVramBase) / kPageSize];
}

std::uint32_t Gpu2D::arm9_read(std::uint32_t addr, int width) {
    if (addr >= kPaletteBase && addr < kVramBase) {
        return load_le(impl_->palettes.data() + ((addr - kPaletteBase) & 0x7ffU), width);
    }
    if (addr >= kOamBase && addr < 0x08000000U) {
        return load_le(impl_->oam.data() + ((addr - kOamBase) & 0x7ffU), width);
    }
    if (addr >= kVramBase && addr < kVramLimit) {
        std::uint32_t value = 0U;
        for (int i = 0; i < width / 8; ++i) {
            value |= static_cast<std::uint32_t>(impl_->read_vram_byte(addr + static_cast<std::uint32_t>(i), false)) << (i * 8);
        }
        return value;
    }
    return 0U;
}

void Gpu2D::arm9_write(std::uint32_t addr, std::uint32_t value, int width) {
    const int bytes = width / 8;
    if (width == 8) {
        return;
    }
    if (addr >= kPaletteBase && addr < kVramBase) {
        const std::uint32_t offset = (addr - kPaletteBase) & 0x7ffU;
        store_le(impl_->palettes.data() + offset, value, width);
        // Decode the stored bytes, not the input value: preserve the store's
        // byte order and refresh every halfword touched by a partial store.
        // The bus naturally aligns 16/32-bit stores before routing them here.
        for (std::uint32_t at = offset & ~1U; at < offset + bytes; at += 2U) {
            impl_->palette_colors[at / 2U] = direct_color(read_half(impl_->palettes, at));
        }
        return;
    }
    if (addr >= kOamBase && addr < 0x08000000U) {
        store_le(impl_->oam.data() + ((addr - kOamBase) & 0x7ffU), value, width);
        return;
    }
    if (addr >= kVramBase && addr < kVramLimit) {
        for (int i = 0; i < bytes; ++i) {
            impl_->write_mapped_byte(impl_->arm9_maps.data(), impl_->arm9_map_count,
                vram_address(addr + static_cast<std::uint32_t>(i)), static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }
}

std::uint32_t Gpu2D::arm7_read(std::uint32_t addr, int width) {
    if (addr < kVramBase || addr >= kVramLimit) {
        return 0U;
    }
    std::uint32_t value = 0U;
    for (int i = 0; i < width / 8; ++i) {
        value |= static_cast<std::uint32_t>(impl_->read_vram_byte(addr + static_cast<std::uint32_t>(i), true)) << (i * 8);
    }
    return value;
}

void Gpu2D::arm7_write(std::uint32_t addr, std::uint32_t value, int width) {
    if (addr < kVramBase || addr >= kVramLimit) {
        return;
    }
    for (int i = 0; i < width / 8; ++i) {
        impl_->write_mapped_byte(impl_->arm7_maps.data(), impl_->arm7_map_count,
            vram_address(addr + static_cast<std::uint32_t>(i), true), static_cast<std::uint8_t>(value >> (i * 8)));
    }
}

std::uint8_t Gpu2D::vramstat() const {
    const bool bank_c = (impl_->vramcnt[2] & 0x87U) == 0x82U;
    const bool bank_d = (impl_->vramcnt[3] & 0x87U) == 0x82U;
    return static_cast<std::uint8_t>((bank_c ? 1U : 0U) | (bank_d ? 2U : 0U));
}

const std::uint8_t* Gpu2D::texture_slot(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(impl_->texture_banks.size()) ||
        impl_->texture_counts[static_cast<std::size_t>(slot)] != 1U) {
        return nullptr;
    }
    const std::uint32_t bank = impl_->texture_banks[static_cast<std::size_t>(slot)];
    return impl_->vram.data() + bank_offset(static_cast<std::uint8_t>(bank), 0U);
}

const std::uint8_t* Gpu2D::texpal_slot(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(impl_->texture_palette_banks.size()) ||
        impl_->texture_palette_counts[static_cast<std::size_t>(slot)] != 1U) {
        return nullptr;
    }
    const std::uint32_t offset = impl_->texture_palette_banks[static_cast<std::size_t>(slot)];
    return impl_->vram.data() + offset;
}

std::uint8_t Gpu2D::texture_byte(std::uint32_t address) const {
    address &= 0x7ffffU;
    const auto mask = impl_->texture_bank_masks[address >> 17U];
    std::uint8_t value = 0;
    for (unsigned bank = 0; bank < 4; ++bank) {
        if ((mask & (1U << bank)) != 0U) {
            value |= impl_->vram[bank_offset(static_cast<std::uint8_t>(bank), address & 0x1ffffU)];
        }
    }
    return value;
}

std::uint8_t Gpu2D::texpal_byte(std::uint32_t address) const {
    auto slot = (address >> 14U) & 7U;
    if (slot > 5U) slot -= 5U;
    const auto mask = impl_->texture_palette_bank_masks[slot];
    std::uint8_t value = 0;
    for (unsigned bank = 4; bank < 7; ++bank) {
        if ((mask & (1U << bank)) != 0U) {
            const auto offset = (bank == 4 ? slot * 0x4000U : 0U) + (address & 0x3fffU);
            value |= impl_->vram[bank_offset(static_cast<std::uint8_t>(bank), offset)];
        }
    }
    return value;
}

void Gpu2D::attach_gpu3d(Gpu3D* gpu3d) {
    impl_->gpu3d = gpu3d;
}

void Gpu2D::set_enhanced_presentation(bool enabled, bool full) {
    impl_->presentation_full = full;
    if (!enabled) {
        impl_->presentation.reset();
    } else if (!impl_->presentation) {
        auto presentation = std::make_unique<Impl::Presentation>();
        presentation->top = impl_->screen_top_buffer;
        presentation->bottom = impl_->screen_bottom_buffer;
        presentation->mask = impl_->three_d_mask_buffer;
        impl_->presentation = std::move(presentation);
    }
}
void Gpu2D::set_presentation_scale(int scale) {
    impl_->presentation_scale = full_presentation_scale(scale);
}


void Gpu2D::render_line(int y) {
    impl_->render_line(y);
}

void Gpu2D::vblank_start() {
    impl_->engine_a.last_affine_line = -1;
    impl_->engine_b.last_affine_line = -1;
    impl_->engine_a.regs[0x64U / 4U] &= ~0x80000000U;
    impl_->capture_pending = false;
    impl_->capture_active = false;
    impl_->fifo_read = 0U;
    impl_->fifo_count = 0U;
}

std::uint64_t Gpu2D::vram_generation() const {
    return impl_->vram_generation;
}

const std::uint8_t* Gpu2D::three_d_mask() const {
    return impl_->three_d_mask_buffer.data();
}

const std::uint8_t* Gpu2D::presentation_three_d_mask() const {
    return impl_->presentation ? impl_->presentation->mask.data() : three_d_mask();
}

std::uint8_t Gpu2D::expand_color5(std::uint32_t value) {
    return static_cast<std::uint8_t>(cleanroom::color5_to_8(value & 31U));
}

const std::uint32_t* Gpu2D::screen_top() const {
    return impl_->screen_top_buffer.data();
}

const std::uint32_t* Gpu2D::screen_bottom() const {
    return impl_->screen_bottom_buffer.data();
}

const std::uint32_t* Gpu2D::presentation_top() const {
    return impl_->presentation ? impl_->presentation->top.data() : screen_top();
}

const std::uint32_t* Gpu2D::presentation_bottom() const {
    return impl_->presentation ? impl_->presentation->bottom.data() : screen_bottom();
}

Gpu2DFrameView Gpu2D::frame_view() const {
    const auto& impl = *impl_;
    Gpu2DFrameView view{};
    const Engine* engines[] = {&impl.engine_a, &impl.engine_b};
    for (std::size_t i = 0; i < 2; ++i) {
        view.engines[i].registers = engines[i]->regs.data();
        view.engines[i].register_count = engines[i]->regs.size();
        for (std::size_t affine = 0; affine < 2; ++affine) {
            view.engines[i].affine_x[affine] = engines[i]->affine_x[affine];
            view.engines[i].affine_y[affine] = engines[i]->affine_y[affine];
        }
        view.engines[i].last_affine_line = engines[i]->last_affine_line;
    }
    view.vramcnt = impl.vramcnt.data();
    view.vramcnt_count = impl.vramcnt.size();
    view.powcnt1 = impl.powcnt1;
    view.capture_pending = impl.capture_pending;
    view.capture_active = impl.capture_active;
    view.palettes = impl.palettes.data();
    view.palette_byte_count = impl.palettes.size();
    view.oam = impl.oam.data();
    view.oam_byte_count = impl.oam.size();
    for (std::size_t bank = 0; bank < 9; ++bank) {
        view.banks[bank] = {
            impl.vram.data() + bank * kBankCapacity,
            kBankSizes[bank],
        };
    }
    for (std::size_t slot = 0; slot < 4; ++slot) {
        auto& mapped = view.texture_slots[slot];
        mapped.mapped = impl.texture_counts[slot] == 1U;
        if (mapped.mapped) {
            mapped.physical_bank = static_cast<std::uint8_t>(impl.texture_banks[slot]);
            mapped.bank_offset = 0;
            mapped.bytes = texture_slot(static_cast<int>(slot));
            mapped.byte_count = 0x20000U;
        }
    }
    for (std::size_t slot = 0; slot < 6; ++slot) {
        auto& mapped = view.texture_palette_slots[slot];
        mapped.mapped = impl.texture_palette_counts[slot] == 1U;
        if (mapped.mapped) {
            const auto offset = impl.texture_palette_banks[slot];
            mapped.physical_bank = static_cast<std::uint8_t>(offset / kBankCapacity);
            mapped.bank_offset = offset % kBankCapacity;
            mapped.bytes = texpal_slot(static_cast<int>(slot));
            mapped.byte_count = 0x4000U;
        }
    }
    view.mapping_generation = impl.mapping_generation;
    return view;
}
} // namespace dsr
