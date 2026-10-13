// Final output safety stage: a stereo-linked lookahead peak limiter with an
// intersample-aware detector, then a bounded backstop.
//
// Gain computer: per sample, the gain this sample needs (ceiling / peak) goes
// through a min-hold over the lookahead window, an exponential release, and a
// box average of the same length. Because every held value covering the
// delayed sample is <= what it needs, the averaged gain is too: the limiter,
// not the backstop, does the work. The backstop count is reported so tests can
// prove it stays at zero on real material.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace atom::dsp {

class Limiter {
public:
    static constexpr int kMaxLookahead = 512;

    void setup(float sampleRate, float ceilingDb = -1.0f, float lookaheadMs = 1.5f,
               float releaseMs = 120.0f) {
        ceiling_ = std::pow(10.0f, ceilingDb / 20.0f);
        int la = static_cast<int>(lookaheadMs * 0.001f * sampleRate);
        lookahead_ = la < 1 ? 1 : (la > kMaxLookahead - 2 ? kMaxLookahead - 2 : la);
        releaseCoef_ = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * sampleRate));
        reset();
    }

    void reset() {
        bufL_.fill(0.0f);
        bufR_.fill(0.0f);
        need_.fill(1.0f);
        held_.fill(1.0f);
        hist_.fill(0.0f);
        heldSum_ = static_cast<double>(lookahead_ + 1);
        released_ = 1.0f;
        pos_ = 0;
        gain_ = 1.0f;
        backstopHits_ = 0;
    }

    void process(float& l, float& r) {
        // Detector: the louder channel, and a cubic (4-point) estimate of the
        // midpoint between the previous two samples, catching most intersample
        // overs without oversampling.
        const float x = std::fabs(l) > std::fabs(r) ? l : r;
        hist_[3] = hist_[2];
        hist_[2] = hist_[1];
        hist_[1] = hist_[0];
        hist_[0] = x;
        const float mid = (-hist_[3] + 9.0f * hist_[2] + 9.0f * hist_[1] - hist_[0]) * 0.0625f;
        const float peak = std::fmax(std::fabs(x), std::fabs(mid));
        const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;

        need_[pos_] = need;
        float hold = 1.0f;
        for (int i = 0; i <= lookahead_; ++i) {
            const float v = need_[(pos_ - i + kMaxLookahead) % kMaxLookahead];
            if (v < hold) hold = v;
        }
        // instant attack on the held minimum, exponential release upward
        released_ = hold < released_ ? hold : released_ + (hold - released_) * releaseCoef_;
        // box average over lookahead+1 samples
        const int oldest = (pos_ - lookahead_ - 1 + kMaxLookahead) % kMaxLookahead;
        heldSum_ += released_ - held_[oldest];
        held_[pos_] = released_;
        gain_ = static_cast<float>(heldSum_ / static_cast<double>(lookahead_ + 1));

        bufL_[pos_] = l;
        bufR_[pos_] = r;
        const int out = (pos_ - lookahead_ + kMaxLookahead) % kMaxLookahead;
        l = backstop(bufL_[out] * gain_);
        r = backstop(bufR_[out] * gain_);
        pos_ = (pos_ + 1) % kMaxLookahead;
    }

    float gainReductionDb() const { return gain_ > 1e-9f ? -20.0f * std::log10(gain_) : 180.0f; }
    uint64_t backstopHits() const { return backstopHits_; }
    int latencySamples() const { return lookahead_; }
    float ceiling() const { return ceiling_; }

private:
    float backstop(float v) {
        if (!std::isfinite(v)) {
            ++backstopHits_;
            return 0.0f;
        }
        // tiny tolerance for float rounding in the box average
        const float lim = ceiling_ * 1.0001f;
        if (v > lim) { ++backstopHits_; return ceiling_; }
        if (v < -lim) { ++backstopHits_; return -ceiling_; }
        return v;
    }

    float ceiling_ = 0.891f;
    int lookahead_ = 72;
    float releaseCoef_ = 0.001f;
    float released_ = 1.0f, gain_ = 1.0f;
    double heldSum_ = 73.0;
    std::array<float, kMaxLookahead> bufL_{}, bufR_{}, need_{}, held_{};
    std::array<float, 4> hist_{};
    int pos_ = 0;
    uint64_t backstopHits_ = 0;
};

}  // namespace atom::dsp
