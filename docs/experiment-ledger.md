# Experiment ledger

One entry per meaningful DSP change: what changed, what we expected to hear,
what we measured, where the renders are, the human verdict, and the outcome.
Newest first. "Awaiting listen" means no human has judged it yet.

## 2026-10-14 — Drum kit with liveness; Adam Drums AU; workshop as a tiny DAW

- **Change:** `adam::Instrument` interface (synth now implements it; output
  bit-identical). Drum kit engine (kick/snare/clap/hats/toms/rim; velocity
  -> timbre; deterministic humanize). Adam Drums AU via the generic
  descriptor. Workshop: per-layer instrument, piano roll + step entry, pads,
  key/scale keyboard, empty loops.
- **Expected:** a punchy, playable kit whose repeats breathe a little; harder
  hits brighter, not only louder.
- **Measured:** kick 140 Hz at the hit -> 50.3 Hz (tuned 50), -55 dB by its
  decay; humanize 0 = identical hits, 60 = varied; snare centroid 8.1 ->
  9.3 kHz from velocity 40 -> 127 (first build only +5% — fixed); closed hat
  chokes open; 3 kits x 3 beats and 30 extreme trials safe; Adam Drums auval
  OK, AU parity -134 / -129 dB.
- **Human verdict:** awaiting listen (pads and beats in the workshop; Adam
  Drums in Logic).
- **Outcome:** accepted pending ears.

## 2026-10-13 — Drive, unison (supersaw), delayed vibrato

- **Change:** six parameters appended (ids frozen above them):
  `filter_drive` (tanh saturation before the filter, peak-normalised),
  `unison_voices` 1–7 + `unison_detune` (symmetric spread, golden-ratio start
  phases, 1/sqrt(N) incoherent-sum normalisation, applied to both
  oscillators), `vibrato_depth`/`rate`/`delay` (per-note LFO: none for the
  delay, then a fade-in over half of it). New preset `supersaw`.
- **Expected:** drive = warmer/grittier via odd harmonics without a big
  level jump; unison = thick, chorused, wide-sounding chords at similar
  loudness; vibrato = singing pitch wobble that arrives after the delay.
- **Measured:** drive 100% on a sine: H3 −113.8 → −9.8 dB, H2 stays −105 dB
  (symmetric), level +2.7 dB. Unison 1 → 7 voices at 30 cents:
  between-harmonic energy −19.2 → −7.6 dB, level +1.1 dB. Vibrato 30 cents at
  5 Hz: pitch swings −29.8…+29.9 cents; with a 1 s delay the first 300 ms stay
  at 0.1 cents (the first build ramped from note-on and leaked ±8 cents —
  caught by this test, now classic delay-then-fade). Defaults render
  bit-identically to before. Worst case (supersaw, dense clusters): 0.16 ms of
  a 2.67 ms block natively; WASM 41× real time; WASM parity −98 dB (more libm
  calls than before, still far below audibility); AU parity −124 dB; auval OK.
- **Human verdict:** awaiting listen (workshop 8432 and Logic).
- **Outcome:** accepted pending ears.

## 2026-10-13 — Analysis: what the agent sees (shared by tests, renderer, workshop)

- **Change:** `adam::analysis` (FFT, YIN pitch, harmonics, centroid/rolloff/
  flatness, bands, crest, stereo, between-harmonic energy); C API
  `adam_analyze`; renderer JSON `analysis` block (loudest 8192 samples);
  workshop panel on the most recent 85 ms.
- **Expected:** the numbers a human sees are the agent's numbers, and they
  track what is heard (cutoff down -> centroid down; saw H_n = -20log10 n).
- **Measured:** sine 440 Hz read to 0.1%, H2 < -60 dB; saw 220 Hz H2 -6.02,
  H3 -9.55 dB (theory -6.02 / -9.54); centroid 198 Hz -> 1474 Hz as cutoff
  goes 400 Hz -> 8 kHz. Two analyzer bugs caught by these tests: max-bin
  harmonic levels suffered scalloping (now main-lobe sums), and a power-
  weighted centroid over-weighted the fundamental (now magnitude, as librosa;
  rolloff likewise, so centroid < rolloff reads sensibly).
- **Human verdict:** awaiting Sam — does "brightness" move the way the sound does?
- **Outcome:** accepted.

## 2026-10-13 — Logic AU (checkpoint 4, engineering half)

- **Change:** generic JUCE wrapper (`AdamInstrumentProcessor<Descriptor>`) +
  `adam_add_audio_unit()`; Adam Core Synth as AUv2 `aumu AdCs Adam`, JUCE
  9.0.4 pinned. Parameters come from the engine contract with the engine's own
  normalized<->physical mapping (JUCE publishes AU params as normalized 0..1,
  so Logic automation moves along the same curve as the workshop sliders).
  Sample-accurate MIDI, sustain, all-notes/sound off, state save/restore,
  reported latency = limiter lookahead.
- **Expected:** the AU is the same instrument as the workshop, sample for sample.
- **Measured:** `auval -v aumu AdCs Adam` → AU VALIDATION SUCCEEDED; ad-hoc
  signature valid. `adam-au-host` (the installed component via JUCE's AU
  hosting) vs `adam-render`, 6 s + 2 s tail, lag 0: bright_chords/tight_chords
  −129.0 dB, soft_pad/pad_chords −123.4 dB, pluck/staccato −133.7 dB,
  init/dense_overlap −130.6 dB residual.
- **Found on the way:** (1) display names with "→" did not survive the AU
  name round-trip — now ASCII ("LFO to Pitch"), with a test; (2) a reopened
  project would glide its first ~20 ms in from the defaults — engines now
  `snapSmoothing()` after a patch loads (AU prepare, renderer).
- **Human verdict:** awaiting Sam in Logic on hardware (keyboard, recording,
  automation, save/reopen, sample-rate and buffer changes) — and the keeper
  presets deferred from checkpoint 3.
- **Outcome:** accepted pending the Logic session.

## 2026-10-13 — Core synth v0 (checkpoints 0–2)

- **Change:** first engine — 2 PolyBLEP/BLAMP oscillators + noise, ZDF
  low-pass (12/24 dB), filter/amp ADSR, LFO, glide, 16-voice poly, limiter.
- **Expected:** clean, unaliased analog-style subtractive tones; pads swell
  smoothly; stabs are punchy; nothing clicks, sticks or clips.
- **Measured** (`build/native/adam-tests`, 661 checks, all green):
  - pitch within 0.05% at 44.1/48/96 kHz, MIDI 33–93;
  - saw inharmonic (aliasing) energy: naive −14.6 → PolyBLEP −33.9 dB at
    1661 Hz; −11.5 → −29.8 dB at 3136 Hz (G7);
  - triangle −40.4 → −59.8 dB, square −14.9 → −37.7 dB at 2489 Hz;
  - resonance 0 → 90% lifts the third-octave around a 1 kHz cutoff from
    −34.2 to −13.8 dB of total (noise in);
  - limiter: a +12 dB-over sine comes out at −1.00 dBFS sample and true peak;
  - 4 presets × 7 patterns: finite, ≤ −1 dBFS, backstop 0, no stuck voices;
  - 60 randomized extreme trials (+6 dB master, 16 notes, odd blocks): same;
  - renders bit-identical run to run and across block sizes 64/128/333;
  - WASM vs native residual −125 dB; WASM 130× real time, native 140×.
- **Known limits:** saw/pulse aliasing at G7 ~−30 dB (next: higher-order BLEP
  or 2× oversampling the oscillator); filter has no drive/saturation; one LFO
  shared by all voices; envelopes not velocity-scaled in time.
- **Human verdict (Sam, 2026-10-13, phone, workshop):** "fantastic… a really
  clear sound." Soft pad on the slow chords is good; the cutoff range and its
  default are sensible (tried very low and very high); detuning works;
  triangle/square swaps fine; staccato fine; nothing clicky, nothing thin.
  Friction: everything-as-sliders is unintuitive on a phone; semitone
  shifting (a −24..+24 slider) is awkward. Keeper presets are deferred to
  Logic on real hardware — checkpoint 3's remainder moves to checkpoint 4.
- **Outcome:** accepted as the baseline.
