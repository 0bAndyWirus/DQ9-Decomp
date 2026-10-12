#include "texture_rgba.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace dsr::gpu3d_detail;

bool check(bool condition, std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

struct Storage {
    std::vector<std::uint8_t> textures = std::vector<std::uint8_t>(0x80000U);
    std::vector<std::uint8_t> palettes = std::vector<std::uint8_t>(0x18000U);

    Storage() {
        for (std::size_t i = 0; i < textures.size(); ++i)
            textures[i] = static_cast<std::uint8_t>(i * 73U + (i >> 8U));
        for (std::size_t i = 0; i < palettes.size() / 2U; ++i) {
            const auto color = static_cast<std::uint16_t>((i & 31U) |
                (((i * 7U) & 31U) << 5U) | (((31U - i) & 31U) << 10U));
            palettes[i * 2U] = static_cast<std::uint8_t>(color);
            palettes[i * 2U + 1U] = static_cast<std::uint8_t>(color >> 8U);
        }
    }

    static const std::uint8_t* texture_lookup(const void* context, int slot) {
        const auto& self = *static_cast<const Storage*>(context);
        return self.textures.data() + static_cast<std::size_t>(slot) * 0x20000U;
    }
    static const std::uint8_t* palette_lookup(const void* context, int slot) {
        const auto& self = *static_cast<const Storage*>(context);
        return self.palettes.data() + static_cast<std::size_t>(slot) * 0x4000U;
    }
    static std::uint8_t texture_read(const void* context, std::uint32_t address) {
        return static_cast<const Storage*>(context)->textures[address & 0x7ffffU];
    }
    static std::uint8_t palette_read(const void* context, std::uint32_t address) {
        auto slot = (address >> 14U) & 7U;
        if (slot > 5U) slot -= 5U;
        return static_cast<const Storage*>(context)->palettes[slot * 0x4000U + (address & 0x3fffU)];
    }
    TextureSource view(bool callbacks = false) const {
        return callbacks ? TextureSource{this, nullptr, nullptr, texture_read, palette_read}
                         : TextureSource{this, texture_lookup, palette_lookup};
    }
    void half(std::uint32_t address, std::uint16_t value) {
        textures[address & 0x7ffffU] = static_cast<std::uint8_t>(value);
        textures[(address + 1U) & 0x7ffffU] = static_cast<std::uint8_t>(value >> 8U);
    }
};

// Compare every component to the shared scalar decoder, including the shader's
// normalized-sample quantisation. Multiplication by white in shade_pixel leaves
// RGB6 unchanged; opaque 2D composition subsequently truncates it to RGB5.
bool compare(const TextureSource& source, std::uint32_t param, std::uint32_t palette) {
    std::vector<std::uint8_t> rgba;
    int width = 0, height = 0;
    if (!check(decode_texture_rgba8(source, param, palette, rgba, width, height), "valid texture decodes")) return false;
    if (!check(width == (8 << ((param >> 20U) & 7U)) &&
               height == (8 << ((param >> 23U) & 7U)) &&
               rgba.size() == static_cast<std::size_t>(width * height) * 4U, "texture dimensions and RGBA size")) return false;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto texel = decode_texel(source, param, palette, x, y);
            const std::array<unsigned, 4> native{texel.r, texel.g, texel.b, texel.a};
            const auto offset = static_cast<std::size_t>(y * width + x) * 4U;
            for (std::size_t channel = 0; channel < 4U; ++channel) {
                const auto value = native[channel];
                const unsigned expanded = channel == 3U ? (value << 3U) | (value >> 2U)
                                                        : (value << 2U) | (value >> 4U);
                const unsigned levels = channel == 3U ? 31U : 63U;
                const auto quantised = (rgba[offset + channel] * levels + 127U) / 255U;
                if (!check(rgba[offset + channel] == expanded && quantised == value,
                           "RGBA bytes and nearest shader quantisation match scalar texel")) return false;
                if (channel != 3U) {
                    const auto shaded = ((quantised + 1U) * 64U - 1U) / 64U;
                    if (!check((shaded >> 1U) == (value >> 1U), "white modulation and opaque compositor preserve RGB5")) return false;
                }
            }
        }
    }
    return true;
}

bool test_all_formats() {
    Storage storage;
    bool ok = true;
    for (unsigned format = 1; format <= 7; ++format) {
        for (const unsigned dimensions : {0U, (3U << 20U) | (2U << 23U)}) {
            // Nonzero base/palette exercises register scaling. Indexed textures
            // cross a slot boundary; direct texture bytes exercise both alpha bits.
            const auto param = (format << 26U) | dimensions | 0x3ffcU;
            for (const auto transparency : {0U, 1U << 29U}) {
                ok &= compare(storage.view(), param | transparency, 3U);
                ok &= compare(storage.view(true), param | transparency, 3U);
            }
        }
    }
    // All possible A3I5/A5I3 byte values, including every alpha and index.
    for (unsigned format : {1U, 6U}) {
        for (unsigned i = 0; i < 256; ++i) storage.textures[i] = static_cast<std::uint8_t>(i);
        ok &= compare(storage.view(), (format << 26U) | (1U << 20U) | (1U << 23U), 0U);
    }
    return ok;
}

bool test_compressed_modes() {
    Storage storage;
    bool ok = true;
    for (const unsigned address : {0x80U, 0x40080U}) {
        const auto metadata = 0x20000U + (address >= 0x40000U ? 0x10000U : 0U) + (address & 0x1ffffU) / 2U;
        for (unsigned tile = 0; tile < 4; ++tile) {
            storage.half(address + tile * 4U, 0xe4e4U);
            storage.half(address + tile * 4U + 2U, 0xe4e4U);
            storage.half(metadata + tile * 2U, static_cast<std::uint16_t>(tile << 14U));
        }
        const auto param = (5U << 26U) | (address / 8U);
        ok &= compare(storage.view(), param, 0U);
        ok &= compare(storage.view(true), param, 0U);
        // Mode 1 averages palette entry 0 and entry 1, yielding an even RGB6
        // channel (unlike ordinary RGB5 expansion). It must not be truncated.
        const auto mixed = decode_texel(storage.view(), param, 0U, 6, 0);
        ok &= check(mixed.a == 31U && mixed.r == 1U && mixed.g == 7U && mixed.b == 62U,
                    "compressed mode-1 interpolation retains even native RGB6");
        ok &= check(decode_texel(storage.view(), param, 0U, 3, 0).a == 0U &&
                    decode_texel(storage.view(), param, 0U, 7, 0).a == 0U &&
                    decode_texel(storage.view(), param, 0U, 3, 4).a == 31U &&
                    decode_texel(storage.view(), param, 0U, 7, 4).a == 31U,
                    "compressed modes 0/1 have transparent index 3; modes 2/3 do not");
    }
    return ok;
}

bool test_sizes_and_footprints() {
    Storage storage;
    bool ok = true;
    constexpr std::array<std::size_t, 8> bytes8{0, 64, 16, 32, 64, 24, 64, 128};
    for (unsigned format = 1; format <= 7; ++format) {
        const auto param = format << 26U;
        ok &= compare(storage.view(), param, 0U);
        ok &= compare(storage.view(), param | (7U << 20U) | (7U << 23U), 0U);
        ok &= check(texture_vram_footprint(param) == bytes8[format], "8x8 VRAM footprint");
        ok &= check(texture_vram_footprint(param | (7U << 20U) | (7U << 23U)) == bytes8[format] * 16384U,
                    "1024x1024 logical VRAM footprint includes compressed metadata");
        ok &= check(texture_vram_footprint(param | (7U << 23U)) == bytes8[format] * 128U,
                    "rectangular texture footprint");
    }
    std::vector<std::uint8_t> rgba{1, 2, 3, 4};
    int width = 123, height = 456;
    ok &= check(!decode_texture_rgba8(storage.view(), 0U, 0U, rgba, width, height) &&
                rgba.empty() && width == 0 && height == 0 && texture_vram_footprint(0U) == 0U,
                "untextured format has no image or footprint");
    ok &= compare(TextureSource{}, 7U << 26U, 0U);
    return ok;
}

bool test_keys() {
    bool ok = true;
    const TextureKey a{0x12345678U, 0x12U, 42U};
    ok &= check(a == TextureKey{a.texture_param, a.palette_base, a.vram_generation}, "texture key equality");
    ok &= check(a != TextureKey{a.texture_param ^ 1U, a.palette_base, a.vram_generation} &&
                a != TextureKey{a.texture_param, a.palette_base ^ 1U, a.vram_generation} &&
                a != TextureKey{a.texture_param, a.palette_base, a.vram_generation + 1U}, "cache identity includes all three fields");
    std::vector<std::uint32_t> keys;
    for (unsigned bit : {16U, 17U, 18U, 19U, 26U, 27U, 28U, 29U})
        keys.push_back(texture_polygon_group_key(1U << bit, 0U, 0U));
    for (unsigned bit : {4U, 5U, 6U, 7U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U})
        keys.push_back(texture_polygon_group_key(0U, 1U << bit, 0U));
    for (unsigned bit = 0; bit < 4; ++bit)
        keys.push_back(texture_polygon_group_key(0U, 0U, 1U << bit));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ok &= check(keys[i] != 0U, "material flags are represented");
        for (std::size_t j = 0; j < i; ++j)
            ok &= check(keys[i] != keys[j], "material bits do not collide");
    }
    ok &= check(texture_polygon_group_key(0xc3f0ffffU, 0x3f00000fU, 0xfffffff0U) == 0U,
                "texture identity, size, transform, polygon ID and light enables do not alter material class");
    return ok;
}

} // namespace

int main() {
    const bool ok = test_all_formats() & test_compressed_modes() & test_sizes_and_footprints() & test_keys();
    if (!ok) return EXIT_FAILURE;
    std::cout << "All whole-texture RGBA8 tests passed\n";
    return EXIT_SUCCESS;
}
