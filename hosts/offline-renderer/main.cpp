// adam-render: deterministic headless rendering — the agent's interface to the
// synth. Pattern + preset (+ overrides) -> 32-bit float stereo WAV and a JSON
// stats sidecar. Goes through the same C API the browser uses.
//
//   adam-render --pattern fixtures/patterns/pad_chords.pat \
//               --preset fixtures/presets/soft_pad.preset \
//               --set filter_cutoff=1200 --loops 2 --tail 2 \
//               --sr 48000 --block 128 --out out/pad.wav
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "c_api.h"
#include "params.hpp"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "adam-render: cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeWav(const std::string& path, const std::vector<float>& inter, int sr) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "adam-render: cannot write %s\n", path.c_str());
        std::exit(2);
    }
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(inter.size() * 4);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3);  // IEEE float
    u16(2);
    u32(static_cast<uint32_t>(sr));
    u32(static_cast<uint32_t>(sr) * 8);
    u16(8);
    u16(32);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(inter.data()), bytes);
}

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: adam-render --pattern P [--preset F] [--set id=value ...]\n"
                 "       [--loops N | --seconds S] [--tail S] [--bpm B] [--sr 48000]\n"
                 "       [--block 128] --out out.wav [--stats out.json]\n");
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    std::string pattern, preset, out, stats;
    std::vector<std::string> sets;
    double loops = 1.0, seconds = -1.0, tail = 2.0, bpm = -1.0;
    int sr = 48000, block = 128;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--pattern") pattern = val();
        else if (a == "--preset") preset = val();
        else if (a == "--set") sets.push_back(val());
        else if (a == "--loops") loops = std::atof(val().c_str());
        else if (a == "--seconds") seconds = std::atof(val().c_str());
        else if (a == "--tail") tail = std::atof(val().c_str());
        else if (a == "--bpm") bpm = std::atof(val().c_str());
        else if (a == "--sr") sr = std::atoi(val().c_str());
        else if (a == "--block") block = std::atoi(val().c_str());
        else if (a == "--out") out = val();
        else if (a == "--stats") stats = val();
        else usage();
    }
    if (pattern.empty() || out.empty()) usage();
    if (block < 1 || block > ADAM_MAX_BLOCK || sr < 8000) usage();
    if (stats.empty()) stats = out.substr(0, out.find_last_of('.')) + ".json";

    AdamSynth* s = adam_synth_create(static_cast<float>(sr));
    if (!preset.empty()) {
        // the same preset parser the browser host uses (adam_synth_apply_preset)
        if (adam_synth_apply_preset(s, readFile(preset).c_str()) <= 0)
            std::fprintf(stderr, "adam-render: preset %s applied no params\n", preset.c_str());
    }
    for (const auto& kv : sets) {
        const auto eq = kv.find('=');
        const int idx = eq == std::string::npos ? -1 : adam::core_synth::paramIndexForId(kv.substr(0, eq).c_str());
        if (idx < 0) {
            std::fprintf(stderr, "adam-render: bad --set %s\n", kv.c_str());
            return 1;
        }
        adam_synth_set_param(s, idx, static_cast<float>(std::atof(kv.c_str() + eq + 1)));
    }

    const std::string patText = readFile(pattern);
    if (adam_seq_load(s, patText.c_str()) <= 0) {
        std::fprintf(stderr, "adam-render: no notes in %s\n", pattern.c_str());
        return 1;
    }
    if (bpm <= 0) bpm = adam_seq_pattern_bpm(s);
    adam_seq_set_bpm(s, bpm);
    const double lengthBeats = adam_seq_length(s);
    if (seconds <= 0) seconds = loops * lengthBeats * 60.0 / bpm;
    adam_seq_set_loop(s, 1);
    adam_seq_play(s, 1);

    const long playFrames = static_cast<long>(seconds * sr);
    const long totalFrames = playFrames + static_cast<long>(tail * sr);
    std::vector<float> inter(static_cast<size_t>(totalFrames) * 2);
    double sumL2 = 0, sumR2 = 0, sumL = 0, sumR = 0;
    float peak = 0;
    float maxGr = 0;
    long nonFinite = 0;
    const auto t0 = std::chrono::steady_clock::now();
    double worstBlockMs = 0.0;
    for (long done = 0; done < totalFrames;) {
        if (done >= playFrames && adam_seq_playing(s)) adam_seq_play(s, 0);  // tail: release
        const int n = static_cast<int>(std::min<long>(block, totalFrames - done));
        const auto b0 = std::chrono::steady_clock::now();
        adam_synth_render(s, n);
        worstBlockMs = std::max(
            worstBlockMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count());
        const float* l = adam_synth_out_left(s);
        const float* r = adam_synth_out_right(s);
        for (int i = 0; i < n; ++i) {
            float a = l[i], b = r[i];
            if (!std::isfinite(a) || !std::isfinite(b)) ++nonFinite;
            inter[static_cast<size_t>(done + i) * 2] = a;
            inter[static_cast<size_t>(done + i) * 2 + 1] = b;
            sumL += a;
            sumR += b;
            sumL2 += double(a) * a;
            sumR2 += double(b) * b;
            peak = std::max(peak, std::max(std::fabs(a), std::fabs(b)));
        }
        const float* m = adam_synth_read_meter(s);
        maxGr = std::max(maxGr, m[4]);
        done += n;
    }
    const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const float* m = adam_synth_read_meter(s);
    writeWav(out, inter, sr);

    const double N = static_cast<double>(totalFrames);
    const double audioMs = 1000.0 * N / sr;
    FILE* js = std::fopen(stats.c_str(), "w");
    if (js) {
        std::fprintf(js,
                     "{\n  \"pattern\": \"%s\",\n  \"preset\": \"%s\",\n  \"sample_rate\": %d,\n"
                     "  \"block\": %d,\n  \"bpm\": %g,\n  \"frames\": %ld,\n  \"seconds\": %.3f,\n"
                     "  \"peak_dbfs\": %.3f,\n  \"rms_dbfs_l\": %.3f,\n  \"rms_dbfs_r\": %.3f,\n"
                     "  \"dc_l\": %.6f,\n  \"dc_r\": %.6f,\n  \"max_gain_reduction_db\": %.3f,\n"
                     "  \"backstop_hits\": %.0f,\n  \"nonfinite_voice_resets\": %.0f,\n"
                     "  \"nonfinite_samples\": %ld,\n  \"realtime_factor\": %.1f,\n"
                     "  \"worst_block_ms\": %.4f,\n  \"block_budget_ms\": %.4f\n}\n",
                     pattern.c_str(), preset.c_str(), sr, block, bpm, totalFrames, N / sr,
                     20 * std::log10(std::max(peak, 1e-9f)), 10 * std::log10(std::max(sumL2 / N, 1e-18)),
                     10 * std::log10(std::max(sumR2 / N, 1e-18)), sumL / N, sumR / N, maxGr, m[6], m[7],
                     nonFinite, audioMs / std::max(wallMs, 1e-6), worstBlockMs, 1000.0 * block / sr);
        std::fclose(js);
    }
    std::printf("%s  peak %.2f dBFS  rms %.1f dBFS  %.0fx realtime\n", out.c_str(),
                20 * std::log10(std::max(peak, 1e-9f)), 10 * std::log10(std::max((sumL2 + sumR2) / (2 * N), 1e-18)),
                audioMs / std::max(wallMs, 1e-6));
    adam_synth_destroy(s);
    return nonFinite == 0 ? 0 : 3;
}
