# Architecture

```
            ┌──────────────── one C++ implementation ────────────────┐
 pattern ─▶ │ Sequencer (integer sample clock) ─▶ CoreSynth::render   │
 preset  ─▶ │ applyPreset ─▶ params (clamped, smoothed)               │
 notes   ─▶ │ voices: osc1+osc2+noise ─▶ LP filter ─▶ amp env ─▶ pan  │
            │ Σ ─▶ master ─▶ DC block ─▶ lookahead limiter (−1 dBFS)  │
            └───────────────────────── C API ─────────────────────────┘
                 │                     │                      │
          WASM (standalone)      atom-render (native)    JUCE AU (next)
          AudioWorklet           WAV + JSON stats        Logic Pro
          workshop UI            tests / agents
```

## Decisions

- **C API as the portable seam.** `c_api.h` is what non-C++ hosts bind to; the
  WASM is that API compiled with `-sSTANDALONE_WASM --no-entry` (no Emscripten
  JS glue, zero imports), so the AudioWorklet instantiates it directly.
- **The sequencer is engine code.** Pattern playback runs inside the engine on
  an integer sample clock; the browser never schedules notes with timers, and
  loop-boundary behaviour is tested natively and identical in the browser.
- **The UI asks the engine.** The workshop runs a second, non-rendering
  instance of the same WASM on the main thread for parameter mapping, preset
  parsing and pattern tempo.
- **Parity is measured.** `tools/wasm_parity.mjs` renders the same pattern
  through the WASM and compares with the native WAV (residual −125 dB on
  2026-10-13; native arm64 may fuse multiply-adds, so bit identity is not
  expected).

## DSP choices (first cut)

| Block | Choice | Why |
|---|---|---|
| Saw / pulse | 2-point PolyBLEP | cheap, good to ~G#6; see ledger for G7 |
| Triangle | PolyBLAMP | slope discontinuities, −60 dB inharmonic at 2.5 kHz |
| Filter | Simper ZDF SVF, ×2 for 24 dB | stable under per-sample modulation and any Q |
| Envelopes | exponential RC segments, retrigger from current level | analog feel, click-free stealing |
| Limiter | min-hold + box-average gain, cubic midpoint detector | provably reaches the needed gain within the lookahead |
| Voice gain | fixed 0.28 per voice | headroom for 8 voices; limiter catches the rest |
