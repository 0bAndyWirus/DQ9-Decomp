namespace dsr::cleanroom {

// Project LCD output compatibility: docs/cleanroom/rgb555.md.
constexpr std::uint32_t color5_to_8(std::uint32_t value) {
    return value * 255u / 31u;
}

#if defined(DSR_X86_PACK_LINE)
template<unsigned Mode>
inline __m128i expand_output_channels(__m128i channels, __m128i factor) {
    static_assert(Mode <= 2);
    if constexpr (Mode == 1) {
        const auto distance = _mm_sub_epi16(_mm_set1_epi16(31), channels);
        channels = _mm_add_epi16(channels, _mm_srli_epi16(_mm_mullo_epi16(distance, factor), 4));
    } else if constexpr (Mode == 2) {
        channels = _mm_sub_epi16(channels, _mm_srli_epi16(_mm_mullo_epi16(channels, factor), 4));
    }
    // 255/31 = 8 + 7/31; for 7*c <= 217 the high half of
    // multiplication by ceil(65536/31) gives exact division by 31.
    const auto remainder = _mm_mullo_epi16(channels, _mm_set1_epi16(7));
    return _mm_add_epi16(_mm_slli_epi16(channels, 3),
                         _mm_mulhi_epu16(remainder, _mm_set1_epi16(2115)));
}
#endif

} // namespace dsr::cleanroom
