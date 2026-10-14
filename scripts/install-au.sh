#!/usr/bin/env bash
# Build the Audio Unit(s), install them for Logic, validate with auval, and
# prove the installed AU sounds the same as the engine (adam-au-host renders
# through the real plugin API; compare_wav.py checks the residual).
#
#   scripts/install-au.sh            # build + install + auval + parity
#   SKIP_PARITY=1 scripts/install-au.sh
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build/au -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DADAM_BUILD_AU=ON -DADAM_BUILD_TESTS=OFF -DADAM_BUILD_RENDERER=OFF >/dev/null
# JUCE copies the .component into ~/Library/Audio/Plug-Ins/Components
cmake --build build/au --target AdamCoreSynth_AU adam-au-host

# make the system (and Logic) rescan components
killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 2

echo "== auval aumu AdCs Adam"
auval -v aumu AdCs Adam > out/auval.log 2>&1 || { tail -30 out/auval.log; exit 1; }
grep -q "AU VALIDATION SUCCEEDED" out/auval.log && echo "auval: AU VALIDATION SUCCEEDED"

if [[ -z "${SKIP_PARITY:-}" ]]; then
  cmake --build build/native --target adam-render >/dev/null
  host=build/au/hosts/juce-au/adam-au-host_artefacts/Release/adam-au-host
  mkdir -p out
  for pair in "bright_chords tight_chords" "soft_pad pad_chords" "pluck staccato" "init dense_overlap" "supersaw tight_chords"; do
    read -r preset pattern <<<"$pair"
    ./build/native/adam-render --pattern "fixtures/patterns/$pattern.pat" --preset "fixtures/presets/$preset.preset" \
      --seconds 6 --tail 2 --out "out/native_$pattern.wav" >/dev/null
    "$host" --pattern "fixtures/patterns/$pattern.pat" --preset "fixtures/presets/$preset.preset" \
      --seconds 6 --tail 2 --out "out/au_$pattern.wav" >/dev/null
    printf "AU parity %-28s " "$preset/$pattern"
    python3 tools/compare_wav.py "out/native_$pattern.wav" "out/au_$pattern.wav"
  done
fi
echo "installed: ~/Library/Audio/Plug-Ins/Components/Adam Core Synth.component"
echo "In Logic: new Software Instrument track -> AU Instruments -> Adam -> Adam Core Synth"
