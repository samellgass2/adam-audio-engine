/* Plain-C surface of the core synth: the portable seam every non-C++ host
 * (the WASM AudioWorklet today) binds to. One instance per handle; output is
 * written into the handle's own L/R buffers (max ADAM_MAX_BLOCK frames per
 * render call) so a WASM host can read them straight from linear memory. */
#ifndef ADAM_CORE_SYNTH_C_API_H
#define ADAM_CORE_SYNTH_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

#define ADAM_MAX_BLOCK 1024

typedef struct AdamSynth AdamSynth;

AdamSynth* adam_synth_create(float sample_rate);
void adam_synth_destroy(AdamSynth* s);
void adam_synth_reset(AdamSynth* s);

void adam_synth_note_on(AdamSynth* s, int note, int velocity);
void adam_synth_note_off(AdamSynth* s, int note);
void adam_synth_sustain(AdamSynth* s, int down);
void adam_synth_all_notes_off(AdamSynth* s);
void adam_synth_all_sound_off(AdamSynth* s);

/* Parameters by index (see adam_params_json for the contract). */
void adam_synth_set_param(AdamSynth* s, int index, float physical);
float adam_synth_get_param(AdamSynth* s, int index);
int adam_param_count(void);
float adam_param_to_physical(int index, float normalized);
float adam_param_to_normalized(int index, float physical);
const char* adam_params_json(void);
/* Apply a preset text ("<param_id> <value>" lines) on top of the defaults.
 * Returns the number of params applied. */
int adam_synth_apply_preset(AdamSynth* s, const char* preset_text);

/* Jump smoothed parameters to their targets (call after loading a patch). */
void adam_synth_snap_params(AdamSynth* s);

/* Render `frames` (<= ADAM_MAX_BLOCK) into the handle's buffers. */
void adam_synth_render(AdamSynth* s, int frames);
float* adam_synth_out_left(AdamSynth* s);
float* adam_synth_out_right(AdamSynth* s);

/* Pattern playback (the shared sequencer; see dsp/include/adam/seq). While
 * playing, adam_synth_render advances it and applies its notes sample-
 * accurately. load returns the number of notes parsed (0 = rejected). */
int adam_seq_load(AdamSynth* s, const char* pattern_text);
void adam_seq_play(AdamSynth* s, int playing);  /* stop also releases notes */
void adam_seq_set_bpm(AdamSynth* s, double bpm);
void adam_seq_set_loop(AdamSynth* s, int loop);
void adam_seq_rewind(AdamSynth* s);
double adam_seq_beat(AdamSynth* s);
double adam_seq_length(AdamSynth* s);
double adam_seq_pattern_bpm(AdamSynth* s);
int adam_seq_playing(AdamSynth* s);

void adam_seq_seek(AdamSynth* s, double beat);

/* Mix bus: the final safety stage when several synths (looper layers) are
 * summed. Write the summed L/R into adam_bus_left/right, then process in
 * place: DC-safe lookahead limiter at -1 dBFS. Meter like the synth's:
 * [peakL, peakR, rmsL, rmsR, gainReductionDb, 0, backstopHits, 0]. */
typedef struct AdamBus AdamBus;
AdamBus* adam_bus_create(float sample_rate);
float* adam_bus_left(AdamBus* b);
float* adam_bus_right(AdamBus* b);
void adam_bus_process(AdamBus* b, int frames);
float* adam_bus_read_meter(AdamBus* b);

/* Scratch buffer for passing text in (e.g. a pattern from JS). */
char* adam_text_buffer(AdamSynth* s, int min_size);

/* Analysis — what an agent sees (dsp/include/adam/analysis). Write up to
 * ADAM_ANALYSIS_MAX frames of L and R into adam_analysis_input(), then call
 * adam_analyze(frames, sample_rate) (frames a power of two). Returns a
 * pointer to ADAM_ANALYSIS_FIELDS floats:
 *  0 peakDb  1 rmsDb  2 crestDb  3 dc  4 centroidHz  5 rolloffHz  6 flatness
 *  7 f0Hz  8 f0Confidence  9 f0Midi  10 inharmonicDb  11..14 band share dB
 *  (<200, 200-2k, 2k-8k, >8k)  15 stereoCorrelation  16 harmonicCount
 *  17..28 harmonics H1..H12 dB rel. H1 */
#define ADAM_ANALYSIS_MAX 16384
#define ADAM_ANALYSIS_FIELDS 29
float* adam_analysis_input(void); /* [L x MAX][R x MAX] */
float* adam_analyze(int frames, float sample_rate);

/* Meter snapshot, written into the handle's meter array (read + clear):
 * [peakL, peakR, rmsL, rmsR, gainReductionDb, activeVoices, backstopHits,
 *  nonFiniteResets]. */
float* adam_synth_read_meter(AdamSynth* s);

#ifdef __cplusplus
}
#endif

#endif
