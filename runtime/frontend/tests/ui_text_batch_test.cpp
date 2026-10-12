#include "ui_toolkit.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

namespace {
unsigned single_calls = 0, batch_calls = 0, rectangles = 0;
}
extern "C" int __real_SDL_RenderFillRect(SDL_Renderer*, const SDL_Rect*);
extern "C" int __real_SDL_RenderFillRects(SDL_Renderer*, const SDL_Rect*, int);
extern "C" int __wrap_SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    ++single_calls;
    return __real_SDL_RenderFillRect(renderer, rect);
}
extern "C" int __wrap_SDL_RenderFillRects(SDL_Renderer* renderer, const SDL_Rect* rects, int count) {
    ++batch_calls;
    rectangles += static_cast<unsigned>(count);
    return __real_SDL_RenderFillRects(renderer, rects, count);
}

int main() {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_ARGB8888);
    assert(surface);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    assert(renderer);
    dsr::ui_detail::UiCanvas canvas(renderer);
    canvas.clear({0, 0, 0, 255});
    assert(canvas.text_width("Aa\r\ng_", 2) == 28);
    assert(canvas.text_width("A\nBBB", 1) == 21);
    assert(canvas.text_width("", 2) == 0);
    assert(canvas.text_width("A", 0) == 7);
    canvas.text(4, 6, "Aa\r\ng_", {255, 255, 255, 255}, 2);
    assert(single_calls == 0 && batch_calls == 1);
    SDL_RenderPresent(renderer);
    // Unmodified BDF rows from Spleen 5x8, upstream revision
    // 5116f8eac7f65da2cfa169626ec43b5d041665fe (BSD-2-Clause):
    // https://github.com/fcambus/spleen/blob/5116f8eac7f65da2cfa169626ec43b5d041665fe/spleen-5x8.bdf
    // Includes distinct lowercase, a descender, and the eighth-row underscore.
    constexpr std::array<std::array<std::uint8_t, 8>, 4> glyphs{{
        {{0x00, 0x60, 0x90, 0x90, 0xF0, 0x90, 0x90, 0x00}}, // A
        {{0x00, 0x00, 0x60, 0x10, 0x70, 0x90, 0x70, 0x00}}, // a
        {{0x00, 0x00, 0x70, 0x90, 0x90, 0x60, 0x10, 0xE0}}, // g
        {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0}}, // _
    }};
    for (int y = 0; y < 64; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < 64; ++x) {
            bool expected = false;
            for (int glyph = 0; glyph < 4; ++glyph) {
                const int left = 6 + (glyph % 2) * 14;
                const int top = 6 + (glyph / 2) * 18;
                const int col = (x - left) / 2, bit = (y - top) / 2;
                if (x >= left && x < left + 10 && y >= top && y < top + 16)
                    expected = (glyphs[glyph][bit] & (0x80u >> col)) != 0;
            }
            assert(row[x] == (expected ? 0xffffffffu : 0xff000000u));
        }
    }
    single_calls = batch_calls = rectangles = 0;
    canvas.text(0, 0, std::string(1000, 'A'), {255, 255, 255, 255}, 1);
    assert(single_calls == 0 && rectangles == 14000);
    assert(batch_calls == (rectangles + 255) / 256); // bounded stack batches, including final partial batch
    canvas.text(0, 0, "", {255, 255, 255, 255}, 1);
    canvas.text(0, 0, "A", {255, 255, 255, 255}, 0);
    assert(batch_calls == (rectangles + 255) / 256);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
}
