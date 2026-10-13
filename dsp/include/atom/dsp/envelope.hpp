// Analog-style ADSR: exponential (RC) segments, retriggerable from the current
// level so a stolen or re-struck voice never jumps (no click).
#pragma once

#include <cmath>

namespace atom::dsp {

class Adsr {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }

    // Times in ms (clamped >= 0.1 ms), sustain 0..1.
    void set(float attackMs, float decayMs, float sustain, float releaseMs) {
        sustain_ = sustain < 0.0f ? 0.0f : (sustain > 1.0f ? 1.0f : sustain);
        // Attack overshoots to 1.2 and stops at 1.0, like a charging capacitor:
        // reaching 1.0 takes ~attackMs.
        attackCoef_ = coef(attackMs, std::log(1.2f / 0.2f));
        // Decay/release: reach ~-60 dB of the gap in the given time.
        decayCoef_ = coef(decayMs, 6.9f);
        releaseCoef_ = coef(releaseMs, 6.9f);
    }

    void gate(bool on) {
        if (on) {
            stage_ = Stage::Attack;
        } else if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
        }
    }

    void kill() {
        stage_ = Stage::Idle;
        level_ = 0.0f;
    }

    float next() {
        switch (stage_) {
            case Stage::Idle: level_ = 0.0f; break;
            case Stage::Attack:
                level_ += (1.2f - level_) * attackCoef_;
                if (level_ >= 1.0f) {
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                }
                break;
            case Stage::Decay:
                level_ += (sustain_ - level_) * decayCoef_;
                if (std::fabs(level_ - sustain_) < 1e-4f) {
                    level_ = sustain_;
                    stage_ = Stage::Sustain;
                }
                break;
            case Stage::Sustain: level_ = sustain_; break;
            case Stage::Release:
                level_ += (0.0f - level_) * releaseCoef_;
                if (level_ < 1e-4f) {
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                }
                break;
        }
        return level_;
    }

    Stage stage() const { return stage_; }
    float level() const { return level_; }
    bool active() const { return stage_ != Stage::Idle; }

private:
    float coef(float ms, float logRatio) const {
        const float samples = (ms < 0.1f ? 0.1f : ms) * 0.001f * sr_;
        return 1.0f - std::exp(-logRatio / samples);
    }

    float sr_ = 48000.0f;
    float sustain_ = 0.7f;
    float attackCoef_ = 1.0f, decayCoef_ = 0.01f, releaseCoef_ = 0.01f;
    float level_ = 0.0f;
    Stage stage_ = Stage::Idle;
};

}  // namespace atom::dsp
