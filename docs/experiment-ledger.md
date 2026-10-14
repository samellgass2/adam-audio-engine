# Experiment ledger

One entry per meaningful DSP change: what changed, what we expected to hear,
what we measured, where the renders are, the human verdict, and the outcome.
Newest first. "Awaiting listen" means no human has judged it yet.

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
