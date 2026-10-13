/* Plain-C surface of the core synth: the portable seam every non-C++ host
 * (the WASM AudioWorklet today) binds to. One instance per handle; output is
 * written into the handle's own L/R buffers (max ATOM_MAX_BLOCK frames per
 * render call) so a WASM host can read them straight from linear memory. */
#ifndef ATOM_CORE_SYNTH_C_API_H
#define ATOM_CORE_SYNTH_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

#define ATOM_MAX_BLOCK 1024

typedef struct AtomSynth AtomSynth;

AtomSynth* atom_synth_create(float sample_rate);
void atom_synth_destroy(AtomSynth* s);
void atom_synth_reset(AtomSynth* s);

void atom_synth_note_on(AtomSynth* s, int note, int velocity);
void atom_synth_note_off(AtomSynth* s, int note);
void atom_synth_sustain(AtomSynth* s, int down);
void atom_synth_all_notes_off(AtomSynth* s);
void atom_synth_all_sound_off(AtomSynth* s);

/* Parameters by index (see atom_params_json for the contract). */
void atom_synth_set_param(AtomSynth* s, int index, float physical);
float atom_synth_get_param(AtomSynth* s, int index);
int atom_param_count(void);
float atom_param_to_physical(int index, float normalized);
float atom_param_to_normalized(int index, float physical);
const char* atom_params_json(void);
/* Apply a preset text ("<param_id> <value>" lines) on top of the defaults.
 * Returns the number of params applied. */
int atom_synth_apply_preset(AtomSynth* s, const char* preset_text);

/* Render `frames` (<= ATOM_MAX_BLOCK) into the handle's buffers. */
void atom_synth_render(AtomSynth* s, int frames);
float* atom_synth_out_left(AtomSynth* s);
float* atom_synth_out_right(AtomSynth* s);

/* Pattern playback (the shared sequencer; see dsp/include/atom/seq). While
 * playing, atom_synth_render advances it and applies its notes sample-
 * accurately. load returns the number of notes parsed (0 = rejected). */
int atom_seq_load(AtomSynth* s, const char* pattern_text);
void atom_seq_play(AtomSynth* s, int playing);  /* stop also releases notes */
void atom_seq_set_bpm(AtomSynth* s, double bpm);
void atom_seq_set_loop(AtomSynth* s, int loop);
void atom_seq_rewind(AtomSynth* s);
double atom_seq_beat(AtomSynth* s);
double atom_seq_length(AtomSynth* s);
double atom_seq_pattern_bpm(AtomSynth* s);
int atom_seq_playing(AtomSynth* s);

/* Scratch buffer for passing text in (e.g. a pattern from JS). */
char* atom_text_buffer(AtomSynth* s, int min_size);

/* Meter snapshot, written into the handle's meter array (read + clear):
 * [peakL, peakR, rmsL, rmsR, gainReductionDb, activeVoices, backstopHits,
 *  nonFiniteResets]. */
float* atom_synth_read_meter(AtomSynth* s);

#ifdef __cplusplus
}
#endif

#endif
