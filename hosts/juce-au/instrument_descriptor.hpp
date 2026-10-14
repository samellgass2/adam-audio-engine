// Generic plugin descriptor: any adam::Instrument + its parameter table.
// One line per instrument (see core_synth_plugin.cpp, drum_kit_plugin.cpp).
#pragma once

#include "AdamInstrumentProcessor.h"
#include "adam/instrument.hpp"

namespace adam::au {

template <class E, const adam::ParamSpec* (*Specs)(), int N>
struct InstrumentDescriptor {
    using Engine = E;
    static int paramCount() { return N; }
    static ParamInfo param(int index) {
        const adam::ParamSpec& s = Specs()[index];
        ParamInfo p{s.id, s.name, s.unit, Kind::Float, s.min, s.max, s.def, {}};
        if (s.mapping == adam::Mapping::Int) p.kind = Kind::Int;
        if (s.mapping == adam::Mapping::Choice) {
            p.kind = Kind::Choice;
            p.choices = juce::StringArray::fromTokens(s.choices, "|", "");
        }
        return p;
    }
    static float toPhysical(int index, float n) { return adam::params::toPhysical(Specs()[index], n); }
    static float toNormalized(int index, float v) { return adam::params::toNormalized(Specs()[index], v); }
};

}  // namespace adam::au
