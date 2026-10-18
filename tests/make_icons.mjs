// Render the PWA icons from the game's title screen (run once; output is committed).
import path from 'path';
import { boot, runFor, press, writePNG, ROOT, AWT } from './common.mjs';

const rg = await boot();
runFor(rg, 17000);
press(rg, AWT.CTRL);           // skip the intro to the title menu
runFor(rg, 2000);
const f = rg.frame(), W = 240;
// A 240x240 square: the title logo (top 84 rows) centred on the screen's background colour.
const LOGO_H = 84, TOP = (240 - LOGO_H) >> 1;
const bg = [f[(100 * W + 236) * 4], f[(100 * W + 236) * 4 + 1], f[(100 * W + 236) * 4 + 2]];
const px = (x, y) => {
  if (y < TOP || y >= TOP + LOGO_H) return bg;
  const i = ((y - TOP) * W + x) * 4;
  return [f[i], f[i + 1], f[i + 2]];
};
const crop = { x: 0, y: 0, s: 240 };
function render(size, file) {
  const out = new Uint8ClampedArray(size * size * 4);
  const k = crop.s / size;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      // area-average the source pixels covered by this output pixel
      let r = 0, g = 0, b = 0, n = 0;
      for (let sy = Math.floor(y * k); sy < Math.max(Math.floor(y * k) + 1, Math.floor((y + 1) * k)); sy++) {
        for (let sx = Math.floor(x * k); sx < Math.max(Math.floor(x * k) + 1, Math.floor((x + 1) * k)); sx++) {
          const p = px(crop.x + sx, crop.y + sy); r += p[0]; g += p[1]; b += p[2]; n++;
        }
      }
      const i = (y * size + x) * 4;
      out[i] = r / n; out[i + 1] = g / n; out[i + 2] = b / n; out[i + 3] = 255;
    }
  }
  writePNG(path.join(ROOT, 'web', file), size, size, out);
}
render(192, 'icon-192.png');
render(512, 'icon-512.png');
console.log('icons written');
