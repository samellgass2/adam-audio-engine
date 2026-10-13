// Small real-time helpers: parameter smoothing, LFO, DC blocker.
#pragma once

#include <cmath>

namespace adam::dsp {

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }
inline float midiToHz(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

// One-pole smoother: reaches ~95% of a step in `ms`.
class Smoother {
public:
    void setup(float sampleRate, float ms) {
        coef_ = ms <= 0.0f ? 1.0f : 1.0f - std::exp(-3.0f / (ms * 0.001f * sampleRate));
    }
    void snap(float v) { value_ = target_ = v; }
    void setTarget(float v) { target_ = v; }
    float next() {
        value_ += (target_ - value_) * coef_;
        if (std::fabs(target_ - value_) < 1e-7f) value_ = target_;
        return value_;
    }
    float value() const { return value_; }
    float target() const { return target_; }

private:
    float coef_ = 1.0f, value_ = 0.0f, target_ = 0.0f;
};

class Lfo {
public:
    void reset() { phase_ = 0.0f; }
    // shape 0 = sine, 1 = triangle; output -1..1
    float next(float rateHz, float sampleRate, int shape) {
        const float t = phase_;
        phase_ += rateHz / sampleRate;
        phase_ -= std::floor(phase_);
        if (shape == 1) return t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
        return std::sin(6.28318530718f * t);
    }

private:
    float phase_ = 0.0f;
};

// 5 Hz one-pole DC blocker.
class DcBlocker {
public:
    void setup(float sampleRate) { r_ = 1.0f - 6.28318530718f * 5.0f / sampleRate; }
    float process(float x) {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = std::fabs(y) > 1e-20f ? y : 0.0f;
        return y1_;
    }

private:
    float r_ = 0.999f, x1_ = 0.0f, y1_ = 0.0f;
};

}  // namespace adam::dsp
