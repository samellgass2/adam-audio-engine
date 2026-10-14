// Resonant low-pass: a trapezoidal (zero-delay-feedback) state-variable filter
// after Andrew Simper (Cytomic). Unconditionally stable for any cutoff below
// Nyquist and any damping k > 0, and well behaved under per-sample modulation.
// Two in series give 24 dB/oct; resonance lives in the last stage only.
#pragma once

#include <algorithm>
#include <cmath>

namespace adam::dsp {

class Svf {
public:
    void reset() { ic1_ = ic2_ = 0.0f; }

    // g = tan(pi * fc / fs); k = 1/Q (2 = no resonance, -> 0 = self-osc).
    float lowpass(float x, float g, float k) {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2_;
        const float v1 = a1 * ic1_ + a2 * v3;
        const float v2 = ic2_ + a2 * ic1_ + a3 * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        // flush denormals / recover from a non-finite state
        if (!(std::fabs(ic1_) > 1e-20f) || !std::isfinite(ic1_)) ic1_ = 0.0f;
        if (!(std::fabs(ic2_) > 1e-20f) || !std::isfinite(ic2_)) ic2_ = 0.0f;
        return v2;
    }

    // All three outputs at once (for band/high-pass voices).
    void process(float x, float g, float k, float& lp, float& bp, float& hp) {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2_;
        const float v1 = a1 * ic1_ + a2 * v3;
        const float v2 = ic2_ + a2 * ic1_ + a3 * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        if (!(std::fabs(ic1_) > 1e-20f) || !std::isfinite(ic1_)) ic1_ = 0.0f;
        if (!(std::fabs(ic2_) > 1e-20f) || !std::isfinite(ic2_)) ic2_ = 0.0f;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }

    static float coef(float hz, float sampleRate) {
        const float f = std::min(hz, 0.45f * sampleRate);
        return std::tan(3.14159265359f * std::max(f, 10.0f) / sampleRate);
    }

private:
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

class LowPass {
public:
    void reset() {
        a_.reset();
        b_.reset();
    }

    // cutoffHz is clamped to [16 Hz, 0.45 fs]; resonance 0..1 maps to Q 0.5..~25
    // (k 2.0 -> 0.04), never to self-oscillating instability.
    float process(float x, float cutoffHz, float resonance, bool steep, float sampleRate) {
        const float nyq = 0.45f * sampleRate;
        if (!(cutoffHz > 16.0f)) cutoffHz = 16.0f;
        if (cutoffHz > nyq) cutoffHz = nyq;
        const float g = std::tan(3.14159265359f * cutoffHz / sampleRate);
        float r = resonance < 0.0f ? 0.0f : (resonance > 1.0f ? 1.0f : resonance);
        const float k = 2.0f - 1.96f * std::sqrt(r);
        if (steep) {
            // first stage Butterworth (k = sqrt 2), resonance on the second
            return b_.lowpass(a_.lowpass(x, g, 1.41421356f), g, k);
        }
        return b_.lowpass(x, g, k);
    }

private:
    Svf a_, b_;
};

}  // namespace adam::dsp
