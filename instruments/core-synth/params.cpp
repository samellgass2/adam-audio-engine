#include "params.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace adam::core_synth {

namespace {

constexpr const char* kWaves = "Sine|Triangle|Saw|Square";

// clang-format off
const ParamSpec kSpecs[kParamCount] = {
    {"osc1_wave",        "Osc 1 Wave",       "",       "Oscillators", Mapping::Choice, 0, 3, 2, 0, kWaves},
    {"osc2_wave",        "Osc 2 Wave",       "",       "Oscillators", Mapping::Choice, 0, 3, 2, 0, kWaves},
    {"osc2_semi",        "Osc 2 Semitones",  "st",     "Oscillators", Mapping::Int, -24, 24, 0, 0, nullptr},
    {"osc2_detune",      "Osc 2 Detune",     "cents",  "Oscillators", Mapping::Linear, -50, 50, 7, 20, nullptr},
    {"osc_mix",          "Osc Mix",          "%",      "Oscillators", Mapping::Linear, 0, 100, 50, 10, nullptr},
    {"pulse_width",      "Pulse Width",      "%",      "Oscillators", Mapping::Linear, 5, 95, 50, 10, nullptr},
    {"noise_level",      "Noise",            "%",      "Oscillators", Mapping::Linear, 0, 100, 0, 10, nullptr},
    {"filter_cutoff",    "Cutoff",           "Hz",     "Filter",      Mapping::Log, 20, 20000, 2400, 15, nullptr},
    {"filter_resonance", "Resonance",        "%",      "Filter",      Mapping::Linear, 0, 100, 20, 15, nullptr},
    {"filter_slope",     "Slope",            "",       "Filter",      Mapping::Choice, 0, 1, 1, 0, "12 dB/oct|24 dB/oct"},
    {"filter_keytrack",  "Key Tracking",     "%",      "Filter",      Mapping::Linear, 0, 100, 50, 10, nullptr},
    {"filter_env_amount","Env Amount",       "oct",    "Filter",      Mapping::Linear, -4, 6, 2, 15, nullptr},
    {"filt_attack",      "Filter Attack",    "ms",     "Filter Env",  Mapping::Log, 0.5f, 10000, 3, 0, nullptr},
    {"filt_decay",       "Filter Decay",     "ms",     "Filter Env",  Mapping::Log, 1, 10000, 400, 0, nullptr},
    {"filt_sustain",     "Filter Sustain",   "%",      "Filter Env",  Mapping::Linear, 0, 100, 30, 10, nullptr},
    {"filt_release",     "Filter Release",   "ms",     "Filter Env",  Mapping::Log, 1, 15000, 400, 0, nullptr},
    {"amp_attack",       "Attack",           "ms",     "Amp Env",     Mapping::Log, 0.5f, 10000, 4, 0, nullptr},
    {"amp_decay",        "Decay",            "ms",     "Amp Env",     Mapping::Log, 1, 10000, 300, 0, nullptr},
    {"amp_sustain",      "Sustain",          "%",      "Amp Env",     Mapping::Linear, 0, 100, 75, 10, nullptr},
    {"amp_release",      "Release",          "ms",     "Amp Env",     Mapping::Log, 1, 15000, 350, 0, nullptr},
    {"velocity_sens",    "Velocity",         "%",      "Amp Env",     Mapping::Linear, 0, 100, 60, 0, nullptr},
    {"lfo_wave",         "LFO Wave",         "",       "LFO",         Mapping::Choice, 0, 1, 0, 0, "Sine|Triangle"},
    {"lfo_rate",         "LFO Rate",         "Hz",     "LFO",         Mapping::Log, 0.05f, 20, 5, 20, nullptr},
    {"lfo_pitch",        "LFO to Pitch",      "cents",  "LFO",         Mapping::Linear, 0, 100, 0, 20, nullptr},
    {"lfo_cutoff",       "LFO to Cutoff",     "oct",    "LFO",         Mapping::Linear, 0, 4, 0, 20, nullptr},
    {"glide",            "Glide",            "ms",     "Voice",       Mapping::Log, 0.5f, 2000, 0.5f, 0, nullptr},
    {"spread",           "Stereo Spread",    "%",      "Voice",       Mapping::Linear, 0, 100, 30, 20, nullptr},
    {"polyphony",        "Voices",           "",       "Voice",       Mapping::Int, 1, 16, 8, 0, nullptr},
    {"master_gain",      "Master",           "dB",     "Output",      Mapping::Linear, -36, 6, -6, 20, nullptr},
    // appended 2026-10-13 — never reorder: Logic sessions key on these ids
    {"filter_drive",     "Drive",            "%",      "Filter",      Mapping::Linear, 0, 100, 0, 15, nullptr},
    {"unison_voices",    "Unison Voices",    "",       "Unison",      Mapping::Int, 1, 7, 1, 0, nullptr},
    {"unison_detune",    "Unison Detune",    "cents",  "Unison",      Mapping::Linear, 0, 100, 20, 20, nullptr},
    {"vibrato_depth",    "Vibrato Depth",    "cents",  "Vibrato",     Mapping::Linear, 0, 50, 0, 20, nullptr},
    {"vibrato_rate",     "Vibrato Rate",     "Hz",     "Vibrato",     Mapping::Log, 0.5f, 12, 5.5f, 20, nullptr},
    {"vibrato_delay",    "Vibrato Delay",    "ms",     "Vibrato",     Mapping::Log, 1, 3000, 250, 0, nullptr},
};
// clang-format on

int choiceCount(const ParamSpec& s) { return static_cast<int>(s.max - s.min) + 1; }

void appendEscaped(std::string& out, const char* text) {
    out += '"';
    for (const char* c = text; c && *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        out += *c;
    }
    out += '"';
}

}  // namespace

const ParamSpec& paramSpec(ParamId id) { return kSpecs[id < kParamCount ? id : 0]; }

size_t paramCount() { return kParamCount; }

int paramIndexForId(const char* id) {
    for (int i = 0; i < kParamCount; ++i)
        if (std::strcmp(kSpecs[i].id, id) == 0) return i;
    return -1;
}

float clampPhysical(ParamId id, float physical) {
    const ParamSpec& s = paramSpec(id);
    if (!std::isfinite(physical)) return s.def;
    float v = std::clamp(physical, s.min, s.max);
    if (s.mapping == Mapping::Int || s.mapping == Mapping::Choice) v = std::round(v);
    return v;
}

float toPhysical(ParamId id, float n) {
    const ParamSpec& s = paramSpec(id);
    if (!std::isfinite(n)) return s.def;
    n = std::clamp(n, 0.0f, 1.0f);
    switch (s.mapping) {
        case Mapping::Log: return s.min * std::pow(s.max / s.min, n);
        case Mapping::Int: return std::round(s.min + n * (s.max - s.min));
        case Mapping::Choice: {
            const int count = choiceCount(s);
            return s.min + std::min(count - 1, static_cast<int>(n * count));
        }
        case Mapping::Linear:
        default: return s.min + n * (s.max - s.min);
    }
}

float toNormalized(ParamId id, float physical) {
    const ParamSpec& s = paramSpec(id);
    const float v = clampPhysical(id, physical);
    switch (s.mapping) {
        case Mapping::Log: return std::log(v / s.min) / std::log(s.max / s.min);
        case Mapping::Choice: {
            const int count = choiceCount(s);
            return (v - s.min + 0.5f) / static_cast<float>(count);
        }
        case Mapping::Int:
        case Mapping::Linear:
        default: return s.max > s.min ? (v - s.min) / (s.max - s.min) : 0.0f;
    }
}

const char* paramsJson() {
    static std::string json = [] {
        static const char* kMappingNames[] = {"linear", "log", "int", "choice"};
        std::string out = "[";
        char num[64];
        for (int i = 0; i < kParamCount; ++i) {
            const ParamSpec& s = kSpecs[i];
            if (i) out += ',';
            out += "{\"index\":" + std::to_string(i) + ",\"id\":";
            appendEscaped(out, s.id);
            out += ",\"name\":";
            appendEscaped(out, s.name);
            out += ",\"unit\":";
            appendEscaped(out, s.unit);
            out += ",\"group\":";
            appendEscaped(out, s.group);
            out += ",\"mapping\":\"";
            out += kMappingNames[static_cast<int>(s.mapping)];
            std::snprintf(num, sizeof num, "\",\"min\":%g,\"max\":%g,\"default\":%g,\"smoothing_ms\":%g",
                          s.min, s.max, s.def, s.smoothing_ms);
            out += num;
            out += ",\"choices\":[";
            if (s.choices) {
                std::string label;
                bool first = true;
                for (const char* c = s.choices;; ++c) {
                    if (*c == '|' || *c == '\0') {
                        if (!first) out += ',';
                        appendEscaped(out, label.c_str());
                        first = false;
                        label.clear();
                        if (*c == '\0') break;
                    } else {
                        label += *c;
                    }
                }
            }
            out += "]}";
        }
        out += "]";
        return out;
    }();
    return json.c_str();
}

}  // namespace adam::core_synth
