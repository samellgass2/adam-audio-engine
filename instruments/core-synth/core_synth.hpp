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

struct Meter {
    float peakL = 0, peakR = 0;   // since the last read
    float rmsL = 0, rmsR = 0;     // over the last render
    float gainReductionDb = 0;    // limiter
    uint64_t backstopHits = 0;    // limiter backstop engagements (should stay 0)
    uint64_t nonFiniteResets = 0; // voices reset after a NaN/Inf
    int activeVoices = 0;
};

class CoreSynth {
public:
    CoreSynth();

    void prepare(float sampleRate);
    void reset();

    void setParam(ParamId id, float physical);
    float param(ParamId id) const { return values_[id]; }
    void resetParamsToDefaults();
    // Jump every smoothed parameter to its target (after loading a patch, so the
    // first note does not glide in from the defaults).
    void snapSmoothing();

    void noteOn(int note, int velocity);
    void noteOff(int note);
    void sustainPedal(bool down);
    void allNotesOff();   // release everything (with release tails)
    void allSoundOff();   // immediate silence (transport stop / panic)

    // Render `frames` samples into separate L/R buffers (overwrites).
    void render(float* outL, float* outR, int frames);

    Meter readMeter();  // returns and clears peak hold
    int latencySamples() const { return limiter_.latencySamples(); }
    float sampleRate() const { return sr_; }

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
