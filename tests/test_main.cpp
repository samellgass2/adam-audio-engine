// adam-tests: correctness and signal-integrity tests on RENDERED audio.
// Each test states the prediction it checks. No framework: a failed CHECK
// prints the expression and measured values; the process exits non-zero.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "adam/analysis/analysis.hpp"
#include "adam/dsp/limiter.hpp"
#include "adam/dsp/oscillator.hpp"
#include "adam/seq/sequencer.hpp"
#include "c_api.h"
#include "core_synth.hpp"
#include "params.hpp"
#include "preset.hpp"

using namespace adam;
using core_synth::CoreSynth;
using core_synth::ParamId;

static int g_failures = 0, g_checks = 0;
static const char* g_test = "";

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAIL [%s] %s:%d: %s — ", g_test, __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
        }                                                                    \
    } while (0)

// ----------------------------------------------------------------- helpers
constexpr float kSr = 48000.0f;

struct Stereo {
    std::vector<float> l, r;
};

Stereo render(CoreSynth& s, int frames, int block = 128) {
    Stereo out{std::vector<float>(frames), std::vector<float>(frames)};
    for (int i = 0; i < frames; i += block) {
        const int n = std::min(block, frames - i);
        s.render(out.l.data() + i, out.r.data() + i, n);
    }
    return out;
}

CoreSynth makeSynth(float sr = kSr) {
    CoreSynth s;
    s.prepare(sr);
    return s;
}

// A plain, open, unmodulated voice: the easiest thing to measure.
void plainPatch(CoreSynth& s, int wave) {
    s.setParam(core_synth::kOsc1Wave, wave);
    s.setParam(core_synth::kOscMix, 0);
    s.setParam(core_synth::kFilterCutoff, 20000);
    s.setParam(core_synth::kFilterResonance, 0);
    s.setParam(core_synth::kFilterEnvAmount, 0);
    s.setParam(core_synth::kFilterKeytrack, 0);
    s.setParam(core_synth::kAmpAttack, 1);
    s.setParam(core_synth::kAmpSustain, 100);
    s.setParam(core_synth::kSpread, 0);
    s.setParam(core_synth::kMasterGain, -12);
}

float peakOf(const std::vector<float>& x, size_t from = 0) {
    float p = 0;
    for (size_t i = from; i < x.size(); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / std::max<size_t>(1, to - from));
}

// Frequency from interpolated positive-going zero crossings (good to ~0.01 Hz
// on a clean periodic signal over a second).
double zeroCrossingHz(const std::vector<float>& x, size_t from, float sr) {
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = from + 1; i < x.size(); ++i) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            else ++count;
            last = t;
        }
    }
    return count > 0 ? count * sr / (last - first) : 0.0;
}

// Shared analysis (the same code the renderer report and the workshop use).
std::vector<double> powerSpectrum(const std::vector<float>& x, size_t from, size_t n) {
    return analysis::powerSpectrum(x.data() + from, n);
}
using analysis::inharmonicDb;

std::string readFixture(const std::string& rel) {
    std::ifstream f(std::string(ADAM_FIXTURES_DIR) + "/" + rel);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ------------------------------------------------------------------- tests
void test_param_contract() {
    // Every parameter round-trips physical -> normalized -> physical, its
    // default is in range, and non-finite input falls back to the default.
    for (int i = 0; i < core_synth::kParamCount; ++i) {
        const auto id = static_cast<ParamId>(i);
        const auto& s = core_synth::paramSpec(id);
        CHECK(s.def >= s.min && s.def <= s.max, "%s default %g outside [%g,%g]", s.id, s.def, s.min, s.max);
        for (float n : {0.0f, 0.13f, 0.5f, 0.77f, 1.0f}) {
            const float phys = core_synth::toPhysical(id, n);
            const float back = core_synth::toPhysical(id, core_synth::toNormalized(id, phys));
            CHECK(std::fabs(back - phys) <= 1e-3f * std::max(1.0f, std::fabs(phys)), "%s n=%g phys=%g back=%g",
                  s.id, n, phys, back);
        }
        CHECK(core_synth::clampPhysical(id, NAN) == s.def, "%s NaN not defaulted", s.id);
        CHECK(core_synth::paramIndexForId(s.id) == i, "%s id lookup", s.id);
        // hosts (Logic's AU layer) mangle non-ASCII display names
        bool ascii = true;
        for (const char* c = s.name; *c; ++c) ascii = ascii && static_cast<unsigned char>(*c) < 128;
        CHECK(ascii, "%s display name '%s' is not plain ASCII", s.id, s.name);
    }
    const std::string json = core_synth::paramsJson();
    CHECK(json.front() == '[' && json.back() == ']', "json brackets");
    CHECK(json.find("\"filter_cutoff\"") != std::string::npos, "json has filter_cutoff");
}

void test_pitch_tracking() {
    // A sine voice at MIDI n sounds at 440 * 2^((n-69)/12) within 0.05%, at
    // every common sample rate.
    for (float sr : {44100.0f, 48000.0f, 96000.0f}) {
        for (int note : {33, 45, 57, 69, 81, 93}) {
            CoreSynth s = makeSynth(sr);
            plainPatch(s, 0);
            s.noteOn(note, 100);
            const auto out = render(s, static_cast<int>(sr));
            const double want = 440.0 * std::pow(2.0, (note - 69) / 12.0);
            const double got = zeroCrossingHz(out.l, static_cast<size_t>(sr * 0.1), sr);
            CHECK(std::fabs(got - want) / want < 5e-4, "sr=%g note=%d want %.3f got %.3f", sr, note, want, got);
        }
    }
}

double sawAliasDb(bool blep, double freq) {
    // a bare oscillator, so the measurement is of the oscillator alone
    const size_t n = 1 << 15;
    std::vector<float> x(n + 4800);
    dsp::Oscillator o;
    const float dt = static_cast<float>(freq / kSr);
    float ph = 0;
    for (auto& v : x) {
        if (blep) v = o.next(dsp::Wave::Saw, dt);
        else {
            v = 2 * ph - 1;
            ph += dt;
            if (ph >= 1) ph -= 1;
        }
    }
    return inharmonicDb(powerSpectrum(x, 4800, n), freq, kSr, n);
}

void test_saw_is_band_limited() {
    // Prediction: PolyBLEP cuts the aliasing (energy between harmonics) of a
    // high saw by >= 15 dB vs the naive ramp, and keeps it below -30 dB up to
    // G#6.
    for (double f : {1661.2, 3135.96}) {  // G#6, G7
        const double naive = sawAliasDb(false, f), blep = sawAliasDb(true, f);
        std::printf("    saw %.0f Hz: inharmonic naive %.1f dB, polyBLEP %.1f dB\n", f, naive, blep);
        CHECK(blep < naive - 15, "f=%.0f naive %.1f blep %.1f", f, naive, blep);
        // the playable lead range stays below -30 dB; at G7 2-point PolyBLEP
        // sits at ~-30 dB (ledger: higher-order BLEP / 2x oversampling)
        if (f < 2000) CHECK(blep < -30, "f=%.0f blep %.1f", f, blep);
    }
}

void test_triangle_and_square_band_limited() {
    // Prediction: the BLAMP-corrected triangle and BLEP square alias less than
    // their naive forms at a high note.
    const size_t n = 1 << 15;
    const double f = 2489.0;
    const float dt = static_cast<float>(f / kSr);
    for (dsp::Wave w : {dsp::Wave::Triangle, dsp::Wave::Square}) {
        std::vector<float> bl(n + 4800), nv(n + 4800);
        dsp::Oscillator o;
        float ph = 0;
        for (size_t i = 0; i < bl.size(); ++i) {
            bl[i] = o.next(w, dt);
            nv[i] = w == dsp::Wave::Triangle ? (ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph) : (ph < 0.5f ? 1.0f : -1.0f);
            ph += dt;
            if (ph >= 1) ph -= 1;
        }
        const double a = inharmonicDb(powerSpectrum(bl, 4800, n), f, kSr, n);
        const double b = inharmonicDb(powerSpectrum(nv, 4800, n), f, kSr, n);
        std::printf("    %s %.0f Hz: naive %.1f dB, corrected %.1f dB\n",
                    w == dsp::Wave::Triangle ? "tri" : "sqr", f, b, a);
        CHECK(a < b - 6, "wave %d corrected %.1f naive %.1f", int(w), a, b);
    }
}

void test_envelope_timing() {
    // Prediction: attack A ms reaches full level in ~A ms (+-25%); after
    // note-off, release R ms decays ~60 dB in ~R ms (+-25%).
    for (float attack : {20.0f, 200.0f}) {
        for (float release : {100.0f, 800.0f}) {
            CoreSynth s = makeSynth();
            plainPatch(s, 0);
            s.setParam(core_synth::kAmpAttack, attack);
            s.setParam(core_synth::kAmpDecay, 1);
            s.setParam(core_synth::kAmpSustain, 100);
            s.setParam(core_synth::kAmpRelease, release);
            s.setParam(core_synth::kVelocitySens, 0);
            s.noteOn(57, 127);
            const int hold = static_cast<int>(kSr * 1.0);
            auto a = render(s, hold);
            s.noteOff(57);
            auto b = render(s, static_cast<int>(kSr * 2.0));
            // envelope by 2 ms RMS windows
            const int w = 96;
            const double full = rms(a.l, hold - 4800, hold);
            int reach = -1;
            for (int i = 0; i + w < hold; i += w / 2)
                if (rms(a.l, i, i + w) >= 0.95 * full) { reach = i; break; }
            const double reachMs = 1000.0 * reach / kSr;
            CHECK(reach >= 0 && std::fabs(reachMs - attack) <= 0.25 * attack + 3, "attack %g reached at %.1f ms",
                  attack, reachMs);
            int gone = -1;
            for (size_t i = 0; i + w < b.l.size(); i += w / 2)
                if (rms(b.l, i, i + w) <= full * 1e-3) { gone = static_cast<int>(i); break; }
            const double goneMs = 1000.0 * gone / kSr;
            CHECK(gone >= 0 && std::fabs(goneMs - release) <= 0.25 * release + 5, "release %g fell 60 dB at %.1f ms",
                  release, goneMs);
        }
    }
}

void test_polyphony_and_stealing() {
    // Prediction: 8 notes use 8 voices; a 9th steals (still 8); after
    // all-notes-off and the release, no voice is left active.
    CoreSynth s = makeSynth();
    s.setParam(core_synth::kAmpRelease, 50);
    for (int i = 0; i < 8; ++i) s.noteOn(48 + i * 2, 100);
    render(s, 4800);
    CHECK(s.readMeter().activeVoices == 8, "8 voices");
    s.noteOn(80, 100);
    render(s, 4800);
    CHECK(s.readMeter().activeVoices == 8, "9th steals: %d", s.readMeter().activeVoices);
    s.noteOn(80, 100);  // same note again: re-struck, not doubled
    render(s, 480);
    CHECK(s.readMeter().activeVoices == 8, "re-strike doubled");
    s.allNotesOff();
    const auto tail = render(s, static_cast<int>(kSr));
    CHECK(s.readMeter().activeVoices == 0, "stuck voices after all-notes-off: %d", s.readMeter().activeVoices);
    CHECK(peakOf(tail.l, tail.l.size() - 4800) < 1e-5f, "tail not silent");
}

void test_sustain_pedal() {
    CoreSynth s = makeSynth();
    s.setParam(core_synth::kAmpRelease, 30);
    s.sustainPedal(true);
    s.noteOn(60, 100);
    render(s, 480);
    s.noteOff(60);
    render(s, 9600);
    CHECK(s.readMeter().activeVoices == 1, "pedal holds the note");
    s.sustainPedal(false);
    render(s, 9600);
    CHECK(s.readMeter().activeVoices == 0, "pedal up releases");
}

void test_sequencer_timing_and_loop() {
    // Prediction: at 120 bpm / 48 kHz an event at beat 1 lands at sample
    // 24000 exactly; a note crossing the loop end is released at the boundary;
    // offs come before ons at the same time; looping wraps to beat 0.
    seq::Sequencer q;
    const int notes = q.load("bpm 120\nlength 2\nnote 1 5 60 100\nnote 0 1 62 90\nnote 1 0.5 62 90\n");
    CHECK(notes == 3, "parsed %d", notes);
    q.setBpm(120);
    q.setPlaying(true);
    std::vector<std::tuple<long, int, int>> ev;
    long base = 0;
    for (int b = 0; b < 1000; ++b) {  // 128000 samples = 2.67 s
        q.advance(128, 48000.0, [&](int off, int n, int v) { ev.emplace_back(base + off, n, v); });
        base += 128;
    }
    CHECK(std::get<0>(ev[0]) == 0 && std::get<1>(ev[0]) == 62 && std::get<2>(ev[0]) > 0, "first on");
    // at beat 1: note 62 off (from the first) before note 62/60 on
    bool sawOff62 = false, orderOk = true;
    for (auto& e : ev) {
        if (std::get<0>(e) != 24000) continue;
        if (std::get<1>(e) == 62 && std::get<2>(e) == 0) sawOff62 = true;
        else if (std::get<2>(e) > 0 && !sawOff62) orderOk = false;
    }
    CHECK(sawOff62 && orderOk, "off-before-on at sample 24000");
    // note 60 (dur 5) is cut at the loop end, sample 48000
    bool off60AtLoop = false;
    for (auto& e : ev)
        if (std::get<0>(e) == 48000 && std::get<1>(e) == 60 && std::get<2>(e) == 0) off60AtLoop = true;
    CHECK(off60AtLoop, "note crossing loop end released at 48000");
    bool wrapped = false;
    for (auto& e : ev)
        if (std::get<0>(e) == 48000 && std::get<1>(e) == 62 && std::get<2>(e) > 0) wrapped = true;
    CHECK(wrapped, "loop wraps to beat 0 at 48000");
}

void test_patterns_render_clean() {
    // Prediction: every fixture pattern x every preset renders finite audio,
    // never above the -1 dBFS ceiling, the backstop never engages, no voice
    // needs a NaN reset, and nothing is left sounding after stop + release.
    const char* patterns[] = {"pad_chords", "tight_chords", "lead_melody", "staccato",
                              "fast_repeats", "wide_arpeggio", "dense_overlap"};
    const char* presets[] = {"init", "soft_pad", "bright_chords", "pluck"};
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    for (const char* pre : presets) {
        for (const char* pat : patterns) {
            AdamSynth* s = adam_synth_create(kSr);
            adam_synth_apply_preset(s, readFixture(std::string("presets/") + pre + ".preset").c_str());
            CHECK(adam_seq_load(s, readFixture(std::string("patterns/") + pat + ".pat").c_str()) > 0, "%s loads", pat);
            adam_seq_set_bpm(s, adam_seq_pattern_bpm(s));
            adam_seq_play(s, 1);
            const double secs = adam_seq_length(s) * 60.0 / adam_seq_pattern_bpm(s) * 1.5;
            float peak = 0;
            double energy = 0;
            bool finite = true;
            const long frames = static_cast<long>(secs * kSr);
            for (long d = 0; d < frames; d += 128) {
                adam_synth_render(s, 128);
                const float* l = adam_synth_out_left(s);
                const float* r = adam_synth_out_right(s);
                for (int i = 0; i < 128; ++i) {
                    finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
                    peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
                    energy += double(l[i]) * l[i];
                }
            }
            adam_seq_play(s, 0);
            float tailPeak = 0;
            // the longest preset release is 1.8 s; a voice goes idle at -80 dB,
            // ~1.33x its release time, so allow 4 s
            for (int b = 0; b < 1500; ++b) adam_synth_render(s, 128);
            for (int b = 0; b < 40; ++b) {
                adam_synth_render(s, 128);
                tailPeak = std::max(tailPeak, peakOf(std::vector<float>(adam_synth_out_left(s), adam_synth_out_left(s) + 128)));
            }
            const float* m = adam_synth_read_meter(s);
            CHECK(finite, "%s/%s non-finite", pre, pat);
            CHECK(peak <= ceiling * 1.0001f, "%s/%s peak %.4f > ceiling", pre, pat, peak);
            CHECK(m[6] == 0, "%s/%s backstop engaged %g times", pre, pat, m[6]);
            CHECK(m[7] == 0, "%s/%s nan resets %g", pre, pat, m[7]);
            CHECK(energy > 1e-3, "%s/%s silent", pre, pat);
            CHECK(tailPeak < 1e-4f && m[5] == 0, "%s/%s still sounding after stop (peak %g, voices %g)", pre, pat,
                  tailPeak, m[5]);
            adam_synth_destroy(s);
        }
    }
}

void test_adversarial_extremes() {
    // Prediction: random settings across the full ranges (extremes favoured),
    // with dense chords, fast parameter jumps and odd block sizes, never produce
    // a non-finite sample, never exceed the ceiling, never engage the backstop.
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    uint32_t rng = 12345;
    auto rnd = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (rng & 0xFFFFFF) / float(0xFFFFFF);
    };
    for (int trial = 0; trial < 60; ++trial) {
        CoreSynth s = makeSynth(trial % 3 == 0 ? 44100.0f : (trial % 3 == 1 ? 48000.0f : 96000.0f));
        auto randomize = [&]() {
            for (int i = 0; i < core_synth::kParamCount; ++i) {
                float u = rnd();
                if (u < 0.2f) u = 0;
                else if (u > 0.8f) u = 1;
                s.setParam(static_cast<ParamId>(i), core_synth::toPhysical(static_cast<ParamId>(i), u));
            }
        };
        randomize();
        s.setParam(core_synth::kMasterGain, 6);  // worst case gain
        for (int k = 0; k < 16; ++k) s.noteOn(24 + static_cast<int>(rnd() * 90), 1 + static_cast<int>(rnd() * 126));
        std::vector<float> l(1024), r(1024);
        float peak = 0;
        bool finite = true;
        for (int b = 0; b < 300; ++b) {
            const int n = 1 + static_cast<int>(rnd() * 1023);
            if (b % 37 == 0) randomize();
            if (b % 23 == 0) s.noteOn(12 + static_cast<int>(rnd() * 110), 127);
            if (b % 29 == 0) s.noteOff(24 + static_cast<int>(rnd() * 90));
            s.render(l.data(), r.data(), n);
            for (int i = 0; i < n; ++i) {
                finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
                peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
            }
        }
        const auto m = s.readMeter();
        CHECK(finite, "trial %d non-finite", trial);
        CHECK(peak <= ceiling * 1.0001f, "trial %d peak %.4f", trial, peak);
        CHECK(m.backstopHits == 0, "trial %d backstop %llu", trial, (unsigned long long)m.backstopHits);
    }
}

void test_resonance_emphasises_cutoff() {
    // Prediction: with noise in, raising resonance from 0 to 90% raises the
    // energy within a third-octave of the cutoff by >= 6 dB, and stays finite.
    auto bandDb = [](float reso) {
        CoreSynth s = makeSynth();
        plainPatch(s, 0);
        s.setParam(core_synth::kOscMix, 0);
        s.setParam(core_synth::kNoiseLevel, 100);
        s.setParam(core_synth::kOsc1Wave, 0);
        s.setParam(core_synth::kFilterCutoff, 1000);
        s.setParam(core_synth::kFilterResonance, reso);
        s.noteOn(21, 127);  // a very low sine under the noise
        const auto out = render(s, 1 << 16);
        const size_t n = 1 << 15;
        const auto p = powerSpectrum(out.l, 1 << 15, n);
        double band = 0, total = 0;
        for (size_t k = 1; k < p.size(); ++k) {
            const double f = k * kSr / n;
            total += p[k];
            if (f > 891 && f < 1122) band += p[k];
        }
        return 10 * std::log10(band / total);
    };
    const double flat = bandDb(0), peaky = bandDb(90);
    std::printf("    band share at 1 kHz: reso 0%% %.1f dB, 90%% %.1f dB\n", flat, peaky);
    CHECK(peaky > flat + 6, "resonance %.1f vs %.1f", peaky, flat);
}

void test_cutoff_sweep_is_smooth() {
    // Prediction: jumping the cutoff from 200 Hz to 12 kHz mid-note is
    // smoothed: no sample-to-sample step larger than the steady bright tone's.
    CoreSynth s = makeSynth();
    plainPatch(s, 2);
    s.setParam(core_synth::kFilterCutoff, 12000);
    s.noteOn(45, 100);
    auto steady = render(s, 9600);
    float maxStep = 0;
    for (size_t i = 4800; i + 1 < steady.l.size(); ++i) maxStep = std::max(maxStep, std::fabs(steady.l[i + 1] - steady.l[i]));
    s.setParam(core_synth::kFilterCutoff, 200);
    render(s, 9600);
    s.setParam(core_synth::kFilterCutoff, 12000);
    auto jump = render(s, 4800);
    float jumpStep = 0;
    for (size_t i = 0; i + 1 < jump.l.size(); ++i) jumpStep = std::max(jumpStep, std::fabs(jump.l[i + 1] - jump.l[i]));
    CHECK(jumpStep <= maxStep * 1.3f, "step after jump %.4f vs steady %.4f", jumpStep, maxStep);
}

void test_dc_and_pulse() {
    // Prediction: a 10% pulse (asymmetric) leaves essentially no DC at the
    // output (oscillator offset correction + DC blocker): |mean| < 0.5% FS.
    CoreSynth s = makeSynth();
    plainPatch(s, 3);
    s.setParam(core_synth::kPulseWidth, 10);
    s.setParam(core_synth::kMasterGain, 0);
    s.noteOn(45, 127);
    const auto out = render(s, static_cast<int>(kSr * 2));
    double mean = 0;
    for (size_t i = 48000; i < out.l.size(); ++i) mean += out.l[i];
    mean /= out.l.size() - 48000;
    CHECK(std::fabs(mean) < 0.005, "dc %.5f", mean);
}

void test_limiter_intersample() {
    // Prediction: a near-Nyquist/4 sine driven 12 dB over the ceiling comes out
    // with sample peaks AND 8x-interpolated (intersample) peaks within 0.3 dB of
    // the -1 dBFS ceiling.
    dsp::Limiter lim;
    lim.setup(kSr, -1.0f, 1.5f, 120.0f);
    std::vector<float> y(48000);
    for (size_t i = 0; i < y.size(); ++i) {
        float l = 4.0f * std::sin(2 * M_PI * 11025.7 * i / kSr + 0.3), r = l;
        lim.process(l, r);
        y[i] = l;
    }
    float samplePeak = peakOf(y, 4800);
    // 8x windowed-sinc interpolation for the true peak
    float truePeak = 0;
    for (size_t i = 4800; i + 16 < y.size(); ++i) {
        for (int k = 0; k < 8; ++k) {
            const double t = k / 8.0;
            double acc = 0;
            for (int j = -15; j <= 16; ++j) {
                const double x = j - t;
                const double sinc = std::fabs(x) < 1e-9 ? 1 : std::sin(M_PI * x) / (M_PI * x);
                const double win = 0.5 + 0.5 * std::cos(M_PI * x / 16.5);
                acc += y[i + j] * sinc * win;
            }
            truePeak = std::max(truePeak, float(std::fabs(acc)));
        }
    }
    const double ceilDb = -1.0;
    std::printf("    limiter: sample peak %.2f dBFS, true peak %.2f dBFS\n", 20 * std::log10(samplePeak),
                20 * std::log10(truePeak));
    CHECK(20 * std::log10(samplePeak) <= ceilDb + 0.01, "sample peak");
    CHECK(20 * std::log10(truePeak) <= ceilDb + 0.3, "true peak %.2f", 20 * std::log10(truePeak));
    CHECK(lim.backstopHits() == 0, "backstop %llu", (unsigned long long)lim.backstopHits());
}

void test_determinism_and_block_size() {
    // Prediction: the same pattern + preset renders bit-identically twice, and
    // identically at block sizes 64 and 333 (events are sample-accurate).
    auto run = [](int block) {
        AdamSynth* s = adam_synth_create(kSr);
        adam_synth_apply_preset(s, readFixture("presets/bright_chords.preset").c_str());
        adam_seq_load(s, readFixture("patterns/tight_chords.pat").c_str());
        adam_seq_set_bpm(s, adam_seq_pattern_bpm(s));
        adam_seq_play(s, 1);
        std::vector<float> out;
        for (long d = 0; d < 96000; d += block) {
            const int n = static_cast<int>(std::min<long>(block, 96000 - d));
            adam_synth_render(s, n);
            out.insert(out.end(), adam_synth_out_left(s), adam_synth_out_left(s) + n);
        }
        adam_synth_destroy(s);
        return out;
    };
    const auto a = run(128), b = run(128), c = run(64), d = run(333);
    CHECK(a == b, "renders differ run to run");
    double maxDiff = 0;
    for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, double(std::fabs(a[i] - c[i])) + std::fabs(a[i] - d[i]));
    CHECK(maxDiff < 1e-6, "block-size dependence %.2e", maxDiff);
}

void test_analysis_sees_what_is_there() {
    // Prediction: the analyzer reads a pure tone's pitch to 0.1%, a sine has
    // no harmonics (H2 < -60 dB), a saw's harmonics fall as 1/n (H2 ~ -6 dB,
    // H3 ~ -9.5 dB), a closed filter lowers the centroid, mono reads +1.
    const size_t n = 8192;
    auto tone = [&](dsp::Wave w, double f) {
        std::vector<float> x(n);
        dsp::Oscillator o;
        for (auto& v : x) v = 0.5f * o.next(w, static_cast<float>(f / kSr));
        return x;
    };
    const auto sine = tone(dsp::Wave::Sine, 440.0);
    auto r = analysis::analyze(sine.data(), sine.data(), n, kSr);
    CHECK(std::fabs(r.f0Hz - 440.0) < 0.44, "sine f0 %.3f", r.f0Hz);
    CHECK(r.f0Confidence > 0.9, "sine confidence %.2f", r.f0Confidence);
    CHECK(r.harmonicsDb[1] < -60, "sine H2 %.1f", r.harmonicsDb[1]);
    CHECK(std::fabs(r.stereoCorrelation - 1.0) < 1e-6, "mono corr %.3f", r.stereoCorrelation);
    const auto saw = tone(dsp::Wave::Saw, 220.0);
    r = analysis::analyze(saw.data(), saw.data(), n, kSr);
    std::printf("    saw 220 Hz: f0 %.2f, H2 %.2f dB, H3 %.2f dB, centroid %.0f Hz\n", r.f0Hz, r.harmonicsDb[1],
                r.harmonicsDb[2], r.centroidHz);
    CHECK(std::fabs(r.harmonicsDb[1] + 6.02) < 0.5 && std::fabs(r.harmonicsDb[2] + 9.54) < 0.5, "saw H2 %.2f H3 %.2f",
          r.harmonicsDb[1], r.harmonicsDb[2]);
    auto centroidAt = [&](float cutoff) {
        CoreSynth s = makeSynth();
        plainPatch(s, 2);
        s.setParam(core_synth::kFilterCutoff, cutoff);
        s.snapSmoothing();
        s.noteOn(45, 100);
        const auto out = render(s, 24000);
        return analysis::analyze(out.l.data() + 8000, out.r.data() + 8000, n, kSr).centroidHz;
    };
    const double dark = centroidAt(400), bright = centroidAt(8000);
    std::printf("    centroid: cutoff 400 Hz -> %.0f Hz, 8 kHz -> %.0f Hz\n", dark, bright);
    CHECK(bright > 2 * dark, "centroid dark %.0f bright %.0f", dark, bright);
}

void test_layers_stay_in_phase_and_bus_is_safe() {
    // Prediction: a layer seeked to the master's beat fires its events on the
    // same samples as the master; four loud layers summed through the bus stay
    // at or below -1 dBFS with the backstop untouched.
    AdamSynth* master = adam_synth_create(kSr);
    AdamSynth* layer = adam_synth_create(kSr);
    const char* pat = "bpm 120\nlength 4\nnote 0 0.5 60 100\nnote 2 0.5 64 100\n";
    adam_seq_load(master, pat);
    adam_seq_load(layer, pat);
    for (AdamSynth* s : {master, layer}) {
        adam_seq_set_bpm(s, 120);
        adam_seq_set_loop(s, 1);
    }
    adam_seq_play(master, 1);
    for (int b = 0; b < 300; ++b) adam_synth_render(master, 128);  // master runs on alone
    adam_seq_play(layer, 1);
    adam_seq_seek(layer, adam_seq_beat(master));
    double maxDiff = 0;
    for (int b = 0; b < 1500; ++b) {  // 4 s = 2 loops
        adam_synth_render(master, 128);
        adam_synth_render(layer, 128);
        if (b > 750)  // after the layer's first full loop the two must be identical
            for (int i = 0; i < 128; ++i)
                maxDiff = std::max(maxDiff, double(std::fabs(adam_synth_out_left(master)[i] - adam_synth_out_left(layer)[i])));
    }
    CHECK(std::fabs(adam_seq_beat(master) - adam_seq_beat(layer)) < 1e-9, "beats %f vs %f", adam_seq_beat(master),
          adam_seq_beat(layer));
    CHECK(maxDiff < 1e-6, "layer out of phase: %.3e", maxDiff);
    adam_synth_destroy(master);
    adam_synth_destroy(layer);

    AdamBus* bus = adam_bus_create(kSr);
    std::vector<AdamSynth*> layers;
    for (int k = 0; k < 4; ++k) {
        AdamSynth* s = adam_synth_create(kSr);
        adam_synth_set_param(s, core_synth::kMasterGain, 6);
        for (int n = 0; n < 8; ++n) adam_synth_note_on(s, 40 + 3 * n + k, 127);
        layers.push_back(s);
    }
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    float peak = 0;
    for (int b = 0; b < 400; ++b) {
        float* L = adam_bus_left(bus);
        float* R = adam_bus_right(bus);
        std::fill(L, L + 128, 0.0f);
        std::fill(R, R + 128, 0.0f);
        for (AdamSynth* s : layers) {
            adam_synth_render(s, 128);
            for (int i = 0; i < 128; ++i) {
                L[i] += adam_synth_out_left(s)[i];
                R[i] += adam_synth_out_right(s)[i];
            }
        }
        adam_bus_process(bus, 128);
        for (int i = 0; i < 128; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    const float* m = adam_bus_read_meter(bus);
    std::printf("    bus: 4 layers x 8 voices at +6 dB -> peak %.2f dBFS, limiting %.1f dB\n", 20 * std::log10(peak), m[4]);
    CHECK(peak <= ceiling * 1.0001f, "bus peak %.4f", peak);
    CHECK(m[6] == 0, "bus backstop %g", m[6]);
    for (AdamSynth* s : layers) adam_synth_destroy(s);
}

int main() {
    struct T {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"param_contract", test_param_contract},
        {"pitch_tracking", test_pitch_tracking},
        {"saw_band_limited", test_saw_is_band_limited},
        {"tri_square_band_limited", test_triangle_and_square_band_limited},
        {"envelope_timing", test_envelope_timing},
        {"polyphony_stealing", test_polyphony_and_stealing},
        {"sustain_pedal", test_sustain_pedal},
        {"sequencer_timing_loop", test_sequencer_timing_and_loop},
        {"patterns_render_clean", test_patterns_render_clean},
        {"adversarial_extremes", test_adversarial_extremes},
        {"resonance_emphasis", test_resonance_emphasises_cutoff},
        {"cutoff_sweep_smooth", test_cutoff_sweep_is_smooth},
        {"dc_and_pulse", test_dc_and_pulse},
        {"limiter_intersample", test_limiter_intersample},
        {"determinism_block_size", test_determinism_and_block_size},
        {"analysis_sees_what_is_there", test_analysis_sees_what_is_there},
        {"layers_in_phase_bus_safe", test_layers_stay_in_phase_and_bus_is_safe},
    };
    const char* only = std::getenv("ADAM_TEST");
    for (auto& t : tests) {
        if (only && std::string(only) != t.name) continue;
        g_test = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
