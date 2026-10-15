// AudioWorklet host for the shared C++ engine (adam_synth.wasm). All sound —
// oscillators, filter, envelopes, voices, pattern playback, the limiters —
// comes from the WASM; this file moves messages in and samples out.
//
// The looper: layer 0 plays the selected pattern and is the loop's clock;
// layers 1..4 are their own synths (own patch) whose notes are what Sam
// recorded, turned into a pattern and played by the engine's sequencer in
// phase with layer 0. Layers sum into the engine's mix bus (final limiter).
/* global sampleRate, registerProcessor, AudioWorkletProcessor */

const MAX_LAYERS = 5;
const RING = 8192;

function ascii(text) {
  // pattern/preset text is ASCII; TextEncoder is not in every worklet scope
  const out = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 0x7f;
  return out;
}

class AdamSynthProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const module = new WebAssembly.Module(options.processorOptions.wasm);
    this.wasm = new WebAssembly.Instance(module, {}).exports;
    if (this.wasm._initialize) this.wasm._initialize();
    this.layers = [];
    this.addLayer();
    this.bus = this.wasm.adam_bus_create(sampleRate);
    this.bpm = 120;
    this.loop = true;
    this.playing = false;
    this.blocks = 0;
    this.ringL = new Float32Array(RING);
    this.ringR = new Float32Array(RING);
    this.ringPos = 0;
    this.inputPeak = 0;
    this.lastBeat = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
    this.port.postMessage({ type: "ready", sampleRate });
  }

  addLayer(kind = 0) {
    if (this.layers.length >= MAX_LAYERS) return null;
    // kind: 0 = core synth, 1 = drum kit (adam_instrument_create)
    const layer = { synth: this.wasm.adam_instrument_create(kind, sampleRate), kind, muted: false,
      recording: false, rec: [], notes: [], hasPattern: false };
    this.layers.push(layer);
    return layer;
  }

  writeText(synth, bytes) {
    const ptr = this.wasm.adam_text_buffer(synth, bytes.length + 1);
    if (!ptr) return 0;
    const mem = new Uint8Array(this.wasm.memory.buffer, ptr, bytes.length + 1);
    mem.set(bytes);
    mem[bytes.length] = 0;
    return ptr;
  }

  masterBeat() {
    return this.wasm.adam_seq_beat(this.layers[0].synth);
  }

  loopLength() {
    return this.wasm.adam_seq_length(this.layers[0].synth) || 16;
  }

  onMessage(m) {
    const w = this.wasm;
    const layer = this.layers[m.layer ?? 0];
    switch (m.type) {
      case "param": if (layer) w.adam_synth_set_param(layer.synth, m.index, m.value); break;
      case "noteOn":
        if (!layer) break;
        w.adam_synth_note_on(layer.synth, m.note, m.velocity);
        if (layer.recording) layer.rec.push({ beat: this.masterBeat(), note: m.note, vel: m.velocity });
        break;
      case "noteOff":
        if (!layer) break;
        w.adam_synth_note_off(layer.synth, m.note);
        if (layer.recording) layer.rec.push({ beat: this.masterBeat(), note: m.note, vel: 0 });
        break;
      case "sustain": if (layer) w.adam_synth_sustain(layer.synth, m.down ? 1 : 0); break;
      case "panic":
        for (const l of this.layers) {
          w.adam_seq_play(l.synth, 0);
          w.adam_synth_all_sound_off(l.synth);
        }
        this.playing = false;
        break;
      case "preset": {
        if (!layer) break;
        const ptr = this.writeText(layer.synth, m.bytes);
        if (ptr) w.adam_synth_apply_preset(layer.synth, ptr);
        // while silent, land on the patch at once; while playing, glide (no clicks)
        if (ptr && !this.playing) w.adam_synth_snap_params(layer.synth);
        break;
      }
      case "pattern": {  // the backing pattern = layer 0, the loop's clock
        const s = this.layers[0].synth;
        const ptr = this.writeText(s, m.bytes);
        const notes = ptr ? w.adam_seq_load(s, ptr) : -1;
        this.layers[0].hasPattern = notes > 0;
        this.layers[0].notes = m.notes || [];
        if (notes >= 0) {
          // a new loop: every layer starts again from its top, together
          for (const l of this.layers) w.adam_seq_seek(l.synth, 0);
          if (this.playing) w.adam_seq_play(s, 1);
        }
        this.port.postMessage({ type: "pattern", notes, lengthBeats: w.adam_seq_length(s) });
        break;
      }
      case "play":
        this.playing = !!m.on;
        if (!m.on) for (const l of this.layers) if (l.recording) this.finishRecording(l);
        for (const l of this.layers) {
          if (!m.on) w.adam_seq_rewind(l.synth);
          if (l.hasPattern || l === this.layers[0]) w.adam_seq_play(l.synth, m.on ? 1 : 0);
        }
        break;
      case "rewind": for (const l of this.layers) w.adam_seq_rewind(l.synth); break;
      case "bpm": this.bpm = m.bpm; for (const l of this.layers) w.adam_seq_set_bpm(l.synth, m.bpm); break;
      case "loop": this.loop = !!m.on; for (const l of this.layers) w.adam_seq_set_loop(l.synth, m.on ? 1 : 0); break;
      case "setNotes": {  // the piano roll edited a layer: replace its notes, keep it in phase
        if (!layer) break;
        const beat = this.masterBeat(); // loading rewinds; keep the loop's place
        const ptr = this.writeText(layer.synth, m.bytes);
        const count = ptr ? w.adam_seq_load(layer.synth, ptr) : -1;
        if (count < 0) break;
        if (m.layer === 0) w.adam_seq_seek(layer.synth, beat);
        layer.notes = m.notes || [];
        layer.hasPattern = count > 0 || m.layer === 0;
        w.adam_seq_set_bpm(layer.synth, this.bpm);
        w.adam_seq_set_loop(layer.synth, this.loop ? 1 : 0);
        if (this.playing && layer.hasPattern) {
          w.adam_seq_play(layer.synth, 1);
          w.adam_seq_seek(layer.synth, beat);
        } else if (!layer.hasPattern) {
          w.adam_seq_play(layer.synth, 0);
        }
        break;
      }
      case "addLayer": {
        const l = this.addLayer(m.kind || 0);
        if (l) {
          const ptr = this.writeText(l.synth, m.bytes);
          if (ptr) w.adam_synth_apply_preset(l.synth, ptr);
          w.adam_synth_snap_params(l.synth);
          w.adam_seq_set_bpm(l.synth, this.bpm);
          w.adam_seq_set_loop(l.synth, this.loop ? 1 : 0);
        }
        this.port.postMessage({ type: "layers", count: this.layers.length });
        break;
      }
      case "mute": if (layer) layer.muted = !!m.on; break;
      // ---- audio layers: a recorded take played in phase with the loop
      case "addAudioLayer":
        if (this.layers.length < MAX_LAYERS) {
          this.layers.push({ kind: "audio", synth: 0, muted: false, recording: false, notes: [], hasPattern: false,
            take: null, passes: 1, pass: 0, gain: 1, offset: 0, armed: false, capture: null, capturePos: 0 });
        }
        this.port.postMessage({ type: "layers", count: this.layers.length });
        break;
      case "setAudio":  // a take from storage (or replaced)
        if (layer && layer.kind === "audio") {
          layer.take = m.samples || null;
          layer.passes = m.passes || 1;
          layer.pass = 0;
        }
        break;
      case "audioParams":
        if (layer && layer.kind === "audio") {
          if (m.gain !== undefined) layer.gain = m.gain;
          if (m.offset !== undefined) layer.offset = Math.round(m.offset); // samples, + = play earlier
        }
        break;
      case "armAudio": {  // record m.passes loop passes, starting at the next bar 1
        if (!layer || layer.kind !== "audio") break;
        if (!m.on) {
          layer.armed = false;
          layer.recording = false;
          layer.capture = null;
          break;
        }
        const loopSamples = Math.round((this.loopLength() * 60 / this.bpm) * sampleRate);
        layer.passes = Math.max(1, Math.min(8, m.passes || 1));
        layer.capture = new Float32Array(loopSamples * layer.passes); // allocated here, not in process()
        layer.capturePos = 0;
        layer.armed = true;
        layer.recording = false;
        break;
      }
      case "resetLayers": {  // opening a session: back to just the backing layer
        for (const l of this.layers.slice(1)) if (l.synth) w.adam_synth_destroy(l.synth);
        this.layers.length = 1;
        const l0 = this.layers[0];
        w.adam_synth_all_sound_off(l0.synth);
        l0.muted = false;
        l0.recording = false;
        l0.notes = [];
        this.playing = false;
        w.adam_seq_play(l0.synth, 0);
        w.adam_seq_rewind(l0.synth);
        break;
      }
      case "record":
        if (!layer) break;
        if (m.on) {
          layer.recording = true;
          layer.rec = [];
        } else if (layer.recording) {
          this.finishRecording(layer);
        }
        break;
      case "clear":
        if (!layer || m.layer === 0) break;
        layer.notes = [];
        layer.hasPattern = false;
        w.adam_seq_play(layer.synth, 0);
        w.adam_synth_all_notes_off(layer.synth);
        this.port.postMessage({ type: "layerPattern", layer: m.layer, text: "", notes: [] });
        break;
      default: break;
    }
  }

  // Recorded key presses -> notes (start, duration in beats) -> merged with
  // what the layer already had (overdub) -> loaded into the layer's own
  // sequencer, seeked to the master's beat so it plays in phase.
  finishRecording(layer) {
    const w = this.wasm;
    const length = this.loopLength();
    const now = this.masterBeat();
    layer.recording = false;
    const open = new Map();
    const notes = [];
    const close = (note, endBeat) => {
      const on = open.get(note);
      if (!on) return;
      open.delete(note);
      let dur = endBeat - on.beat;
      if (dur <= 0) dur = length - on.beat; // crossed the loop end: end it there
      notes.push({ start: on.beat, dur: Math.max(0.03, dur), note, vel: on.vel });
    };
    for (const e of layer.rec) {
      if (e.vel > 0) {
        close(e.note, e.beat);
        open.set(e.note, e);
      } else close(e.note, e.beat);
    }
    for (const note of [...open.keys()]) close(note, now);
    layer.rec = [];
    layer.notes = layer.notes.concat(notes);
    if (!layer.notes.length) return;
    const lines = ["# recorded in the Adam workshop looper", `bpm ${this.bpm}`, `length ${length}`];
    for (const n of layer.notes.slice().sort((a, b) => a.start - b.start))
      lines.push(`note ${n.start.toFixed(4)} ${n.dur.toFixed(4)} ${n.note} ${n.vel}`);
    const text = lines.join("\n") + "\n";
    const ptr = this.writeText(layer.synth, ascii(text));
    const count = ptr ? w.adam_seq_load(layer.synth, ptr) : 0;
    layer.hasPattern = count > 0;
    w.adam_seq_set_bpm(layer.synth, this.bpm);
    w.adam_seq_set_loop(layer.synth, this.loop ? 1 : 0);
    if (this.playing && count > 0) {
      w.adam_seq_play(layer.synth, 1);
      w.adam_seq_seek(layer.synth, now); // `now` was read before the load rewound it
    }
    this.port.postMessage({ type: "layerPattern", layer: this.layers.indexOf(layer), text, notes: layer.notes });
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    const w = this.wasm;
    const mic = inputs[0] && inputs[0][0]; // mono mic (or nothing)
    if (mic) for (let i = 0; i < mic.length; i++) this.inputPeak = Math.max(this.inputPeak, Math.abs(mic[i]));
    const beatBefore = this.masterBeat();
    const buf = () => w.memory.buffer;
    const L = new Float32Array(buf(), w.adam_bus_left(this.bus), frames);
    const R = new Float32Array(buf(), w.adam_bus_right(this.bus), frames);
    L.fill(0);
    R.fill(0);
    const metering = this.blocks % 12 === 0;
    let voices = 0;
    let nanResets = 0;
    let layerBackstop = 0;
    // where in the loop this block starts, in samples (for audio layers)
    const spb = (60 / this.bpm) * sampleRate;
    const loopSamples = Math.round(this.loopLength() * spb);
    const blockStart = Math.round(beatBefore * spb);
    for (const layer of this.layers) {
      if (layer.kind === "audio") {
        this.audioLayer(layer, mic, L, R, frames, blockStart, loopSamples);
        continue;
      }
      w.adam_synth_render(layer.synth, frames); // muted layers still run (clock, tails)
      if (metering) {
        const m = new Float32Array(buf(), w.adam_synth_read_meter(layer.synth), 8);
        voices += m[5];
        layerBackstop += m[6];
        nanResets += m[7];
      }
      if (layer.muted) continue;
      const l = new Float32Array(buf(), w.adam_synth_out_left(layer.synth), frames);
      const r = new Float32Array(buf(), w.adam_synth_out_right(layer.synth), frames);
      for (let i = 0; i < frames; i++) {
        L[i] += l[i];
        R[i] += r[i];
      }
    }
    // loop wrapped during this block? (audio layers count passes)
    const beatAfter = this.masterBeat();
    if (this.playing && beatAfter < beatBefore - 1e-9)
      for (const l of this.layers) if (l.kind === "audio") l.pass++;
    w.adam_bus_process(this.bus, frames);
    out[0].set(L);
    if (out[1]) out[1].set(R);

    for (let i = 0; i < frames; i++) {
      this.ringL[this.ringPos] = L[i];
      this.ringR[this.ringPos] = R[i];
      this.ringPos = (this.ringPos + 1) & (RING - 1);
    }
    if (this.blocks % 90 === 0) {
      const l = new Float32Array(RING), r = new Float32Array(RING);
      l.set(this.ringL.subarray(this.ringPos));
      l.set(this.ringL.subarray(0, this.ringPos), RING - this.ringPos);
      r.set(this.ringR.subarray(this.ringPos));
      r.set(this.ringR.subarray(0, this.ringPos), RING - this.ringPos);
      this.port.postMessage({ type: "snapshot", l, r }, [l.buffer, r.buffer]);
    }
    if (metering) {
      const m = new Float32Array(buf(), w.adam_bus_read_meter(this.bus), 8);
      this.port.postMessage({
        type: "meter",
        peakL: m[0], peakR: m[1], rmsL: m[2], rmsR: m[3],
        grDb: m[4], voices, backstop: m[6] + layerBackstop, nanResets,
        beat: this.masterBeat(),
        lengthBeats: this.loopLength(),
        playing: w.adam_seq_playing(this.layers[0].synth) === 1,
        recording: this.layers.map((l) => l.recording || (l.armed ? "armed" : false)),
        inputPeak: this.inputPeak,
      });
    }
    if (metering) this.inputPeak = 0;
    this.blocks++;
    return true;
  }

  // One audio layer for one block: capture from the mic while recording, and
  // play its take in phase with the loop.
  audioLayer(layer, mic, L, R, frames, blockStart, loopSamples) {
    if (layer.armed && this.playing) {
      for (let i = 0; i < frames; i++) {
        const pos = blockStart + i;
        if (!layer.recording) {
          if (pos % loopSamples !== 0) continue; // wait for bar 1
          layer.recording = true;
          layer.capturePos = 0;
        }
        layer.capture[layer.capturePos++] = mic ? mic[i] : 0;
        if (layer.capturePos >= layer.capture.length) {
          layer.armed = false;
          layer.recording = false;
          layer.take = layer.capture;
          layer.capture = null;
          // a take always ends on a loop boundary, and that wrap (counted after
          // this) brings the pass to 0: the take replays from its first pass
          layer.pass = -1;
          const copy = layer.take.slice();
          this.port.postMessage({ type: "take", layer: this.layers.indexOf(layer), samples: copy, passes: layer.passes,
            sampleRate }, [copy.buffer]);
          break;
        }
      }
    }
    const take = layer.take;
    if (!take || layer.muted || !this.playing || layer.recording) return;
    const len = take.length;
    const pass = Math.max(0, layer.pass);
    const passBase = (pass % layer.passes) * loopSamples;
    for (let i = 0; i < frames; i++) {
      let pos = blockStart + i;
      let base = passBase;
      if (pos >= loopSamples) { // the wrap falls inside this block
        pos -= loopSamples;
        base = ((pass + 1) % layer.passes) * loopSamples;
      }
      let idx = base + pos + layer.offset;
      idx = ((idx % len) + len) % len;
      const s = take[idx] * layer.gain;
      L[i] += s;
      R[i] += s;
    }
  }
}

registerProcessor("adam-synth", AdamSynthProcessor);
