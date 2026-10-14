// Core synth parameter contract — the ONE definition every host uses (the
// WASM workshop reads it as JSON; the JUCE AU will build its parameters from
// it). Ids are stable once a Logic session depends on them: append, never
// renumber or rename.
#pragma once

#include <cstddef>
#include <cstdint>

namespace adam::core_synth {

enum class Mapping : uint8_t {
    Linear,  // physical = min + n * (max - min)
    Log,     // physical = min * (max / min)^n           (min > 0)
    Int,     // linear, rounded to whole steps
    Choice,  // index into `choices`, n spread evenly
};

struct ParamSpec {
    const char* id;       // stable machine id, e.g. "filter_cutoff"
    const char* name;     // display name
    const char* unit;     // "Hz", "ms", "cents", "dB", "%", "" ...
    const char* group;    // UI grouping
    Mapping mapping;
    float min;
    float max;
    float def;            // physical default
    float smoothing_ms;   // 0 = applied at once (choices, voice count)
    const char* choices;  // "|"-separated labels for Mapping::Choice
};

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
size_t paramCount();
int paramIndexForId(const char* id);  // -1 when unknown

float toPhysical(ParamId id, float normalized);
float toNormalized(ParamId id, float physical);
float clampPhysical(ParamId id, float physical);

// The whole contract as JSON (built once; static storage).
const char* paramsJson();

}  // namespace adam::core_synth
