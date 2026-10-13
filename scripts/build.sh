#!/usr/bin/env bash
# Build everything: native (tests + renderer), WASM, and the static workshop.
#   scripts/build.sh            -> build/native, build/wasm, dist/workshop
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/native

if ! command -v emcc >/dev/null 2>&1; then
  # emsdk lives outside the repo (see README: Prerequisites)
  # shellcheck disable=SC1091
  source "${EMSDK:-$HOME/emsdk}/emsdk_env.sh" >/dev/null 2>&1
fi
emcmake cmake -S . -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/wasm

python3 tools/gen_patterns.py >/dev/null
python3 tools/build_workshop.py
