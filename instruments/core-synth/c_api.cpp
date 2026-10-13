#include "c_api.h"

#include <new>

#include "atom/seq/sequencer.hpp"
#include "core_synth.hpp"
#include "preset.hpp"

using atom::core_synth::CoreSynth;
using atom::core_synth::ParamId;

struct AtomSynth {
    CoreSynth synth;
    float left[ATOM_MAX_BLOCK];
    float right[ATOM_MAX_BLOCK];
    float meter[8];
    atom::seq::Sequencer seq;
    char text[65536];
};

namespace {
bool validIndex(int i) { return i >= 0 && i < static_cast<int>(atom::core_synth::kParamCount); }
}  // namespace

extern "C" {

AtomSynth* atom_synth_create(float sample_rate) {
    auto* s = new (std::nothrow) AtomSynth();
    if (!s) return nullptr;
    s->synth.prepare(sample_rate);
    return s;
}

void atom_synth_destroy(AtomSynth* s) { delete s; }
void atom_synth_reset(AtomSynth* s) { if (s) s->synth.reset(); }
void atom_synth_note_on(AtomSynth* s, int note, int velocity) { if (s) s->synth.noteOn(note, velocity); }
void atom_synth_note_off(AtomSynth* s, int note) { if (s) s->synth.noteOff(note); }
void atom_synth_sustain(AtomSynth* s, int down) { if (s) s->synth.sustainPedal(down != 0); }
void atom_synth_all_notes_off(AtomSynth* s) { if (s) s->synth.allNotesOff(); }
void atom_synth_all_sound_off(AtomSynth* s) { if (s) s->synth.allSoundOff(); }

void atom_synth_set_param(AtomSynth* s, int index, float physical) {
    if (s && validIndex(index)) s->synth.setParam(static_cast<ParamId>(index), physical);
}

float atom_synth_get_param(AtomSynth* s, int index) {
    return (s && validIndex(index)) ? s->synth.param(static_cast<ParamId>(index)) : 0.0f;
}

int atom_param_count(void) { return static_cast<int>(atom::core_synth::kParamCount); }

float atom_param_to_physical(int index, float normalized) {
    return validIndex(index) ? atom::core_synth::toPhysical(static_cast<ParamId>(index), normalized) : 0.0f;
}

float atom_param_to_normalized(int index, float physical) {
    return validIndex(index) ? atom::core_synth::toNormalized(static_cast<ParamId>(index), physical) : 0.0f;
}

const char* atom_params_json(void) { return atom::core_synth::paramsJson(); }

int atom_synth_apply_preset(AtomSynth* s, const char* text) {
    return s ? atom::core_synth::applyPreset(s->synth, text) : 0;
}

void atom_synth_render(AtomSynth* s, int frames) {
    if (!s) return;
    if (frames > ATOM_MAX_BLOCK) frames = ATOM_MAX_BLOCK;
    if (frames <= 0) return;
    int rendered = 0;
    // render up to each sequencer event, apply it, continue: sample-accurate
    s->seq.advance(frames, s->synth.sampleRate(), [&](int offset, int note, int velocity) {
        if (offset > rendered) {
            s->synth.render(s->left + rendered, s->right + rendered, offset - rendered);
            rendered = offset;
        }
        if (velocity > 0) s->synth.noteOn(note, velocity);
        else s->synth.noteOff(note);
    });
    if (rendered < frames) s->synth.render(s->left + rendered, s->right + rendered, frames - rendered);
}

int atom_seq_load(AtomSynth* s, const char* text) {
    if (!s) return 0;
    const bool wasPlaying = s->seq.playing();
    const int notes = s->seq.load(text);
    if (notes > 0) {
        s->synth.allNotesOff();
        s->seq.setPlaying(wasPlaying);
    }
    return notes;
}

void atom_seq_play(AtomSynth* s, int playing) {
    if (!s) return;
    s->seq.setPlaying(playing != 0);
    if (!playing) s->synth.allNotesOff();
}

void atom_seq_set_bpm(AtomSynth* s, double bpm) { if (s) s->seq.setBpm(bpm); }
void atom_seq_set_loop(AtomSynth* s, int loop) { if (s) s->seq.setLoop(loop != 0); }
void atom_seq_rewind(AtomSynth* s) {
    if (!s) return;
    s->seq.rewind();
    s->synth.allNotesOff();
}
double atom_seq_beat(AtomSynth* s) { return s ? s->seq.beat() : 0.0; }
double atom_seq_length(AtomSynth* s) { return s ? s->seq.lengthBeats() : 0.0; }
double atom_seq_pattern_bpm(AtomSynth* s) { return s ? s->seq.patternBpm() : 0.0; }
int atom_seq_playing(AtomSynth* s) { return s && s->seq.playing() ? 1 : 0; }

char* atom_text_buffer(AtomSynth* s, int min_size) {
    if (!s || min_size > static_cast<int>(sizeof s->text)) return nullptr;
    return s->text;
}

float* atom_synth_out_left(AtomSynth* s) { return s ? s->left : nullptr; }
float* atom_synth_out_right(AtomSynth* s) { return s ? s->right : nullptr; }

float* atom_synth_read_meter(AtomSynth* s) {
    if (!s) return nullptr;
    const auto m = s->synth.readMeter();
    s->meter[0] = m.peakL;
    s->meter[1] = m.peakR;
    s->meter[2] = m.rmsL;
    s->meter[3] = m.rmsR;
    s->meter[4] = m.gainReductionDb;
    s->meter[5] = static_cast<float>(m.activeVoices);
    s->meter[6] = static_cast<float>(m.backstopHits);
    s->meter[7] = static_cast<float>(m.nonFiniteResets);
    return s->meter;
}

}  // extern "C"
