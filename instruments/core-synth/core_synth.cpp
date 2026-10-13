#include "core_synth.hpp"

#include <algorithm>
#include <cmath>

namespace adam::core_synth {

namespace {
// Per-voice level before summing. Two full-scale saws at osc_mix 50% peak near
// 1.0; eight such voices summing in phase would reach ~2.2 here, which the
// limiter catches, while typical chords sit well below the ceiling.
constexpr float kVoiceGain = 0.28f;
}  // namespace

CoreSynth::CoreSynth() { resetParamsToDefaults(); }

void CoreSynth::prepare(float sampleRate) {
    sr_ = sampleRate > 1000.0f ? sampleRate : 48000.0f;
    for (int i = 0; i < kParamCount; ++i) {
        smooth_[i].setup(sr_, paramSpec(static_cast<ParamId>(i)).smoothing_ms);
        smooth_[i].snap(values_[i]);
    }
    for (auto& v : voices_) {
        v.amp.setSampleRate(sr_);
        v.filt.setSampleRate(sr_);
    }
    dcL_.setup(sr_);
    dcR_.setup(sr_);
    limiter_.setup(sr_, -1.0f, 1.5f, 120.0f);
    envDirty_ = true;
    reset();
}

void CoreSynth::reset() {
    for (auto& v : voices_) {
        v.amp.kill();
        v.filt.kill();
        v.filter.reset();
        v.note = -1;
        v.held = v.sustained = false;
    }
    lfo_.reset();
    noise_.seed(0x9E3779B9u);
    limiter_.reset();
    pedal_ = false;
    lastNote_ = -1.0f;
    meter_ = Meter{};
}

void CoreSynth::resetParamsToDefaults() {
    for (int i = 0; i < kParamCount; ++i) {
        values_[i] = paramSpec(static_cast<ParamId>(i)).def;
        smooth_[i].snap(values_[i]);
    }
    envDirty_ = true;
}

void CoreSynth::setParam(ParamId id, float physical) {
    if (id >= kParamCount) return;
    const float v = clampPhysical(id, physical);
    values_[id] = v;
    smooth_[id].setTarget(v);
    if (paramSpec(id).smoothing_ms <= 0.0f) smooth_[id].snap(v);
    if (id >= kFiltAttack && id <= kAmpRelease) envDirty_ = true;
    if (id == kPolyphony) {
        const int limit = static_cast<int>(v);
        for (int i = limit; i < kMaxVoices; ++i)
            if (voices_[i].active()) voices_[i].amp.gate(false), voices_[i].filt.gate(false);
    }
}

void CoreSynth::applyEnvelopeParams() {
    const float fs = smooth_[kFiltSustain].target() * 0.01f;
    const float as = smooth_[kAmpSustain].target() * 0.01f;
    for (auto& v : voices_) {
        v.filt.set(values_[kFiltAttack], values_[kFiltDecay], fs, values_[kFiltRelease]);
        v.amp.set(values_[kAmpAttack], values_[kAmpDecay], as, values_[kAmpRelease]);
    }
    envDirty_ = false;
}

CoreSynth::Voice* CoreSynth::allocate(int note) {
    const int limit = std::clamp(static_cast<int>(values_[kPolyphony]), 1, kMaxVoices);
    // 1) the same note still sounding: re-strike it (no doubled voices)
    for (int i = 0; i < limit; ++i)
        if (voices_[i].active() && voices_[i].note == note) return &voices_[i];
    // 2) a free voice
    for (int i = 0; i < limit; ++i)
        if (!voices_[i].active()) return &voices_[i];
    // 3) steal: the quietest releasing voice, else the oldest held one
    Voice* best = nullptr;
    for (int i = 0; i < limit; ++i) {
        Voice& v = voices_[i];
        if (v.amp.stage() == dsp::Adsr::Stage::Release && (!best || v.amp.level() < best->amp.level()))
            best = &v;
    }
    if (best) return best;
    for (int i = 0; i < limit; ++i)
        if (!best || voices_[i].age < best->age) best = &voices_[i];
    return best;
}

void CoreSynth::startVoice(Voice& v, int note, int velocity, bool fresh) {
    v.note = note;
    v.velocity = std::clamp(velocity, 1, 127) / 127.0f;
    v.held = true;
    v.sustained = false;
    v.age = ++noteCounter_;
    const bool glide = values_[kGlide] > 1.0f && lastNote_ >= 0.0f;
    if (fresh) {
        v.filter.reset();
        // deterministic per-note phases keep renders reproducible
        v.osc1.reset(0.0f);
        v.osc2.reset(0.25f);
        v.pitch = glide ? lastNote_ : static_cast<float>(note);
    } else if (!glide) {
        v.pitch = static_cast<float>(note);
    }
    // alternate voices left/right; amount scaled by `spread` at render time
    const int slot = static_cast<int>(&v - voices_.data());
    v.pan = (slot % 2 == 0 ? -1.0f : 1.0f) * (0.35f + 0.65f * ((slot / 2) % 4) / 3.0f);
    v.amp.gate(true);
    v.filt.gate(true);
    lastNote_ = static_cast<float>(note);
}

void CoreSynth::noteOn(int note, int velocity) {
    if (note < 0 || note > 127) return;
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    if (envDirty_) applyEnvelopeParams();
    Voice* v = allocate(note);
    if (!v) return;
    startVoice(*v, note, velocity, !v->active());
}

void CoreSynth::noteOff(int note) {
    for (auto& v : voices_) {
        if (v.note != note || !v.held) continue;
        v.held = false;
        if (pedal_) {
            v.sustained = true;
        } else {
            v.amp.gate(false);
            v.filt.gate(false);
        }
    }
}

void CoreSynth::sustainPedal(bool down) {
    pedal_ = down;
    if (down) return;
    for (auto& v : voices_) {
        if (v.sustained) {
            v.sustained = false;
            v.amp.gate(false);
            v.filt.gate(false);
        }
    }
}

void CoreSynth::allNotesOff() {
    pedal_ = false;
    for (auto& v : voices_) {
        v.held = v.sustained = false;
        v.amp.gate(false);
        v.filt.gate(false);
    }
}

void CoreSynth::allSoundOff() {
    for (auto& v : voices_) {
        v.amp.kill();
        v.filt.kill();
        v.filter.reset();
        v.held = v.sustained = false;
        v.note = -1;
    }
    pedal_ = false;
}

void CoreSynth::render(float* outL, float* outR, int frames) {
    if (envDirty_) applyEnvelopeParams();
    const int limit = std::clamp(static_cast<int>(values_[kPolyphony]), 1, kMaxVoices);
    const dsp::Wave w1 = static_cast<dsp::Wave>(static_cast<int>(values_[kOsc1Wave]));
    const dsp::Wave w2 = static_cast<dsp::Wave>(static_cast<int>(values_[kOsc2Wave]));
    const bool steep = values_[kFilterSlope] >= 0.5f;
    const int lfoShape = static_cast<int>(values_[kLfoWave]);
    const float semi2 = values_[kOsc2Semi];
    const float glideMs = values_[kGlide];
    const float glideCoef = glideMs > 1.0f ? 1.0f - std::exp(-3.0f / (glideMs * 0.001f * sr_)) : 1.0f;
    const float invSr = 1.0f / sr_;

    double sumL2 = 0.0, sumR2 = 0.0;
    float peakL = meter_.peakL, peakR = meter_.peakR;

    for (int n = 0; n < frames; ++n) {
        // per-sample smoothed controls
        const float detune = smooth_[kOsc2Detune].next();
        const float mix = smooth_[kOscMix].next() * 0.01f;
        const float pw = smooth_[kPulseWidth].next() * 0.01f;
        const float noiseLvl = smooth_[kNoiseLevel].next() * 0.01f;
        const float cutoff = smooth_[kFilterCutoff].next();
        const float reso = smooth_[kFilterResonance].next() * 0.01f;
        const float keytrack = smooth_[kFilterKeytrack].next() * 0.01f;
        const float envAmt = smooth_[kFilterEnvAmount].next();
        smooth_[kFiltSustain].next();
        smooth_[kAmpSustain].next();
        const float lfoRate = smooth_[kLfoRate].next();
        const float lfoPitch = smooth_[kLfoPitch].next();
        const float lfoCut = smooth_[kLfoCutoff].next();
        const float spread = smooth_[kSpread].next() * 0.01f;
        const float master = dsp::dbToGain(smooth_[kMasterGain].next());

        const float lfo = lfo_.next(lfoRate, sr_, lfoShape);
        const float lfoSemis = lfo * lfoPitch * 0.01f;
        const float noise = noise_.next();
        const float logCut = std::log2(cutoff);

        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < limit; ++i) {
            Voice& v = voices_[i];
            if (!v.active()) continue;
            v.pitch += (static_cast<float>(v.note) - v.pitch) * glideCoef;
            const float p1 = v.pitch + lfoSemis;
            const float p2 = p1 + semi2 + detune * 0.01f;
            const float dt1 = dsp::midiToHz(p1) * invSr;
            const float dt2 = dsp::midiToHz(p2) * invSr;
            float s = (1.0f - mix) * v.osc1.next(w1, dt1, pw) + mix * v.osc2.next(w2, dt2, pw);
            s += noiseLvl * noise * 0.7f;

            const float fenv = v.filt.next();
            const float octaves = envAmt * fenv + lfoCut * lfo + keytrack * (v.pitch - 60.0f) / 12.0f;
            const float fc = std::exp2(logCut + octaves);
            s = v.filter.process(s, fc, reso, steep, sr_);

            const float vel = 1.0f - values_[kVelocitySens] * 0.01f * (1.0f - v.velocity * v.velocity);
            s *= v.amp.next() * vel * kVoiceGain;

            if (!std::isfinite(s)) {
                // recover the voice instead of poisoning the mix
                v.amp.kill();
                v.filt.kill();
                v.filter.reset();
                ++meter_.nonFiniteResets;
                continue;
            }
            const float pan = v.pan * spread;  // equal-power
            const float angle = (pan + 1.0f) * 0.785398163f;
            l += s * std::cos(angle) * 1.41421356f;
            r += s * std::sin(angle) * 1.41421356f;
        }

        l = dcL_.process(l * master);
        r = dcR_.process(r * master);
        limiter_.process(l, r);
        outL[n] = l;
        outR[n] = r;
        sumL2 += static_cast<double>(l) * l;
        sumR2 += static_cast<double>(r) * r;
        peakL = std::fmax(peakL, std::fabs(l));
        peakR = std::fmax(peakR, std::fabs(r));
    }

    int active = 0;
    for (int i = 0; i < kMaxVoices; ++i) active += voices_[i].active() ? 1 : 0;
    meter_.activeVoices = active;
    meter_.peakL = peakL;
    meter_.peakR = peakR;
    if (frames > 0) {
        meter_.rmsL = static_cast<float>(std::sqrt(sumL2 / frames));
        meter_.rmsR = static_cast<float>(std::sqrt(sumR2 / frames));
    }
    meter_.gainReductionDb = limiter_.gainReductionDb();
    meter_.backstopHits = limiter_.backstopHits();
}

Meter CoreSynth::readMeter() {
    Meter m = meter_;
    meter_.peakL = meter_.peakR = 0.0f;
    return m;
}

}  // namespace adam::core_synth
