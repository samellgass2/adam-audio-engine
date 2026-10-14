// Piano roll for one looper layer: see the notes, add / move / resize /
// delete them, a playhead, and a step cursor for non-real-time entry.
// The notes it edits are the layer's truth; every committed edit is handed to
// onChange(notes) (the app turns them into a pattern for the engine).
//
// Gestures: tap an empty cell = add a note (one grid step); drag a note =
// move; drag a note's right edge = resize; double-tap a note = delete; tap the
// ruler = put the step cursor there. Delete/Backspace removes the selection.

export const DRUM_ROWS = [
  [46, "Open hat"],
  [42, "Closed hat"],
  [48, "High tom"],
  [45, "Mid tom"],
  [41, "Low tom"],
  [39, "Clap"],
  [37, "Rim"],
  [38, "Snare"],
  [36, "Kick"],
];
const DRUM_ALIAS = { 35: 36, 40: 38, 44: 42, 43: 41, 47: 45, 50: 48 };
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export const noteName = (n) => `${NOTE_NAMES[((n % 12) + 12) % 12]}${Math.floor(n / 12) - 1}`;

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

export class PianoRoll {
  constructor(canvas, { onChange, onSelect } = {}) {
    this.canvas = canvas;
    this.onChange = onChange || (() => {});
    this.onSelect = onSelect || (() => {});
    this.kind = "synth";
    this.notes = [];
    this.length = 16;
    this.grid = 0.5;
    this.cursor = 0;
    this.stepMode = false;
    this.playhead = -1;
    this.selected = -1;
    this.inScale = null; // (pitchClass) => bool, or null = chromatic
    this.root = 0;
    this.undoStack = [];
    this.drag = null;
    this.lastTap = { index: -1, time: 0 };
    this.labelW = 52;
    this.rulerH = 18;
    canvas.addEventListener("pointerdown", (e) => this.down(e));
    canvas.addEventListener("pointermove", (e) => this.move(e));
    canvas.addEventListener("pointerup", (e) => this.up(e));
    canvas.addEventListener("pointercancel", () => (this.drag = null));
    canvas.tabIndex = 0;
    canvas.addEventListener("keydown", (e) => {
      if (e.key === "Delete" || e.key === "Backspace") {
        e.preventDefault();
        this.deleteSelected();
      }
    });
  }

  // ------------------------------------------------------------- model
  setLayer({ kind, notes, lengthBeats }) {
    this.kind = kind;
    this.notes = notes.map((n) => ({ ...n }));
    this.length = lengthBeats || 16;
    this.selected = -1;
    this.undoStack = [];
    this.cursor = Math.min(this.cursor, this.length - this.grid);
    this.layoutRows();
    this.draw();
  }

  setScale(root, inScale) {
    this.root = root;
    this.inScale = inScale;
    this.draw();
  }

  layoutRows() {
    if (this.kind === "drums") {
      this.rows = DRUM_ROWS.map(([note, label]) => ({ note, label }));
      this.rowH = 22;
    } else {
      let lo = 48, hi = 72; // C3..C5 by default
      if (this.notes.length) {
        lo = Math.min(lo, ...this.notes.map((n) => n.note)) - 2;
        hi = Math.max(hi, ...this.notes.map((n) => n.note)) + 2;
      }
      this.rows = [];
      for (let n = hi; n >= lo; n--) this.rows.push({ note: n, label: noteName(n) });
      this.rowH = 14;
    }
    const h = this.rulerH + this.rows.length * this.rowH;
    this.canvas.style.height = `${h}px`;
  }

  rowOf(note) {
    const n = this.kind === "drums" ? DRUM_ALIAS[note] ?? note : note;
    return this.rows.findIndex((r) => r.note === n);
  }

  snapshot() {
    this.undoStack.push(this.notes.map((n) => ({ ...n })));
    if (this.undoStack.length > 60) this.undoStack.shift();
  }

  commit() {
    this.notes.sort((a, b) => a.start - b.start || a.note - b.note);
    this.onChange(this.notes.map((n) => ({ ...n })));
    this.draw();
  }

  undo() {
    if (!this.undoStack.length) return;
    this.notes = this.undoStack.pop();
    this.selected = -1;
    this.commit();
  }

  clear() {
    if (!this.notes.length) return;
    this.snapshot();
    this.notes = [];
    this.selected = -1;
    this.commit();
  }

  deleteSelected() {
    if (this.selected < 0) return;
    this.snapshot();
    this.notes.splice(this.selected, 1);
    this.selected = -1;
    this.commit();
  }

  // Step entry: add a note at the cursor (a chord builds up at one cursor).
  addAtCursor(note, velocity = 100) {
    const start = this.cursor;
    const exists = this.notes.some((n) => Math.abs(n.start - start) < 1e-6 && n.note === note);
    if (exists) return;
    this.snapshot();
    const dur = this.grid; // drums: one-shots; the length only shapes the MIDI export
    this.notes.push({ start, dur, note, vel: velocity });
    this.selected = -1;
    this.layoutRows();
    this.commit();
  }

  moveCursor(steps) {
    const n = Math.round(this.length / this.grid);
    let k = Math.round(this.cursor / this.grid) + steps;
    k = ((k % n) + n) % n;
    this.cursor = k * this.grid;
    this.draw();
  }

  // ---------------------------------------------------------- geometry
  metrics() {
    const dpr = window.devicePixelRatio || 1;
    const w = this.canvas.clientWidth;
    const plot = Math.max(10, w - this.labelW);
    return { dpr, w, plot, bx: (beat) => this.labelW + (beat / this.length) * plot, pxPerBeat: plot / this.length };
  }

  hit(x, y) {
    const { bx } = this.metrics();
    for (let i = this.notes.length - 1; i >= 0; i--) {
      const n = this.notes[i];
      const r = this.rowOf(n.note);
      if (r < 0) continue;
      const top = this.rulerH + r * this.rowH;
      const x0 = bx(n.start), x1 = Math.max(bx(n.start + n.dur), x0 + 6);
      if (x >= x0 && x <= x1 && y >= top && y <= top + this.rowH) return { index: i, edge: x1 - x < 8 };
    }
    return null;
  }

  beatAt(x, snap = true) {
    const { pxPerBeat } = this.metrics();
    let b = (x - this.labelW) / pxPerBeat;
    if (snap) b = Math.floor(b / this.grid + 1e-6) * this.grid;
    return Math.max(0, Math.min(this.length - this.grid, b));
  }

  rowAt(y) {
    return Math.max(0, Math.min(this.rows.length - 1, Math.floor((y - this.rulerH) / this.rowH)));
  }

  local(e) {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  // ---------------------------------------------------------- gestures
  down(e) {
    const { x, y } = this.local(e);
    this.canvas.setPointerCapture(e.pointerId);
    this.canvas.focus({ preventScroll: true });
    if (y < this.rulerH) {
      this.cursor = this.beatAt(x);
      this.draw();
      return;
    }
    if (x < this.labelW) {
      // tapping a row label auditions it
      this.onSelect({ audition: this.rows[this.rowAt(y)].note });
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
      this.selected = h.index;
      const n = this.notes[h.index];
      this.drag = { mode: h.edge && this.kind !== "drums" ? "resize" : "move", x, y, orig: { ...n }, moved: false,
        before: this.notes.map((m) => ({ ...m })) };
      this.onSelect({ audition: n.note });
    } else {
      const start = this.beatAt(x);
      const note = this.rows[this.rowAt(y)].note;
      // an occupied cell selects the note already there (never a duplicate)
      const there = this.notes.findIndex((n) => this.rowOf(n.note) === this.rowAt(y) && Math.abs(n.start - start) < 1e-6);
      if (there >= 0) {
        this.selected = there;
        this.lastTap = { index: there, time: now };
        this.onSelect({ audition: this.notes[there].note });
        this.draw();
        return;
      }
      this.snapshot();
      const dur = this.grid; // drums: one-shots; the length only shapes the MIDI export
      this.notes.push({ start, dur, note, vel: 100 });
      this.selected = this.notes.length - 1;
      this.drag = { mode: this.kind === "drums" ? "none" : "resize", x, y, orig: { ...this.notes[this.selected] },
        moved: true, before: null };
      this.onSelect({ audition: note });
    }
    this.draw();
  }

  move(e) {
    if (!this.drag || this.selected < 0) return;
    const { x, y } = this.local(e);
    const { pxPerBeat } = this.metrics();
    const n = this.notes[this.selected];
    const d = this.drag;
    const dBeats = Math.round((x - d.x) / pxPerBeat / this.grid) * this.grid;
    if (d.mode === "move") {
      n.start = Math.max(0, Math.min(this.length - this.grid, d.orig.start + dBeats));
      const r0 = this.rowOf(d.orig.note);
      const r = Math.max(0, Math.min(this.rows.length - 1, r0 + Math.round((y - d.y) / this.rowH)));
      n.note = this.rows[r].note;
    } else if (d.mode === "resize") {
      n.dur = Math.max(this.grid, Math.min(this.length - n.start, d.orig.dur + dBeats));
    }
    if (n.start !== d.orig.start || n.note !== d.orig.note || n.dur !== d.orig.dur) d.moved = true;
    this.draw();
  }

  up() {
    const d = this.drag;
    this.drag = null;
    if (!d) return;
    if (d.moved) {
      if (d.before) this.undoStack.push(d.before);
      this.commit();
    }
  }

  // --------------------------------------------------------------- draw
  draw() {
    const c = this.canvas;
    const { dpr, w, bx } = this.metrics();
    const h = this.rulerH + this.rows.length * this.rowH;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    const panel2 = css("--panel-2"), line = css("--line"), muted = css("--muted"), accent = css("--accent");
    g.font = "10px -apple-system, sans-serif";
    // rows
    this.rows.forEach((row, i) => {
      const top = this.rulerH + i * this.rowH;
      const pc = ((row.note % 12) + 12) % 12;
      const outOfKey = this.kind !== "drums" && this.inScale && !this.inScale(pc);
      const black = this.kind !== "drums" && [1, 3, 6, 8, 10].includes(pc);
      g.fillStyle = outOfKey ? line : black ? panel2 : "transparent";
      if (g.fillStyle !== "transparent") g.fillRect(this.labelW, top, w - this.labelW, this.rowH);
      if (this.kind !== "drums" && this.inScale && pc === this.root) {
        g.fillStyle = `${accent}22`;
        g.fillRect(this.labelW, top, w - this.labelW, this.rowH);
      }
      g.fillStyle = outOfKey ? line : muted;
      if (this.kind === "drums" || pc === 0 || this.rowH >= 16 || (this.inScale && pc === this.root))
        g.fillText(row.label, 4, top + this.rowH - 4);
      g.strokeStyle = line;
      g.globalAlpha = 0.35;
      g.beginPath();
      g.moveTo(this.labelW, top + this.rowH + 0.5);
      g.lineTo(w, top + this.rowH + 0.5);
      g.stroke();
      g.globalAlpha = 1;
    });
    // grid
    for (let b = 0; b <= this.length + 1e-6; b += this.grid) {
      const x = Math.round(bx(b)) + 0.5;
      const bar = Math.abs(b % 4) < 1e-6, beat = Math.abs(b % 1) < 1e-6;
      g.strokeStyle = bar ? muted : line;
      g.globalAlpha = bar ? 0.9 : beat ? 0.6 : 0.25;
      g.beginPath();
      g.moveTo(x, bar || beat ? 0 : this.rulerH);
      g.lineTo(x, h);
      g.stroke();
      if (bar && b < this.length) {
        g.globalAlpha = 1;
        g.fillStyle = muted;
        g.fillText(`${b / 4 + 1}`, x + 3, 12);
      }
    }
    g.globalAlpha = 1;
    // notes
    this.notes.forEach((n, i) => {
      const r = this.rowOf(n.note);
      if (r < 0) return;
      const top = this.rulerH + r * this.rowH;
      const x0 = bx(n.start), x1 = Math.max(bx(n.start + n.dur), x0 + 5);
      g.fillStyle = accent;
      g.globalAlpha = 0.35 + 0.65 * (n.vel / 127);
      g.fillRect(x0 + 0.5, top + 1, x1 - x0 - 1, this.rowH - 2);
      g.globalAlpha = 1;
      if (i === this.selected) {
        g.strokeStyle = css("--text");
        g.lineWidth = 2;
        g.strokeRect(x0 + 0.5, top + 1, x1 - x0 - 1, this.rowH - 2);
        g.lineWidth = 1;
      }
    });
    // step cursor
    if (this.stepMode) {
      const x = bx(this.cursor);
      g.fillStyle = css("--accent-2");
      g.globalAlpha = 0.18;
      g.fillRect(x, this.rulerH, Math.max(2, bx(this.cursor + this.grid) - x), h - this.rulerH);
      g.globalAlpha = 1;
      g.fillRect(x - 1, 0, 2, h);
    }
    // playhead
    if (this.playhead >= 0) {
      g.fillStyle = css("--bad");
      g.fillRect(bx(this.playhead % this.length) - 1, 0, 2, h);
    }
  }
}
