// Shared helpers for the headless Node harnesses.
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import { fileURLToPath } from 'url';
import { RG, PAKS, SCREEN_W, SCREEN_H } from '../web/rg.js';

export const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
export const ORIG = path.join(ROOT, 'original', 'ResetGeneration');
export const AWT = { UP: 38, DOWN: 40, LEFT: 37, RIGHT: 39, CTRL: 17, SHIFT: 16 };

// Boot the engine with a simulated clock.  `saves` adds the save files that
// ship in original/ (a fresh browser profile starts without them).
export async function boot({ saves = false, log = false } = {}) {
  const rg = new RG();
  let clock = 0;
  rg.now = () => clock;
  rg.advance = (ms) => { clock += ms; };
  rg.onLog = log ? (s) => console.log('[wasm]', s) : () => {};
  rg.onAbort = (s) => { throw new Error('engine abort: ' + s); };
  await rg.load(fs.readFileSync(path.join(ROOT, 'build', 'reset.wasm')));
  rg.setup();
  rg.loadImage(fs.readFileSync(path.join(ROOT, 'runtime', 'rg_image.bin')));
  for (const p of PAKS) rg.addFile(p, fs.readFileSync(path.join(ORIG, p)));
  if (saves) {
    for (const f of ['settings.cfg', 'single_player_game.save', 'achievements.dat']) {
      const fp = path.join(ORIG, f);
      if (fs.existsSync(fp)) rg.addFile(f, fs.readFileSync(fp));
    }
  }
  rg.boot();
  rg.init(SCREEN_W, SCREEN_H, 0);
  for (const p of PAKS) rg.notifyPak(p);
  return rg;
}

// Advance simulated time in 60 Hz display frames, ticking the engine on its
// own schedule exactly as the browser loop does.  Returns ticks run.
export function runFor(rg, ms, frameMs = 1000 / 60) {
  let ticks = 0;
  for (let t = 0; t < ms; t += frameMs) {
    rg.advance(frameMs);
    ticks += rg.pump();
  }
  return ticks;
}

export function press(rg, code, holdMs = 150, afterMs = 150) {
  rg.keyDown(rg.now(), code);
  runFor(rg, holdMs);
  rg.keyUp(rg.now(), code);
  runFor(rg, afterMs);
}

export function nonBlack(rgba) {
  let n = 0;
  for (let i = 0; i < rgba.length; i += 4) if (rgba[i] | rgba[i + 1] | rgba[i + 2]) n++;
  return n;
}

function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return ~c >>> 0;
}

export function writePNG(file, w, h, rgba) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const chunks = [Buffer.from([137, 80, 78, 71, 13, 10, 26, 10])];
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    chunks.push(len, td, crc);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  chunk('IHDR', ihdr);
  chunk('IDAT', zlib.deflateSync(raw));
  chunk('IEND', Buffer.alloc(0));
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat(chunks));
}

// Lay frames side by side into one strip.
export function strip(frames, w = SCREEN_W, h = SCREEN_H) {
  const n = frames.length, out = new Uint8ClampedArray(w * n * h * 4);
  frames.forEach((f, s) => {
    for (let y = 0; y < h; y++) out.set(f.subarray(y * w * 4, (y + 1) * w * 4), (y * w * n + s * w) * 4);
  });
  return { w: w * n, h, rgba: out };
}
