// The generic JUCE wrapper: any Adam engine + its parameter table -> a plugin
// (AUv2 for Logic; Standalone for quick checks). Thin by design — no DSP here.
//
// An engine plugs in through a Descriptor:
//   struct MyDescriptor {
//       using Engine = ...;                 // prepare/setParam/noteOn/noteOff/
//                                           // sustainPedal/allNotesOff/
//                                           // allSoundOff/render/latencySamples
//       static int paramCount();
//       static adam::au::ParamInfo param(int index);
//       static float toPhysical(int index, float normalized);
//       static float toNormalized(int index, float physical);
//   };
// Parameter ids, names, units, ranges and the normalized<->physical mapping
// all come from the engine's contract, so Logic automation and the workshop
// address the same physical values.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <vector>

namespace adam::au {

enum class Kind { Float, Int, Choice };

struct ParamInfo {
    const char* id;
    const char* name;
    const char* unit;
    Kind kind;
    float min, max, def;
    juce::StringArray choices;
};

template <class Descriptor>
class AdamInstrumentProcessor : public juce::AudioProcessor {
public:
    using Engine = typename Descriptor::Engine;

    AdamInstrumentProcessor()
        : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          state_(*this, nullptr, "ADAM_STATE", makeLayout()) {
        const int n = Descriptor::paramCount();
        raw_.resize(static_cast<size_t>(n));
        last_.assign(static_cast<size_t>(n), std::numeric_limits<float>::quiet_NaN());
        for (int i = 0; i < n; ++i)
            raw_[static_cast<size_t>(i)] = state_.getRawParameterValue(Descriptor::param(i).id);
    }

    // ----------------------------------------------------------- lifecycle
    void prepareToPlay(double sampleRate, int /*maxBlock*/) override {
        engine_.prepare(static_cast<float>(sampleRate));
        std::fill(last_.begin(), last_.end(), std::numeric_limits<float>::quiet_NaN());
        pushParams();
        engine_.snapSmoothing();  // a reopened project starts on its patch
        setLatencySamples(engine_.latencySamples());
    }

    void releaseResources() override { engine_.allSoundOff(); }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    // ---------------------------------------------------------------- audio
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        pushParams();
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : scratch_.data();
        const int frames = buffer.getNumSamples();
        int done = 0;
        // render up to each MIDI event, apply it: sample-accurate
        for (const auto metadata : midi) {
            const int pos = juce::jlimit(done, frames, metadata.samplePosition);
            renderSpan(l, r, done, pos);
            done = pos;
            handle(metadata.getMessage());
        }
        renderSpan(l, r, done, frames);
    }

    // ----------------------------------------------------------- the rest
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 15.0; }  // longest release
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override {
        if (auto xml = state_.copyState().createXml()) copyXmlToBinary(*xml, dest);
    }

    void setStateInformation(const void* data, int size) override {
        if (auto xml = getXmlFromBinary(data, size))
            if (xml->hasTagName(state_.state.getType())) state_.replaceState(juce::ValueTree::fromXml(*xml));
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        for (int i = 0; i < Descriptor::paramCount(); ++i) {
            const ParamInfo p = Descriptor::param(i);
            const juce::ParameterID pid{p.id, 1};
            const auto attrs = juce::AudioParameterFloatAttributes().withLabel(p.unit);
            switch (p.kind) {
                case Kind::Choice:
                    layout.add(std::make_unique<juce::AudioParameterChoice>(
                        pid, p.name, p.choices, static_cast<int>(p.def - p.min)));
                    break;
                case Kind::Int:
                    layout.add(std::make_unique<juce::AudioParameterInt>(
                        pid, p.name, static_cast<int>(p.min), static_cast<int>(p.max), static_cast<int>(p.def),
                        juce::AudioParameterIntAttributes().withLabel(p.unit)));
                    break;
                case Kind::Float:
                default: {
                    // the engine's own mapping, so 50% here = 50% in the workshop
                    juce::NormalisableRange<float> range(
                        p.min, p.max,
                        [i](float, float, float n) { return Descriptor::toPhysical(i, n); },
                        [i](float, float, float v) { return Descriptor::toNormalized(i, v); });
                    layout.add(std::make_unique<juce::AudioParameterFloat>(pid, p.name, range, p.def, attrs));
                    break;
                }
            }
        }
        return layout;
    }

    void pushParams() {
        for (size_t i = 0; i < raw_.size(); ++i) {
            const float v = raw_[i]->load(std::memory_order_relaxed);
            if (v != last_[i]) {
                last_[i] = v;
                const ParamInfo p = Descriptor::param(static_cast<int>(i));
                // AudioParameterChoice stores the index; the engine wants min+index
                engine_.setParam(static_cast<int>(i), p.kind == Kind::Choice ? p.min + v : v);
            }
        }
    }

    void renderSpan(float* l, float* r, int from, int to) {
        while (from < to) {
            const int n = std::min(to - from, kChunk);
            engine_.render(l + from, (r == scratch_.data() ? scratch_.data() : r + from), n);
            from += n;
        }
    }

    void handle(const juce::MidiMessage& m) {
        if (m.isNoteOn()) engine_.noteOn(m.getNoteNumber(), m.getVelocity());
        else if (m.isNoteOff()) engine_.noteOff(m.getNoteNumber());
        else if (m.isSustainPedalOn()) engine_.sustainPedal(true);
        else if (m.isSustainPedalOff()) engine_.sustainPedal(false);
        else if (m.isAllSoundOff()) engine_.allSoundOff();
        else if (m.isAllNotesOff() || m.isResetAllControllers()) engine_.allNotesOff();
    }

    static constexpr int kChunk = 1024;
    Engine engine_;
    juce::AudioProcessorValueTreeState state_;
    std::vector<std::atomic<float>*> raw_;
    std::vector<float> last_;
    std::array<float, kChunk> scratch_{};
};

}  // namespace adam::au
