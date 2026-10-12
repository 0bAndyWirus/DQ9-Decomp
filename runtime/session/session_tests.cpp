#include "dsr/session.h"

#include <cassert>

int main() {
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
