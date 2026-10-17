// Shared loader for the ResetGeneration WASM port (Node and browser).
// Exposes an RG class that wraps the wasm module and the game loop.

const IMAGE_BASE = 0x10000000;

export class RG {
  constructor() {
    this.mem = null;
    this.ex = null;
    this.onLog = (s) => console.log('[rg]', s);
    this.onAbort = (s) => { throw new Error('rg abort: ' + s); };
  }

  _str(ptr, len) {
    const b = new Uint8Array(this.mem.buffer, ptr, len);
    return new TextDecoder().decode(b);
  }

  async load(wasmBytes) {
    const imports = {
      env: {
        js_log: (ptr, len) => this.onLog(this._str(ptr, len)),
        js_abort: (ptr, len) => this.onAbort(this._str(ptr, len)),
        js_now: () => (typeof performance !== 'undefined'
          ? performance.now() : Date.now()),
      },
    };
    const { instance } = await WebAssembly.instantiate(wasmBytes, imports);
    this.ex = instance.exports;
    this.mem = this.ex.memory;
  }

  // Copy a byte array into wasm memory at a freshly allocated address.
  _push(bytes) {
    const p = this.ex.rg_alloc(bytes.length + 1);
    new Uint8Array(this.mem.buffer, p, bytes.length).set(bytes);
    return p;
  }
  _pushStr(s) {
    const b = new TextEncoder().encode(s);
    const p = this.ex.rg_alloc(b.length + 1);
    const u = new Uint8Array(this.mem.buffer, p, b.length + 1);
    u.set(b); u[b.length] = 0;
    return p;
  }

  // Load the flat image blob into guest memory at its base.
  loadImage(imageBytes) {
    const dst = this.ex.rg_image_ptr() >>> 0;
    new Uint8Array(this.mem.buffer, dst, imageBytes.length).set(imageBytes);
  }

  addFile(name, bytes) {
    const np = this._pushStr(name);
    const dp = this._push(bytes);
    return this.ex.rg_add_file(np, dp, bytes.length);
  }

  setup() { this.ex.rg_setup(); }
  boot(entry) { return this.ex.rg_boot(entry >>> 0); }

  init(w, h, playerMode = 0) {
    this.w = w; this.h = h;
    return this.ex.rg_init(w, h, playerMode) >>> 0;
  }

  notifyPak(name) { this.ex.rg_notify_pak(this._pushStr(name)); }

  tick() { return this.ex.rg_tick(); }

  // Returns an ImageData-compatible RGBA buffer (w*h).
  frame() {
    const base = this.ex.rg_backbuffer() >>> 0;
    const w = this.w, h = this.h;
    const src = new Uint8Array(this.mem.buffer, base, w * h * 4);
    const out = new Uint8ClampedArray(w * h * 4);
    // guest pixels are 0x00RRGGBB (TYPE_INT_RGB), little-endian in memory.
    for (let i = 0; i < w * h; i++) {
      const b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
      out[i * 4 + 0] = r; out[i * 4 + 1] = g; out[i * 4 + 2] = b; out[i * 4 + 3] = 255;
    }
    return out;
  }

  isWait() { return this.ex.rg_is_wait(); }
  selectKey() { return this.ex.rg_select_key() >>> 0; }
  backKey() { return this.ex.rg_back_key() >>> 0; }

  keyDown(when, keycode, keychar, keyloc) { this.ex.rg_key_down(when >>> 0, keycode, keychar, keyloc); }
  keyUp(when, keycode, keychar, keyloc) { this.ex.rg_key_up(when >>> 0, keycode, keychar, keyloc); }
  keyTyped(when, keychar) { this.ex.rg_key_typed(when >>> 0, keychar); }
  mousePressed(btn) { this.ex.rg_mouse_pressed(btn); }
  releaseAll() { this.ex.rg_release_all(); }

  // Pull `n` bytes of 8kHz/16-bit/mono PCM from the engine.
  soundFill(n) {
    const p = this.ex.rg_sound_fill(n) >>> 0;
    return new Int16Array(this.mem.buffer.slice(p, p + n));
  }
  setVolume(v) { this.ex.rg_set_volume(v); }

  // Enumerate the in-memory filesystem (for persistence).
  files() {
    const out = [];
    const n = this.ex.rg_file_count();
    for (let i = 0; i < n; i++) {
      const np = this.ex.rg_file_name(i) >>> 0;
      let len = 0; while (new Uint8Array(this.mem.buffer, np + len, 1)[0]) len++;
      const name = new TextDecoder().decode(new Uint8Array(this.mem.buffer, np, len));
      out.push({ index: i, name, size: this.ex.rg_file_len(i) >>> 0, dirty: !!this.ex.rg_file_dirty(i) });
    }
    return out;
  }
  fileBytes(i) {
    const p = this.ex.rg_file_ptr(i) >>> 0, n = this.ex.rg_file_len(i) >>> 0;
    return new Uint8Array(this.mem.buffer.slice(p, p + n));
  }
  clearDirty(i) { this.ex.rg_file_clear_dirty(i); }
}

export const RG_IMAGE_BASE = IMAGE_BASE;

// Browser KeyboardEvent -> AWT virtual key code (what the engine expects).
// Select = Ctrl (17), Back = Shift (16), D-pad = arrows.
export function awtKey(e) {
  const m = {
    ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39,
    Enter: 10, Escape: 27, Backspace: 8, Tab: 9, ' ': 32,
    Control: 17, Shift: 16, Alt: 18,
  };
  if (e.key in m) return m[e.key];
  if (e.key.length === 1) {
    const c = e.key.toUpperCase().charCodeAt(0);
    if (c >= 48 && c <= 90) return c;          // 0-9, A-Z share AWT codes
    return e.key.charCodeAt(0);
  }
  return e.keyCode || 0;
}
export function awtChar(e) {
  if (e.key && e.key.length === 1) return e.key.charCodeAt(0);
  if (e.key === 'Enter') return 10;
  if (e.key === 'Backspace') return 8;
  return 0xffff;  // CHAR_UNDEFINED
}
