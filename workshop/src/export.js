// Export for Logic: one .zip with everything a demo needs to move into a DAW.
//   song.mid        multitrack MIDI (tempo + one track per MIDI layer; drums on ch 10)
//   stems/*.wav     each layer rendered alone, and the full mix (32-bit float, stereo)
//   takes/*.wav     raw audio takes
//   sounds/*.preset each layer's full sound (Adam Core Synth / Adam Drums parameters)
//   session.adamsession, README.txt
// Stems come from an OfflineAudioContext running the SAME worklet and WASM
// engine as live playback, so they are exactly what the workshop plays.

// ------------------------------------------------------------------ MIDI
function vlq(x) {
  const out = [x & 0x7f];
  while ((x >>= 7)) out.unshift((x & 0x7f) | 0x80);
  return out;
}
const u32 = (x) => [(x >>> 24) & 255, (x >>> 16) & 255, (x >>> 8) & 255, x & 255];
const u16 = (x) => [(x >> 8) & 255, x & 255];
const ascii = (s) => [...s].map((c) => c.charCodeAt(0) & 0x7f);

function chunk(tag, bytes) {
  return [...ascii(tag), ...u32(bytes.length), ...bytes];
}

// Format 1: track 0 = tempo + name; then one track per MIDI layer.
export function multitrackMidi({ bpm, lengthBeats, loops, layers, name }) {
  const ppq = 480;
  const tempo = Math.round(60000000 / bpm);
  const t0 = [0, 0xff, 0x51, 3, (tempo >> 16) & 255, (tempo >> 8) & 255, tempo & 255,
    0, 0xff, 0x58, 4, 4, 2, 24, 8, // 4/4
    0, 0xff, 0x03, ...vlq(name.length), ...ascii(name),
    ...vlq(Math.round(lengthBeats * loops * ppq)), 0xff, 0x2f, 0];
  const tracks = [chunk("MTrk", t0)];
  let synthCh = 0;
  for (const l of layers) {
    if (l.kind === "audio" || !l.notes.length) continue;
    const ch = l.kind === "drums" ? 9 : synthCh++ % 9;
    const ev = [];
    for (let k = 0; k < loops; k++)
      for (const n of l.notes) {
        const s = n.start + k * lengthBeats;
        const e = Math.min(n.start + n.dur, lengthBeats) + k * lengthBeats;
        ev.push([Math.round(s * ppq), 1, n.note, n.vel], [Math.round(e * ppq), 0, n.note, 0]);
      }
    ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    const tr = [0, 0xff, 0x03, ...vlq(l.name.length), ...ascii(l.name)];
    let last = 0;
    for (const [t, on, n, v] of ev) {
      tr.push(...vlq(t - last), (on ? 0x90 : 0x80) | ch, n, on ? v : 64);
      last = t;
    }
    tr.push(...vlq(Math.max(0, Math.round(lengthBeats * loops * ppq) - last)), 0xff, 0x2f, 0);
    tracks.push(chunk("MTrk", tr));
  }
  const header = chunk("MThd", [...u16(1), ...u16(tracks.length), ...u16(ppq)]);
  return new Uint8Array([...header, ...tracks.flat()]);
}

// ------------------------------------------------------------------- WAV
export function wavStereo(left, right, sr) {
  const n = left.length;
  const buf = new ArrayBuffer(44 + n * 8);
  const v = new DataView(buf);
  const str = (o, s) => [...s].forEach((c, i) => v.setUint8(o + i, c.charCodeAt(0)));
  str(0, "RIFF"); v.setUint32(4, 36 + n * 8, true); str(8, "WAVEfmt ");
  v.setUint32(16, 16, true); v.setUint16(20, 3, true); v.setUint16(22, 2, true);
  v.setUint32(24, sr, true); v.setUint32(28, sr * 8, true); v.setUint16(32, 8, true); v.setUint16(34, 32, true);
  str(36, "data"); v.setUint32(40, n * 8, true);
  const out = new Float32Array(buf, 44, n * 2);
  for (let i = 0; i < n; i++) {
    out[2 * i] = left[i];
    out[2 * i + 1] = right[i];
  }
  return new Uint8Array(buf);
}

// ------------------------------------------------------------------- ZIP
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// Stored (uncompressed) zip: audio does not compress anyway.
export function zip(files) {
  const enc = new TextEncoder();
  const parts = [];
  const central = [];
  let offset = 0;
  const le16 = (x) => [x & 255, (x >> 8) & 255];
  const le32 = (x) => [x & 255, (x >>> 8) & 255, (x >>> 16) & 255, (x >>> 24) & 255];
  for (const { name, data } of files) {
    const bytes = typeof data === "string" ? enc.encode(data) : data;
    const nameBytes = enc.encode(name);
    const crc = crc32(bytes);
    const local = new Uint8Array([...le32(0x04034b50), ...le16(20), ...le16(0x0800), ...le16(0), ...le16(0), ...le16(0x21),
      ...le32(crc), ...le32(bytes.length), ...le32(bytes.length), ...le16(nameBytes.length), ...le16(0), ...nameBytes]);
    parts.push(local, bytes);
    central.push(new Uint8Array([...le32(0x02014b50), ...le16(20), ...le16(20), ...le16(0x0800), ...le16(0), ...le16(0),
      ...le16(0x21), ...le32(crc), ...le32(bytes.length), ...le32(bytes.length), ...le16(nameBytes.length), ...le16(0),
      ...le16(0), ...le16(0), ...le16(0), ...le32(0), ...le32(offset), ...nameBytes]));
    offset += local.length + bytes.length;
  }
  const cdSize = central.reduce((s, c) => s + c.length, 0);
  const end = new Uint8Array([...le32(0x06054b50), ...le16(0), ...le16(0), ...le16(files.length), ...le16(files.length),
    ...le32(cdSize), ...le32(offset), ...le16(0)]);
  return new Blob([...parts, ...central, end], { type: "application/zip" });
}

// ---------------------------------------------------------- offline render
// Build the session in a fresh engine inside an OfflineAudioContext and render
// `seconds`. `solo` = index of the only audible layer (or -1 for the mix).
export async function renderOffline({ wasmBytes, setup, seconds, sampleRate }) {
  const frames = Math.ceil(seconds * sampleRate);
  const ctx = new OfflineAudioContext(2, frames, sampleRate);
  await ctx.audioWorklet.addModule("./worklet.js");
  const node = new AudioWorkletNode(ctx, "adam-synth", {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
    processorOptions: { wasm: wasmBytes, setup },
  });
  node.connect(ctx.destination);
  const out = await ctx.startRendering();
  return { left: out.getChannelData(0), right: out.getChannelData(1) };
}
