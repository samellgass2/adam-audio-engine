// Preset text format, shared by every host (see adam::Instrument::applyPreset):
// one "<param_id> <physical value>" per line, '#' comments, unknown ids
// ignored. Choice params take their index.
#pragma once

#include "core_synth.hpp"

namespace adam::core_synth {

inline int applyPreset(CoreSynth& synth, const char* text, bool fromDefaults = true) {
    return synth.applyPreset(text, fromDefaults);
}

}  // namespace adam::core_synth
