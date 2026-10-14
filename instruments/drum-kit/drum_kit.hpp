// Drum kit: a synthesized kit with "liveness". Classic analog-style recipes
// (808/909 family), each piece its own voice:
//   kick  — sine with a fast pitch drop + a click, soft-saturated
//   snare — two tuned body modes + band-passed noise ("snappy")
//   clap  — band-passed noise in three quick bursts then a tail
//   hats  — six detuned square oscillators (808 ratios), high-passed; the
//           closed hat chokes the open one
//   toms  — sine with a small pitch drop, three pitches
//   rim   — a short high mode + noise tick
// Liveness: velocity changes TIMBRE (brighter, punchier when hit harder) as
// well as level, and `humanize` varies each hit a little (tune, decay, level,
// noise) from a deterministic per-hit generator — so renders stay
// reproducible while repeated hits do not sound machine-identical.
//
// General MIDI-style map: 35/36 kick, 37 rim, 38/40 snare, 39 clap, 42/44
// closed hat, 46 open hat, 41/43 low tom, 45/47 mid tom, 48/50 high tom.
#pragma once

#include <array>
#include <cstdint>

#include "adam/dsp/filter.hpp"
#include "adam/dsp/limiter.hpp"
#include "adam/dsp/oscillator.hpp"
#include "adam/dsp/util.hpp"
#include "adam/instrument.hpp"

namespace adam::drum_kit {

enum ParamId : uint16_t {
    kKickTune = 0,
    kKickDecay,
    kKickPunch,
    kKickClick,
    kKickLevel,
    kSnareTune,
    kSnareDecay,
    kSnareSnappy,
    kSnareLevel,
    kClapDecay,
    kClapLevel,
    kHatTune,
    kHatClosedDecay,
    kHatOpenDecay,
    kHatLevel,
    kTomTune,
    kTomDecay,
    kTomLevel,
    kRimLevel,
    kHumanize,
    kVelocityTimbre,
    kSpread,
    kKitGain,
    kParamCount
};

const ParamSpec* paramSpecs();
const char* paramsJson();

enum Piece : uint8_t { Kick, Snare, Clap, HatClosed, HatOpen, TomLow, TomMid, TomHigh, Rim, kPieceCount };

// MIDI note -> piece (or -1).
int pieceForNote(int note);

class DrumKit : public adam::Instrument {
public:
    DrumKit();

    const ParamSpec* paramSpecs() const override { return drum_kit::paramSpecs(); }
    int paramCount() const override { return kParamCount; }

    void prepare(float sampleRate) override;
    void reset() override;
    void setParam(int index, float physical) override;
    float param(int index) const override { return index >= 0 && index < kParamCount ? values_[index] : 0.0f; }
    void resetParamsToDefaults() override;
    void snapSmoothing() override {}

    void noteOn(int note, int velocity) override;
    void noteOff(int) override {}  // one-shots: a hit plays out
    void sustainPedal(bool) override {}
    void allNotesOff() override {}
    void allSoundOff() override;

    void render(float* outL, float* outR, int frames) override;
    Meter readMeter() override;
    int latencySamples() const override { return limiter_.latencySamples(); }
    float sampleRate() const override { return sr_; }

private:
    struct Voice {
        bool active = false;
        float t = 0;          // seconds since the hit
        float vel = 1;        // 0..1
        float bright = 1;     // velocity-timbre factor
        float tuneMul = 1;    // humanize
        float decayMul = 1;
        float levelMul = 1;
        float phase = 0, phase2 = 0;
        float env = 0;
        std::array<float, 6> hatPhase{};
        dsp::Svf f1, f2;
        dsp::Noise noise;
    };

    float renderPiece(int piece, Voice& v);
    float rand01();  // deterministic per-hit variation

    float sr_ = 48000.0f;
    std::array<float, kParamCount> values_{};
    std::array<Voice, kPieceCount> voices_{};
    dsp::DcBlocker dcL_, dcR_;
    dsp::Limiter limiter_;
    uint32_t rng_ = 0x2545F491u;
    Meter meter_;
};

}  // namespace adam::drum_kit
