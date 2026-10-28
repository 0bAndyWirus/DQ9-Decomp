#include "../src/sound_tables.h"

#include <array>
#include <cstdio>

int main() {
    // Values are the documented IMA step sequence, indexed from zero.
    constexpr std::array<int, 89> steps = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
    };
    constexpr std::array<int, 8> adjust = {-1,-1,-1,-1,2,4,6,8};
    // Bit p specifies the output at phase p, including the all-low duty 7.
    constexpr std::array<unsigned, 8> high_masks = {0x80,0xc0,0xe0,0xf0,0xf8,0xfc,0xfe,0x00};
    for (unsigned i = 0; i < steps.size(); ++i) {
        if (dsr::sound_tables::adpcm_step(i) != steps[i]) {
            std::fprintf(stderr, "ADPCM step mismatch at %u\n", i);
            return 1;
        }
    }
    for (unsigned i = 0; i < adjust.size(); ++i) {
        if (dsr::sound_tables::adpcm_index_adjust(i) != adjust[i]) {
            std::fprintf(stderr, "ADPCM index adjustment mismatch at %u\n", i);
            return 1;
        }
    }
    for (unsigned duty = 0; duty < high_masks.size(); ++duty) {
        for (unsigned phase = 0; phase < 8; ++phase) {
            const bool expected = ((high_masks[duty] >> phase) & 1U) != 0;
            if (dsr::sound_tables::psg_high(duty, phase) != expected) {
                std::fprintf(stderr, "PSG mismatch at duty %u phase %u\n", duty, phase);
                return 1;
            }
        }
    }
    return 0;
}
