// Adam Drums as a plugin. No DSP lives here.
#include "drum_kit.hpp"
#include "instrument_descriptor.hpp"

using DrumKitDescriptor = adam::au::InstrumentDescriptor<adam::drum_kit::DrumKit, adam::drum_kit::paramSpecs,
                                                         adam::drum_kit::kParamCount>;

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new adam::au::AdamInstrumentProcessor<DrumKitDescriptor>();
}
