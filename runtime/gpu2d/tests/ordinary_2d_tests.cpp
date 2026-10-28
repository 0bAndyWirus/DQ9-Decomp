#include "../src/ordinary_2d.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
struct Color { std::uint8_t r, g, b, alpha; };

void require(bool condition, const char* what, unsigned a, unsigned b, unsigned c, unsigned d) {
    if (!condition) {
        std::fprintf(stderr, "%s: %u %u %u %u\n", what, a, b, c, d);
        std::abort();
    }
}

void arithmetic() {
    // Every raw BLDALPHA value pair and every RGB5 channel pair; each of the
    // three color fields is checked with a different source/target permutation.
    for (unsigned a = 0; a < 32; ++a) for (unsigned b = 0; b < 32; ++b)
    for (unsigned s = 0; s < 32; ++s) for (unsigned d = 0; d < 32; ++d) {
        const Color src{static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(31-s),
                        static_cast<std::uint8_t>(d), 7};
        const Color dst{static_cast<std::uint8_t>(d), static_cast<std::uint8_t>(31-d),
                        static_cast<std::uint8_t>(s), 9};
        const auto result = dsr::cleanroom::ordinary_blend(src, dst, a, b);
        const auto calc = [a,b](unsigned x, unsigned y) {
            return std::min(31U, (x * std::min(a, 16U) + y * std::min(b, 16U)) / 16U);
        };
        require(result.r == calc(s,d) && result.g == calc(31-s,31-d) &&
                result.b == calc(d,s) && result.alpha == 31, "blend", a,b,s,d);
    }
    // Brightness input is already clamped by the register reader.
    for (unsigned f = 0; f <= 16; ++f) for (unsigned v = 0; v < 32; ++v) {
        const Color c{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(31-v),
                      static_cast<std::uint8_t>(v), 0};
        const auto up = dsr::cleanroom::ordinary_brighten(c, f);
        const auto down = dsr::cleanroom::ordinary_darken(c, f);
        require(up.r == v + (31-v)*f/16 && up.g == 31-v + v*f/16 && up.b == up.r && up.alpha == 31,
                "brighten", f,v,0,0);
        require(down.r == v - v*f/16 && down.g == 31-v - (31-v)*f/16 && down.b == down.r && down.alpha == 31,
                "darken", f,v,0,0);
    }
}

void selection() {
    for (unsigned layer = 0; layer < 6; ++layer) for (unsigned below = 0; below < 6; ++below)
    for (unsigned first = 0; first < 64; ++first) for (unsigned second = 0; second < 64; ++second)
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned window = 0; window < 2; ++window)
    for (unsigned semi = 0; semi < 2; ++semi) {
        const bool target = layer != below && ((second >> below) & 1U);
        unsigned expected = 0;
        if (layer == 4 && semi && target) expected = 1;
        else if (window && ((first >> layer) & 1U)) {
            if (mode == 1 && target) expected = 1;
            if (mode == 2 || mode == 3) expected = mode;
        }
        require(dsr::cleanroom::ordinary_effect(layer, below, semi, window, mode, first, second) == expected,
                "selection", layer,below,first,second * 16 + mode * 4 + window * 2 + semi);
    }
    for (unsigned first = 0; first < 64; ++first) for (unsigned second = 0; second < 64; ++second)
    for (unsigned mode = 0; mode < 4; ++mode) {
        require(dsr::cleanroom::ordinary_copy_only(mode, first, second) ==
                (second == 0 && (mode == 0 || first == 0)), "copy", mode,first,second,0);
        const unsigned expected = (first & 32U) && mode >= 2 ? mode : 0;
        require(dsr::cleanroom::ordinary_backdrop_effect(mode, first) == expected,
                "backdrop", mode,first,0,0);
    }
}
}

int main() { arithmetic(); selection(); }
