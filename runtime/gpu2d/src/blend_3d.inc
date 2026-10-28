namespace dsr::cleanroom {

// Project RGB6-over-RGB5 compatibility: docs/cleanroom/blend-3d.md.
inline std::array<std::uint8_t, 3> blend_3d(
    const std::array<std::uint8_t, 3>& source, std::uint8_t alpha,
    const std::array<std::uint8_t, 3>& target) {
    std::array<std::uint8_t, 3> result{};
    for (unsigned channel = 0; channel < 3; ++channel) {
        result[channel] = static_cast<std::uint8_t>(
            (source[channel] * (alpha + 1u) + 2u * target[channel] * (31u - alpha)) / 64u);
    }
    return result;
}

} // namespace dsr::cleanroom
