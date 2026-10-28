#pragma once

namespace dsr::sound_tables {
// Preconditions: index in 0..88; magnitude, duty and phase in 0..7.
inline int adpcm_step(unsigned index);
inline int adpcm_index_adjust(unsigned magnitude);
inline bool psg_high(unsigned duty, unsigned phase);
}

#include "sound_tables.inc"
