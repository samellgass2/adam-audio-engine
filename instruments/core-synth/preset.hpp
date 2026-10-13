// Preset text format, shared by every host: one "<param_id> <physical value>"
// per line, '#' comments, unknown ids ignored (a preset outlives a renamed
// experiment). Choice params take their index.
#pragma once

#include <cstdlib>
#include <cstring>

#include "core_synth.hpp"

namespace adam::core_synth {

// Applies the preset on top of the defaults. Returns params applied.
inline int applyPreset(CoreSynth& synth, const char* text, bool fromDefaults = true) {
    if (!text) return 0;
    if (fromDefaults) synth.resetParamsToDefaults();
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
                synth.setParam(static_cast<ParamId>(idx), value);
                ++applied;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return applied;
}

}  // namespace adam::core_synth
