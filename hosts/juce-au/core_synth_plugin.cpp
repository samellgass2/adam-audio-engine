// Adam Core Synth as a plugin. No DSP lives here.
#include "core_synth.hpp"
#include "instrument_descriptor.hpp"

using CoreSynthDescriptor = adam::au::InstrumentDescriptor<adam::core_synth::CoreSynth, adam::core_synth::paramSpecs,
                                                           adam::core_synth::kParamCount>;

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new adam::au::AdamInstrumentProcessor<CoreSynthDescriptor>();
}
