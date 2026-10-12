#include "texture_rgba.h"

namespace dsr::gpu3d_detail {
namespace {

constexpr int texture_width(std::uint32_t param) {
    return 8 << ((param >> 20U) & 7U);
}

constexpr int texture_height(std::uint32_t param) {
    return 8 << ((param >> 23U) & 7U);
}

constexpr std::uint8_t rgb6_to8(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
}

constexpr std::uint8_t alpha5_to8(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
}

} // namespace

bool decode_texture_rgba8(const TextureSource& source,
                          std::uint32_t texture_param,
                          std::uint32_t palette_base,
                          std::vector<std::uint8_t>& rgba,
                          int& width, int& height) {
    if (((texture_param >> 26U) & 7U) == 0U) {
        rgba.clear();
        width = height = 0;
        return false;
    }
    width = texture_width(texture_param);
    height = texture_height(texture_param);
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    auto* destination = rgba.data();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto texel = decode_texel(source, texture_param, palette_base, x, y);
            destination[0] = rgb6_to8(texel.r);
            destination[1] = rgb6_to8(texel.g);
            destination[2] = rgb6_to8(texel.b);
            destination[3] = alpha5_to8(texel.a);
            destination += 4;
        }
    }
    return true;
}

std::size_t texture_vram_footprint(std::uint32_t texture_param) noexcept {
    const auto pixels = static_cast<std::size_t>(texture_width(texture_param)) *
                        static_cast<std::size_t>(texture_height(texture_param));
    switch ((texture_param >> 26U) & 7U) {
    case 1: case 4: case 6: return pixels;
    case 2: return pixels / 4U;
    case 3: return pixels / 2U;
    case 5: return (pixels / 16U) * 6U;
    case 7: return pixels * 2U;
    default: return 0;
    }
}

std::uint32_t texture_polygon_group_key(std::uint32_t texture_param,
                                        std::uint32_t polygon_attr,
                                        std::uint32_t disp3dcnt) noexcept {
    return ((polygon_attr >> 4U) & 3U) |
           (((polygon_attr >> 16U) & 31U) << 2U) |
           (((texture_param >> 26U) & 7U) << 7U) |
           (((texture_param >> 16U) & 15U) << 10U) |
           (((polygon_attr >> 6U) & 3U) << 14U) |
           (((polygon_attr >> 11U) & 31U) << 16U) |
           ((disp3dcnt & 15U) << 21U) |
           (((texture_param >> 29U) & 1U) << 25U);
}

} // namespace dsr::gpu3d_detail
