#include "c_api.h"

#include <new>

#include "adam/seq/sequencer.hpp"
#include "core_synth.hpp"
#include "preset.hpp"
#include "adam/analysis/analysis.hpp"

using adam::core_synth::CoreSynth;
using adam::core_synth::ParamId;

struct AdamSynth {
    CoreSynth synth;
    float left[ADAM_MAX_BLOCK];
    float right[ADAM_MAX_BLOCK];
    float meter[8];
    adam::seq::Sequencer seq;
    char text[65536];
};

namespace {
bool validIndex(int i) { return i >= 0 && i < static_cast<int>(adam::core_synth::kParamCount); }
}  // namespace

extern "C" {

AdamSynth* adam_synth_create(float sample_rate) {
    auto* s = new (std::nothrow) AdamSynth();
    if (!s) return nullptr;
    s->synth.prepare(sample_rate);
    return s;
}

void adam_synth_destroy(AdamSynth* s) { delete s; }
void adam_synth_reset(AdamSynth* s) { if (s) s->synth.reset(); }
void adam_synth_note_on(AdamSynth* s, int note, int velocity) { if (s) s->synth.noteOn(note, velocity); }
void adam_synth_note_off(AdamSynth* s, int note) { if (s) s->synth.noteOff(note); }
void adam_synth_sustain(AdamSynth* s, int down) { if (s) s->synth.sustainPedal(down != 0); }
void adam_synth_all_notes_off(AdamSynth* s) { if (s) s->synth.allNotesOff(); }
void adam_synth_all_sound_off(AdamSynth* s) { if (s) s->synth.allSoundOff(); }

void adam_synth_set_param(AdamSynth* s, int index, float physical) {
    if (s && validIndex(index)) s->synth.setParam(static_cast<ParamId>(index), physical);
}

float adam_synth_get_param(AdamSynth* s, int index) {
    return (s && validIndex(index)) ? s->synth.param(static_cast<ParamId>(index)) : 0.0f;
}

int adam_param_count(void) { return static_cast<int>(adam::core_synth::kParamCount); }

float adam_param_to_physical(int index, float normalized) {
    return validIndex(index) ? adam::core_synth::toPhysical(static_cast<ParamId>(index), normalized) : 0.0f;
}

float adam_param_to_normalized(int index, float physical) {
    return validIndex(index) ? adam::core_synth::toNormalized(static_cast<ParamId>(index), physical) : 0.0f;
}

const char* adam_params_json(void) { return adam::core_synth::paramsJson(); }

int adam_synth_apply_preset(AdamSynth* s, const char* text) {
    return s ? adam::core_synth::applyPreset(s->synth, text) : 0;
}

void adam_synth_snap_params(AdamSynth* s) { if (s) s->synth.snapSmoothing(); }

void adam_synth_render(AdamSynth* s, int frames) {
    if (!s) return;
    if (frames > ADAM_MAX_BLOCK) frames = ADAM_MAX_BLOCK;
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

int adam_seq_load(AdamSynth* s, const char* text) {
    if (!s) return 0;
    const bool wasPlaying = s->seq.playing();
    const int notes = s->seq.load(text);
    if (notes > 0) {
        s->synth.allNotesOff();
        s->seq.setPlaying(wasPlaying);
    }
    return notes;
}

void adam_seq_play(AdamSynth* s, int playing) {
    if (!s) return;
    s->seq.setPlaying(playing != 0);
    if (!playing) s->synth.allNotesOff();
}

void adam_seq_set_bpm(AdamSynth* s, double bpm) { if (s) s->seq.setBpm(bpm); }
void adam_seq_set_loop(AdamSynth* s, int loop) { if (s) s->seq.setLoop(loop != 0); }
void adam_seq_rewind(AdamSynth* s) {
    if (!s) return;
    s->seq.rewind();
    s->synth.allNotesOff();
}
double adam_seq_beat(AdamSynth* s) { return s ? s->seq.beat() : 0.0; }
double adam_seq_length(AdamSynth* s) { return s ? s->seq.lengthBeats() : 0.0; }
double adam_seq_pattern_bpm(AdamSynth* s) { return s ? s->seq.patternBpm() : 0.0; }
int adam_seq_playing(AdamSynth* s) { return s && s->seq.playing() ? 1 : 0; }

char* adam_text_buffer(AdamSynth* s, int min_size) {
    if (!s || min_size > static_cast<int>(sizeof s->text)) return nullptr;
    return s->text;
}

float* adam_synth_out_left(AdamSynth* s) { return s ? s->left : nullptr; }
float* adam_synth_out_right(AdamSynth* s) { return s ? s->right : nullptr; }

static float g_analysisIn[2 * ADAM_ANALYSIS_MAX];
static float g_analysisOut[ADAM_ANALYSIS_FIELDS];

float* adam_analysis_input(void) { return g_analysisIn; }

float* adam_analyze(int frames, float sample_rate) {
    size_t n = 1;
    while (n * 2 <= static_cast<size_t>(frames) && n * 2 <= ADAM_ANALYSIS_MAX) n *= 2;
    const auto r = adam::analysis::analyze(g_analysisIn, g_analysisIn + ADAM_ANALYSIS_MAX, n, sample_rate);
    float* o = g_analysisOut;
    const double head[] = {r.peakDb, r.rmsDb, r.crestDb, r.dc, r.centroidHz, r.rolloffHz, r.flatness,
                           r.f0Hz, r.f0Confidence, r.f0Midi, r.inharmonicDb};
    for (int i = 0; i < 11; ++i) o[i] = static_cast<float>(head[i]);
    for (int i = 0; i < 4; ++i) o[11 + i] = static_cast<float>(r.bandDb[i]);
    o[15] = static_cast<float>(r.stereoCorrelation);
    o[16] = static_cast<float>(r.harmonicCount);
    for (int i = 0; i < 12; ++i) o[17 + i] = static_cast<float>(r.harmonicsDb[i]);
    return o;
}

float* adam_synth_read_meter(AdamSynth* s) {
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
