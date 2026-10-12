#pragma once

#include "texture.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsr::gpu3d_detail {

// Decode row-major, straight-alpha RGBA8 using the software texel decoder.
// RGB expands native RGB6 with (v << 2) | (v >> 4), not RGB5: compressed
// textures can contain even RGB6 values. Alpha expands A5 with (v << 3) |
// (v >> 2). A nearest-filtered normalized sample quantised with round(rgb*63)
// and round(alpha*31) recovers the software texel exactly. The shader must then
// use the software integer shading rules; final opaque DS display colour is
// RGB6 >> 1 expanded to RGB8 with floor(v * 255 / 31).
// Format 0 (untextured) returns false and clears rgba and both dimensions.
// Unmapped VRAM retains TextureSource's zero-read semantics, not an error.
bool decode_texture_rgba8(const TextureSource& source,
                          std::uint32_t texture_param,
                          std::uint32_t palette_base,
                          std::vector<std::uint8_t>& rgba,
                          int& width, int& height);

// Encoded texture data bytes, excluding palettes. Compressed format 5 includes
// both its 2-bit indices and its separate slot-1 metadata (4 + 2 bytes/tile).
// This is a logical footprint; DS address wrapping can alias larger textures.
std::size_t texture_vram_footprint(std::uint32_t texture_param) noexcept;

// Collision-free packed material state (not a texture/cache identity):
// bits 0..1 mode, 2..6 polygon alpha (0 denotes wireframe), 7..9 format,
// 10..13 repeat/flip S/T, 14..15 face flags, 16..20 POLYGON_ATTR bits 11..15,
// 21..24 DISP3DCNT bits 0..3, 25 colour-zero transparency.
// Texture addresses, sizes, palette, polygon ID and generation are deliberately
// excluded: use TextureKey alongside this key. Sorting/order is also separate.
std::uint32_t texture_polygon_group_key(std::uint32_t texture_param,
                                        std::uint32_t polygon_attr,
                                        std::uint32_t disp3dcnt = 0) noexcept;

} // namespace dsr::gpu3d_detail
