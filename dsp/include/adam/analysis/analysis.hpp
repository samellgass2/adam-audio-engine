// Signal analysis — what an agent "sees" in a render. ONE implementation used
// by the tests, the offline renderer's report and (via WASM) the workshop's
// analysis panel, so a human can check the numbers against their ears.
//
// Not real-time code: allocation is fine (it runs off the audio thread).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace adam::analysis {

inline void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Blackman-Harris power spectrum of x[0, n) (n a power of two), n/2 bins.
inline std::vector<double> powerSpectrum(const float* x, size_t n) {
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * t) + 0.14128 * std::cos(4 * M_PI * t) -
                         0.01168 * std::cos(6 * M_PI * t);
        a[i] = x[i] * w;
    }
    fft(a);
    std::vector<double> p(n / 2);
    for (size_t i = 0; i < n / 2; ++i) p[i] = std::norm(a[i]);
    return p;
}

// Energy away from the harmonics of f0 vs on them, 20 Hz..20 kHz, in dB.
// For a single held note this is the aliasing/noise indicator; for chords it
// mostly measures the other notes (read it on single notes).
inline double inharmonicDb(const std::vector<double>& p, double f0, double sr, size_t n, int halfWidthBins = 6) {
    double harm = 0, other = 0;
    const double binHz = sr / static_cast<double>(n);
    for (size_t k = 1; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f < 20 || f > 20000) continue;
        const double h = f / f0;
        const double dist = std::fabs(h - std::round(h)) * f0 / binHz;
        (std::round(h) >= 1 && dist <= halfWidthBins ? harm : other) += p[k];
    }
    return 10 * std::log10(other / std::max(harm, 1e-30) + 1e-30);
}

// YIN pitch estimate (de Cheveigné & Kawahara). Returns {f0 Hz, confidence 0..1};
// f0 = 0 when nothing periodic is found.
inline std::pair<double, double> yinPitch(const float* x, size_t n, double sr, double minHz = 40, double maxHz = 2500) {
    const size_t maxLag = std::min(n / 2, static_cast<size_t>(sr / minHz));
    const size_t minLag = std::max<size_t>(2, static_cast<size_t>(sr / maxHz));
    const size_t w = n - maxLag;
    if (maxLag <= minLag + 2 || w < 64) return {0, 0};
    std::vector<double> d(maxLag + 1, 0.0), cmnd(maxLag + 1, 1.0);
    for (size_t tau = 1; tau <= maxLag; ++tau) {
        double s = 0;
        for (size_t i = 0; i < w; ++i) {
            const double diff = static_cast<double>(x[i]) - x[i + tau];
            s += diff * diff;
        }
        d[tau] = s;
    }
    double running = 0;
    for (size_t tau = 1; tau <= maxLag; ++tau) {
        running += d[tau];
        cmnd[tau] = running > 0 ? d[tau] * static_cast<double>(tau) / running : 1.0;
    }
    size_t best = 0;
    for (size_t tau = minLag; tau < maxLag; ++tau) {
        if (cmnd[tau] < 0.15) {
            while (tau + 1 < maxLag && cmnd[tau + 1] < cmnd[tau]) ++tau;
            best = tau;
            break;
        }
    }
    if (!best) {  // no dip under the threshold: take the global minimum, low confidence
        best = static_cast<size_t>(std::min_element(cmnd.begin() + minLag, cmnd.begin() + maxLag) - cmnd.begin());
    }
    // parabolic interpolation around the dip
    double tau = static_cast<double>(best);
    if (best > 1 && best + 1 <= maxLag) {
        const double a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) tau += 0.5 * (a - c) / den;
    }
    const double conf = std::clamp(1.0 - cmnd[best], 0.0, 1.0);
    return {sr / tau, conf};
}

struct Report {
    double peakDb = -180, rmsDb = -180, crestDb = 0, dc = 0;
    double centroidHz = 0, rolloffHz = 0, flatness = 0;   // spectral shape
    double f0Hz = 0, f0Confidence = 0, f0Midi = 0;         // pitch (YIN)
    double inharmonicDb = 0;                               // vs harmonics of f0
    double bandDb[4] = {-180, -180, -180, -180};           // <200, 200-2k, 2k-8k, >8k (share of total)
    double harmonicsDb[12] = {};                           // H1..H12 rel. H1
    double stereoCorrelation = 1;                          // +1 mono .. -1 out of phase
    int harmonicCount = 0;
};

// Analyse n samples (n a power of two, e.g. 8192) of a stereo signal.
inline Report analyze(const float* l, const float* r, size_t n, double sr) {
    Report out;
    if (n < 256) return out;
    std::vector<float> mono(n);
    double sum = 0, sum2 = 0, peak = 0, lr = 0, ll = 0, rr = 0;
    for (size_t i = 0; i < n; ++i) {
        const double a = l[i], b = r ? r[i] : l[i];
        mono[i] = static_cast<float>(0.5 * (a + b));
        sum += mono[i];
        sum2 += 0.5 * (a * a + b * b);
        peak = std::max(peak, std::max(std::fabs(a), std::fabs(b)));
        lr += a * b;
        ll += a * a;
        rr += b * b;
    }
    const double rms = std::sqrt(sum2 / static_cast<double>(n));
    out.peakDb = 20 * std::log10(peak + 1e-12);
    out.rmsDb = 20 * std::log10(rms + 1e-12);
    out.crestDb = out.peakDb - out.rmsDb;
    out.dc = sum / static_cast<double>(n);
    out.stereoCorrelation = (ll > 0 && rr > 0) ? lr / std::sqrt(ll * rr) : 1.0;
    if (rms < 1e-6) return out;  // silence: no spectrum worth reading

    const auto p = powerSpectrum(mono.data(), n);
    const double binHz = sr / static_cast<double>(n);
    // centroid and rolloff on the magnitude spectrum (the usual definitions,
    // as librosa), flatness and band shares on power
    double total = 0, magTotal = 0, weighted = 0, logSum = 0;
    size_t counted = 0;
    double bands[4] = {0, 0, 0, 0};
    for (size_t k = 1; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f < 20 || f > 20000) continue;
        total += p[k];
        const double mag = std::sqrt(p[k]);
        magTotal += mag;
        weighted += mag * f;
        logSum += std::log(p[k] + 1e-30);
        ++counted;
        bands[f < 200 ? 0 : f < 2000 ? 1 : f < 8000 ? 2 : 3] += p[k];
    }
    if (total <= 0) return out;
    out.centroidHz = weighted / magTotal;
    out.flatness = std::exp(logSum / static_cast<double>(counted)) / (total / static_cast<double>(counted));
    double acc = 0;
    for (size_t k = 1; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f < 20 || f > 20000) continue;
        acc += std::sqrt(p[k]);
        if (acc >= 0.85 * magTotal) {
            out.rolloffHz = f;
            break;
        }
    }
    for (int b = 0; b < 4; ++b) out.bandDb[b] = 10 * std::log10(bands[b] / total + 1e-30);

    // pitch from the most recent part of the window
    const size_t yinN = std::min<size_t>(n, 4096);
    const auto [f0, conf] = yinPitch(mono.data() + (n - yinN), yinN, sr);
    out.f0Hz = f0;
    out.f0Confidence = conf;
    if (f0 > 0) {
        out.f0Midi = 69 + 12 * std::log2(f0 / 440.0);
        out.inharmonicDb = inharmonicDb(p, f0, sr, n);
        double h1 = 0;
        for (int h = 1; h <= 12; ++h) {
            const double f = f0 * h;
            if (f > 0.48 * sr) break;
            const size_t c = static_cast<size_t>(std::lround(f / binHz));
            // sum the window's main lobe (+-4 bins): independent of where the
            // harmonic falls between bins (no scalloping error)
            double m = 0;
            for (size_t k = c > 4 ? c - 4 : 1; k <= c + 4 && k < p.size(); ++k) m += p[k];
            if (h == 1) h1 = std::max(m, 1e-30);
            out.harmonicsDb[h - 1] = 10 * std::log10(m / h1 + 1e-30);
            out.harmonicCount = h;
        }
    }
    return out;
}

}  // namespace adam::analysis
