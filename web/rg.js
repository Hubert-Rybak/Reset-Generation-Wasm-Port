// Shared loader for the Reset Generation WASM port (browser and Node).
// The RG class wraps the wasm module: loading, the engine API, frame pacing,
// audio pulls and access to the in-memory files.

export const SCREEN_W = 240, SCREEN_H = 320;
export const PAKS = ['base.pak', 'fonts.pak', 'menus.pak', 'intro.pak', 'levels.pak',
  'gameplay.pak', 'gameplay2.pak', 'story.pak', 'fight.pak'];
export const SOUND_RATE = 8000;          // engine audio: 8 kHz, 16-bit signed, mono
export const SOUND_CHUNK = 1024;         // bytes per pull, as the Java host used

// The engine keeps an absolute 30 fps schedule (33 ms per frame): it sleeps
// until its next frame, or runs frames back to back to catch up when it is
// late, and it streams in data only during that idle time.  If a frame ever
// overran its 33 ms the engine would fall behind for good and stop loading
// (a frozen "wait" screen).  So the game's clock is trimmed:
//  - a single tick counts at most TICK_MAX_MS (the first frames take over a
//    second; a slow device gets slow motion instead of a stall);
//  - a tick that starts more than MAX_LAG_MS late (display refresh granularity,
//    hidden tab, debugger) only shows MAX_LAG_MS of lateness.
// TICK_MAX_MS + MAX_LAG_MS stays within one 33 ms frame.
const TICK_MAX_MS = 19;
const MAX_LAG_MS = 14;

const utf8 = new TextDecoder();

export class RG {
  constructor() {
    this.mem = null;
    this.ex = null;
    this.onLog = (s) => console.log('[rg]', s);
    this.onAbort = (s) => { throw new Error('rg abort: ' + s); };
    this.now = () => performance.now();   // real time, ms
    this.clockOffset = 0;                  // game time = real time - offset
    this.pausedAt = null;                  // real time the game clock was paused
    this.tickStart = null;                 // real time the running tick started
    this.lastTime = -Infinity;             // last game time handed out
    this.nextTickAt = 0;                   // real time the next tick is due
    this.alive = true;
  }

  _str(ptr, len) { return utf8.decode(new Uint8Array(this.mem.buffer, ptr, len)); }
  _cstr(ptr) {
    const b = new Uint8Array(this.mem.buffer, ptr);
    return utf8.decode(b.subarray(0, b.indexOf(0)));
  }

  async load(wasmBytes) {
    const imports = {
      env: {
        js_log: (ptr, len) => this.onLog(this._str(ptr, len)),
        js_abort: (ptr, len) => this.onAbort(this._str(ptr, len)),
        js_now: () => this.gameTime(),
        js_wall_ms: () => Date.now(),
      },
    };
    const { instance } = await WebAssembly.instantiate(wasmBytes, imports);
    this.ex = instance.exports;
    this.mem = this.ex.memory;
  }

  // Copy bytes into a fresh wasm allocation (NUL-terminated for strings).
  _push(bytes) {
    const p = this.ex.rg_alloc(bytes.length + 1) >>> 0;
    const u = new Uint8Array(this.mem.buffer, p, bytes.length + 1);
    u.set(bytes); u[bytes.length] = 0;
    return p;
  }
  _pushStr(s) { return this._push(new TextEncoder().encode(s)); }

  setup() { this.ex.rg_setup(); }

  // The flat DLL image, already laid out at its preferred base.
  loadImage(imageBytes) {
    const size = this.ex.rg_image_size() >>> 0;
    if (imageBytes.length !== size) throw new Error(`rg_image.bin is ${imageBytes.length} bytes, expected ${size}`);
    new Uint8Array(this.mem.buffer, this.ex.rg_image_ptr() >>> 0, size).set(imageBytes);
  }

  // Add a file to the in-memory filesystem (the runtime keeps the copy).
  addFile(name, bytes) {
    return this.ex.rg_add_file(this._pushStr(name), this._push(bytes), bytes.length);
  }

  // Run the DLL entry point (CRT + static constructors).
  boot() {
    if (!this.ex.rg_boot()) throw new Error('engine boot failed');
  }

  init(w = SCREEN_W, h = SCREEN_H, playerMode = 0) {
    this.w = w; this.h = h;
    return this.ex.rg_init(w, h, playerMode) >>> 0;
  }

  notifyPak(name) { this.ex.rg_notify_pak(this._pushStr(name)); }

  // The game's clock in ms (monotonic; frozen while paused).
  gameTime() {
    let t;
    if (this.pausedAt !== null) t = this.pausedAt - this.clockOffset;
    else if (this.tickStart !== null) t = this.tickStart - this.clockOffset + Math.min(this.now() - this.tickStart, TICK_MAX_MS);
    else t = this.now() - this.clockOffset;
    return (this.lastTime = Math.max(this.lastTime, t));
  }

  // One engine frame; false once the game has quit.
  tick() {
    const paused = this.pausedAt !== null;
    this.tickStart = this.now();
    try {
      return !!this.ex.rg_tick();
    } finally {
      const d = this.now() - this.tickStart;
      if (!paused && d > TICK_MAX_MS) this.clockOffset += d - TICK_MAX_MS;
      this.tickStart = null;
    }
  }
  // Milliseconds the engine asked to sleep since the last call.
  sleepMs() { return this.ex.rg_sleep_ms() >>> 0; }

  // Run the ticks that are due at time `now`, honouring the engine's own
  // 30 fps schedule: it Sleep()s until its next frame, and we start the next
  // tick once that time has passed.  Returns the number of ticks run.
  pump(now = this.now(), maxTicks = 4, budgetMs = 30) {
    let n = 0;
    const start = this.now();
    while (this.alive && now >= this.nextTickAt && n < maxTicks) {
      const late = now - this.nextTickAt;
      if (n === 0 && this.nextTickAt && late > MAX_LAG_MS) this.clockOffset += late - MAX_LAG_MS;
      this.alive = this.tick();
      n++;
      const t = this.now();
      this.nextTickAt = Math.max(t, now) + this.sleepMs();
      if (t - start > budgetMs) break;
    }
    return n;
  }

  // Stop the game's clock (until the first frame, while the page is hidden) so
  // the paused time is not raced through afterwards.
  pause() { if (this.pausedAt === null) this.pausedAt = this.now(); }
  resume() {
    if (this.pausedAt === null) return;
    const gap = this.now() - this.pausedAt;
    this.clockOffset += gap;
    if (this.nextTickAt) this.nextTickAt += gap;
    this.pausedAt = null;
  }

  // Fill an RGBA byte array (w*h*4) from the engine's 0x00RRGGBB pixels.
  frameInto(rgba) {
    const n = this.w * this.h;
    const src = new Uint32Array(this.mem.buffer, this.ex.rg_backbuffer() >>> 0, n);
    const dst = new Uint32Array(rgba.buffer, rgba.byteOffset, n);
    for (let i = 0; i < n; i++) {
      const p = src[i];   // little-endian bytes B,G,R,x -> R,G,B,255
      dst[i] = 0xff000000 | ((p & 0xff) << 16) | (p & 0xff00) | ((p >>> 16) & 0xff);
    }
    return rgba;
  }
  frame() { return this.frameInto(new Uint8ClampedArray(this.w * this.h * 4)); }

  isWait() { return !!this.ex.rg_is_wait(); }
  selectKey() { return this.ex.rg_select_key() >>> 0; }
  backKey() { return this.ex.rg_back_key() >>> 0; }

  // AWT-style key events: (when, keyCode, keyChar, keyLocation).
  keyDown(when, code, ch = 0xffff, loc = 1) { this.ex.rg_key_down(when >>> 0, code, ch, loc); }
  keyUp(when, code, ch = 0xffff, loc = 1) { this.ex.rg_key_up(when >>> 0, code, ch, loc); }
  keyTyped(when, ch) { this.ex.rg_key_typed(when >>> 0, ch); }
  releaseAll() { this.ex.rg_release_all(); }

  // Pull `nbytes` of 8 kHz / 16-bit / mono PCM from the engine.
  soundFill(nbytes = SOUND_CHUNK) {
    const p = this.ex.rg_sound_fill(nbytes) >>> 0;
    return new Int16Array(this.mem.buffer.slice(p, p + nbytes));
  }

  // In-memory files, for persisting what the game writes.
  files() {
    const out = [];
    const n = this.ex.rg_file_count();
    for (let i = 0; i < n; i++) {
      out.push({
        index: i,
        name: this._cstr(this.ex.rg_file_name(i) >>> 0),
        size: this.ex.rg_file_len(i) >>> 0,
        dirty: !!this.ex.rg_file_dirty(i),
        deleted: !!this.ex.rg_file_deleted(i),
      });
    }
    return out;
  }
  fileBytes(i) {
    const p = this.ex.rg_file_ptr(i) >>> 0, n = this.ex.rg_file_len(i) >>> 0;
    return new Uint8Array(this.mem.buffer.slice(p, p + n));
  }
  clearDirty(i) { this.ex.rg_file_clear_dirty(i); }
}

// ---- keyboard mapping ---------------------------------------------------
// Browser KeyboardEvent -> AWT virtual key code (what the engine expects).
// Select = Ctrl (17), Back = Shift (16), D-pad = arrows.
const AWT_KEYS = {
  ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39,
  Enter: 10, Escape: 27, Backspace: 8, Tab: 9, ' ': 32,
  Control: 17, Shift: 16, Alt: 18,
};
export function awtKey(e) {
  if (e.key in AWT_KEYS) return AWT_KEYS[e.key];
  if (e.key && e.key.length === 1) {
    const c = e.key.toUpperCase().charCodeAt(0);
    if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90)) return c;   // digits/letters share AWT codes
  }
  return e.keyCode || 0;
}
export function awtChar(e) {
  if (e.key && e.key.length === 1) return e.key.charCodeAt(0);
  if (e.key === 'Enter') return 10;
  if (e.key === 'Backspace') return 8;
  if (e.key === 'Tab') return 9;
  if (e.key === 'Escape') return 27;
  return 0xffff;  // CHAR_UNDEFINED
}
// DOM_KEY_LOCATION_* (0..3) -> AWT KEY_LOCATION_STANDARD/LEFT/RIGHT/NUMPAD (1..4)
export function awtLocation(e) { return (e.location | 0) + 1; }
