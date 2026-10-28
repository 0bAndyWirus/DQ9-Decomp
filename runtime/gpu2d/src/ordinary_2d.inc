#include <algorithm>

namespace dsr::cleanroom {

template <typename Color>
inline Color ordinary_blend(Color source, Color target, std::uint32_t eva, std::uint32_t evb) {
    eva = std::min(eva, 16u);
    evb = std::min(evb, 16u);
    const auto channel = [eva, evb](unsigned s, unsigned d) {
        return std::min(31u, (s * eva + d * evb) / 16u);
    };
    return Color{static_cast<decltype(source.r)>(channel(source.r, target.r)),
                 static_cast<decltype(source.g)>(channel(source.g, target.g)),
                 static_cast<decltype(source.b)>(channel(source.b, target.b)),
                 static_cast<decltype(source.alpha)>(31)};
}

template <typename Color>
inline Color ordinary_brighten(Color color, std::uint32_t factor) {
    const auto channel = [factor](unsigned c) { return c + (31u - c) * factor / 16u; };
    return Color{static_cast<decltype(color.r)>(channel(color.r)),
                 static_cast<decltype(color.g)>(channel(color.g)),
                 static_cast<decltype(color.b)>(channel(color.b)),
                 static_cast<decltype(color.alpha)>(31)};
}

template <typename Color>
inline Color ordinary_darken(Color color, std::uint32_t factor) {
    const auto channel = [factor](unsigned c) { return c - c * factor / 16u; };
    return Color{static_cast<decltype(color.r)>(channel(color.r)),
                 static_cast<decltype(color.g)>(channel(color.g)),
                 static_cast<decltype(color.b)>(channel(color.b)),
                 static_cast<decltype(color.alpha)>(31)};
}

inline unsigned ordinary_effect(unsigned layer, unsigned dst_layer, bool semi_obj,
                                bool effects_allowed, unsigned effect,
                                unsigned target1, unsigned target2) {
    const bool eligible = layer != dst_layer && (target2 & (1u << dst_layer)) != 0;
    if (layer == 4 && semi_obj && eligible) return 1;
    if (!effects_allowed || !(target1 & (1u << layer))) return 0;
    if (effect == 1) return eligible ? 1 : 0;
    return effect == 2 || effect == 3 ? effect : 0;
}

inline bool ordinary_copy_only(unsigned effect, unsigned target1, unsigned target2) {
    return target2 == 0 && (effect == 0 || target1 == 0);
}

inline unsigned ordinary_backdrop_effect(unsigned effect, unsigned target1) {
    return (target1 & (1u << 5)) && (effect == 2 || effect == 3) ? effect : 0;
}

} // namespace dsr::cleanroom
