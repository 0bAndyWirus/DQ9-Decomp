#pragma once

#include <cstdint>

#if defined(_WIN32) && defined(__clang__) && defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace dsr::gpu3d_detail {

constexpr std::uint32_t kAvxStateFeatures = (1U << 26U) | (1U << 27U) | (1U << 28U);
constexpr bool avx2_state_enabled(std::uint32_t leaf1_ecx, std::uint32_t leaf7_ebx,
                                  std::uint64_t xcr0) {
    // XSAVE, OSXSAVE and AVX, followed by AVX2 and OS-enabled XMM/YMM state.
    return (leaf1_ecx & kAvxStateFeatures) == kAvxStateFeatures &&
           (leaf7_ebx & (1U << 5U)) != 0U && (xcr0 & 6U) == 6U;
}

#if defined(_WIN32) && defined(__clang__) && defined(__x86_64__)
__attribute__((target("xsave"))) inline std::uint64_t enabled_xstate() {
    return _xgetbv(0);
}
#endif

inline bool host_supports_avx2() {
#if defined(_WIN32) && defined(__clang__) && defined(__x86_64__)
    // Zig's Windows compiler-rt does not provide __cpu_model or
    // __cpu_indicator_init used by Clang's __builtin_cpu_supports.
    if (__get_cpuid_max(0, nullptr) < 7U) return false;
    unsigned eax, ebx, ecx, edx;
    __cpuid_count(1, 0, eax, ebx, ecx, edx);
    const auto leaf1_ecx = ecx;
    // XGETBV itself is illegal unless the OS has enabled XSAVE.
    if ((leaf1_ecx & kAvxStateFeatures) != kAvxStateFeatures) return false;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return avx2_state_enabled(leaf1_ecx, ebx, enabled_xstate());
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

} // namespace dsr::gpu3d_detail
