#include "texture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <list>
#include <unordered_map>

namespace dsr::gpu3d_detail {
namespace {

constexpr std::uint32_t kTextureAddressMask = 0x7ffffU;
constexpr std::uint32_t kTextureSlotSize = 0x20000U;

std::uint8_t read_texture_byte(const TextureSource& source, std::uint32_t address) {
    return source.texture_byte(address);
}

std::uint16_t read_texture_half(const TextureSource& source, std::uint32_t address) {
    return static_cast<std::uint16_t>(read_texture_byte(source, address) |
        (static_cast<std::uint16_t>(read_texture_byte(source, address + 1U)) << 8));
}

std::uint16_t read_palette_half(const TextureSource& source, std::uint32_t address) {
    return source.palette_half(address);
}

std::uint32_t read_texture_word(const TextureSource& source, std::uint32_t address) {
    return static_cast<std::uint32_t>(read_texture_byte(source, address)) |
           (static_cast<std::uint32_t>(read_texture_byte(source, address + 1U)) << 8) |
           (static_cast<std::uint32_t>(read_texture_byte(source, address + 2U)) << 16) |
           (static_cast<std::uint32_t>(read_texture_byte(source, address + 3U)) << 24);
}

std::uint8_t channel5(std::uint16_t color, unsigned shift) {
    return static_cast<std::uint8_t>((color >> shift) & 31U);
}

std::uint8_t color5_to6(std::uint8_t value) {
    return static_cast<std::uint8_t>(value == 0U ? 0U : value * 2U + 1U);
}

std::uint8_t color5_to8(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
}

std::uint8_t mix_mode1_to6(std::uint8_t a, std::uint8_t b) {
    return static_cast<std::uint8_t>((color5_to8(a) + color5_to8(b)) >> 3U);
}

std::uint8_t mix_mode3_to6(std::uint8_t a, std::uint8_t b,
                           unsigned wa, unsigned wb) {
    const auto mixed5 = static_cast<std::uint8_t>(
        (color5_to8(a) * wa + color5_to8(b) * wb) >> 6U);
    return color5_to6(mixed5);
}

Texel palette_texel(const TextureSource& source, std::uint32_t base,
                    unsigned index, bool color_zero_transparent) {
    const auto color = read_palette_half(source, base + index * 2U);
    if (index == 0 && color_zero_transparent) return {};
    return {color5_to6(channel5(color, 0)), color5_to6(channel5(color, 5)),
            color5_to6(channel5(color, 10)), 31};
}

Texel direct_texel(std::uint16_t color) {
    if ((color & 0x8000U) == 0) return {};
    return {color5_to6(channel5(color, 0)), color5_to6(channel5(color, 5)),
            color5_to6(channel5(color, 10)), 31};
}

// Bind slot pointers once per image, not once per byte/texel. A null pointer
// retains the original overlapping-bank OR or unmapped-zero callback semantics.
struct ImageReader {
    const TextureSource& source;
    std::array<const std::uint8_t*, 4> textures{};
    std::array<const std::uint8_t*, 6> palettes{};

    explicit ImageReader(const TextureSource& input) : source(input) {
        for (int i = 0; i < 4; ++i) textures[i] = source.texture_slot(i);
        for (int i = 0; i < 6; ++i) palettes[i] = source.texpal_slot(i);
    }
    std::uint8_t texture_byte(std::uint32_t address) const {
        address &= kTextureAddressMask;
        const auto* bytes = textures[address >> 17U];
        if (bytes != nullptr) return bytes[address & 0x1ffffU];
        return source.texture_read == nullptr ? 0 : source.texture_read(source.context, address);
    }
    std::uint16_t texture_half(std::uint32_t address) const {
        return static_cast<std::uint16_t>(texture_byte(address) |
            (static_cast<std::uint16_t>(texture_byte(address + 1U)) << 8U));
    }
    std::uint32_t texture_word(std::uint32_t address) const {
        return texture_byte(address) |
            (static_cast<std::uint32_t>(texture_byte(address + 1U)) << 8U) |
            (static_cast<std::uint32_t>(texture_byte(address + 2U)) << 16U) |
            (static_cast<std::uint32_t>(texture_byte(address + 3U)) << 24U);
    }
    std::uint16_t palette_half(std::uint32_t address) const {
        auto slot = (address >> 14U) & 7U;
        if (slot > 5U) slot -= 5U;
        const auto offset = address & 0x3fffU;
        const auto* bytes = palettes[slot];
        if (bytes != nullptr) {
            return static_cast<std::uint16_t>(bytes[offset] |
                (static_cast<std::uint16_t>(bytes[(offset + 1U) & 0x3fffU]) << 8U));
        }
        if (source.palette_read == nullptr) return 0;
        return static_cast<std::uint16_t>(source.palette_read(source.context, address) |
            (static_cast<std::uint16_t>(source.palette_read(source.context,
                (address & ~0x3fffU) | ((offset + 1U) & 0x3fffU))) << 8U));
    }
};

template<unsigned Format>
void decode_indexed_image(const ImageReader& reader, std::uint32_t texture_address,
                          std::uint32_t palette_address, bool transparent,
                          std::uint32_t pixels, Texel* destination) {
    constexpr unsigned colors = Format == 1U ? 32U : Format == 2U ? 4U :
        Format == 3U ? 16U : Format == 4U ? 256U : 8U;
    std::array<Texel, colors> palette;
    for (unsigned i = 0; i < colors; ++i)
        palette[i] = direct_texel(reader.palette_half(palette_address + i * 2U) | 0x8000U);
    if constexpr (Format >= 2U && Format <= 4U)
        if (transparent) palette[0] = {};
    for (std::uint32_t i = 0; i < pixels; ++i) {
        if constexpr (Format == 2U) {
            const auto byte = reader.texture_byte(texture_address + i / 4U);
            destination[i] = palette[(byte >> ((i & 3U) * 2U)) & 3U];
        } else if constexpr (Format == 3U) {
            const auto byte = reader.texture_byte(texture_address + i / 2U);
            destination[i] = palette[(byte >> ((i & 1U) * 4U)) & 15U];
        } else {
            const auto byte = reader.texture_byte(texture_address + i);
            if constexpr (Format == 1U) {
                destination[i] = palette[byte & 31U];
                // A3I5 expands alpha with truncation, not nearest rounding.
                destination[i].a = static_cast<std::uint8_t>(((byte >> 5U) * 31U) / 7U);
            } else if constexpr (Format == 6U) {
                destination[i] = palette[byte & 7U];
                destination[i].a = static_cast<std::uint8_t>(byte >> 3U);
            } else {
                destination[i] = palette[byte];
            }
        }
    }
}

void decode_compressed_image(const ImageReader& reader, std::uint32_t texture_address,
                             std::uint32_t palette_address, unsigned width,
                             unsigned height, Texel* destination) {
    const auto metadata_base = kTextureSlotSize +
        (texture_address / kTextureSlotSize == 2U ? 0x10000U : 0U) +
        (texture_address % kTextureSlotSize) / 2U;
    for (unsigned tile_y = 0; tile_y < height / 4U; ++tile_y) {
        for (unsigned tile_x = 0; tile_x < width / 4U; ++tile_x) {
            const auto tile = tile_y * (width / 4U) + tile_x;
            auto indices = reader.texture_word(texture_address + tile * 4U);
            const auto metadata = reader.texture_half(metadata_base + tile * 2U);
            const auto mode = metadata >> 14U;
            const auto address = palette_address + (metadata & 0x3fffU) * 4U;
            const auto c0 = reader.palette_half(address);
            const auto c1 = reader.palette_half(address + 2U);
            std::array<Texel, 4> colors{
                direct_texel(c0 | 0x8000U), direct_texel(c1 | 0x8000U), {}, {}};
            if (mode == 0U || mode == 2U) {
                colors[2] = direct_texel(reader.palette_half(address + 4U) | 0x8000U);
                if (mode == 2U)
                    colors[3] = direct_texel(reader.palette_half(address + 6U) | 0x8000U);
            } else if (mode == 1U) {
                colors[2] = {mix_mode1_to6(channel5(c0, 0), channel5(c1, 0)),
                             mix_mode1_to6(channel5(c0, 5), channel5(c1, 5)),
                             mix_mode1_to6(channel5(c0, 10), channel5(c1, 10)), 31};
            } else {
                colors[2] = {mix_mode3_to6(channel5(c0, 0), channel5(c1, 0), 5, 3),
                             mix_mode3_to6(channel5(c0, 5), channel5(c1, 5), 5, 3),
                             mix_mode3_to6(channel5(c0, 10), channel5(c1, 10), 5, 3), 31};
                colors[3] = {mix_mode3_to6(channel5(c0, 0), channel5(c1, 0), 3, 5),
                             mix_mode3_to6(channel5(c0, 5), channel5(c1, 5), 3, 5),
                             mix_mode3_to6(channel5(c0, 10), channel5(c1, 10), 3, 5), 31};
            }
            for (unsigned y = 0; y < 4U; ++y) {
                auto* row = destination + (tile_y * 4U + y) * width + tile_x * 4U;
                for (unsigned x = 0; x < 4U; ++x) {
                    row[x] = colors[indices & 3U];
                    indices >>= 2U;
                }
            }
        }
    }
}

std::uint64_t image_key(std::uint32_t texture_param, std::uint32_t palette_base) {
    return (static_cast<std::uint64_t>(texture_param) << 32U) | palette_base;
}

} // namespace

Texel decode_texel(const TextureSource& source, std::uint32_t texture_param,
                   std::uint32_t palette_base, int x, int y) {
    if (x < 0 || y < 0) return {};
    const auto width = static_cast<std::uint32_t>(8U << ((texture_param >> 20) & 7U));
    const auto height = static_cast<std::uint32_t>(8U << ((texture_param >> 23) & 7U));
    if (static_cast<std::uint32_t>(x) >= width || static_cast<std::uint32_t>(y) >= height) return {};

    const auto texture_address = ((texture_param & 0xffffU) * 8U) & kTextureAddressMask;
    const auto format = (texture_param >> 26) & 7U;
    const auto palette_scale = format == 2U ? 8U : 16U;
    const auto palette_address = (palette_base & 0x1fffU) * palette_scale;
    const auto pixel = static_cast<std::uint32_t>(y) * width + static_cast<std::uint32_t>(x);
    switch (format) {
    case 1: { // A3I5
        const auto value = read_texture_byte(source, texture_address + pixel);
        const auto alpha = static_cast<std::uint8_t>(((value >> 5) * 31U) / 7U);
        auto result = palette_texel(source, palette_address, value & 31U, false);
        result.a = alpha;
        return result;
    }
    case 2: { // 2-bit indexed, 4 colors
        const auto byte = read_texture_byte(source, texture_address + pixel / 4U);
        const auto index = (byte >> ((pixel & 3U) * 2U)) & 3U;
        return palette_texel(source, palette_address, index, (texture_param & (1U << 29)) != 0);
    }
    case 3: { // 4-bit indexed, 16 colors
        const auto byte = read_texture_byte(source, texture_address + pixel / 2U);
        const auto index = (byte >> ((pixel & 1U) * 4U)) & 15U;
        return palette_texel(source, palette_address, index, (texture_param & (1U << 29)) != 0);
    }
    case 4: { // 8-bit indexed, 256 colors
        const auto index = read_texture_byte(source, texture_address + pixel);
        return palette_texel(source, palette_address, index, (texture_param & (1U << 29)) != 0);
    }
    case 5: { // 4x4 compressed: 2-bit indices plus per-tile metadata in texture slot 1
        const auto texture_slot = texture_address / kTextureSlotSize;
        const auto texture_offset = texture_address % kTextureSlotSize;
        const auto metadata_base = kTextureSlotSize +
            (texture_slot == 2U ? 0x10000U : 0U) + texture_offset / 2U;
        const auto tiles_per_row = width / 4U;
        const auto tile = (static_cast<std::uint32_t>(y) / 4U) * tiles_per_row +
                          static_cast<std::uint32_t>(x) / 4U;
        const auto tile_word = read_texture_word(source, texture_address + tile * 4U);
        const auto within_tile = (static_cast<std::uint32_t>(y) & 3U) * 4U +
                                 (static_cast<std::uint32_t>(x) & 3U);
        const auto index = (tile_word >> (within_tile * 2U)) & 3U;
        const auto tile_palette = read_texture_half(source, metadata_base + tile * 2U);
        const auto palette_mode = tile_palette >> 14;
        const auto colors_address = palette_address + (tile_palette & 0x3fffU) * 4U;
        const auto c0 = read_palette_half(source, colors_address);
        const auto c1 = read_palette_half(source, colors_address + 2U);
        if (palette_mode == 0U) {
            if (index == 3U) return {};
            return direct_texel(read_palette_half(source, colors_address + index * 2U) | 0x8000U);
        }
        if (palette_mode == 1U) {
            if (index == 3U) return {};
            if (index < 2U) return direct_texel(index == 0U ? c0 | 0x8000U : c1 | 0x8000U);
            return {mix_mode1_to6(channel5(c0, 0), channel5(c1, 0)),
                    mix_mode1_to6(channel5(c0, 5), channel5(c1, 5)),
                    mix_mode1_to6(channel5(c0, 10), channel5(c1, 10)), 31};
        }
        if (palette_mode == 2U) {
            return direct_texel(read_palette_half(source, colors_address + index * 2U) | 0x8000U);
        }
        if (index < 2U) return direct_texel(index == 0U ? c0 | 0x8000U : c1 | 0x8000U);
        const auto w0 = index == 2U ? 5U : 3U;
        const auto w1 = 8U - w0;
        return {mix_mode3_to6(channel5(c0, 0), channel5(c1, 0), w0, w1),
                mix_mode3_to6(channel5(c0, 5), channel5(c1, 5), w0, w1),
                mix_mode3_to6(channel5(c0, 10), channel5(c1, 10), w0, w1), 31};
    }
    case 6: { // A5I3
        const auto value = read_texture_byte(source, texture_address + pixel);
        auto result = palette_texel(source, palette_address, value & 7U, false);
        result.a = static_cast<std::uint8_t>(value >> 3);
        return result;
    }
    case 7: { // Direct color, high bit is the alpha bit
        const auto address = texture_address + pixel * 2U;
        const auto color = static_cast<std::uint16_t>(read_texture_byte(source, address) |
                           (static_cast<std::uint16_t>(read_texture_byte(source, address + 1U)) << 8));
        return direct_texel(color);
    }
    default:
        return {};
    }
}

void decode_texture(const TextureSource& source, std::uint32_t texture_param,
                    std::uint32_t palette_base, Texel* destination) {
    const auto width = 8U << ((texture_param >> 20U) & 7U);
    const auto height = 8U << ((texture_param >> 23U) & 7U);
    const auto pixels = width * height;
    const auto format = (texture_param >> 26U) & 7U;
    if (format == 0U) {
        std::fill_n(destination, pixels, Texel{});
        return;
    }
    const ImageReader reader(source);
    const auto address = (texture_param & 0xffffU) * 8U;
    const auto palette = (palette_base & 0x1fffU) * (format == 2U ? 8U : 16U);
    const bool transparent = (texture_param & (1U << 29U)) != 0U;
    switch (format) {
    case 1: decode_indexed_image<1>(reader, address, palette, transparent, pixels, destination); break;
    case 2: decode_indexed_image<2>(reader, address, palette, transparent, pixels, destination); break;
    case 3: decode_indexed_image<3>(reader, address, palette, transparent, pixels, destination); break;
    case 4: decode_indexed_image<4>(reader, address, palette, transparent, pixels, destination); break;
    case 5: decode_compressed_image(reader, address, palette, width, height, destination); break;
    case 6: decode_indexed_image<6>(reader, address, palette, transparent, pixels, destination); break;
    case 7:
        for (std::uint32_t i = 0; i < pixels; ++i)
            destination[i] = direct_texel(reader.texture_half(address + i * 2U));
        break;
    }
}

struct DecodedTextureCache::Impl {
    struct Entry {
        TextureKey key;
        std::unique_ptr<Texel[]> image;
        std::size_t bytes;
        std::uint64_t pinned_frame;
    };
    std::list<Entry> lru;
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> images;
    std::size_t bytes = 0;
};

DecodedTextureCache::DecodedTextureCache(std::size_t byte_limit, std::size_t entry_limit)
    : byte_limit_(byte_limit), entry_limit_(entry_limit) {}
DecodedTextureCache::~DecodedTextureCache() = default;

void DecodedTextureCache::begin_frame(std::uint64_t generation) {
    if (++frame_ == 0U) {
        clear();
        frame_ = 1U;
    }
    generation_ = generation;
}

const Texel* DecodedTextureCache::acquire(const TextureSource& source,
                                        std::uint32_t texture_param,
                                        std::uint32_t palette_base) {
    if (((texture_param >> 26U) & 7U) == 0U) return nullptr;
    const auto identity = image_key(texture_param, palette_base);
    if (impl_) {
        const auto found = impl_->images.find(identity);
        if (found != impl_->images.end()) {
            auto entry = found->second;
            if (entry->key.vram_generation != generation_) {
                decode_texture(source, texture_param, palette_base, entry->image.get());
                entry->key.vram_generation = generation_;
            }
            entry->pinned_frame = frame_;
            impl_->lru.splice(impl_->lru.begin(), impl_->lru, entry);
            return entry->image.get();
        }
    }
    const auto pixels = static_cast<std::size_t>(8U << ((texture_param >> 20U) & 7U)) *
                        (8U << ((texture_param >> 23U) & 7U));
    const auto bytes = pixels * sizeof(Texel);
    if (bytes > byte_limit_ || entry_limit_ == 0U) return nullptr;
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
        impl_->images.reserve(entry_limit_);
    }
    while (impl_->bytes > byte_limit_ - bytes || impl_->lru.size() >= entry_limit_) {
        auto victim = impl_->lru.end();
        do {
            if (victim == impl_->lru.begin()) return nullptr;
            --victim;
        } while (victim->pinned_frame == frame_);
        impl_->bytes -= victim->bytes;
        impl_->images.erase(image_key(victim->key.texture_param, victim->key.palette_base));
        impl_->lru.erase(victim);
    }
    // Texels are completely overwritten; avoid a redundant zero-fill.
    auto image = std::unique_ptr<Texel[]>(new Texel[pixels]);
    decode_texture(source, texture_param, palette_base, image.get());
    const auto [found, inserted] = impl_->images.emplace(identity, impl_->lru.end());
    (void)inserted;
    try {
        impl_->lru.push_front({{texture_param, palette_base, generation_},
                              std::move(image), bytes, frame_});
    } catch (...) {
        impl_->images.erase(found);
        throw;
    }
    found->second = impl_->lru.begin();
    impl_->bytes += bytes;
    return impl_->lru.front().image.get();
}

const Texel* DecodedTextureCache::find(std::uint32_t texture_param,
                                     std::uint32_t palette_base) const {
    if (!impl_) return nullptr;
    const auto found = impl_->images.find(image_key(texture_param, palette_base));
    if (found == impl_->images.end() || found->second->key.vram_generation != generation_)
        return nullptr;
    return found->second->image.get();
}

void DecodedTextureCache::clear() {
    impl_.reset();
    generation_ = frame_ = 0;
}
std::size_t DecodedTextureCache::memory_bytes() const { return impl_ ? impl_->bytes : 0; }
std::size_t DecodedTextureCache::entry_count() const { return impl_ ? impl_->lru.size() : 0; }

} // namespace dsr::gpu3d_detail
