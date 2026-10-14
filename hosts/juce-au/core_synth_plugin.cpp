// Adam Core Synth as a plugin: the descriptor that hands the core synth and
// its parameter contract to the generic wrapper. No DSP lives here.
#include "AdamInstrumentProcessor.h"
#include "core_synth.hpp"
#include "params.hpp"

namespace {

namespace cs = adam::core_synth;

struct CoreSynthEngine : cs::CoreSynth {
    void setParam(int index, float physical) { cs::CoreSynth::setParam(static_cast<cs::ParamId>(index), physical); }
};

struct CoreSynthDescriptor {
    using Engine = CoreSynthEngine;

    static int paramCount() { return static_cast<int>(cs::paramCount()); }

    static adam::au::ParamInfo param(int index) {
        const cs::ParamSpec& s = cs::paramSpec(static_cast<cs::ParamId>(index));
        adam::au::ParamInfo p{s.id, s.name, s.unit, adam::au::Kind::Float, s.min, s.max, s.def, {}};
        if (s.mapping == cs::Mapping::Int) p.kind = adam::au::Kind::Int;
        if (s.mapping == cs::Mapping::Choice) {
            p.kind = adam::au::Kind::Choice;
            p.choices = juce::StringArray::fromTokens(s.choices, "|", "");
        }
        return p;
    }

    static float toPhysical(int index, float n) { return cs::toPhysical(static_cast<cs::ParamId>(index), n); }
    static float toNormalized(int index, float v) { return cs::toNormalized(static_cast<cs::ParamId>(index), v); }
};

}  // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new adam::au::AdamInstrumentProcessor<CoreSynthDescriptor>();
}
