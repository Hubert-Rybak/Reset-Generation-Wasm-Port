// Headless test: boot the wasm, run init + a number of ticks, dump a PNG.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { RG, RG_IMAGE_BASE } from './rg.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
const ORIG = path.join(ROOT, 'original', 'ResetGeneration');

const ENTRY = 0x1014f74e;      // DLL entry (DllMainCRTStartup)
const PAKS = ['base.pak', 'fonts.pak', 'menus.pak', 'intro.pak',
              'levels.pak', 'gameplay.pak', 'gameplay2.pak',
              'story.pak', 'fight.pak'];

function savePNG(file, w, h, rgba) {
  // minimal PNG writer (no deps): build IHDR + IDAT (zlib store) + IEND
  const zlib = require('zlib');
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    rgba.copyWithin ? 0 : 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4)
      .copy(raw, y * (w * 4 + 1) + 1);
  }
  const idat = zlib.deflateSync(raw);
  const chunks = [];
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  function chunk(type, data) {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
    const t = Buffer.from(type);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([t, data])) >>> 0, 0);
    chunks.push(len, t, data, crc);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA
  chunk('IHDR', ihdr);
  chunk('IDAT', idat);
  chunk('IEND', Buffer.alloc(0));
  fs.writeFileSync(file, Buffer.concat([sig, ...chunks]));
}
function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return ~c;
}
import { createRequire } from 'module';
const require = createRequire(import.meta.url);

async function main() {
  const nticks = parseInt(process.argv[2] || '120', 10);
  const rg = new RG();
  let logs = 0;
  rg.onLog = (s) => { if (logs++ < 200) console.log('[wasm]', s); };
  rg.onAbort = (s) => { console.error('[ABORT]', s); process.exitCode = 1; throw new Error(s); };

  await rg.load(fs.readFileSync(path.join(ROOT, 'build', 'reset.wasm')));
  console.log('loaded wasm');
  rg.setup();
  rg.loadImage(fs.readFileSync(path.join(ROOT, 'runtime', 'rg_image.bin')));
  console.log('image loaded');
  for (const p of PAKS) {
    const fp = path.join(ORIG, p);
    if (fs.existsSync(fp)) rg.addFile(p, fs.readFileSync(fp));
  }
  // settings/save files if present
  for (const f of ['settings.cfg', 'single_player_game.save', 'achievements.dat']) {
    const fp = path.join(ORIG, f);
    if (fs.existsSync(fp)) rg.addFile(f, fs.readFileSync(fp));
  }
  console.log('files added');

  console.log('booting (CRT init)...');
  const ok = rg.boot(ENTRY);
  console.log('boot ok =', ok);

  console.log('init...');
  rg.init(240, 320, 0);
  for (const p of PAKS) rg.notifyPak(p);
  console.log('init done, ticking', nticks);

  let running = true;
  for (let i = 0; i < nticks && running; i++) {
    running = rg.tick();
    if (i % 30 === 0) console.log(`tick ${i} running=${running} wait=${rg.isWait()}`);
  }
  const rgba = rg.frame();
  const out = path.join(ROOT, 'build', 'frame.png');
  savePNG(out, 240, 320, rgba);
  console.log('wrote', out);
}
main().catch((e) => { console.error(e); process.exit(1); });
