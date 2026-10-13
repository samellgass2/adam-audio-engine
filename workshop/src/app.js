// Atom Workshop — main thread. Builds the UI from the engine's own parameter
// contract and talks to the AudioWorklet. A non-rendering "mirror" instance of
// the same WASM answers every mapping/parsing question (normalized<->physical,
// presets, pattern tempo), so no synth logic is re-implemented in JavaScript.

const $ = (id) => document.getElementById(id);
const enc = new TextEncoder();
const dec = new TextDecoder();

const state = {
  wasmBytes: null,
  mirror: null, // { w, s }
  params: [],
  controls: new Map(), // index -> { set(physical) }
  ctx: null,
  node: null,
  gain: null,
  analyser: null,
  playing: false,
  loop: true,
  lengthBeats: 0,
  muted: false,
  octave: 0,
  peakHold: 0,
};

// ---------------------------------------------------------------- mirror
function mirrorText(text) {
  const { w, s } = state.mirror;
  const bytes = enc.encode(text);
  const ptr = w.atom_text_buffer(s, bytes.length + 1);
  const mem = new Uint8Array(w.memory.buffer, ptr, bytes.length + 1);
  mem.set(bytes);
  mem[bytes.length] = 0;
  return ptr;
}

function cString(w, ptr) {
  const mem = new Uint8Array(w.memory.buffer);
  let end = ptr;
  while (mem[end]) end++;
  return dec.decode(mem.subarray(ptr, end));
}

async function loadEngine() {
  const res = await fetch("./atom_synth.wasm");
  if (!res.ok) throw new Error(`atom_synth.wasm: HTTP ${res.status}`);
  state.wasmBytes = await res.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(state.wasmBytes, {});
  const w = instance.exports;
  w._initialize?.();
  state.mirror = { w, s: w.atom_synth_create(48000) };
  state.params = JSON.parse(cString(w, w.atom_params_json()));
}

const toPhysical = (i, n) => state.mirror.w.atom_param_to_physical(i, n);
const toNormalized = (i, v) => state.mirror.w.atom_param_to_normalized(i, v);
const getParam = (i) => state.mirror.w.atom_synth_get_param(state.mirror.s, i);

function setParam(i, physical) {
  const { w, s } = state.mirror;
  w.atom_synth_set_param(s, i, physical);
  const v = w.atom_synth_get_param(s, i); // clamped by the engine
  state.node?.port.postMessage({ type: "param", index: i, value: v });
  state.controls.get(i)?.set(v);
  savePreset();
  return v;
}

function presetText() {
  return state.params.map((p) => `${p.id} ${+getParam(p.index).toPrecision(6)}`).join("\n") + "\n";
}

function applyPresetText(text) {
  const { w, s } = state.mirror;
  w.atom_synth_apply_preset(s, mirrorText(text));
  for (const p of state.params) state.controls.get(p.index)?.set(getParam(p.index));
  state.node?.port.postMessage({ type: "preset", bytes: enc.encode(text) });
  savePreset();
}

function savePreset() {
  $("presetText").value = presetText();
  try {
    localStorage.setItem("atom.workshop.preset", $("presetText").value);
  } catch {
    /* a convenience only */
  }
}

// ------------------------------------------------------------------ UI
function fmt(p, v) {
  if (p.mapping === "choice") return p.choices[Math.round(v - p.min)] ?? v;
  if (p.unit === "Hz") return v >= 1000 ? `${(v / 1000).toFixed(2)} kHz` : `${v.toFixed(v < 10 ? 2 : 0)} Hz`;
  if (p.unit === "ms") return v >= 1000 ? `${(v / 1000).toFixed(2)} s` : `${v.toFixed(v < 10 ? 1 : 0)} ms`;
  if (p.mapping === "int") return `${v > 0 && p.min < 0 ? "+" : ""}${Math.round(v)}${p.unit ? " " + p.unit : ""}`;
  const digits = Math.abs(v) < 10 ? 1 : 0;
  return `${v.toFixed(digits)}${p.unit === "%" ? "%" : p.unit ? " " + p.unit : ""}`;
}

function buildParams() {
  const groups = new Map();
  for (const p of state.params) {
    if (!groups.has(p.group)) groups.set(p.group, []);
    groups.get(p.group).push(p);
  }
  const root = $("params");
  root.innerHTML = "";
  for (const [group, list] of groups) {
    const card = document.createElement("div");
    card.className = "card";
    card.innerHTML = `<h2>${group}</h2>`;
    for (const p of list) card.appendChild(p.mapping === "choice" ? choiceControl(p) : sliderControl(p));
    root.appendChild(card);
  }
}

function sliderControl(p) {
  const el = document.createElement("label");
  el.className = "param";
  el.innerHTML = `<div class="top"><span>${p.name}</span><b></b></div>
    <input type="range" min="0" max="1000" step="1" aria-label="${p.name}" />`;
  const out = el.querySelector("b");
  const input = el.querySelector("input");
  input.addEventListener("input", () => setParam(p.index, toPhysical(p.index, input.value / 1000)));
  input.addEventListener("dblclick", () => setParam(p.index, p.default));
  state.controls.set(p.index, {
    set(v) {
      input.value = Math.round(toNormalized(p.index, v) * 1000);
      out.textContent = fmt(p, v);
    },
  });
  state.controls.get(p.index).set(getParam(p.index));
  return el;
}

function choiceControl(p) {
  const el = document.createElement("div");
  el.className = "param";
  el.innerHTML = `<div class="top"><span>${p.name}</span></div><div class="choices"></div>`;
  const box = el.querySelector(".choices");
  const buttons = p.choices.map((label, k) => {
    const b = document.createElement("button");
    b.textContent = label;
    b.addEventListener("click", () => setParam(p.index, p.min + k));
    box.appendChild(b);
    return b;
  });
  state.controls.set(p.index, {
    set(v) {
      buttons.forEach((b, k) => b.classList.toggle("on", k === Math.round(v - p.min)));
    },
  });
  state.controls.get(p.index).set(getParam(p.index));
  return el;
}

// ------------------------------------------------------------- content
async function loadIndexes() {
  const [patterns, presets, build] = await Promise.all([
    fetch("./patterns/index.json").then((r) => r.json()),
    fetch("./presets/index.json").then((r) => r.json()),
    fetch("./build.json").then((r) => (r.ok ? r.json() : {})).catch(() => ({})),
  ]);
  $("build").textContent = build.commit ? `build ${build.commit} · ${build.built}` : "local build";
  const ps = $("pattern");
  for (const p of patterns) ps.add(new Option(p.name, p.file));
  ps.dataset.purposes = JSON.stringify(Object.fromEntries(patterns.map((p) => [p.file, p.purpose])));
  const pr = $("preset");
  for (const p of presets) pr.add(new Option(p.name, p.file));
  pr.add(new Option("(last session)", "__last"));
}

async function selectPattern(file) {
  const text = await fetch(`./patterns/${file}`).then((r) => r.text());
  const { w, s } = state.mirror;
  w.atom_seq_load(s, mirrorText(text));
  const bpm = Math.round(w.atom_seq_pattern_bpm(s));
  state.lengthBeats = w.atom_seq_length(s);
  $("bpm").value = bpm;
  $("bpmval").textContent = `${bpm} bpm`;
  $("purpose").textContent = JSON.parse($("pattern").dataset.purposes)[file] || "";
  state.node?.port.postMessage({ type: "pattern", bytes: enc.encode(text) });
  state.node?.port.postMessage({ type: "bpm", bpm });
  try {
    localStorage.setItem("atom.workshop.pattern", file);
  } catch {
    /* convenience */
  }
}

async function selectPreset(file) {
  if (file === "__last") {
    let text = null;
    try {
      text = localStorage.getItem("atom.workshop.preset");
    } catch {
      /* convenience */
    }
    if (text) applyPresetText(text);
    return;
  }
  applyPresetText(await fetch(`./presets/${file}`).then((r) => r.text()));
}

// ---------------------------------------------------------------- audio
async function startAudio() {
  const ctx = new AudioContext({ latencyHint: "interactive" });
  await ctx.audioWorklet.addModule("./worklet.js");
  const node = new AudioWorkletNode(ctx, "atom-synth", {
    numberOfInputs: 0,
    numberOfOutputs: 1,
    outputChannelCount: [2],
    processorOptions: { wasm: state.wasmBytes },
  });
  const analyser = ctx.createAnalyser();
  analyser.fftSize = 4096;
  analyser.smoothingTimeConstant = 0.75;
  const gain = ctx.createGain();
  node.connect(analyser);
  node.connect(gain).connect(ctx.destination);
  Object.assign(state, { ctx, node, analyser, gain });
  setVolume();

  await new Promise((resolve) => {
    node.port.onmessage = (e) => {
      if (e.data.type === "ready") resolve();
      onWorkletMessage(e.data);
    };
  });
  node.port.onmessage = (e) => onWorkletMessage(e.data);
  // bring the audio engine to the UI's state
  node.port.postMessage({ type: "preset", bytes: enc.encode(presetText()) });
  await selectPattern($("pattern").value);
  node.port.postMessage({ type: "loop", on: state.loop });
  await ctx.resume();
  $("app").classList.remove("locked");
  $("start").textContent = `Audio on · ${ctx.sampleRate / 1000} kHz`;
  $("start").disabled = true;
  requestAnimationFrame(draw);
}

function onWorkletMessage(m) {
  if (m.type !== "meter") return;
  const db = (x) => (x > 1e-6 ? 20 * Math.log10(x) : -120);
  const h = (x) => `${Math.max(0, Math.min(100, ((db(x) + 60) / 60) * 100))}%`;
  $("mL").style.height = h(m.peakL);
  $("mR").style.height = h(m.peakR);
  $("rL").style.height = h(m.rmsL);
  $("rR").style.height = h(m.rmsR);
  const pk = Math.max(m.peakL, m.peakR);
  state.peakHold = Math.max(pk, state.peakHold * 0.97);
  $("peakdb").textContent = db(state.peakHold) < -100 ? "−∞" : db(state.peakHold).toFixed(1);
  $("grdb").textContent = m.grDb.toFixed(1);
  $("voices").textContent = m.voices;
  const unsafe = m.backstop > 0 || m.nanResets > 0;
  $("safety").className = unsafe ? "bad" : "ok";
  $("safety").textContent = unsafe
    ? `safety stage engaged (backstop ${m.backstop}, NaN resets ${m.nanResets})`
    : "output safe";
  if (state.lengthBeats > 0) $("beatbar").style.width = `${((m.beat % state.lengthBeats) / state.lengthBeats) * 100}%`;
  if (state.playing && !m.playing) setPlaying(false); // a non-looping pattern ended
}

function setPlaying(on) {
  state.playing = on;
  $("play").textContent = on ? "❚❚ Stop" : "▶ Play";
  state.node?.port.postMessage({ type: "play", on });
  if (!on) state.node?.port.postMessage({ type: "rewind" });
}

function setVolume() {
  const v = Number($("volume").value);
  $("volval").textContent = `${Math.round(v * 100)}%`;
  // listening level only (the synth's own Master is a parameter)
  if (state.gain) state.gain.gain.setTargetAtTime(state.muted ? 0 : v * v, state.ctx.currentTime, 0.02);
}

// ---------------------------------------------------------------- scopes
function draw() {
  const a = state.analyser;
  if (a) {
    drawSpectrum(a);
    drawScope(a);
  }
  requestAnimationFrame(draw);
}

function canvasCtx(c) {
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr;
  const h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) {
    c.width = w;
    c.height = h;
  }
  const g = c.getContext("2d");
  g.clearRect(0, 0, w, h);
  return { g, w, h };
}

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function drawSpectrum(a) {
  const { g, w, h } = canvasCtx($("spectrum"));
  const bins = new Float32Array(a.frequencyBinCount);
  a.getFloatFrequencyData(bins);
  const nyq = state.ctx.sampleRate / 2;
  const fx = (f) => (Math.log10(f / 20) / Math.log10(20000 / 20)) * w;
  g.strokeStyle = css("--line");
  g.fillStyle = css("--muted");
  g.font = `${10 * (window.devicePixelRatio || 1)}px sans-serif`;
  for (const f of [100, 1000, 10000]) {
    g.beginPath();
    g.moveTo(fx(f), 0);
    g.lineTo(fx(f), h);
    g.stroke();
    g.fillText(f >= 1000 ? `${f / 1000}k` : `${f}`, fx(f) + 3, h - 4);
  }
  g.strokeStyle = css("--accent");
  g.lineWidth = 1.5 * (window.devicePixelRatio || 1);
  g.beginPath();
  let started = false;
  for (let i = 1; i < bins.length; i++) {
    const f = (i * nyq) / bins.length;
    if (f < 20 || f > 20000) continue;
    const y = h - ((Math.max(-110, bins[i]) + 110) / 110) * h;
    if (!started) {
      g.moveTo(fx(f), y);
      started = true;
    } else g.lineTo(fx(f), y);
  }
  g.stroke();
}

function drawScope(a) {
  const { g, w, h } = canvasCtx($("scope"));
  const buf = new Float32Array(a.fftSize);
  a.getFloatTimeDomainData(buf);
  // trigger on a rising zero crossing for a stable picture
  let start = 0;
  for (let i = 1; i < buf.length / 2; i++) if (buf[i - 1] < 0 && buf[i] >= 0) { start = i; break; }
  const span = Math.min(1024, buf.length - start);
  g.strokeStyle = css("--accent-2");
  g.lineWidth = 1.5 * (window.devicePixelRatio || 1);
  g.beginPath();
  for (let i = 0; i < span; i++) {
    const x = (i / span) * w;
    const y = h / 2 - buf[start + i] * (h / 2) * 0.95;
    i ? g.lineTo(x, y) : g.moveTo(x, y);
  }
  g.stroke();
}

// -------------------------------------------------------------- keyboard
const KEYMAP = "awsedftgyhujk"; // C C# D D# E F F# G G# A A# B C
const held = new Map(); // pointerId|key -> note

function noteOn(note, id) {
  if (held.has(id)) return;
  held.set(id, note);
  state.node?.port.postMessage({ type: "noteOn", note, velocity: 100 });
  document.querySelector(`.key[data-note="${note}"]`)?.classList.add("down");
}

function noteOff(id) {
  const note = held.get(id);
  if (note === undefined) return;
  held.delete(id);
  state.node?.port.postMessage({ type: "noteOff", note });
  if (![...held.values()].includes(note)) document.querySelector(`.key[data-note="${note}"]`)?.classList.remove("down");
}

function buildKeyboard() {
  const kb = $("keyboard");
  kb.innerHTML = "";
  const base = 60 + 12 * state.octave - 12; // two octaves from C3 (+ octave shift)
  const whites = [0, 2, 4, 5, 7, 9, 11];
  let whiteIndex = 0;
  const totalWhites = 15;
  for (let n = base; n <= base + 24; n++) {
    const pc = n % 12;
    const el = document.createElement("div");
    el.dataset.note = n;
    if (whites.includes(pc)) {
      el.className = "key";
      whiteIndex++;
    } else {
      el.className = "key black";
      el.style.left = `calc(${(whiteIndex / totalWhites) * 100}% - ${(100 / totalWhites) * 0.31}%)`;
    }
    kb.appendChild(el);
  }
}

function keyboardEvents() {
  const kb = $("keyboard");
  const noteAt = (e) => e.target.closest?.(".key")?.dataset.note;
  kb.addEventListener("pointerdown", (e) => {
    const n = noteAt(e);
    if (n === undefined) return;
    kb.setPointerCapture(e.pointerId);
    noteOn(Number(n), `p${e.pointerId}`);
  });
  kb.addEventListener("pointermove", (e) => {
    if (!held.has(`p${e.pointerId}`)) return;
    const el = document.elementFromPoint(e.clientX, e.clientY);
    const n = el?.closest?.(".key")?.dataset.note;
    if (n !== undefined && Number(n) !== held.get(`p${e.pointerId}`)) {
      noteOff(`p${e.pointerId}`);
      noteOn(Number(n), `p${e.pointerId}`);
    }
  });
  for (const t of ["pointerup", "pointercancel", "lostpointercapture"]) kb.addEventListener(t, (e) => noteOff(`p${e.pointerId}`));

  window.addEventListener("keydown", (e) => {
    if (e.repeat || e.target.closest?.("input,select,textarea")) return;
    const k = e.key.toLowerCase();
    if (k === "z" || k === "x") {
      state.octave = Math.max(-2, Math.min(2, state.octave + (k === "x" ? 1 : -1)));
      buildKeyboard();
      return;
    }
    const i = KEYMAP.indexOf(k);
    if (i >= 0) noteOn(60 + 12 * state.octave + i, `k${k}`);
  });
  window.addEventListener("keyup", (e) => noteOff(`k${e.key.toLowerCase()}`));
  window.addEventListener("blur", () => [...held.keys()].forEach(noteOff));
}

// ------------------------------------------------------------------ boot
async function boot() {
  try {
    await loadEngine();
    await loadIndexes();
    buildParams();
    buildKeyboard();
    keyboardEvents();
    let lastPattern = null;
    try {
      lastPattern = localStorage.getItem("atom.workshop.pattern");
    } catch {
      /* convenience */
    }
    if (lastPattern && [...$("pattern").options].some((o) => o.value === lastPattern)) $("pattern").value = lastPattern;
    await selectPreset($("preset").value);
    await selectPattern($("pattern").value);
  } catch (err) {
    $("purpose").textContent = `Engine failed to load: ${err.message}`;
    $("start").disabled = true;
    throw err;
  }

  $("start").addEventListener("click", () => startAudio().catch((err) => alert(`Audio failed: ${err.message}`)));
  $("play").addEventListener("click", () => setPlaying(!state.playing));
  $("loop").addEventListener("click", () => {
    state.loop = !state.loop;
    $("loop").classList.toggle("on", state.loop);
    $("loop").setAttribute("aria-pressed", String(state.loop));
    state.node?.port.postMessage({ type: "loop", on: state.loop });
  });
  $("panic").addEventListener("click", () => {
    setPlaying(false);
    state.node?.port.postMessage({ type: "panic" });
  });
  $("bpm").addEventListener("input", () => {
    $("bpmval").textContent = `${$("bpm").value} bpm`;
    state.node?.port.postMessage({ type: "bpm", bpm: Number($("bpm").value) });
  });
  $("pattern").addEventListener("change", () => selectPattern($("pattern").value));
  $("preset").addEventListener("change", () => selectPreset($("preset").value));
  $("volume").addEventListener("input", setVolume);
  $("mute").addEventListener("click", () => {
    state.muted = !state.muted;
    $("mute").setAttribute("aria-pressed", String(state.muted));
    setVolume();
  });
  $("copy").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(presetText());
      $("copy").textContent = "Copied ✓";
    } catch {
      $("presetText").select();
      $("copy").textContent = "Select + copy below";
    }
    setTimeout(() => ($("copy").textContent = "Copy preset"), 1600);
  });
}

boot();
