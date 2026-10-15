// Grand-staff view of a layer: the same notes as the piano roll, as notation.
// Shares the roll's model (notes, undo, step cursor, onChange) and draws /
// edits differently:
//   tap the staff        = drop a note (palette duration) at that line/space
//   tap a note           = give it the palette duration (again = cycle)
//   drag a note          = up/down by scale step, sideways in time
//   double-tap a note    = delete
// Pitches are spelled for the key (sharps or flats, key signature, ledger
// lines); treble holds middle C and up, bass below it.
import { PianoRoll } from "./pianoroll.js";

const NATURAL = [0, 2, 4, 5, 7, 9, 11]; // C D E F G A B
const LETTERS = "CDEFGAB";
const SHARP_ORDER = [3, 0, 4, 1, 5, 2, 6]; // F C G D A E B (letter indices)
const FLAT_ORDER = [6, 2, 5, 1, 4, 0, 3]; // B E A D G C F
const FIFTHS = { 0: 0, 7: 1, 2: 2, 9: 3, 4: 4, 11: 5, 6: 6, 1: -5, 8: -4, 3: -3, 10: -2, 5: -1 };
export const DURATIONS = [4, 3, 2, 1.5, 1, 0.75, 0.5, 0.375, 0.25];
const CYCLE = [1, 0.5, 0.25, 4, 2];
const MINOR_LIKE = new Set(["minor", "minpent", "dorian", "blues"]);

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const GLYPH_FONT = '"Apple Symbols", "Noto Music", "Segoe UI Symbol", "Bravura", serif';

// Key signature as fifths (+ sharps, - flats) for a root + scale name.
export function keyFifths(root, scale) {
  const major = MINOR_LIKE.has(scale) ? (root + 3) % 12 : root;
  return FIFTHS[major];
}

function keyAcc(letter, fifths) {
  if (fifths > 0) return SHARP_ORDER.slice(0, fifths).includes(letter) ? 1 : 0;
  if (fifths < 0) return FLAT_ORDER.slice(0, -fifths).includes(letter) ? -1 : 0;
  return 0;
}

// MIDI -> { step (diatonic index: C-1 = 0), letter, acc, shown (accidental to draw or null) }
export function spell(midi, fifths) {
  const pc = ((midi % 12) + 12) % 12;
  let best = null;
  for (let L = 0; L < 7; L++) {
    for (const a of [-1, 0, 1]) {
      if ((((NATURAL[L] + a) % 12) + 12) % 12 !== pc) continue;
      const k = keyAcc(L, fifths);
      const score = a === k ? 0 : a === 0 ? 1 : (fifths >= 0 ? a === 1 : a === -1) ? 2 : 3;
      if (!best || score < best.score) best = { L, a, score };
    }
  }
  const octave = Math.floor((midi - best.a - NATURAL[best.L]) / 12) - 1;
  const k = keyAcc(best.L, fifths);
  const shown = best.a === k ? null : best.a === 1 ? "♯" : best.a === -1 ? "♭" : "♮";
  return { step: best.L + 7 * (octave + 1), letter: best.L, acc: best.a, shown };
}

// A staff position (+ an explicit accidental, or the key's) -> MIDI
export function unspell(step, fifths, modifier = null) {
  const L = ((step % 7) + 7) % 7;
  const octave = Math.floor(step / 7) - 1;
  const acc = modifier === "♯" ? 1 : modifier === "♭" ? -1 : modifier === "♮" ? 0 : keyAcc(L, fifths);
  return 12 * (octave + 1) + NATURAL[L] + acc;
}

export const nearestDuration = (d) => DURATIONS.reduce((b, x) => (Math.abs(x - d) < Math.abs(b - d) ? x : b), 1);

const STEP_E4 = 2 + 7 * 5; // treble bottom line
const STEP_G2 = 4 + 7 * 3; // bass bottom line
const STEP_C4 = 0 + 7 * 5;

export class StaffView extends PianoRoll {
  constructor(canvas, opts) {
    super(canvas, opts);
    this.fifths = 0;
    this.palette = 1; // beats
    this.modifier = null; // "♯" | "♭" | "♮" | null
    this.space = 10; // px between staff lines
    this.pxPerBeat = 52;
  }

  setKey(root, scale) {
    this.fifths = keyFifths(root, scale);
    this.draw();
  }

  // layout: no rows; a grand staff
  layoutRows() {
    const s = this.space;
    this.trebleTop = 34; // y of the top line (F5)
    this.bassTop = this.trebleTop + 4 * s + 7 * s; // gap of ~3.5 spaces between staves
    this.height = this.bassTop + 4 * s + 40;
    this.headerW = 58 + Math.abs(this.fifths) * 8;
    const width = Math.max(this.canvas.parentElement?.clientWidth || 300, this.headerW + this.length * this.pxPerBeat + 12);
    this.canvas.style.width = `${width}px`;
    this.canvas.style.height = `${this.height}px`;
    this.rows = [];
  }

  yOf(step) {
    const s = this.space;
    if (step >= STEP_C4) return this.trebleTop + 4 * s - (step - STEP_E4) * (s / 2);
    return this.bassTop + 4 * s - (step - STEP_G2) * (s / 2);
  }

  stepAt(y) {
    const s = this.space;
    const mid = (this.trebleTop + 4 * s + this.bassTop) / 2;
    if (y < mid) return Math.round(STEP_E4 + (this.trebleTop + 4 * s - y) / (s / 2));
    return Math.round(STEP_G2 + (this.bassTop + 4 * s - y) / (s / 2));
  }

  xOf(beat) {
    return this.headerW + beat * this.pxPerBeat;
  }

  beatAt(x, snap = true) {
    let b = (x - this.headerW) / this.pxPerBeat;
    if (snap) b = Math.round(b / this.grid) * this.grid;
    return Math.max(0, Math.min(this.length - this.grid, b));
  }

  hit(x, y) {
    for (let i = this.notes.length - 1; i >= 0; i--) {
      const n = this.notes[i];
      const sp = spell(n.note, this.fifths);
      const nx = this.xOf(n.start) + 8, ny = this.yOf(sp.step);
      if (Math.abs(x - nx) <= 9 && Math.abs(y - ny) <= this.space * 0.75) return { index: i };
    }
    return null;
  }

  // ---------------------------------------------------------- gestures
  down(e) {
    const { x, y } = this.local(e);
    this.canvas.setPointerCapture(e.pointerId);
    this.canvas.focus({ preventScroll: true });
    if (y < 18) {
      this.cursor = this.beatAt(x, true);
      this.draw();
      return;
    }
    const h = this.hit(x, y);
    const now = performance.now();
    if (h) {
      if (this.lastTap.index === h.index && now - this.lastTap.time < 320) {
        this.selected = h.index;
        this.deleteSelected();
        this.lastTap = { index: -1, time: 0 };
        return;
      }
      this.lastTap = { index: h.index, time: now };
      const n = this.notes[h.index];
      const wasSelected = this.selected === h.index;
      this.selected = h.index;
      this.drag = { mode: "move", x, y, orig: { ...n }, moved: false, before: this.notes.map((m) => ({ ...m })),
        tapDuration: wasSelected || nearestDuration(n.dur) !== this.palette };
      this.onSelect({ audition: n.note });
    } else if (x > this.headerW) {
      const step = this.stepAt(y);
      const note = unspell(step, this.fifths, this.modifier);
      if (note < 21 || note > 108) return;
      this.snapshot();
      this.notes.push({ start: this.beatAt(x), dur: Math.min(this.palette, this.length), note, vel: 100 });
      this.selected = this.notes.length - 1;
      this.drag = null;
      this.onSelect({ audition: note });
      this.commit();
      return;
    }
    this.draw();
  }

  move(e) {
    if (!this.drag || this.selected < 0) return;
    const { x, y } = this.local(e);
    const d = this.drag;
    const n = this.notes[this.selected];
    const dBeats = Math.round((x - d.x) / this.pxPerBeat / this.grid) * this.grid;
    const dSteps = Math.round((d.y - y) / (this.space / 2));
    if (dBeats === 0 && dSteps === 0) return;
    const sp = spell(d.orig.note, this.fifths);
    n.start = Math.max(0, Math.min(this.length - this.grid, d.orig.start + dBeats));
    n.note = dSteps ? unspell(sp.step + dSteps, this.fifths) : d.orig.note;
    d.moved = true;
    this.draw();
  }

  up() {
    const d = this.drag;
    this.drag = null;
    if (!d) return;
    if (d.moved) {
      this.undoStack.push(d.before);
      this.commit();
      return;
    }
    // a tap on a note sets its duration (the palette's, or the next in the cycle)
    const n = this.notes[this.selected];
    if (!n) return;
    this.undoStack.push(d.before);
    const cur = nearestDuration(n.dur);
    n.dur = cur !== this.palette ? this.palette : CYCLE[(CYCLE.indexOf(cur) + 1) % CYCLE.length] ?? this.palette;
    n.dur = Math.min(n.dur, this.length - n.start);
    this.commit();
  }

  addAtCursor(note, velocity = 100) {
    const exists = this.notes.some((n) => Math.abs(n.start - this.cursor) < 1e-6 && n.note === note);
    if (exists) return;
    this.snapshot();
    this.notes.push({ start: this.cursor, dur: Math.min(this.palette, this.length - this.cursor), note, vel: velocity });
    this.selected = -1;
    this.commit();
  }

  moveCursor(steps) {
    // in the staff, Next moves by the palette duration (write a melody in rhythm)
    const stride = steps > 0 ? this.palette : this.grid;
    this.cursor = Math.max(0, Math.min(this.length - this.grid, this.cursor + steps * stride));
    this.draw();
  }

  // --------------------------------------------------------------- draw
  draw() {
    if (this.kind === "drums") return this.drawMessage("The staff is for pitched layers — drums stay in the roll.");
    this.layoutRows();
    const c = this.canvas;
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = this.height;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    // engraving is always dark ink on paper (the staff keeps a light background)
    const ink = "#1b2130", muted = "#6b7385", line = "#c9cfdb", accent = css("--accent");
    const s = this.space;
    // staves
    g.strokeStyle = ink;
    g.lineWidth = 1;
    for (const top of [this.trebleTop, this.bassTop])
      for (let i = 0; i < 5; i++) {
        g.beginPath();
        g.moveTo(8, top + i * s + 0.5);
        g.lineTo(w - 4, top + i * s + 0.5);
        g.stroke();
      }
    // brace line + clefs
    g.beginPath();
    g.moveTo(8.5, this.trebleTop);
    g.lineTo(8.5, this.bassTop + 4 * s);
    g.stroke();
    g.fillStyle = ink;
    g.font = `${s * 4.2}px ${GLYPH_FONT}`;
    g.fillText("𝄞", 12, this.trebleTop + 3.4 * s);
    g.font = `${s * 3.1}px ${GLYPH_FONT}`;
    g.fillText("𝄢", 13, this.bassTop + 2.6 * s);
    // key signature
    g.font = `${s * 1.8}px ${GLYPH_FONT}`;
    // standard key-signature placements (steps: C4 = 35): sharps F C G D A E B,
    // flats B E A D G C F
    const sigSteps = this.fifths > 0
      ? { treble: [45, 42, 46, 43, 40, 44, 41], bass: [31, 28, 32, 29, 26, 30, 27] }
      : { treble: [41, 44, 40, 43, 39, 42, 38], bass: [27, 30, 26, 29, 25, 28, 24] };
    for (let i = 0; i < Math.abs(this.fifths); i++) {
      const glyph = this.fifths > 0 ? "♯" : "♭";
      g.fillText(glyph, 40 + i * 8, this.yOf(sigSteps.treble[i]) + s * 0.55);
      g.fillText(glyph, 40 + i * 8, this.yOf(sigSteps.bass[i]) + s * 0.55);
    }
    // bar lines + bar numbers
    g.font = "10px -apple-system, sans-serif";
    for (let b = 0; b <= this.length + 1e-6; b += 1) {
      const x = Math.round(this.xOf(b)) + 0.5;
      if (Math.abs(b % 4) < 1e-6) {
        g.strokeStyle = ink;
        g.beginPath();
        g.moveTo(x, this.trebleTop);
        g.lineTo(x, this.bassTop + 4 * s);
        g.stroke();
        if (b < this.length) {
          g.fillStyle = muted;
          g.fillText(`${b / 4 + 1}`, x + 3, 12);
        }
      } else {
        g.strokeStyle = line;
        g.globalAlpha = 0.35;
        g.beginPath();
        g.moveTo(x, 16);
        g.lineTo(x, 22);
        g.stroke();
        g.globalAlpha = 1;
      }
    }
    // step cursor
    if (this.stepMode) {
      const x = this.xOf(this.cursor);
      g.fillStyle = css("--accent-2");
      g.globalAlpha = 0.16;
      g.fillRect(x, 18, this.palette * this.pxPerBeat, h - 18);
      g.globalAlpha = 1;
      g.fillRect(x - 1, 0, 2, h);
    }
    // notes: group by start + staff for shared stems
    const groups = new Map();
    this.notes.forEach((n, i) => {
      const sp = spell(n.note, this.fifths);
      const treble = sp.step >= STEP_C4;
      const key = `${n.start.toFixed(4)}|${treble}|${nearestDuration(n.dur)}`;
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push({ n, i, sp, treble });
    });
    for (const members of groups.values()) this.drawChord(g, members, ink, accent);
    // playhead
    if (this.playhead >= 0) {
      g.fillStyle = css("--bad");
      g.fillRect(this.xOf(this.playhead % this.length) - 1, 0, 2, h);
    }
  }

  drawChord(g, members, ink, accent) {
    const s = this.space;
    const { n } = members[0];
    const dur = nearestDuration(n.dur);
    const x = this.xOf(n.start) + 8;
    const treble = members[0].treble;
    const top = treble ? this.trebleTop : this.bassTop;
    const middleStep = treble ? STEP_E4 + 4 : STEP_G2 + 4;
    const steps = members.map((m) => m.sp.step);
    const avg = steps.reduce((a, b) => a + b, 0) / steps.length;
    const stemUp = avg < middleStep;
    const hollow = dur >= 2;
    const dotted = [3, 1.5, 0.75, 0.375].includes(dur);
    for (const m of members) {
      const y = this.yOf(m.sp.step);
      const color = m.i === this.selected ? accent : ink;
      g.strokeStyle = color;
      g.fillStyle = color;
      // ledger lines
      const bottomStep = treble ? STEP_E4 : STEP_G2;
      const topStep = bottomStep + 8;
      for (let st = (m.sp.step < bottomStep ? m.sp.step : topStep + 2); st <= (m.sp.step < bottomStep ? bottomStep - 2 : m.sp.step); st++) {
        if ((st - bottomStep) % 2 !== 0) continue;
        const ly = Math.round(this.yOf(st)) + 0.5;
        g.beginPath();
        g.moveTo(x - 9, ly);
        g.lineTo(x + 9, ly);
        g.stroke();
      }
      // notehead
      g.beginPath();
      g.ellipse(x, y, s * 0.62, s * 0.45, -0.35, 0, Math.PI * 2);
      if (hollow) {
        g.lineWidth = 1.6;
        g.stroke();
        g.lineWidth = 1;
      } else g.fill();
      if (m.sp.shown) {
        g.font = `${s * 1.7}px ${GLYPH_FONT}`;
        g.fillText(m.sp.shown, x - s * 1.9, y + s * 0.55);
      }
      if (dotted) {
        g.beginPath();
        g.arc(x + s * 1.05, y - ((m.sp.step - (treble ? STEP_E4 : STEP_G2)) % 2 === 0 ? s * 0.3 : 0), 1.8, 0, Math.PI * 2);
        g.fill();
      }
    }
    if (dur >= 4) return; // whole note: no stem
    const ys = members.map((m) => this.yOf(m.sp.step));
    const sx = stemUp ? x + s * 0.58 : x - s * 0.58;
    const y0 = stemUp ? Math.max(...ys) : Math.min(...ys);
    const y1 = stemUp ? Math.min(...ys) - s * 3.4 : Math.max(...ys) + s * 3.4;
    g.strokeStyle = members.some((m) => m.i === this.selected) ? accent : ink;
    g.lineWidth = 1.3;
    g.beginPath();
    g.moveTo(sx, y0);
    g.lineTo(sx, y1);
    g.stroke();
    // flags: 1 for eighths, 2 for sixteenths
    const flags = dur <= 0.375 ? 2 : dur <= 0.75 ? 1 : 0;
    for (let f = 0; f < flags; f++) {
      const fy = y1 + (stemUp ? f * s * 0.9 : -f * s * 0.9);
      g.beginPath();
      g.moveTo(sx, fy);
      g.quadraticCurveTo(sx + s * 1.1, fy + (stemUp ? s * 1.2 : -s * 1.2), sx + s * 0.9, fy + (stemUp ? s * 2.4 : -s * 2.4));
      g.stroke();
    }
    g.lineWidth = 1;
    void top;
  }

  drawMessage(text) {
    const c = this.canvas;
    const dpr = window.devicePixelRatio || 1;
    c.style.width = `${c.parentElement?.clientWidth || 300}px`;
    c.style.height = "60px";
    c.width = Math.round((c.parentElement?.clientWidth || 300) * dpr);
    c.height = Math.round(60 * dpr);
    const g = c.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, c.width, c.height);
    g.fillStyle = css("--muted");
    g.font = "13px -apple-system, sans-serif";
    g.fillText(text, 12, 34);
  }
}
