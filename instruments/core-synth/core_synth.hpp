// Core synth: a polyphonic subtractive synthesizer.
//   MIDI -> voice allocator -> 2 osc + noise -> resonant LP (+ filter env, LFO)
//        -> amp ADSR -> stereo pan -> master gain -> DC block -> limiter
// Host-agnostic: no allocation, I/O or locking after prepare(). Hosts call
// noteOn/noteOff/setParam between render() calls, splitting a block at an
// event's sample offset for sample-accurate timing.
#pragma once

#include <array>
#include <cstdint>

#include "adam/dsp/envelope.hpp"
#include "adam/dsp/filter.hpp"
#include "adam/dsp/limiter.hpp"
#include "adam/dsp/oscillator.hpp"
#include "adam/dsp/util.hpp"
#include "params.hpp"

namespace adam::core_synth {

constexpr int kMaxVoices = 16;
constexpr int kMaxUnison = 7;

using Meter = adam::Meter;

class CoreSynth : public adam::Instrument {
public:
    CoreSynth();

    const ParamSpec* paramSpecs() const override { return core_synth::paramSpecs(); }
    int paramCount() const override { return kParamCount; }
    void setParam(int index, float physical) override { setParam(static_cast<ParamId>(index), physical); }
    float param(int index) const override { return index >= 0 && index < kParamCount ? values_[index] : 0.0f; }

    void prepare(float sampleRate) override;
    void reset() override;

    void setParam(ParamId id, float physical);
    using adam::Instrument::applyPreset;
    float param(ParamId id) const { return values_[id]; }
    void resetParamsToDefaults() override;
    // Jump every smoothed parameter to its target (after loading a patch, so the
    // first note does not glide in from the defaults).
    void snapSmoothing() override;

    void noteOn(int note, int velocity) override;
    void noteOff(int note) override;
    void sustainPedal(bool down) override;
    void allNotesOff() override;   // release everything (with release tails)
    void allSoundOff() override;   // immediate silence (transport stop / panic)

    // Render `frames` samples into separate L/R buffers (overwrites).
    void render(float* outL, float* outR, int frames) override;

    Meter readMeter() override;  // returns and clears peak hold
    int latencySamples() const override { return limiter_.latencySamples(); }
    float sampleRate() const override { return sr_; }

private:
    struct Voice {
        std::array<dsp::Oscillator, kMaxUnison> osc1, osc2;  // [0] = the plain voice
        float vibPhase = 0.0f;      // per-note vibrato LFO
        float vibAgeMs = 0.0f;      // time since note-on (vibrato fade-in)
        dsp::Adsr amp, filt;
        dsp::LowPass filter;
        int note = -1;
        float velocity = 0.0f;
        float pitch = 60.0f;        // current (glides toward note)
        float pan = 0.0f;           // -1..1
        bool held = false;          // key down
        bool sustained = false;     // key up while pedal down
        uint64_t age = 0;           // note-on order
        bool active() const { return amp.active(); }
    };

    void applyEnvelopeParams();
    Voice* allocate(int note);
    void startVoice(Voice& v, int note, int velocity, bool fresh);

    float sr_ = 48000.0f;
    std::array<float, kParamCount> values_{};
    std::array<dsp::Smoother, kParamCount> smooth_{};
    std::array<Voice, kMaxVoices> voices_{};
    dsp::Noise noise_;
    dsp::Lfo lfo_;
    dsp::DcBlocker dcL_, dcR_;
    dsp::Limiter limiter_;
    uint64_t noteCounter_ = 0;
    bool pedal_ = false;
    bool envDirty_ = true;
    float lastNote_ = -1.0f;
    Meter meter_;
};

}  // namespace adam::core_synth
