#!/usr/bin/env bash
# Native DSP tests + the native/WASM parity check (the browser must make the
# same sound). Run scripts/build.sh first.
set -euo pipefail
cd "$(dirname "$0")/.."
./build/native/adam-tests
mkdir -p out
./build/native/adam-render --pattern fixtures/patterns/tight_chords.pat \
  --preset fixtures/presets/bright_chords.preset --seconds 8 --tail 0 --out out/parity_native.wav >/dev/null
node tools/wasm_parity.mjs build/wasm/adam_synth.wasm fixtures/patterns/tight_chords.pat \
  fixtures/presets/bright_chords.preset out/parity_native.wav 8
echo "all green"
