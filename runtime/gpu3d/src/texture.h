#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dsr::gpu3d_detail {

// Same defined domain as floor-then-int: the floored finite value fits in int.
inline int floor_texcoord(float value) {
    const int integer = static_cast<int>(value);
    return integer - (value < static_cast<float>(integer));
}

struct TextureSource {
    using Lookup = const std::uint8_t* (*)(const void* context, int slot);
    using Read = std::uint8_t (*)(const void* context, std::uint32_t address);
    using Generation = std::uint64_t (*)(const void* context);

    const void* context = nullptr;
    Lookup texture_lookup = nullptr;
    Lookup palette_lookup = nullptr;
    Read texture_read = nullptr;
    Read palette_read = nullptr;
    // Optional invalidation token. Without it, the renderer keeps only its
    // per-polygon cache: arbitrary injected sources can change between renders.
    Generation generation = nullptr;

    const std::uint8_t* texture_slot(int slot) const {
        return texture_lookup == nullptr ? nullptr : texture_lookup(context, slot);
    }
    const std::uint8_t* texpal_slot(int slot) const {
        return palette_lookup == nullptr ? nullptr : palette_lookup(context, slot);
    }
    std::uint8_t texture_byte(std::uint32_t address) const {
        address &= 0x7ffffU;
        const auto* bytes = texture_slot(static_cast<int>(address >> 17U));
        if (bytes != nullptr) return bytes[address & 0x1ffffU];
        return texture_read == nullptr ? 0 : texture_read(context, address);
    }
    std::uint16_t texture_half(std::uint32_t address) const {
        return static_cast<std::uint16_t>(texture_byte(address) |
            (static_cast<std::uint16_t>(texture_byte(address + 1U)) << 8U));
    }
    std::uint16_t palette_half(std::uint32_t address) const {
        auto slot = (address >> 14U) & 7U;
        if (slot > 5U) slot -= 5U;
        const auto* bytes = texpal_slot(static_cast<int>(slot));
        const auto offset = address & 0x3fffU;
        if (bytes != nullptr) {
            return static_cast<std::uint16_t>(bytes[offset] |
                (static_cast<std::uint16_t>(bytes[(offset + 1U) & 0x3fffU]) << 8U));
        }
        if (palette_read == nullptr) return 0;
        return static_cast<std::uint16_t>(palette_read(context, address) |
            (static_cast<std::uint16_t>(palette_read(context,
                (address & ~0x3fffU) | ((offset + 1U) & 0x3fffU))) << 8U));
    }
};

// RGB channels are native 6-bit texture-cache values; alpha remains 5-bit.
struct Texel {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

// Shared software/GPU cache identity, including texture AND palette VRAM.
struct TextureKey {
    std::uint32_t texture_param = 0;
    std::uint32_t palette_base = 0;
    std::uint64_t vram_generation = 0;

    bool operator==(const TextureKey&) const = default;
};

// Decode one texel at integer texture coordinates using DS texture parameter
// and palette-base register values. This internal seam also lets tests inject
// deterministic texture/palette storage without constructing a Gpu2D.
Texel decode_texel(const TextureSource& source, std::uint32_t texture_param,
                   std::uint32_t palette_base, int x, int y);

// Decode the complete row-major image at native RGB6/A5 precision. Destination
// contains (8 << size_s) * (8 << size_t) Texels; no coordinate wrapping occurs.
// Single-bank pointers and overlapping/unmapped callback reads have identical
// semantics to decode_texel. No allocation occurs in this bulk decoder.
void decode_texture(const TextureSource& source, std::uint32_t texture_param,
                    std::uint32_t palette_base, Texel* destination);

// Machine-thread preparation followed by an immutable render snapshot. acquire
// pins its result until the next begin_frame, so LRU eviction cannot invalidate
// borrowed images while bands use them. find is read-only and safe for workers;
// begin_frame/acquire/clear must never overlap rendering. If this frame's images
// exceed either bound, acquire returns nullptr and the scalar sampler remains
// exact. Generation changes invalidate logically, reusing same-key allocations
// when the owner reacquires them instead of allocating on every guest upload.
class DecodedTextureCache {
public:
    explicit DecodedTextureCache(std::size_t byte_limit = 16U * 1024U * 1024U,
                                 std::size_t entry_limit = 1024U);
    ~DecodedTextureCache();
    DecodedTextureCache(const DecodedTextureCache&) = delete;
    DecodedTextureCache& operator=(const DecodedTextureCache&) = delete;

    void begin_frame(std::uint64_t generation);
    const Texel* acquire(const TextureSource& source, std::uint32_t texture_param,
                         std::uint32_t palette_base);
    const Texel* find(std::uint32_t texture_param, std::uint32_t palette_base) const;
    void clear();
    std::size_t memory_bytes() const;
    std::size_t entry_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const std::size_t byte_limit_;
    const std::size_t entry_limit_;
    std::uint64_t generation_ = 0;
    std::uint64_t frame_ = 0;
};

} // namespace dsr::gpu3d_detail
