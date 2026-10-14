// AudioWorklet host for the shared C++ engine (adam_synth.wasm). All sound —
// oscillators, filter, envelopes, voices, the pattern sequencer, the limiter —
// comes from the WASM; this file only moves messages in and samples out.
/* global sampleRate, registerProcessor, AudioWorkletProcessor */

class AdamSynthProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const bytes = options.processorOptions.wasm;
    // Synchronous compile is allowed off the main thread.
    const module = new WebAssembly.Module(bytes);
    this.wasm = new WebAssembly.Instance(module, {}).exports;
    if (this.wasm._initialize) this.wasm._initialize();
    this.synth = this.wasm.adam_synth_create(sampleRate);
    this.blocks = 0;
    // last 8192 samples for the analysis panel (copied off the audio thread)
    this.ringL = new Float32Array(8192);
    this.ringR = new Float32Array(8192);
    this.ringPos = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
    this.port.postMessage({ type: "ready", sampleRate });
  }

  // Text arrives pre-encoded (TextEncoder is not available in every
  // AudioWorkletGlobalScope).
  writeText(bytes) {
    const ptr = this.wasm.adam_text_buffer(this.synth, bytes.length + 1);
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
      case "param": w.adam_synth_set_param(s, m.index, m.value); break;
      case "noteOn": w.adam_synth_note_on(s, m.note, m.velocity); break;
      case "noteOff": w.adam_synth_note_off(s, m.note); break;
      case "sustain": w.adam_synth_sustain(s, m.down ? 1 : 0); break;
      case "panic": w.adam_seq_play(s, 0); w.adam_synth_all_sound_off(s); break;
      case "preset": {
        const ptr = this.writeText(m.bytes);
        if (ptr) w.adam_synth_apply_preset(s, ptr);
        // while silent, land on the patch at once; while playing, glide (no clicks)
        if (ptr && !w.adam_seq_playing(s)) w.adam_synth_snap_params(s);
        break;
      }
      case "pattern": {
        const ptr = this.writeText(m.bytes);
        const notes = ptr ? w.adam_seq_load(s, ptr) : 0;
        this.port.postMessage({ type: "pattern", notes, lengthBeats: w.adam_seq_length(s) });
        break;
      }
      case "play": w.adam_seq_play(s, m.on ? 1 : 0); break;
      case "rewind": w.adam_seq_rewind(s); break;
      case "bpm": w.adam_seq_set_bpm(s, m.bpm); break;
      case "loop": w.adam_seq_set_loop(s, m.on ? 1 : 0); break;
      default: break;
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    const w = this.wasm;
    w.adam_synth_render(this.synth, frames);
    const buf = w.memory.buffer;
    out[0].set(new Float32Array(buf, w.adam_synth_out_left(this.synth), frames));
    if (out[1]) out[1].set(new Float32Array(buf, w.adam_synth_out_right(this.synth), frames));
    const R = out[1] || out[0];
    for (let i = 0; i < frames; i++) {
      this.ringL[this.ringPos] = out[0][i];
      this.ringR[this.ringPos] = R[i];
      this.ringPos = (this.ringPos + 1) & 8191;
    }
    if (this.blocks % 90 === 0) {
      // ~4 snapshots a second, oldest sample first
      const l = new Float32Array(8192), r = new Float32Array(8192);
      l.set(this.ringL.subarray(this.ringPos));
      l.set(this.ringL.subarray(0, this.ringPos), 8192 - this.ringPos);
      r.set(this.ringR.subarray(this.ringPos));
      r.set(this.ringR.subarray(0, this.ringPos), 8192 - this.ringPos);
      this.port.postMessage({ type: "snapshot", l, r }, [l.buffer, r.buffer]);
    }

    // ~30 meter updates a second
    if (++this.blocks % 12 === 0) {
      const m = new Float32Array(buf, w.adam_synth_read_meter(this.synth), 8);
      this.port.postMessage({
        type: "meter",
        peakL: m[0], peakR: m[1], rmsL: m[2], rmsR: m[3],
        grDb: m[4], voices: m[5], backstop: m[6], nanResets: m[7],
        beat: w.adam_seq_beat(this.synth),
        playing: w.adam_seq_playing(this.synth) === 1,
      });
    }
    return true;
  }
}

registerProcessor("adam-synth", AdamSynthProcessor);
