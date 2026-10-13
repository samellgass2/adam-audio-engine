// AudioWorklet host for the shared C++ engine (atom_synth.wasm). All sound —
// oscillators, filter, envelopes, voices, the pattern sequencer, the limiter —
// comes from the WASM; this file only moves messages in and samples out.
/* global sampleRate, registerProcessor, AudioWorkletProcessor */

class AtomSynthProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const bytes = options.processorOptions.wasm;
    // Synchronous compile is allowed off the main thread.
    const module = new WebAssembly.Module(bytes);
    this.wasm = new WebAssembly.Instance(module, {}).exports;
    if (this.wasm._initialize) this.wasm._initialize();
    this.synth = this.wasm.atom_synth_create(sampleRate);
    this.blocks = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
    this.port.postMessage({ type: "ready", sampleRate });
  }

  // Text arrives pre-encoded (TextEncoder is not available in every
  // AudioWorkletGlobalScope).
  writeText(bytes) {
    const ptr = this.wasm.atom_text_buffer(this.synth, bytes.length + 1);
    if (!ptr) return 0;
    const mem = new Uint8Array(this.wasm.memory.buffer, ptr, bytes.length + 1);
    mem.set(bytes);
    mem[bytes.length] = 0;
    return ptr;
  }

  onMessage(m) {
    const w = this.wasm;
    const s = this.synth;
    switch (m.type) {
      case "param": w.atom_synth_set_param(s, m.index, m.value); break;
      case "noteOn": w.atom_synth_note_on(s, m.note, m.velocity); break;
      case "noteOff": w.atom_synth_note_off(s, m.note); break;
      case "sustain": w.atom_synth_sustain(s, m.down ? 1 : 0); break;
      case "panic": w.atom_seq_play(s, 0); w.atom_synth_all_sound_off(s); break;
      case "preset": {
        const ptr = this.writeText(m.bytes);
        if (ptr) w.atom_synth_apply_preset(s, ptr);
        break;
      }
      case "pattern": {
        const ptr = this.writeText(m.bytes);
        const notes = ptr ? w.atom_seq_load(s, ptr) : 0;
        this.port.postMessage({ type: "pattern", notes, lengthBeats: w.atom_seq_length(s) });
        break;
      }
      case "play": w.atom_seq_play(s, m.on ? 1 : 0); break;
      case "rewind": w.atom_seq_rewind(s); break;
      case "bpm": w.atom_seq_set_bpm(s, m.bpm); break;
      case "loop": w.atom_seq_set_loop(s, m.on ? 1 : 0); break;
      default: break;
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    const w = this.wasm;
    w.atom_synth_render(this.synth, frames);
    const buf = w.memory.buffer;
    out[0].set(new Float32Array(buf, w.atom_synth_out_left(this.synth), frames));
    if (out[1]) out[1].set(new Float32Array(buf, w.atom_synth_out_right(this.synth), frames));

    // ~30 meter updates a second
    if (++this.blocks % 12 === 0) {
      const m = new Float32Array(buf, w.atom_synth_read_meter(this.synth), 8);
      this.port.postMessage({
        type: "meter",
        peakL: m[0], peakR: m[1], rmsL: m[2], rmsR: m[3],
        grDb: m[4], voices: m[5], backstop: m[6], nanResets: m[7],
        beat: w.atom_seq_beat(this.synth),
        playing: w.atom_seq_playing(this.synth) === 1,
      });
    }
    return true;
  }
}

registerProcessor("atom-synth", AtomSynthProcessor);
