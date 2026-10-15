// Adam Workshop — main thread. A tiny DAW over the shared C++ engine:
// layers (synth or drum kit), a piano roll per layer, step entry, a
// key-locked keyboard and drum pads, live meters and "what the agent sees".
//
// The engine (WASM, in the AudioWorklet) makes every sound and plays every
// pattern. This thread owns the editable notes of each layer and turns them
// into pattern text for the engine. A non-rendering mirror instance of the
// same WASM per instrument kind answers parameter mapping / preset / tempo
// questions, so no synth logic is re-implemented in JavaScript.
import { PianoRoll, DRUM_ROWS, noteName } from "./pianoroll.js";
import { sessions as sessionStore, audio as audioStore } from "./store.js";

const $ = (id) => document.getElementById(id);
const enc = new TextEncoder();
const dec = new TextDecoder();
const KIND = { synth: 0, drums: 1 };
const MAX_LAYERS = 5;
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const SCALES = {
  chromatic: ["Chromatic (piano)", null],
  major: ["Major", [0, 2, 4, 5, 7, 9, 11]],
  minor: ["Minor", [0, 2, 3, 5, 7, 8, 10]],
  majpent: ["Major pentatonic", [0, 2, 4, 7, 9]],
  minpent: ["Minor pentatonic", [0, 3, 5, 7, 10]],
  dorian: ["Dorian", [0, 2, 3, 5, 7, 9, 10]],
  mixolydian: ["Mixolydian", [0, 2, 4, 5, 7, 9, 10]],
  blues: ["Blues", [0, 3, 5, 6, 7, 10]],
};

const state = {
  wasmBytes: null,
  w: null, // the main-thread WASM exports
  mirrors: {}, // kind -> synth handle (never rendered)
  paramsByKind: {}, // kind -> param contract
  controls: new Map(),
  ctx: null,
  node: null,
  gain: null,
  analyser: null,
  playing: false,
  loop: true,
  lengthBeats: 16,
  bpm: 100,
  muted: false,
  octave: 0,
  peakHold: 0,
  keyRoot: 0,
  keyScale: "chromatic",
  presets: [],
  patterns: [],
  // layer 0 = the backing pattern (and the loop's clock)
  layers: [{ name: "Backing", kind: "synth", presetText: "", presetName: "", muted: false, recording: false, notes: [] }],
  selected: 0,
  roll: null,
  sessionName: "Untitled",
  dirty: false,
};

const layer = () => state.layers[state.selected];
const kind = () => layer().kind;
const isAudio = () => kind() === "audio";
const mirror = () => state.mirrors[kind()];
const params = () => state.paramsByKind[kind()] || [];

// ---------------------------------------------------------------- engine
function writeText(handle, text) {
  const w = state.w;
  const bytes = enc.encode(text);
  const ptr = w.adam_text_buffer(handle, bytes.length + 1);
  const mem = new Uint8Array(w.memory.buffer, ptr, bytes.length + 1);
  mem.set(bytes);
  mem[bytes.length] = 0;
  return ptr;
}

function cString(ptr) {
  const mem = new Uint8Array(state.w.memory.buffer);
  let end = ptr;
  while (mem[end]) end++;
  return dec.decode(mem.subarray(ptr, end));
}

async function loadEngine() {
  const res = await fetch("./adam_synth.wasm");
  if (!res.ok) throw new Error(`adam_synth.wasm: HTTP ${res.status}`);
  state.wasmBytes = await res.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(state.wasmBytes, {});
  state.w = instance.exports;
  state.w._initialize?.();
  for (const [name, type] of Object.entries(KIND)) {
    state.mirrors[name] = state.w.adam_instrument_create(type, 48000);
    state.paramsByKind[name] = JSON.parse(cString(state.w.adam_instrument_params_json(type)));
  }
}

const post = (msg) => state.node?.port.postMessage(msg);
const toPhysical = (i, n) => state.w.adam_instrument_param_to_physical(KIND[kind()], i, n);
const toNormalized = (i, v) => state.w.adam_instrument_param_to_normalized(KIND[kind()], i, v);
const getParam = (i) => state.w.adam_synth_get_param(mirror(), i);

function setParam(i, physical) {
  state.w.adam_synth_set_param(mirror(), i, physical);
  const v = getParam(i); // clamped by the engine
  post({ type: "param", index: i, value: v, layer: state.selected });
  state.controls.get(i)?.set(v);
  rememberSound();
  markDirty();
  return v;
}

function presetText() {
  return params().map((p) => `${p.id} ${+getParam(p.index).toPrecision(6)}`).join("\n") + "\n";
}

function applyPresetText(text, name) {
  state.w.adam_synth_apply_preset(mirror(), writeText(mirror(), text));
  for (const p of params()) state.controls.get(p.index)?.set(getParam(p.index));
  post({ type: "preset", bytes: enc.encode(text), layer: state.selected });
  if (name) layer().presetName = name;
  rememberSound();
  markDirty();
}

function rememberSound() {
  if (isAudio()) return;
  $("presetText").value = presetText();
  layer().presetText = $("presetText").value;
}

// ------------------------------------------------------------ params UI
function fmt(p, v) {
  if (p.mapping === "choice") return p.choices[Math.round(v - p.min)] ?? v;
  if (p.unit === "Hz") return v >= 1000 ? `${(v / 1000).toFixed(2)} kHz` : `${v.toFixed(v < 10 ? 2 : 0)} Hz`;
  if (p.unit === "ms") return v >= 1000 ? `${(v / 1000).toFixed(2)} s` : `${v.toFixed(v < 10 ? 1 : 0)} ms`;
  if (p.mapping === "int") return `${v > 0 && p.min < 0 ? "+" : ""}${Math.round(v)}${p.unit ? " " + p.unit : ""}`;
  const digits = Math.abs(v) < 10 ? 1 : 0;
  return `${v.toFixed(digits)}${p.unit === "%" ? "%" : p.unit ? " " + p.unit : ""}`;
}

function buildParams() {
  state.controls.clear();
  const groups = new Map();
  for (const p of params()) {
    if (!groups.has(p.group)) groups.set(p.group, []);
    groups.get(p.group).push(p);
  }
  const root = $("params");
  root.innerHTML = "";
  for (const [group, list] of groups) {
    const card = document.createElement("div");
    card.className = "card";
    card.innerHTML = `<h2>${group}</h2>`;
    for (const p of list)
      card.appendChild(p.mapping === "choice" ? choiceControl(p) : p.mapping === "int" ? stepperControl(p) : sliderControl(p));
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

// Whole-number params (semitones, voices): tap to step, not a slider.
function stepperControl(p) {
  const el = document.createElement("div");
  el.className = "param";
  const big = p.max - p.min >= 24 ? 12 : 0;
  const steps = [...(big ? [-big] : []), -1, 1, ...(big ? [big] : [])];
  el.innerHTML = `<div class="top"><span>${p.name}</span></div><div class="stepper"></div>`;
  const box = el.querySelector(".stepper");
  const value = document.createElement("b");
  steps.forEach((d) => {
    if (d === 1) box.appendChild(value);
    const b = document.createElement("button");
    b.textContent = `${d > 0 ? "+" : "−"}${Math.abs(d)}`;
    b.setAttribute("aria-label", `${p.name} ${d > 0 ? "up" : "down"} ${Math.abs(d)}`);
    b.addEventListener("click", () => setParam(p.index, getParam(p.index) + d));
    box.appendChild(b);
  });
  value.addEventListener("dblclick", () => setParam(p.index, p.default));
  state.controls.set(p.index, { set: (v) => (value.textContent = fmt(p, v)) });
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

// ------------------------------------------------------------- patterns
// Pattern text <-> editable notes. (The engine parses the same text.)
function parsePattern(text) {
  const bpm = Number(/^bpm\s+([\d.]+)/m.exec(text)?.[1] || 0);
  const length = Number(/^length\s+([\d.]+)/m.exec(text)?.[1] || 0);
  const notes = [];
  for (const m of text.matchAll(/^note\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s+(\d+)/gm))
    notes.push({ start: Number(m[1]), dur: Number(m[2]), note: Number(m[3]), vel: Number(m[4]) });
  return { bpm, length: length || Math.max(0, ...notes.map((n) => n.start + n.dur)), notes };
}

function patternText(notes, bpm, length) {
  const lines = ["# written by the Adam workshop", `bpm ${bpm}`, `length ${length}`];
  for (const n of notes.slice().sort((a, b) => a.start - b.start))
    lines.push(`note ${n.start.toFixed(4)} ${n.dur.toFixed(4)} ${n.note} ${n.vel}`);
  return lines.join("\n") + "\n";
}

// Notes of layer i -> the engine (in phase while playing).
function sendNotes(i) {
  const l = state.layers[i];
  const notes = l.notes.filter((n) => n.start < state.lengthBeats);
  post({ type: "setNotes", layer: i, notes, bytes: enc.encode(patternText(notes, state.bpm, state.lengthBeats)) });
}

async function loadIndexes() {
  const [patterns, presets, build] = await Promise.all([
    fetch("./patterns/index.json").then((r) => r.json()),
    fetch("./presets/index.json").then((r) => r.json()),
    fetch("./build.json").then((r) => (r.ok ? r.json() : {})).catch(() => ({})),
  ]);
  state.patterns = patterns;
  state.presets = presets;
  $("build").textContent = build.commit ? `build ${build.commit} · ${build.built}` : "local build";
  const ps = $("pattern");
  ps.add(new Option("Empty loop — 4 bars (record into it)", "__empty4"));
  ps.add(new Option("Empty loop — 8 bars", "__empty8"));
  for (const p of patterns.filter((p) => p.kind !== "drums")) ps.add(new Option(p.name, p.file));
}

async function patternFile(file) {
  if (file.startsWith("__empty")) return `bpm ${state.bpm || 100}\nlength ${file === "__empty8" ? 32 : 16}\n`;
  return fetch(`./patterns/${file}`).then((r) => r.text());
}

// The backing pattern sets the loop: its length and tempo, and layer 0's notes.
async function selectPattern(file) {
  const text = await patternFile(file);
  const p = parsePattern(text);
  state.lengthBeats = p.length || 16;
  state.bpm = Math.round(p.bpm || state.bpm || 100);
  $("bpm").value = state.bpm;
  $("bpmval").textContent = `${state.bpm} bpm`;
  const info = state.patterns.find((x) => x.file === file);
  $("purpose").textContent = info?.purpose || (file.startsWith("__empty") ? "an empty loop to record into" : "");
  state.layers[0].notes = p.notes;
  post({ type: "pattern", notes: p.notes, bytes: enc.encode(patternText(p.notes, state.bpm, state.lengthBeats)) });
  post({ type: "bpm", bpm: state.bpm });
  for (let i = 1; i < state.layers.length; i++) sendNotes(i); // keep other layers on the new loop length
  refreshRoll();
  renderLayers();
}

// ------------------------------------------------------------- presets
const PATCH_KEY = "adam.workshop.patches";

function loadPatches() {
  try {
    const raw = JSON.parse(localStorage.getItem(PATCH_KEY) || "{}");
    // older saves were plain synth preset text
    return Object.fromEntries(Object.entries(raw).map(([k, v]) => [k, typeof v === "string" ? { kind: "synth", text: v } : v]));
  } catch {
    return {};
  }
}

function storePatches(patches) {
  try {
    localStorage.setItem(PATCH_KEY, JSON.stringify(patches));
    return true;
  } catch {
    return false;
  }
}

// The Sound picker lists presets and saved patches for the selected layer's instrument.
function refreshPresetOptions() {
  const sel = $("preset");
  sel.innerHTML = "";
  sel.disabled = isAudio();
  $("presetFor").textContent = layer().name;
  if (isAudio()) {
    sel.add(new Option("(a recorded take)", ""));
    return;
  }
  for (const p of state.presets.filter((p) => p.kind === kind())) sel.add(new Option(p.name, p.file));
  const mine = Object.entries(loadPatches()).filter(([, v]) => v.kind === kind());
  if (mine.length) {
    const group = document.createElement("optgroup");
    group.label = "My patches";
    for (const [n] of mine.sort()) group.appendChild(new Option(n, `patch:${n}`));
    sel.appendChild(group);
  }
  const current = layer().presetName;
  if (current && [...sel.options].some((o) => o.value === current)) sel.value = current;
  $("presetFor").textContent = layer().name;
  // the per-layer Load list: patterns for this instrument
  const lp = $("loadPattern");
  lp.innerHTML = "";
  lp.add(new Option("—", ""));
  for (const p of state.patterns.filter((p) => (p.kind || "synth") === kind())) lp.add(new Option(p.name, p.file));
}

async function selectPreset(value) {
  if (value.startsWith("patch:")) {
    const p = loadPatches()[value.slice(6)];
    if (p) applyPresetText(p.text, value);
    return;
  }
  applyPresetText(await fetch(`./presets/${value}`).then((r) => r.text()), value);
}

function savePatch() {
  const cur = $("preset").value.startsWith("patch:") ? $("preset").value.slice(6) : "";
  const name = prompt("Name this patch", cur || (kind() === "drums" ? "My kit" : "My patch"))?.trim();
  if (!name) return;
  const patches = loadPatches();
  patches[name] = { kind: kind(), text: `# ${name} (saved ${new Date().toISOString().slice(0, 10)})\n${presetText()}` };
  if (!storePatches(patches)) return alert("This browser would not save it (private mode?). Use Copy or Download.");
  layer().presetName = `patch:${name}`;
  refreshPresetOptions();
}

function deletePatch() {
  const v = $("preset").value;
  if (!v.startsWith("patch:")) return alert("Pick one of My patches to delete.");
  if (!confirm(`Delete "${v.slice(6)}"?`)) return;
  const patches = loadPatches();
  delete patches[v.slice(6)];
  storePatches(patches);
  refreshPresetOptions();
}

function download(name, data, type) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([data], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// ---------------------------------------------------------------- layers
function selectLayer(i) {
  if (i === state.selected || !state.layers[i]) return;
  releaseAll(); // notes belong to the layer they started on
  const prevKind = state.layers[state.selected]?.kind;
  state.selected = i;
  if (!isAudio() && layer().presetText) state.w.adam_synth_apply_preset(mirror(), writeText(mirror(), layer().presetText));
  if (kind() !== prevKind) buildParams();
  for (const p of params()) state.controls.get(p.index)?.set(getParam(p.index));
  $("presetText").value = layer().presetText || "";
  refreshPresetOptions();
  refreshKeyboard();
  refreshRoll();
  renderLayers();
}

async function addLayer(k) {
  if (state.layers.length >= MAX_LAYERS) return;
  const count = state.layers.filter((l) => l.kind === k).length + (k === "synth" ? 0 : 1);
  const preset = state.presets.find((p) => p.kind === k);
  const text = preset ? await fetch(`./presets/${preset.file}`).then((r) => r.text()) : "";
  state.layers.push({ name: k === "drums" ? `Drums ${count}` : `Synth ${count}`, kind: k, presetText: "",
    presetName: preset?.file || "", muted: false, recording: false, notes: [] });
  post({ type: "addLayer", kind: KIND[k], bytes: enc.encode(text) });
  // the mirror learns the starting sound, then the layer remembers it
  state.selected = state.layers.length - 1;
  state.w.adam_synth_apply_preset(state.mirrors[k], writeText(state.mirrors[k], text));
  state.layers[state.selected].presetText = presetText();
  state.selected = -1;
  selectLayer(state.layers.length - 1);
  sendNotes(state.layers.length - 1);
  markDirty();
}

function toggleRecord(i) {
  const l = state.layers[i];
  if (l.kind === "audio") return armTake(i);
  if (!l.recording && !state.playing) setPlaying(true); // recording needs the loop running
  l.recording = !l.recording;
  if (l.recording) selectLayer(i);
  post({ type: "record", layer: i, on: l.recording });
  renderLayers();
}

function renderLayers() {
  const box = $("layers");
  box.innerHTML = "";
  state.layers.forEach((l, i) => {
    const row = document.createElement("div");
    row.className = `layer${i === state.selected ? " selected" : ""}${l.recording ? " recording" : ""}`;
    const icon = l.kind === "drums" ? "🥁" : l.kind === "audio" ? "🎙" : "🎹";
    const info = l.kind === "audio"
      ? l.recording === "armed" ? "armed — recording starts at bar 1"
        : l.recording ? "recording…" : l.takeSeconds ? `take · ${l.takeSeconds.toFixed(1)} s` : "empty — press Rec"
      : l.notes.length ? `${l.notes.length} notes` : i === 0 ? "empty loop" : "empty — Rec, or tap the roll";
    row.innerHTML = `<button class="pick" aria-label="Edit ${l.name}">${i === state.selected ? "●" : "○"}</button>
      <div class="lname"><b>${icon} ${l.name}</b><span>${info}</span></div>`;
    const actions = document.createElement("div");
    actions.className = "lactions";
    const btn = (label, title, fn, cls = "") => {
      const b = document.createElement("button");
      b.textContent = label;
      b.title = title;
      if (cls) b.className = cls;
      b.addEventListener("click", (e) => {
        e.stopPropagation();
        fn();
      });
      actions.appendChild(b);
    };
    btn(l.recording ? (l.kind === "audio" ? "■ Cancel" : "■ Stop rec") : "● Rec",
      l.kind === "audio" ? "Record a take from the next bar 1" : "Record what you play into this layer (overdubs)",
      () => toggleRecord(i), l.recording ? "rec on" : "rec");
    btn(l.muted ? "Unmute" : "Mute", "Mute this layer", () => {
      l.muted = !l.muted;
      post({ type: "mute", layer: i, on: l.muted });
      renderLayers();
      markDirty();
    }, l.muted ? "toggle on" : "toggle");
    row.appendChild(actions);
    row.addEventListener("click", () => selectLayer(i));
    box.appendChild(row);
  });
  $("addSynth").disabled = $("addDrums").disabled = $("addAudio").disabled = state.layers.length >= MAX_LAYERS;
  $("editing").textContent = layer().name;
  $("rollName").textContent = layer().name;
}

// --------------------------------------------------------------- sessions
// A session is everything needed to come back to a sketch: tempo, loop, key,
// and per layer its instrument, full sound (preset text), notes and mute.
// Audio takes are stored separately (store.js) and referenced by id.
function sessionSnapshot() {
  return {
    format: "adam-session/1",
    name: state.sessionName,
    savedAt: new Date().toISOString(),
    bpm: state.bpm,
    lengthBeats: state.lengthBeats,
    backing: $("pattern").value,
    key: { root: state.keyRoot, scale: state.keyScale },
    layers: state.layers.map((l) => ({
      name: l.name, kind: l.kind, presetText: l.presetText, presetName: l.presetName, muted: l.muted,
      notes: l.notes, audioId: l.audioId || null, gain: l.gain ?? 1, offsetMs: l.offsetMs ?? 0,
      passes: l.passes || 1, audioSr: l.audioSr || 0,
    })),
  };
}

function markDirty() {
  state.dirty = true;
  $("sessionState").textContent = "unsaved changes";
  clearTimeout(markDirty.timer);
  // autosave so a reload or a closed tab never loses the sketch
  markDirty.timer = setTimeout(() => sessionStore.put("(autosave)", sessionSnapshot()).catch(() => {}), 1500);
}

async function refreshSessionList(select) {
  const sel = $("sessOpen");
  sel.innerHTML = "";
  sel.add(new Option("—", ""));
  let names = [];
  try {
    names = (await sessionStore.list()).sort();
  } catch (err) {
    $("sessionState").textContent = `can't store sessions here: ${err.message}`;
  }
  for (const n of names) sel.add(new Option(n, n));
  if (select) sel.value = select;
}

async function saveSession(asNew) {
  let name = state.sessionName;
  if (asNew || name === "Untitled") {
    name = prompt("Name this session", name === "Untitled" ? "My sketch" : `${name} 2`)?.trim();
    if (!name) return;
  }
  state.sessionName = name;
  try {
    await sessionStore.put(name, sessionSnapshot());
    state.dirty = false;
    $("sessionName").textContent = name;
    $("sessionState").textContent = `saved ${new Date().toLocaleTimeString()}`;
    await refreshSessionList(name);
  } catch (err) {
    alert(`Could not save: ${err.message}. Use Export instead.`);
  }
}

async function applySession(sess) {
  if (!sess || !Array.isArray(sess.layers) || !sess.layers.length) throw new Error("not a session");
  releaseAll();
  setPlaying(false);
  post({ type: "resetLayers" });
  state.sessionName = sess.name || "Untitled";
  $("sessionName").textContent = state.sessionName;
  state.bpm = Math.round(sess.bpm || 100);
  state.lengthBeats = sess.lengthBeats || 16;
  $("bpm").value = state.bpm;
  $("bpmval").textContent = `${state.bpm} bpm`;
  post({ type: "bpm", bpm: state.bpm });
  if ([...$("pattern").options].some((o) => o.value === sess.backing)) $("pattern").value = sess.backing;
  state.keyRoot = sess.key?.root ?? 0;
  state.keyScale = sess.key?.scale ?? "chromatic";
  $("keyRoot").value = state.keyRoot;
  $("keyScale").value = state.keyScale;
  // layer 0 is the clock: its notes + the session's loop length
  const [first, ...rest] = sess.layers;
  state.layers = [{ ...first, kind: "synth", recording: false, notes: first.notes || [] }];
  state.selected = 0;
  post({ type: "pattern", notes: state.layers[0].notes,
    bytes: enc.encode(patternText(state.layers[0].notes, state.bpm, state.lengthBeats)) });
  post({ type: "preset", bytes: enc.encode(first.presetText || ""), layer: 0 });
  if (first.muted) post({ type: "mute", layer: 0, on: true });
  for (const l of rest) {
    const i = state.layers.length;
    state.layers.push({ ...l, recording: false, notes: l.notes || [] });
    if (l.kind === "audio") {
      await restoreAudioLayer(i, l);
    } else {
      post({ type: "addLayer", kind: KIND[l.kind], bytes: enc.encode(l.presetText || "") });
      sendNotes(i);
    }
    if (l.muted) post({ type: "mute", layer: i, on: true });
  }
  // the mirror + UI follow layer 0
  state.w.adam_synth_apply_preset(mirror(), writeText(mirror(), first.presetText || ""));
  buildParams();
  $("presetText").value = layer().presetText || "";
  refreshPresetOptions();
  refreshKeyboard();
  refreshRoll();
  renderLayers();
  state.dirty = false;
  $("sessionState").textContent = `opened ${sess.savedAt ? new Date(sess.savedAt).toLocaleString() : ""}`;
}

async function newSession() {
  if (state.dirty && !confirm("Start a new session? Unsaved changes stay in the autosave.")) return;
  const first = state.presets.find((p) => p.kind === "synth");
  const text = await fetch(`./presets/${first.file}`).then((r) => r.text());
  await applySession({ name: "Untitled", bpm: 100, lengthBeats: 16, backing: "__empty4",
    layers: [{ name: "Backing", kind: "synth", presetText: text, presetName: first.file, notes: [] }] });
  state.w.adam_synth_apply_preset(mirror(), writeText(mirror(), text));
  rememberSound();
}

function sessionEvents() {
  $("sessSave").addEventListener("click", () => saveSession(false));
  $("sessSaveAs").addEventListener("click", () => saveSession(true));
  $("sessNew").addEventListener("click", newSession);
  $("sessOpen").addEventListener("change", async () => {
    const name = $("sessOpen").value;
    if (!name) return;
    if (state.dirty && !confirm(`Open "${name}"? Unsaved changes stay in the autosave.`)) {
      $("sessOpen").value = "";
      return;
    }
    try {
      await applySession(await sessionStore.get(name));
      if (name === "(autosave)") state.sessionName = "Untitled";
    } catch (err) {
      alert(`Could not open "${name}": ${err.message}`);
    }
    $("sessOpen").value = "";
  });
  $("sessExport").addEventListener("click", async () => {
    const sess = sessionSnapshot();
    // inline audio takes so the file is self-contained
    for (const l of sess.layers) {
      if (!l.audioId) continue;
      const samples = await audioStore.get(l.audioId).catch(() => null);
      if (samples) l.audioBase64 = floatsToBase64(samples);
    }
    download(`${state.sessionName.replace(/\W+/g, "_").toLowerCase() || "session"}.adamsession`,
      JSON.stringify(sess), "application/json");
  });
  $("sessImport").addEventListener("change", async () => {
    const file = $("sessImport").files[0];
    if (!file) return;
    try {
      const sess = JSON.parse(await file.text());
      for (const l of sess.layers || []) {
        if (l.audioBase64) {
          l.audioId = l.audioId || `take-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
          await audioStore.put(l.audioId, base64ToFloats(l.audioBase64));
          delete l.audioBase64;
        }
      }
      await applySession(sess);
      markDirty();
    } catch (err) {
      alert(`Could not import: ${err.message}`);
    }
    $("sessImport").value = "";
  });
}

function floatsToBase64(f32) {
  const bytes = new Uint8Array(f32.buffer, f32.byteOffset, f32.byteLength);
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

function base64ToFloats(b64) {
  const s = atob(b64);
  const bytes = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i);
  return new Float32Array(bytes.buffer);
}

// ---------------------------------------------------------- audio layers
// A take is mono samples recorded on the loop's own clock (the mic feeds the
// same worklet the engine runs in). Stored in IndexedDB, referenced by id.
let micStream = null;

async function ensureMic() {
  if (micStream) return micStream;
  if (!navigator.mediaDevices?.getUserMedia) throw new Error("this browser cannot record audio here");
  micStream = await navigator.mediaDevices.getUserMedia({
    // raw input: what a vocal or instrument take wants
    audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 },
  });
  state.ctx.createMediaStreamSource(micStream).connect(state.node);
  return micStream;
}

function estimatedLatencyMs() {
  const track = micStream?.getAudioTracks()[0];
  const input = track?.getSettings?.().latency || 0;
  return Math.round(((state.ctx.baseLatency || 0) + (state.ctx.outputLatency || 0) + input) * 1000);
}

async function addAudioLayer() {
  if (state.layers.length >= MAX_LAYERS) return;
  try {
    await ensureMic();
  } catch (err) {
    const framed = window.top !== window;
    alert(`Microphone unavailable: ${err.message}.${framed ? " Use ⤢ Full page (top right) — the Staging frame may block the mic." : ""}`);
    return;
  }
  const n = state.layers.filter((l) => l.kind === "audio").length + 1;
  state.layers.push({ name: `Audio ${n}`, kind: "audio", presetText: "", presetName: "", muted: false, recording: false,
    notes: [], gain: 1, offsetMs: estimatedLatencyMs(), passes: 1, audioId: null, audioSr: 0, takeSeconds: 0 });
  post({ type: "addAudioLayer" });
  const i = state.layers.length - 1;
  sendAudioParams(i);
  state.selected = -1;
  selectLayer(i);
  markDirty();
}

function sendAudioParams(i) {
  const l = state.layers[i];
  post({ type: "audioParams", layer: i, gain: l.gain ?? 1,
    offset: ((l.offsetMs ?? 0) / 1000) * (state.ctx?.sampleRate || 48000) });
}

function armTake(i) {
  const l = state.layers[i];
  if (l.recording) {
    post({ type: "armAudio", layer: i, on: false });
    l.recording = false;
    renderLayers();
    return;
  }
  if (!state.playing) setPlaying(true); // the take records over the loop
  post({ type: "armAudio", layer: i, on: true, passes: l.passes || 1 });
  l.recording = "armed";
  selectLayer(i);
  renderLayers();
}

async function onTake(m) {
  const l = state.layers[m.layer];
  if (!l) return;
  l.recording = false;
  l.audioId = l.audioId || `take-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
  l.audioSr = m.sampleRate;
  l.passes = m.passes;
  l.takeSeconds = m.samples.length / m.sampleRate;
  l.take = m.samples;
  try {
    await audioStore.put(l.audioId, m.samples);
  } catch (err) {
    $("sessionState").textContent = `take not stored (${err.message}) — export the session to keep it`;
  }
  if (state.layers[state.selected] === l) drawTake(l);
  renderLayers();
  markDirty();
}

function resample(samples, from, to) {
  if (!from || from === to) return samples;
  const out = new Float32Array(Math.round((samples.length * to) / from));
  const r = from / to;
  for (let i = 0; i < out.length; i++) {
    const x = i * r, k = Math.floor(x), f = x - k;
    out[i] = (samples[k] || 0) * (1 - f) + (samples[k + 1] || 0) * f;
  }
  return out;
}

async function restoreAudioLayer(i, l) {
  post({ type: "addAudioLayer" });
  if (l.audioId) {
    const samples = await audioStore.get(l.audioId).catch(() => null);
    if (samples) {
      const sr = state.ctx?.sampleRate || 48000;
      const take = resample(samples, l.audioSr || sr, sr);
      state.layers[i].take = take;
      state.layers[i].takeSeconds = take.length / sr;
      post({ type: "setAudio", layer: i, samples: take.slice(), passes: l.passes || 1 });
    }
  }
  sendAudioParams(i);
}

function drawTake(l) {
  const c = $("wave");
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) {
    c.width = w;
    c.height = h;
  }
  const g = c.getContext("2d");
  g.clearRect(0, 0, w, h);
  g.fillStyle = css("--muted");
  g.font = `${11 * dpr}px sans-serif`;
  if (!l.take) {
    g.fillText(l.audioId ? "take stored — open the session after starting audio" : "no take yet", 10 * dpr, h / 2);
    return;
  }
  // loop boundaries for multi-pass takes
  g.strokeStyle = css("--line");
  for (let p = 1; p < (l.passes || 1); p++) {
    const x = (p / l.passes) * w;
    g.beginPath();
    g.moveTo(x, 0);
    g.lineTo(x, h);
    g.stroke();
  }
  g.fillStyle = css("--accent");
  const step = Math.max(1, Math.floor(l.take.length / w));
  for (let x = 0; x < w; x++) {
    let peak = 0;
    for (let i = x * step, end = Math.min(l.take.length, (x + 1) * step); i < end; i++)
      peak = Math.max(peak, Math.abs(l.take[i]));
    const bh = Math.max(1, peak * h * 0.95);
    g.fillRect(x, (h - bh) / 2, 1, bh);
  }
}

// 32-bit float mono WAV
function wavBytes(samples, sr) {
  const buf = new ArrayBuffer(44 + samples.length * 4);
  const v = new DataView(buf);
  const str = (o, s) => [...s].forEach((ch, i) => v.setUint8(o + i, ch.charCodeAt(0)));
  str(0, "RIFF"); v.setUint32(4, 36 + samples.length * 4, true); str(8, "WAVEfmt ");
  v.setUint32(16, 16, true); v.setUint16(20, 3, true); v.setUint16(22, 1, true);
  v.setUint32(24, sr, true); v.setUint32(28, sr * 4, true); v.setUint16(32, 4, true); v.setUint16(34, 32, true);
  str(36, "data"); v.setUint32(40, samples.length * 4, true);
  new Float32Array(buf, 44).set(samples);
  return new Uint8Array(buf);
}

function audioPanelEvents() {
  $("takePasses").addEventListener("change", () => {
    layer().passes = Number($("takePasses").value);
    markDirty();
  });
  $("takeGain").addEventListener("input", () => {
    layer().gain = Number($("takeGain").value);
    $("takeGainVal").textContent = `${Math.round(layer().gain * 100)}%`;
    sendAudioParams(state.selected);
    markDirty();
  });
  $("takeOffset").addEventListener("input", () => {
    layer().offsetMs = Number($("takeOffset").value);
    $("takeOffsetVal").textContent = `${layer().offsetMs} ms`;
    sendAudioParams(state.selected);
    markDirty();
  });
  $("takeWav").addEventListener("click", () => {
    const l = layer();
    if (!l.take) return alert("No take recorded yet.");
    download(`${l.name.replace(/\s+/g, "_").toLowerCase()}.wav`, wavBytes(l.take, state.ctx.sampleRate), "audio/wav");
  });
  $("addAudio").addEventListener("click", addAudioLayer);
}

// ------------------------------------------------------------ piano roll
function refreshRoll() {
  const audio = isAudio();
  $("roll").hidden = audio;
  $("rollTools").hidden = audio;
  $("audioPanel").hidden = !audio;
  $("loadPattern").disabled = audio;
  if (audio) {
    const l = layer();
    $("takePasses").value = String(l.passes || 1);
    $("takeGain").value = l.gain ?? 1;
    $("takeGainVal").textContent = `${Math.round((l.gain ?? 1) * 100)}%`;
    $("takeOffset").value = l.offsetMs ?? 0;
    $("takeOffsetVal").textContent = `${Math.round(l.offsetMs ?? 0)} ms`;
    drawTake(l);
    return;
  }
  state.roll.setLayer({ kind: kind(), notes: layer().notes, lengthBeats: state.lengthBeats });
  applyScaleToRoll();
}

function onRollChange(notes) {
  layer().notes = notes;
  sendNotes(state.selected);
  renderLayers();
  markDirty();
}

function stepEntryActive() {
  return state.roll.stepMode && !state.playing;
}

// ------------------------------------------------------- keyboard & pads
const held = new Map(); // pointerId|key -> note

function noteOn(note, id, velocity = 100) {
  if (held.has(id)) return;
  held.set(id, note);
  post({ type: "noteOn", note, velocity, layer: state.selected });
  document.querySelector(`[data-note="${note}"]`)?.classList.add("down");
  if (stepEntryActive()) state.roll.addAtCursor(note, velocity); // spell a chord note by note
}

function noteOff(id) {
  const note = held.get(id);
  if (note === undefined) return;
  held.delete(id);
  post({ type: "noteOff", note, layer: state.selected });
  if (![...held.values()].includes(note)) document.querySelector(`[data-note="${note}"]`)?.classList.remove("down");
}

function releaseAll() {
  [...held.keys()].forEach(noteOff);
}

function scaleSteps() {
  return SCALES[state.keyScale][1];
}

function applyScaleToRoll() {
  const steps = scaleSteps();
  state.roll.setScale(state.keyRoot, steps ? (pc) => steps.includes((pc - state.keyRoot + 12) % 12) : null);
}

// Chromatic: a piano. A scale: only the scale's notes, as big equal keys.
function buildKeyboard() {
  const kb = $("keyboard");
  kb.innerHTML = "";
  const steps = scaleSteps();
  const base = 60 + 12 * state.octave - 12;
  if (!steps) {
    kb.className = "keyboard";
    const name = (n) => `C${Math.floor(n / 12) - 1}`;
    $("octLabel").textContent = `${name(base)}–${name(base + 24)}`;
    const whites = [0, 2, 4, 5, 7, 9, 11];
    let whiteIndex = 0;
    for (let n = base; n <= base + 24; n++) {
      const el = document.createElement("div");
      el.dataset.note = n;
      if (whites.includes(n % 12)) {
        el.className = "key";
        whiteIndex++;
      } else {
        el.className = "key black";
        el.style.left = `calc(${(whiteIndex / 15) * 100}% - ${(100 / 15) * 0.31}%)`;
      }
      kb.appendChild(el);
    }
    return;
  }
  kb.className = "keyboard scale";
  const rootNote = base + state.keyRoot;
  const notes = [];
  for (let o = 0; o < 2; o++) for (const s of steps) notes.push(rootNote + 12 * o + s);
  notes.push(rootNote + 24);
  $("octLabel").textContent = `${noteName(notes[0])}–${noteName(notes[notes.length - 1])}`;
  for (const n of notes) {
    const el = document.createElement("div");
    el.className = `key${(n - state.keyRoot) % 12 === 0 ? " root" : ""}`;
    el.dataset.note = n;
    el.textContent = NOTE_NAMES[n % 12];
    kb.appendChild(el);
  }
}

function buildPads() {
  const pads = $("pads");
  pads.innerHTML = "";
  // top row: hats and clap; middle: toms and rim; bottom: kick, snare
  const order = [42, 46, 39, 48, 45, 41, 37, 38, 36];
  for (const note of order) {
    const el = document.createElement("div");
    el.className = "pad";
    el.dataset.note = note;
    el.textContent = DRUM_ROWS.find(([n]) => n === note)[1];
    pads.appendChild(el);
  }
}

function refreshKeyboard() {
  const drums = kind() === "drums";
  const audio = isAudio();
  $("keyboard").hidden = drums || audio;
  $("pads").hidden = !drums;
  $("keyControls").hidden = drums || audio;
  $("playTitle").textContent = audio ? "Recording a take — sing or play into the mic" : drums ? "Drum pads" : "Play it";
  if (!drums && !audio) buildKeyboard();
}

function shiftOctave(d) {
  releaseAll();
  state.octave = Math.max(-3, Math.min(3, state.octave + d));
  buildKeyboard();
}

function pointerPlay(container, selector) {
  const at = (e) => {
    const el = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(selector);
    return el && container.contains(el) ? el : null;
  };
  container.addEventListener("pointerdown", (e) => {
    const el = at(e);
    if (!el) return;
    container.setPointerCapture(e.pointerId);
    // pads: where you hit sets the velocity (top = loud), like a real pad
    let vel = 100;
    if (selector === ".pad") {
      const r = el.getBoundingClientRect();
      vel = Math.round(127 - ((e.clientY - r.top) / r.height) * 70);
    }
    noteOn(Number(el.dataset.note), `p${e.pointerId}`, vel);
  });
  container.addEventListener("pointermove", (e) => {
    if (!held.has(`p${e.pointerId}`) || selector === ".pad") return;
    const el = at(e);
    if (el && Number(el.dataset.note) !== held.get(`p${e.pointerId}`)) {
      noteOff(`p${e.pointerId}`);
      noteOn(Number(el.dataset.note), `p${e.pointerId}`);
    }
  });
  for (const t of ["pointerup", "pointercancel", "lostpointercapture"])
    container.addEventListener(t, (e) => noteOff(`p${e.pointerId}`));
}

function keyboardEvents() {
  $("octDown").addEventListener("click", () => shiftOctave(-1));
  $("octUp").addEventListener("click", () => shiftOctave(1));
  pointerPlay($("keyboard"), ".key");
  pointerPlay($("pads"), ".pad");
  const KEYMAP = "awsedftgyhujk"; // C C# D ... C (chromatic), or scale degrees in a scale
  const DRUMKEYS = { a: 36, s: 38, d: 42, f: 46, g: 39, h: 41, j: 45, k: 48, l: 37 };
  window.addEventListener("keydown", (e) => {
    if (e.repeat || e.target.closest?.("input,select,textarea")) return;
    const k = e.key.toLowerCase();
    if (kind() === "drums") {
      if (DRUMKEYS[k]) noteOn(DRUMKEYS[k], `k${k}`, 110);
      return;
    }
    if (k === "z" || k === "x") return shiftOctave(k === "x" ? 1 : -1);
    const i = KEYMAP.indexOf(k);
    if (i < 0) return;
    const steps = scaleSteps();
    const note = steps
      ? 60 + 12 * state.octave + state.keyRoot + 12 * Math.floor(i / steps.length) + steps[i % steps.length]
      : 60 + 12 * state.octave + i;
    noteOn(note, `k${k}`);
  });
  window.addEventListener("keyup", (e) => noteOff(`k${e.key.toLowerCase()}`));
  window.addEventListener("blur", releaseAll);
}

// ---------------------------------------------------------------- audio
async function startAudio() {
  const ctx = new AudioContext({ latencyHint: "interactive" });
  await ctx.audioWorklet.addModule("./worklet.js");
  const node = new AudioWorkletNode(ctx, "adam-synth", {
    numberOfInputs: 1, // the microphone, when an audio layer asks for it
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
    };
  });
  node.port.onmessage = (e) => onWorkletMessage(e.data);
  // bring the engine to the UI's state
  post({ type: "preset", bytes: enc.encode(state.layers[0].presetText), layer: 0 });
  await selectPattern($("pattern").value);
  post({ type: "loop", on: state.loop });
  await ctx.resume();
  $("app").classList.remove("locked");
  $("start").textContent = `Audio on · ${ctx.sampleRate / 1000} kHz`;
  $("start").disabled = true;
  requestAnimationFrame(draw);
}

function onWorkletMessage(m) {
  if (m.type === "layerPattern") {
    const l = state.layers[m.layer];
    if (l) {
      l.notes = m.notes || [];
      if (m.layer === state.selected) refreshRoll();
      renderLayers();
      markDirty();
    }
    return;
  }
  if (m.type === "take") {
    onTake(m);
    return;
  }
  if (m.type === "snapshot") {
    // the most recent ~85 ms; only while the panel is open
    if ($("agentPanel").open) analyzeSnapshot(m.l.subarray(4096), m.r.subarray(4096));
    return;
  }
  if (m.type !== "meter") return;
  const db = (x) => (x > 1e-6 ? 20 * Math.log10(x) : -120);
  const h = (x) => `${Math.max(0, Math.min(100, ((db(x) + 60) / 60) * 100))}%`;
  $("mL").style.height = h(m.peakL);
  $("mR").style.height = h(m.peakR);
  $("rL").style.height = h(m.rmsL);
  $("rR").style.height = h(m.rmsR);
  state.peakHold = Math.max(Math.max(m.peakL, m.peakR), state.peakHold * 0.97);
  $("peakdb").textContent = db(state.peakHold) < -100 ? "−∞" : db(state.peakHold).toFixed(1);
  $("grdb").textContent = m.grDb.toFixed(1);
  $("voices").textContent = m.voices;
  const unsafe = m.backstop > 0 || m.nanResets > 0;
  $("safety").className = unsafe ? "bad" : "ok";
  $("safety").textContent = unsafe ? `safety stage engaged (backstop ${m.backstop}, NaN resets ${m.nanResets})` : "output safe";
  $("beatbar").style.width = `${((m.beat % state.lengthBeats) / state.lengthBeats) * 100}%`;
  state.roll.playhead = state.playing ? m.beat : -1;
  if (state.playing && !m.playing) setPlaying(false); // a non-looping pattern ended
  if (m.inputPeak !== undefined) $("inLevel").style.width = `${Math.min(100, Math.sqrt(m.inputPeak) * 100)}%`;
  if (m.recording) m.recording.forEach((r, i) => {
    if (state.layers[i] && state.layers[i].recording !== r) {
      state.layers[i].recording = r;
      renderLayers();
    }
  });
}

function setPlaying(on) {
  state.playing = on;
  $("play").textContent = on ? "❚❚ Stop" : "▶ Play";
  post({ type: "play", on });
  if (!on) {
    post({ type: "rewind" });
    state.roll.playhead = -1;
  }
  updateStepUi();
}

function updateStepUi() {
  const on = state.roll.stepMode;
  $("stepMode").setAttribute("aria-pressed", String(on));
  $("stepBack").disabled = $("stepNext").disabled = !on;
  $("stepMode").textContent = on ? (state.playing ? "✎ Step entry (stop to write)" : "✎ Step entry on") : "✎ Step entry";
  state.roll.draw();
}

function setVolume() {
  const v = Number($("volume").value);
  $("volval").textContent = `${Math.round(v * 100)}%`;
  if (state.gain) state.gain.gain.setTargetAtTime(state.muted ? 0 : v * v, state.ctx.currentTime, 0.02);
}

// ------------------------------------------------- what the agent sees
function analyzeSnapshot(l, r) {
  const w = state.w;
  const MAX = 16384;
  const input = new Float32Array(w.memory.buffer, w.adam_analysis_input(), 2 * MAX);
  input.set(l, 0);
  input.set(r, MAX);
  const o = new Float32Array(w.memory.buffer, w.adam_analyze(l.length, state.ctx.sampleRate), 29);
  renderAnalysis({
    peakDb: o[0], rmsDb: o[1], crestDb: o[2], dc: o[3], centroidHz: o[4], rolloffHz: o[5], flatness: o[6],
    f0Hz: o[7], f0Confidence: o[8], f0Midi: o[9], inharmonicDb: o[10], bands: Array.from(o.subarray(11, 15)),
    stereo: o[15], harmonics: Array.from(o.subarray(17, 17 + o[16])),
  });
}

const hz = (f) => (f >= 1000 ? `${(f / 1000).toFixed(2)} kHz` : `${Math.round(f)} Hz`);

// Always the same eight cards in the same places: values change, the layout
// does not (no jumping while you play).
function renderAnalysis(a) {
  const silent = a.rmsDb < -70;
  const metrics = [];
  if (silent) {
    for (const k of ["Pitch (YIN)", "Between-harmonic energy", "Brightness (centroid)", "Rolloff (85% energy)",
      "Level", "Crest factor", "Noisiness (flatness)", "Stereo correlation"])
      metrics.push([k, "—", k === "Level" ? "silence — play something" : ""]);
  } else {
    if (a.f0Hz > 0 && a.f0Confidence > 0.6) {
      const midi = Math.round(a.f0Midi);
      const cents = Math.round((a.f0Midi - midi) * 100);
      metrics.push(["Pitch (YIN)", `${noteName(midi)} ${cents >= 0 ? "+" : ""}${cents}¢`,
        `${a.f0Hz.toFixed(1)} Hz · confidence ${(a.f0Confidence * 100).toFixed(0)}%`]);
      metrics.push(["Between-harmonic energy", `${a.inharmonicDb.toFixed(1)} dB`,
        "aliasing + noise on a HELD SINGLE note (lower = cleaner); chords and note changes raise it"]);
    } else {
      metrics.push(["Pitch (YIN)", "—", "no single pitch (a chord, drums or noise)"]);
      metrics.push(["Between-harmonic energy", "—", "needs a single held note"]);
    }
    metrics.push(["Brightness (centroid)", hz(a.centroidHz), "where the spectrum's weight sits; higher = brighter"]);
    metrics.push(["Rolloff (85% energy)", hz(a.rolloffHz), "85% of the energy is below this"]);
    metrics.push(["Level", `${a.rmsDb.toFixed(1)} dB RMS`, `peak ${a.peakDb.toFixed(1)} dBFS`]);
    metrics.push(["Crest factor", `${a.crestDb.toFixed(1)} dB`, "peak over RMS; high = punchy/spiky, low = dense"]);
    metrics.push(["Noisiness (flatness)", a.flatness.toFixed(3), "0 = pure tones, 1 = white noise"]);
    metrics.push(["Stereo correlation", a.stereo.toFixed(2), "+1 mono · 0 wide · <0 phasey"]);
  }
  $("metrics").innerHTML = metrics
    .map(([k, v, d]) => `<div class="metric"><div class="k">${k}</div><div class="v">${v}</div><div class="d">${d}</div></div>`)
    .join("");
  drawBars($("harmonics"), silent || !(a.f0Confidence > 0.6) ? [] : a.harmonics, (i) => `${i + 1}`, -60);
  drawBars($("bands"), silent ? [] : a.bands, (i) => ["<200", "200–2k", "2k–8k", ">8k"][i], -60);
}

function drawBars(canvas, values, label, floorDb) {
  const { g, w, h } = canvasCtx(canvas);
  const dpr = window.devicePixelRatio || 1;
  g.font = `${10 * dpr}px sans-serif`;
  g.fillStyle = css("--muted");
  if (!values.length) {
    g.fillText("—", 8 * dpr, h / 2);
    return;
  }
  const slot = w / values.length;
  values.forEach((db, i) => {
    const frac = Math.max(0, Math.min(1, (db - floorDb) / -floorDb));
    const bh = frac * (h - 16 * dpr);
    g.fillStyle = css("--accent");
    g.fillRect(i * slot + slot * 0.15, h - 14 * dpr - bh, slot * 0.7, bh);
    g.fillStyle = css("--muted");
    g.fillText(label(i), i * slot + slot * 0.15, h - 2 * dpr);
    if (values.length <= 4) g.fillText(`${db.toFixed(0)} dB`, i * slot + slot * 0.15, h - 18 * dpr - bh);
  });
}

// ---------------------------------------------------------------- scopes
let lastRollDraw = 0;
function draw(t) {
  const a = state.analyser;
  if (a && $("monitorPanel").open) {
    drawSpectrum(a);
    drawScope(a);
  }
  if (state.playing && t - lastRollDraw > 33) {
    state.roll.draw(); // move the playhead
    lastRollDraw = t;
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

// ------------------------------------------------------------------ boot
async function boot() {
  try {
    await loadEngine();
    await loadIndexes();
    state.roll = new PianoRoll($("roll"), {
      onChange: onRollChange,
      onSelect: ({ audition }) => {
        // tapping a note or a row label plays it, so editing is by ear
        if (audition === undefined) return;
        post({ type: "noteOn", note: audition, velocity: 90, layer: state.selected });
        setTimeout(() => post({ type: "noteOff", note: audition, layer: state.selected }), 220);
      },
    });
    for (let i = 0; i < 12; i++) $("keyRoot").add(new Option(NOTE_NAMES[i], i));
    for (const [k, [label]] of Object.entries(SCALES)) $("keyScale").add(new Option(label, k));
    buildParams();
    buildPads();
    keyboardEvents();
    sessionEvents();
    audioPanelEvents();
    await refreshSessionList();
    // the backing layer starts on the first synth preset
    const first = state.presets.find((p) => p.kind === "synth");
    state.layers[0].presetName = first.file;
    await selectPreset(first.file);
    refreshPresetOptions();
    refreshKeyboard();
    renderLayers();
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
    post({ type: "loop", on: state.loop });
  });
  $("panic").addEventListener("click", () => {
    setPlaying(false);
    post({ type: "panic" });
  });
  $("bpm").addEventListener("input", () => {
    state.bpm = Number($("bpm").value);
    $("bpmval").textContent = `${state.bpm} bpm`;
    post({ type: "bpm", bpm: state.bpm });
    markDirty();
  });
  $("pattern").addEventListener("change", () => selectPattern($("pattern").value));
  $("preset").addEventListener("change", () => selectPreset($("preset").value));
  $("addSynth").addEventListener("click", () => addLayer("synth"));
  $("addDrums").addEventListener("click", () => addLayer("drums"));
  $("grid").addEventListener("change", () => {
    state.roll.grid = Number($("grid").value);
    state.roll.cursor = Math.round(state.roll.cursor / state.roll.grid) * state.roll.grid;
    state.roll.draw();
  });
  $("loadPattern").addEventListener("change", async () => {
    const file = $("loadPattern").value;
    if (!file) return;
    const p = parsePattern(await patternFile(file));
    state.roll.snapshot();
    // tile a shorter pattern across the loop; drop what runs past it
    const notes = [];
    const len = p.length || state.lengthBeats;
    for (let off = 0; off < state.lengthBeats; off += len)
      for (const n of p.notes) if (n.start + off < state.lengthBeats) notes.push({ ...n, start: n.start + off });
    layer().notes = notes;
    sendNotes(state.selected);
    refreshRoll();
    renderLayers();
    $("loadPattern").value = "";
  });
  $("stepMode").addEventListener("click", () => {
    state.roll.stepMode = !state.roll.stepMode;
    updateStepUi();
  });
  $("stepBack").addEventListener("click", () => state.roll.moveCursor(-1));
  $("stepNext").addEventListener("click", () => state.roll.moveCursor(1));
  $("undo").addEventListener("click", () => state.roll.undo());
  $("delNote").addEventListener("click", () => state.roll.deleteSelected());
  $("clearNotes").addEventListener("click", () => {
    if (layer().notes.length && confirm(`Clear all notes on ${layer().name}?`)) state.roll.clear();
  });
  $("expMid").addEventListener("click", () =>
    download(`${layer().name.replace(/\s+/g, "_").toLowerCase()}.mid`,
      patternToMidi(patternText(layer().notes, state.bpm, state.lengthBeats)), "audio/midi"));
  $("expPat").addEventListener("click", () =>
    download(`${layer().name.replace(/\s+/g, "_").toLowerCase()}.pat`,
      patternText(layer().notes, state.bpm, state.lengthBeats), "text/plain"));
  $("keyRoot").addEventListener("change", () => {
    releaseAll();
    state.keyRoot = Number($("keyRoot").value);
    buildKeyboard();
    applyScaleToRoll();
  });
  $("keyScale").addEventListener("change", () => {
    releaseAll();
    state.keyScale = $("keyScale").value;
    buildKeyboard();
    applyScaleToRoll();
  });
  $("volume").addEventListener("input", setVolume);
  $("mute").addEventListener("click", () => {
    state.muted = !state.muted;
    $("mute").setAttribute("aria-pressed", String(state.muted));
    setVolume();
  });
  $("save").addEventListener("click", savePatch);
  $("delete").addEventListener("click", deletePatch);
  $("download").addEventListener("click", () =>
    download(`${(layer().presetName || "patch").replace(/^patch:/, "").replace(/\W+/g, "_")}.preset`, presetText(), "text/plain"));
  $("copy").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(presetText());
      $("copy").textContent = "Copied ✓";
    } catch {
      $("presetText").select();
      $("copy").textContent = "Select + copy below";
    }
    setTimeout(() => ($("copy").textContent = "Copy"), 1600);
  });
  updateStepUi();
  window.addEventListener("resize", () => state.roll.draw());
  // collapsible panels remember how you left them
  for (const id of ["monitorPanel", "agentPanel"]) {
    try {
      $(id).open = localStorage.getItem(`adam.workshop.${id}`) === "open";
    } catch {
      /* convenience */
    }
    $(id).addEventListener("toggle", () => {
      try {
        localStorage.setItem(`adam.workshop.${id}`, $(id).open ? "open" : "closed");
      } catch {
        /* convenience */
      }
    });
  }
  // Tap tempo: the average of the last few taps; a 2 s pause starts over
  const taps = [];
  $("tap").addEventListener("click", () => {
    const now = performance.now();
    if (taps.length && now - taps[taps.length - 1] > 2000) taps.length = 0;
    taps.push(now);
    if (taps.length > 5) taps.shift();
    if (taps.length < 2) return;
    const gaps = taps.slice(1).map((t, i) => t - taps[i]);
    const bpm = Math.round(60000 / (gaps.reduce((x, y) => x + y, 0) / gaps.length));
    state.bpm = Math.max(40, Math.min(200, bpm));
    $("bpm").value = state.bpm;
    $("bpmval").textContent = `${state.bpm} bpm`;
    post({ type: "bpm", bpm: state.bpm });
  });
  // inside the Staging frame the browser blocks the microphone: offer the full page
  if (window.top !== window) $("fullPage").hidden = false;
}

// Standard MIDI File (format 0) from pattern text — for Logic.
function patternToMidi(text) {
  const { bpm, length, notes } = parsePattern(text);
  const ppq = 480;
  const ev = [];
  for (const n of notes)
    ev.push([Math.round(n.start * ppq), 1, n.note, n.vel], [Math.round(Math.min(n.start + n.dur, length) * ppq), 0, n.note, 0]);
  ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const vlq = (x) => {
    const out = [x & 0x7f];
    while ((x >>= 7)) out.unshift((x & 0x7f) | 0x80);
    return out;
  };
  const tempo = Math.round(60000000 / (bpm || 120));
  const track = [0, 0xff, 0x51, 3, (tempo >> 16) & 255, (tempo >> 8) & 255, tempo & 255];
  let last = 0;
  for (const [t, on, n, v] of ev) {
    track.push(...vlq(t - last), on ? 0x90 : 0x80, n, on ? v : 64);
    last = t;
  }
  track.push(...vlq(Math.max(0, Math.round(length * ppq) - last)), 0xff, 0x2f, 0);
  const u32 = (x) => [(x >>> 24) & 255, (x >>> 16) & 255, (x >>> 8) & 255, x & 255];
  return new Uint8Array([0x4d, 0x54, 0x68, 0x64, ...u32(6), 0, 0, 0, 1, (ppq >> 8) & 255, ppq & 255,
    0x4d, 0x54, 0x72, 0x6b, ...u32(track.length), ...track]);
}

boot();
