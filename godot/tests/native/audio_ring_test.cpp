#include "audio_ring.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "audio_ring: %s\n", message);
        std::exit(1);
    }
}
}

int main() {
    dq9::AudioRing<4> ring;
    constexpr std::array<std::int16_t, 12> samples{1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6};
    std::array<std::int16_t, 12> output{};
    require(ring.size() == 0, "initially empty");
    ring.push(nullptr, 4, 4);
    ring.push(samples.data(), 0, 4);
    require(ring.size() == 0, "empty input is harmless");
    ring.push(samples.data(), 3, 4);
    require(ring.copy(output.data(), 6) == 3, "copy is bounded by queued frames");
    require(output[0] == 1 && output[5] == -3, "stereo ordering is unchanged");
    const auto snapshot = ring.read_position();
    ring.consume(snapshot, 2);
    ring.push(samples.data() + 6, 3, 4);
    require(ring.size() == 4 && ring.dropped() == 0, "exact capacity wraps without drops");
    ring.copy(output.data(), 4);
    require(output[0] == 3 && output[2] == 4 && output[6] == 6, "wrapped copy remains ordered");
    ring.push(samples.data(), 1, 3);
    require(ring.size() == 3 && ring.dropped() == 2, "latency trimming drops oldest frames");
    const auto after_drop = ring.read_position();
    ring.consume(snapshot, 1);
    require(ring.read_position() == after_drop, "stale snapshot cannot resurrect discarded audio");
    ring.copy(output.data(), 3);
    require(output[0] == 5 && output[2] == 6 && output[4] == 1, "latency trim retains newest PCM");
    require(ring.high_water() == 4 && ring.overflows() == 0, "high water differs from physical overflow");
    require(ring.clear() == 3 && ring.size() == 0, "clear reports discarded backlog");
    ring.push(samples.data(), 6, 4);
    require(ring.size() == 4 && ring.overflows() == 2, "oversized packet stays within physical capacity");
    ring.copy(output.data(), 4);
    require(output[0] == 3 && output[6] == 6, "oversized packet retains newest frames");
    require(ring.trim_to(1) == 3 && ring.size() == 1, "fast-forward release retains only its bridge frame");
    ring.copy(output.data(), 1);
    require(output[0] == 6 && output[1] == -6, "bridge is the newest complete stereo frame");
    require(ring.trim_to(4) == 0 && ring.size() == 1, "trimming cannot add samples");
    ring.consume(ring.read_position(), 99);
    require(ring.size() == 0, "overconsumption cannot advance past producer");
    ring.reset();
    require(ring.size() == 0 && ring.dropped() == 0 && ring.overflows() == 0 && ring.high_water() == 0,
            "new session resets cursors and counters");
    std::puts("AUDIO_RING PASS");
}
