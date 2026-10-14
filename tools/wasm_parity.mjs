#!/usr/bin/env node
// Parity: the WASM build must make the same sound as the native build.
// Renders a pattern + preset through adam_synth.wasm (exactly as the worklet
// drives it: 128-frame blocks) and compares against adam-render's WAV.
//
//   node tools/wasm_parity.mjs <adam_synth.wasm> <pattern.pat> <preset> <native.wav> [seconds]
import fs from "node:fs";

const [wasmPath, patPath, presetPath, wavPath, secondsArg] = process.argv.slice(2);
if (!wavPath) {
  console.error("usage: wasm_parity.mjs <wasm> <pattern> <preset> <native.wav> [seconds]");
  process.exit(1);
}

const w = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(wasmPath)), {}).exports;
w._initialize?.();
const sr = 48000;
const s = w.adam_synth_create(sr);
const enc = new TextEncoder();
const text = (t) => {
  const b = enc.encode(t);
  const p = w.adam_text_buffer(s, b.length + 1);
  const m = new Uint8Array(w.memory.buffer, p, b.length + 1);
  m.set(b);
  m[b.length] = 0;
  return p;
};
w.adam_synth_apply_preset(s, text(fs.readFileSync(presetPath, "utf8")));
w.adam_synth_snap_params(s); // load the patch exactly as adam-render does
if (w.adam_seq_load(s, text(fs.readFileSync(patPath, "utf8"))) <= 0) throw new Error("pattern rejected");
w.adam_seq_set_bpm(s, w.adam_seq_pattern_bpm(s));
w.adam_seq_set_loop(s, 1);
w.adam_seq_play(s, 1);

// native WAV (32-bit float, stereo, 44-byte header from adam-render)
const wav = fs.readFileSync(wavPath);
const native = new Float32Array(wav.buffer.slice(wav.byteOffset + 44, wav.byteOffset + wav.length));
const frames = Math.min(native.length / 2, Math.round(Number(secondsArg || 8) * sr));

let maxDiff = 0, sumSq = 0, sumDiffSq = 0;
const t0 = performance.now();
for (let d = 0; d < frames; d += 128) {
  const n = Math.min(128, frames - d);
  w.adam_synth_render(s, n);
  const L = new Float32Array(w.memory.buffer, w.adam_synth_out_left(s), n);
  const R = new Float32Array(w.memory.buffer, w.adam_synth_out_right(s), n);
  for (let i = 0; i < n; i++) {
    const a = native[(d + i) * 2], b = native[(d + i) * 2 + 1];
    const dl = L[i] - a, dr = R[i] - b;
    maxDiff = Math.max(maxDiff, Math.abs(dl), Math.abs(dr));
    sumSq += a * a + b * b;
    sumDiffSq += dl * dl + dr * dr;
  }
}
const ms = performance.now() - t0;
const residualDb = 10 * Math.log10(sumDiffSq / Math.max(sumSq, 1e-30) + 1e-30);
const realtime = (frames / sr) * 1000 / ms;
console.log(JSON.stringify({ frames, maxAbsDiff: maxDiff, residualDb: +residualDb.toFixed(1),
  wasmRealtimeFactor: +realtime.toFixed(1) }));
// Same algorithm, two compilers (native arm64 may fuse multiply-adds): the
// residual must sit far below audibility.
process.exit(residualDb < -80 ? 0 : 1);
