// What every Adam instrument is, to a host: a parameter contract plus the
// real-time interface. Hosts (C API -> WASM workshop, renderer, JUCE AU) talk
// to this, so a new instrument plugs into all of them at once.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace adam {

// ------------------------------------------------------- parameter contract
enum class Mapping : uint8_t {
    Linear,  // physical = min + n * (max - min)
    Log,     // physical = min * (max / min)^n           (min > 0)
    Int,     // linear, rounded to whole steps
    Choice,  // index into `choices`, n spread evenly
};

struct ParamSpec {
    const char* id;       // stable machine id — never renamed once shipped
    const char* name;     // display name (plain ASCII: hosts mangle the rest)
    const char* unit;
    const char* group;    // UI grouping
    Mapping mapping;
    float min, max, def;  // physical
    float smoothing_ms;   // 0 = applied at once
    const char* choices;  // "|"-separated labels for Mapping::Choice
};

namespace params {

inline int choiceCount(const ParamSpec& s) { return static_cast<int>(s.max - s.min) + 1; }

inline float clamp(const ParamSpec& s, float v) {
    if (!std::isfinite(v)) return s.def;
    v = std::clamp(v, s.min, s.max);
    if (s.mapping == Mapping::Int || s.mapping == Mapping::Choice) v = std::round(v);
    return v;
}

inline float toPhysical(const ParamSpec& s, float n) {
    if (!std::isfinite(n)) return s.def;
    n = std::clamp(n, 0.0f, 1.0f);
    switch (s.mapping) {
        case Mapping::Log: return s.min * std::pow(s.max / s.min, n);
        case Mapping::Int: return std::round(s.min + n * (s.max - s.min));
        case Mapping::Choice: return s.min + std::min(choiceCount(s) - 1, static_cast<int>(n * choiceCount(s)));
        case Mapping::Linear:
        default: return s.min + n * (s.max - s.min);
    }
}

inline float toNormalized(const ParamSpec& s, float physical) {
    const float v = clamp(s, physical);
    switch (s.mapping) {
        case Mapping::Log: return std::log(v / s.min) / std::log(s.max / s.min);
        case Mapping::Choice: return (v - s.min + 0.5f) / static_cast<float>(choiceCount(s));
        case Mapping::Int:
        case Mapping::Linear:
        default: return s.max > s.min ? (v - s.min) / (s.max - s.min) : 0.0f;
    }
}

inline int indexForId(const ParamSpec* specs, int count, const char* id) {
    for (int i = 0; i < count; ++i)
        if (std::strcmp(specs[i].id, id) == 0) return i;
    return -1;
}

inline void appendEscaped(std::string& out, const char* text) {
    out += '"';
    for (const char* c = text; c && *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        out += *c;
    }
    out += '"';
}

inline std::string toJson(const ParamSpec* specs, int count) {
    static const char* kMappingNames[] = {"linear", "log", "int", "choice"};
    std::string out = "[";
    char num[96];
    for (int i = 0; i < count; ++i) {
        const ParamSpec& s = specs[i];
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
        std::snprintf(num, sizeof num, "\",\"min\":%g,\"max\":%g,\"default\":%g,\"smoothing_ms\":%g", s.min, s.max,
                      s.def, s.smoothing_ms);
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
    return out + "]";
}

}  // namespace params

// ------------------------------------------------------------ the interface
struct Meter {
    float peakL = 0, peakR = 0;
    float rmsL = 0, rmsR = 0;
    float gainReductionDb = 0;
    uint64_t backstopHits = 0;
    uint64_t nonFiniteResets = 0;
    int activeVoices = 0;
};

class Instrument {
public:
    virtual ~Instrument() = default;

    virtual const ParamSpec* paramSpecs() const = 0;
    virtual int paramCount() const = 0;

    virtual void prepare(float sampleRate) = 0;
    virtual void reset() = 0;
    virtual void setParam(int index, float physical) = 0;
    virtual float param(int index) const = 0;
    virtual void resetParamsToDefaults() = 0;
    virtual void snapSmoothing() = 0;

    virtual void noteOn(int note, int velocity) = 0;
    virtual void noteOff(int note) = 0;
    virtual void sustainPedal(bool down) = 0;
    virtual void allNotesOff() = 0;
    virtual void allSoundOff() = 0;

    virtual void render(float* outL, float* outR, int frames) = 0;
    virtual Meter readMeter() = 0;
    virtual int latencySamples() const = 0;
    virtual float sampleRate() const = 0;

    int paramIndexForId(const char* id) const { return params::indexForId(paramSpecs(), paramCount(), id); }

    // Preset text: "<param_id> <physical>" per line, '#' comments, unknown ids
    // ignored. Applied on top of the defaults. Returns params applied.
    int applyPreset(const char* text, bool fromDefaults = true) {
        if (!text) return 0;
        if (fromDefaults) resetParamsToDefaults();
        int applied = 0;
        const char* p = text;
        while (*p) {
            const char* eol = std::strchr(p, '\n');
            const size_t len = eol ? static_cast<size_t>(eol - p) : std::strlen(p);
            char line[192];
            const size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
            std::memcpy(line, p, n);
            line[n] = '\0';
            if (char* hash = std::strchr(line, '#')) *hash = '\0';
            char id[96];
            float value = 0.0f;
            if (std::sscanf(line, "%95s %f", id, &value) == 2) {
                const int idx = paramIndexForId(id);
                if (idx >= 0) {
                    setParam(idx, value);
                    ++applied;
                }
            }
            if (!eol) break;
            p = eol + 1;
        }
        return applied;
    }
};

}  // namespace adam
