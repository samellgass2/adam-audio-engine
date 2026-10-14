// Core synth parameter contract — the ONE definition every host uses (the
// WASM workshop reads it as JSON; the JUCE AU will build its parameters from
// it). Ids are stable once a Logic session depends on them: append, never
// renumber or rename.
#pragma once

#include <cstddef>
#include <cstdint>

#include "adam/instrument.hpp"

namespace adam::core_synth {

using adam::Mapping;
using adam::ParamSpec;

enum ParamId : uint16_t {
    kOsc1Wave = 0,
    kOsc2Wave,
    kOsc2Semi,
    kOsc2Detune,
    kOscMix,
    kPulseWidth,
    kNoiseLevel,
    kFilterCutoff,
    kFilterResonance,
    kFilterSlope,
    kFilterKeytrack,
    kFilterEnvAmount,
    kFiltAttack,
    kFiltDecay,
    kFiltSustain,
    kFiltRelease,
    kAmpAttack,
    kAmpDecay,
    kAmpSustain,
    kAmpRelease,
    kVelocitySens,
    kLfoWave,
    kLfoRate,
    kLfoPitch,
    kLfoCutoff,
    kGlide,
    kSpread,
    kPolyphony,
    kMasterGain,
    // appended 2026-10-13 (ids above are frozen)
    kFilterDrive,
    kUnisonVoices,
    kUnisonDetune,
    kVibratoDepth,
    kVibratoRate,
    kVibratoDelay,
    kParamCount
};

const ParamSpec& paramSpec(ParamId id);
const ParamSpec* paramSpecs();
size_t paramCount();
int paramIndexForId(const char* id);  // -1 when unknown

float toPhysical(ParamId id, float normalized);
float toNormalized(ParamId id, float physical);
float clampPhysical(ParamId id, float physical);

// The whole contract as JSON (built once; static storage).
const char* paramsJson();

}  // namespace adam::core_synth
