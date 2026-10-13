# Agent instructions — adam-audio-engine

Read this, `README.md`, `docs/architecture.md` and the top of
`docs/experiment-ledger.md` before changing anything.

## The rules that matter

1. **One implementation of the sound.** DSP lives in `dsp/` and
   `instruments/`. Hosts (WASM, renderer, AU) only move events in and samples
   out. Never write synthesis, mapping or parsing logic in JavaScript — the
   workshop asks the WASM (`adam_param_to_physical`, `adam_synth_apply_preset`,
   `adam_seq_*`).
2. **Parameter ids are forever.** `params.cpp` is the contract. Append new
   parameters at the end of `ParamId`; never renumber, rename or change a
   unit/range meaning once a Logic session may depend on it.
3. **Real-time safe.** No allocation, I/O, locks or exceptions in `render()`
   or anything it calls. Fixed storage only (see the sequencer's scratch).
4. **Say what you expect to hear, then measure it.** Every DSP change states
   its predicted audible effect and a test or render that checks it
   (`tests/test_main.cpp` shows the pattern). Record it in the ledger.
5. **Never claim a sound is good.** Tests prove correctness and safety; only
   Sam's listening (staged workshop) judges quality. Write "measured", not
   "sounds".
6. **Safety is not optional.** `patterns_render_clean` and
   `adversarial_extremes` must stay green: finite, ≤ −1 dBFS, backstop never
   engaged.

## Loop

```bash
scripts/build.sh && scripts/test.sh     # must be green
scripts/stage.sh                        # Sam reloads port 8432
```

Then add a ledger entry: change, intended difference, measurements, render
paths, human verdict (or "awaiting listen"), accepted/rejected.
