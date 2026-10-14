#include "drum_kit.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace adam::drum_kit {

namespace {

// clang-format off
const ParamSpec kSpecs[kParamCount] = {
    {"kick_tune",         "Kick Tune",          "Hz",    "Kick",     Mapping::Log, 30, 120, 50, 0, nullptr},
    {"kick_decay",        "Kick Decay",         "ms",    "Kick",     Mapping::Log, 60, 1500, 420, 0, nullptr},
    {"kick_punch",        "Kick Punch",         "st",    "Kick",     Mapping::Linear, 0, 48, 26, 0, nullptr},
    {"kick_click",        "Kick Click",         "%",     "Kick",     Mapping::Linear, 0, 100, 35, 0, nullptr},
    {"kick_level",        "Kick Level",         "dB",    "Kick",     Mapping::Linear, -24, 6, 0, 0, nullptr},
    {"snare_tune",        "Snare Tune",         "Hz",    "Snare",    Mapping::Log, 120, 400, 190, 0, nullptr},
    {"snare_decay",       "Snare Decay",        "ms",    "Snare",    Mapping::Log, 40, 800, 200, 0, nullptr},
    {"snare_snappy",      "Snare Snappy",       "%",     "Snare",    Mapping::Linear, 0, 100, 60, 0, nullptr},
    {"snare_level",       "Snare Level",        "dB",    "Snare",    Mapping::Linear, -24, 6, -1, 0, nullptr},
    {"clap_decay",        "Clap Decay",         "ms",    "Clap",     Mapping::Log, 60, 1000, 240, 0, nullptr},
    {"clap_level",        "Clap Level",         "dB",    "Clap",     Mapping::Linear, -24, 6, -3, 0, nullptr},
    {"hat_tune",          "Hat Tune",           "%",     "Hats",     Mapping::Linear, 0, 100, 50, 0, nullptr},
    {"hat_closed_decay",  "Closed Hat Decay",   "ms",    "Hats",     Mapping::Log, 15, 300, 55, 0, nullptr},
    {"hat_open_decay",    "Open Hat Decay",     "ms",    "Hats",     Mapping::Log, 100, 2000, 450, 0, nullptr},
    {"hat_level",         "Hat Level",          "dB",    "Hats",     Mapping::Linear, -24, 6, -7, 0, nullptr},
    {"tom_tune",          "Tom Tune",           "Hz",    "Toms",     Mapping::Log, 60, 300, 105, 0, nullptr},
    {"tom_decay",         "Tom Decay",          "ms",    "Toms",     Mapping::Log, 80, 1500, 380, 0, nullptr},
    {"tom_level",         "Tom Level",          "dB",    "Toms",     Mapping::Linear, -24, 6, -3, 0, nullptr},
    {"rim_level",         "Rim Level",          "dB",    "Rim",      Mapping::Linear, -24, 6, -8, 0, nullptr},
    {"humanize",          "Humanize",           "%",     "Feel",     Mapping::Linear, 0, 100, 35, 0, nullptr},
    {"velocity_timbre",   "Velocity Timbre",    "%",     "Feel",     Mapping::Linear, 0, 100, 60, 0, nullptr},
    {"spread",            "Stereo Spread",      "%",     "Output",   Mapping::Linear, 0, 100, 40, 0, nullptr},
    {"kit_gain",          "Kit Level",          "dB",    "Output",   Mapping::Linear, -24, 6, -4, 0, nullptr},
};
// clang-format on

// 808 hat oscillator frequencies (Hz) at hat_tune 50%.
constexpr float kHatFreqs[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
// Stereo position of each piece (-1 L .. 1 R) before `spread`.
constexpr float kPan[kPieceCount] = {0.0f, 0.1f, -0.15f, 0.45f, 0.45f, -0.5f, -0.1f, 0.35f, 0.25f};

inline float expDecay(float t, float ms) { return std::exp(-t * 6.9f / (ms * 0.001f)); }  // -60 dB at `ms`

}  // namespace

const ParamSpec* paramSpecs() { return kSpecs; }

const char* paramsJson() {
    static const std::string json = params::toJson(kSpecs, kParamCount);
    return json.c_str();
}

int pieceForNote(int note) {
    switch (note) {
        case 35: case 36: return Kick;
        case 37: return Rim;
        case 38: case 40: return Snare;
        case 39: return Clap;
        case 42: case 44: return HatClosed;
        case 46: return HatOpen;
        case 41: case 43: return TomLow;
        case 45: case 47: return TomMid;
        case 48: case 50: return TomHigh;
        default: return -1;
    }
}

DrumKit::DrumKit() { resetParamsToDefaults(); }

void DrumKit::prepare(float sampleRate) {
    sr_ = sampleRate > 1000.0f ? sampleRate : 48000.0f;
    dcL_.setup(sr_);
    dcR_.setup(sr_);
    limiter_.setup(sr_, -1.0f, 1.5f, 120.0f);
    reset();
}

void DrumKit::reset() {
    for (auto& v : voices_) {
        v = Voice{};
    }
    rng_ = 0x2545F491u;
    limiter_.reset();
    meter_ = Meter{};
}

void DrumKit::resetParamsToDefaults() {
    for (int i = 0; i < kParamCount; ++i) values_[i] = kSpecs[i].def;
}

void DrumKit::setParam(int index, float physical) {
    if (index < 0 || index >= kParamCount) return;
    values_[index] = params::clamp(kSpecs[index], physical);
}

float DrumKit::rand01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_) / 4294967296.0f;
}

void DrumKit::noteOn(int note, int velocity) {
    const int piece = pieceForNote(note);
    if (piece < 0 || velocity <= 0) return;
    if (piece == HatClosed) voices_[HatOpen].active = false;  // choke
    Voice& v = voices_[piece];
    const float vel = std::clamp(velocity, 1, 127) / 127.0f;
    const float h = values_[kHumanize] * 0.01f;
    const float vt = values_[kVelocityTimbre] * 0.01f;
    v.active = true;
    v.t = 0.0f;
    v.vel = vel;
    // velocity -> timbre: 1 at full velocity, darker/softer below (scaled by vt)
    v.bright = 1.0f - vt * 0.6f * (1.0f - vel);
    // humanize: small, deterministic per-hit variation
    v.tuneMul = std::exp2((rand01() - 0.5f) * 2.0f * h * 0.35f / 12.0f);  // up to +-0.35 st
    v.decayMul = 1.0f + (rand01() - 0.5f) * 2.0f * h * 0.15f;           // up to +-15%
    v.levelMul = std::pow(10.0f, (rand01() - 0.5f) * 2.0f * h * 1.5f / 20.0f);  // up to +-1.5 dB
    // humanize 0 = a drum machine: every hit identical. Above 0, the noise and
    // the hat oscillators start differently each hit too.
    v.noise.seed(h > 0.0f ? 0x9E3779B9u ^ (rng_ * 2654435761u) : 0x9E3779B9u + static_cast<uint32_t>(piece));
    v.phase = 0.0f;
    v.phase2 = 0.0f;
    for (int k = 0; k < 6; ++k) v.hatPhase[k] = h > 0.0f ? rand01() : 0.1f * k;
    v.f1.reset();
    v.f2.reset();
}

void DrumKit::allSoundOff() {
    for (auto& v : voices_) v.active = false;
}

float DrumKit::renderPiece(int piece, Voice& v) {
    const float dt = 1.0f / sr_;
    const float t = v.t;
    float out = 0.0f, amp = 1.0f;
    const float velAmp = 0.25f + 0.75f * v.vel * v.vel;
    switch (piece) {
        case Kick: {
            const float decay = values_[kKickDecay] * v.decayMul;
            const float punch = values_[kKickPunch] * (0.6f + 0.4f * v.bright);
            const float pitchEnv = std::exp(-t / 0.028f);
            const float f = values_[kKickTune] * v.tuneMul * std::exp2(punch * pitchEnv / 12.0f);
            v.phase += f * dt;
            v.phase -= std::floor(v.phase);
            amp = expDecay(t, decay);
            float body = std::sin(6.28318530718f * v.phase) * amp;
            body = std::tanh(1.6f * body) / std::tanh(1.6f);
            float click = 0.0f;
            if (t < 0.006f) {
                float lp, bp, hp;
                v.f1.process(v.noise.next(), dsp::Svf::coef(3500.0f * v.bright, sr_), 1.2f, lp, bp, hp);
                click = bp * expDecay(t, 6.0f) * values_[kKickClick] * 0.02f * v.bright;
            }
            out = (body + click) * dsp::dbToGain(values_[kKickLevel]);
            break;
        }
        case Snare: {
            const float decay = values_[kSnareDecay] * v.decayMul;
            const float f = values_[kSnareTune] * v.tuneMul;
            v.phase += f * dt;
            v.phase -= std::floor(v.phase);
            v.phase2 += f * 1.62f * dt;
            v.phase2 -= std::floor(v.phase2);
            const float bodyEnv = expDecay(t, decay * 0.55f);
            const float body = (std::sin(6.28318530718f * v.phase) + 0.5f * std::sin(6.28318530718f * v.phase2)) *
                               bodyEnv * 0.55f;
            float lp, bp, hp;
            // harder hits: more snare-wire noise, and brighter wires
            v.f1.process(v.noise.next(), dsp::Svf::coef(2600.0f + 3600.0f * v.bright * v.bright, sr_), 0.9f, lp,
                         bp, hp);
            const float noise = (bp + 0.35f * hp * v.bright * v.bright) * expDecay(t, decay) *
                                values_[kSnareSnappy] * 0.016f * (0.35f + 0.65f * v.bright);
            amp = std::max(bodyEnv, expDecay(t, decay));
            out = (body + noise) * dsp::dbToGain(values_[kSnareLevel]);
            break;
        }
        case Clap: {
            const float decay = values_[kClapDecay] * v.decayMul;
            // three quick bursts (~11 ms apart) then the reverberant tail
            float env = 0.0f;
            for (int b = 0; b < 3; ++b) {
                const float tb = t - b * 0.011f;
                if (tb >= 0) env = std::max(env, expDecay(tb, 9.0f));
            }
            const float tail = t > 0.022f ? expDecay(t - 0.022f, decay) * 0.6f : 0.0f;
            amp = std::max(env, tail);
            float lp, bp, hp;
            v.f1.process(v.noise.next(), dsp::Svf::coef(1250.0f * (0.7f + 0.3f * v.bright), sr_), 0.6f, lp, bp, hp);
            out = bp * amp * 1.4f * dsp::dbToGain(values_[kClapLevel]);
            break;
        }
        case HatClosed:
        case HatOpen: {
            const float decay = (piece == HatOpen ? values_[kHatOpenDecay] : values_[kHatClosedDecay]) * v.decayMul;
            const float scale = std::exp2((values_[kHatTune] - 50.0f) / 50.0f) * v.tuneMul;
            float metal = 0.0f;
            for (int k = 0; k < 6; ++k) {
                v.hatPhase[k] += kHatFreqs[k] * scale * dt;
                v.hatPhase[k] -= std::floor(v.hatPhase[k]);
                metal += v.hatPhase[k] < 0.5f ? 1.0f : -1.0f;
            }
            metal *= 1.0f / 6.0f;
            float lp, bp, hp;
            v.f1.process(metal, dsp::Svf::coef(7500.0f + 2500.0f * v.bright, sr_), 0.8f, lp, bp, hp);
            float lp2, bp2, hp2;
            v.f2.process(hp, dsp::Svf::coef(6500.0f, sr_), 1.4f, lp2, bp2, hp2);
            amp = expDecay(t, decay);
            out = (hp2 + 0.25f * bp) * amp * 1.6f * dsp::dbToGain(values_[kHatLevel]);
            break;
        }
        case TomLow:
        case TomMid:
        case TomHigh: {
            const float mul = piece == TomLow ? 1.0f : (piece == TomMid ? 1.335f : 1.78f);
            const float decay = values_[kTomDecay] * v.decayMul * (piece == TomHigh ? 0.8f : 1.0f);
            const float f = values_[kTomTune] * mul * v.tuneMul * std::exp2(5.0f * std::exp(-t / 0.05f) * v.bright / 12.0f);
            v.phase += f * dt;
            v.phase -= std::floor(v.phase);
            amp = expDecay(t, decay);
            float lp, bp, hp;
            v.f1.process(v.noise.next(), dsp::Svf::coef(f * 4.0f, sr_), 1.0f, lp, bp, hp);
            out = (std::sin(6.28318530718f * v.phase) + 0.15f * bp * expDecay(t, 25.0f)) * amp *
                  dsp::dbToGain(values_[kTomLevel]);
            break;
        }
        case Rim: {
            v.phase += 1650.0f * v.tuneMul * dt;
            v.phase -= std::floor(v.phase);
            amp = expDecay(t, 45.0f * v.decayMul);
            float lp, bp, hp;
            v.f1.process(v.noise.next(), dsp::Svf::coef(4000.0f, sr_), 1.0f, lp, bp, hp);
            out = (std::sin(6.28318530718f * v.phase) * 0.8f + bp * 0.6f) * amp * dsp::dbToGain(values_[kRimLevel]);
            break;
        }
        default: break;
    }
    v.t += dt;
    if (amp < 1e-4f && v.t > 0.01f) v.active = false;
    return out * velAmp * v.levelMul;
}

void DrumKit::render(float* outL, float* outR, int frames) {
    const float kit = dsp::dbToGain(values_[kKitGain]) * 0.7f;
    const float spread = values_[kSpread] * 0.01f;
    float gl[kPieceCount], gr[kPieceCount];
    for (int p = 0; p < kPieceCount; ++p) {
        const float angle = (kPan[p] * spread + 1.0f) * 0.785398163f;
        gl[p] = std::cos(angle) * 1.41421356f;
        gr[p] = std::sin(angle) * 1.41421356f;
    }
    double s2l = 0, s2r = 0;
    float peakL = meter_.peakL, peakR = meter_.peakR;
    for (int n = 0; n < frames; ++n) {
        float l = 0.0f, r = 0.0f;
        for (int p = 0; p < kPieceCount; ++p) {
            Voice& v = voices_[p];
            if (!v.active) continue;
            float s = renderPiece(p, v);
            if (!std::isfinite(s)) {
                v.active = false;
                ++meter_.nonFiniteResets;
                continue;
            }
            l += s * gl[p];
            r += s * gr[p];
        }
        l = dcL_.process(l * kit);
        r = dcR_.process(r * kit);
        limiter_.process(l, r);
        outL[n] = l;
        outR[n] = r;
        s2l += double(l) * l;
        s2r += double(r) * r;
        peakL = std::fmax(peakL, std::fabs(l));
        peakR = std::fmax(peakR, std::fabs(r));
    }
    int active = 0;
    for (auto& v : voices_) active += v.active ? 1 : 0;
    meter_.activeVoices = active;
    meter_.peakL = peakL;
    meter_.peakR = peakR;
    if (frames > 0) {
        meter_.rmsL = static_cast<float>(std::sqrt(s2l / frames));
        meter_.rmsR = static_cast<float>(std::sqrt(s2r / frames));
    }
    meter_.gainReductionDb = limiter_.gainReductionDb();
    meter_.backstopHits = limiter_.backstopHits();
}

Meter DrumKit::readMeter() {
    Meter m = meter_;
    meter_.peakL = meter_.peakR = 0.0f;
    return m;
}

}  // namespace adam::drum_kit
