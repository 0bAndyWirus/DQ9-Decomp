#include "dsr/session.h"

#include "dsr/gpu2d.h"
#include "dsr/gpu3d.h"

#include <cassert>
#include <vector>

// A full-screen opaque red 3D clear plane on BG0 at scale N: the hi-res overlay must be
// all-red at 256N x 192N, and it must follow POWCNT1 bit 15 (the screen engine A drives).
static void test_compose_hires(int scale, std::uint32_t powcnt1, bool engine_a_on_top) {
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Session session(config);
    auto& gpu2d = session.machine().gpu2d();
    auto& gpu3d = session.machine().gpu3d();
    gpu2d.io_write(0x04000304U, powcnt1, 16);
    constexpr auto display = (1U << 16) | (1U << 8) | (1U << 3); // normal display, BG0 = 3D
    gpu2d.io_write(0x04000000U, display, 32);
    gpu3d.io_write(0x04000350U, (31U << 16) | 31U, 32); // opaque red clear colour
    std::vector<std::uint32_t> out(static_cast<std::size_t>(256 * scale) * (192 * scale), 0U);
    bool on_top = !engine_a_on_top;
    // Nothing is latched before a step, and scale 1 never composes.
    assert(!session.compose_hires(out.data(), scale, &on_top));
    gpu3d.set_output_scale(scale);
    // The first step renders the scaled frame at scanline 214; the next one displays it.
    session.step({});
    session.step({});
    assert(session.compose_hires(out.data(), scale, &on_top));
    assert(on_top == engine_a_on_top);
    for (const std::uint32_t pixel : out) assert(pixel == 0xFFFF0000U);
    // A different requested scale than the latched one is refused, never reinterpreted.
    assert(!session.compose_hires(out.data(), scale % 4 + 1, &on_top));
    gpu3d.set_output_scale(1);
    session.step({});
    assert(!session.compose_hires(out.data(), scale, &on_top));
}

int main() {
    test_compose_hires(2, 0x8207U, true);
    test_compose_hires(3, 0x0207U, false); // engine A on the bottom screen
    dsr::MachineConfig config;
    config.direct_boot = false;
    dsr::Session session(config);
    assert(session.frame_count() == 0);
    assert(session.audio_rate() > 32'000.0 && session.audio_rate() < 33'000.0);
    const auto before = session.ticks();
    session.step({});
    assert(session.frame_count() == 1);
    assert(session.ticks() > before);
    assert(session.screen_top() != nullptr);
    assert(session.screen_bottom() != nullptr);
    assert(session.audio_frames() <= 4096);
    return 0;
}
