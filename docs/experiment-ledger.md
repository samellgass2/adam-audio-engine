# Experiment ledger

One entry per meaningful DSP change: what changed, what we expected to hear,
what we measured, where the renders are, the human verdict, and the outcome.
Newest first. "Awaiting listen" means no human has judged it yet.

## 2026-10-13 — Core synth v0 (checkpoints 0–2)

- **Change:** first engine — 2 PolyBLEP/BLAMP oscillators + noise, ZDF
  low-pass (12/24 dB), filter/amp ADSR, LFO, glide, 16-voice poly, limiter.
- **Expected:** clean, unaliased analog-style subtractive tones; pads swell
  smoothly; stabs are punchy; nothing clicks, sticks or clips.
- **Measured** (`build/native/atom-tests`, 661 checks, all green):
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
- **Human verdict:** awaiting listen (staged on port 8432).
- **Outcome:** accepted as the baseline.
