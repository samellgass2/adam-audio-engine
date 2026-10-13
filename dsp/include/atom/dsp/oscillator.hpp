// Band-limited oscillators (PolyBLEP / PolyBLAMP). Portable, allocation-free,
// deterministic. Phase is in cycles [0, 1).
#pragma once

#include <cmath>
#include <cstdint>

namespace atom::dsp {

enum class Wave : uint8_t { Sine = 0, Triangle = 1, Saw = 2, Square = 3 };

// 2-sample polynomial band-limited step residual (Välimäki/Huovilainen).
inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// Integrated polyBLEP (band-limited ramp residual) for slope discontinuities.
inline float polyBlamp(float t, float dt) {
    if (t < dt) {
        t = t / dt - 1.0f;
        return -1.0f / 3.0f * t * t * t;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return 1.0f / 3.0f * t * t * t;
    }
    return 0.0f;
}

class Oscillator {
public:
    void reset(float phase = 0.0f) { phase_ = phase; }
    float phase() const { return phase_; }

    // dt = frequency / sampleRate (cycles per sample), clamped below Nyquist.
    float next(Wave wave, float dt, float pulseWidth = 0.5f) {
        if (!(dt > 0.0f)) dt = 0.0f;
        if (dt > 0.49f) dt = 0.49f;
        const float t = phase_;
        float out = 0.0f;
        switch (wave) {
            case Wave::Sine: out = std::sin(6.28318530718f * t); break;
            case Wave::Saw: out = 2.0f * t - 1.0f - polyBlep(t, dt); break;
            case Wave::Square: {
                const float pw = pulseWidth < 0.02f ? 0.02f : (pulseWidth > 0.98f ? 0.98f : pulseWidth);
                float t2 = t + 1.0f - pw;
                t2 -= std::floor(t2);
                out = (t < pw ? 1.0f : -1.0f) + polyBlep(t, dt) - polyBlep(t2, dt);
                // keep a narrow pulse centred so its DC does not grow
                out -= 2.0f * pw - 1.0f;
                break;
            }
            case Wave::Triangle: {
                out = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
                float t2 = t + 0.5f;
                t2 -= std::floor(t2);
                out += 4.0f * dt * (polyBlamp(t, dt) - polyBlamp(t2, dt));
                break;
            }
        }
        phase_ += dt;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        return out;
    }

private:
    float phase_ = 0.0f;
};

// Deterministic white noise (xorshift32), uniform in [-1, 1).
class Noise {
public:
    explicit Noise(uint32_t seed = 0x9E3779B9u) : state_(seed ? seed : 1u) {}
    void seed(uint32_t s) { state_ = s ? s : 1u; }
    float next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(state_) * (2.0f / 4294967296.0f) - 1.0f;
    }

private:
    uint32_t state_;
};

}  // namespace atom::dsp
