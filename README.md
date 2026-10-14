# Adam Audio Engine

Our own instruments and effects: one portable C++ DSP core, heard in a browser
workshop (WebAssembly + AudioWorklet) and — next — played in Logic Pro as an
Audio Unit. Spec: adamOS `docs/specs/ADAM_AUDIO_ENGINE_2026_10_13.md`.

*Build it, hear it, measure it, improve it.*

## What exists (checkpoints 0–2)

- **Core synth** (`instruments/core-synth`): 2 band-limited oscillators
  (sine/triangle/saw/pulse, PolyBLEP/BLAMP) + noise, resonant ZDF low-pass
  (12/24 dB), filter + amp ADSR, LFO → pitch/cutoff, glide, 16-voice
  polyphony (8 default) with click-free stealing, sustain pedal, stereo spread,
  DC blocker and a −1 dBFS lookahead limiter with an intersample-aware
  detector. 29 parameters, one contract (`params.cpp`).
- **One sequencer** (`dsp/include/adam/seq`): sample-accurate pattern playback
  on an integer sample clock, used by the renderer, the tests and the browser.
- **Hosts:** a C API (`c_api.h`) → standalone WASM (82 KB, zero imports) for
  the workshop; `adam-render` for headless renders; **an AUv2 instrument for
  Logic** (JUCE 9.0.4, `hosts/juce-au`), validated with `auval` and proven
  sample-identical to the engine through the real plugin API.
- **Workshop** (`workshop/src`): pattern + preset pickers, play/loop/tempo, all
  parameters live, meters, limiter and safety readout, spectrum + scope, an
  on-screen/computer keyboard, "Copy preset" for feedback.
- **What the agent sees** (`dsp/include/adam/analysis`): pitch (YIN),
  harmonic profile H1–H12, spectral centroid/rolloff/flatness, band shares,
  crest, stereo correlation, between-harmonic (aliasing) energy — one
  implementation used by the tests, `adam-render`'s JSON report and the
  workshop's live panel (via WASM), so ears and numbers can be compared.
- **Evidence:** `adam-tests` (661 checks on rendered audio) and a native↔WASM
  parity check (residual −125 dB).

## Prerequisites (macOS, Apple Silicon)

- Xcode command-line tools (clang), CMake ≥ 3.22, Ninja, Python 3, Node ≥ 18.
- Emscripten via emsdk, outside the repo:
  `git clone https://github.com/emscripten-core/emsdk ~/emsdk && ~/emsdk/emsdk install 6.0.12 && ~/emsdk/emsdk activate 6.0.12`
  (`scripts/build.sh` sources `~/emsdk/emsdk_env.sh`, or `$EMSDK`).

## Commands

```bash
scripts/build.sh     # native (tests + renderer) + WASM + dist/workshop
scripts/test.sh      # 661 checks + native/WASM parity
build/native/adam-render --pattern fixtures/patterns/pad_chords.pat \
  --preset fixtures/presets/soft_pad.preset --loops 2 --out out/pad.wav
python3 tools/gen_patterns.py   # regenerate fixtures/patterns + fixtures/midi
scripts/stage.sh     # rebuild the workshop in place (staging serves dist/workshop)
scripts/install-au.sh  # build + install the AU, auval, AU-vs-engine parity
```

Work lands on `feature/audio-engine`; Sam PRs it to `main`.

## Logic Pro

`scripts/install-au.sh` builds **Adam Core Synth** (`aumu AdCs Adam`), copies
it to `~/Library/Audio/Plug-Ins/Components`, rescans, runs `auval`, and renders
four preset/pattern pairs through the installed AU (`adam-au-host`, the real
plugin API) against the engine (`tools/compare_wav.py`, residual < −120 dB).
In Logic: new Software Instrument track → AU Instruments → Adam → Adam Core
Synth. Every parameter is automatable; the generic editor lists them all.

**Adding another instrument as an AU:** write a descriptor (engine type +
parameter table; see `hosts/juce-au/core_synth_plugin.cpp`) and one call:

```cmake
adam_add_audio_unit(AdamFoo PRODUCT_NAME "Adam Foo" PLUGIN_CODE AdFo
                    SOURCE foo_plugin.cpp ENGINE_LIB adam_foo)
```

Never change a shipped `PLUGIN_CODE`, the manufacturer code `Adam`, or a
parameter id: Logic projects key on them.

Staged at `https://controlplane.samellgass.com/staging?port=8432` (the
adamOS staging tool serves `dist/workshop`; a rebuild is live on reload).

## Layout

```
dsp/include/adam/dsp/     oscillator, envelope, filter, limiter, util (header-only)
dsp/include/adam/seq/     the shared pattern sequencer
instruments/core-synth/   params (the contract), engine, presets, C API
hosts/wasm/               the C API as a standalone WASM module
hosts/offline-renderer/   adam-render
workshop/src/             the browser workshop (static, relative URLs)
fixtures/                 patterns (.pat + .mid), presets
tests/                    adam-tests
tools/                    pattern generator, workshop assembler, parity check
docs/                     architecture, experiment ledger
```
