// Headless smoke test: boot, run the intro for a few seconds of simulated
// time at the game's own pace, and write build/frame.png.
import path from 'path';
import { boot, runFor, nonBlack, writePNG, ROOT } from './common.mjs';

const seconds = parseFloat(process.argv[2] || '20');
const rg = await boot({ log: true });
const t0 = Date.now();
const ticks = runFor(rg, seconds * 1000);
const frame = rg.frame();
const out = path.join(ROOT, 'build', 'frame.png');
writePNG(out, rg.w, rg.h, frame);
console.log(`${seconds}s simulated: ${ticks} ticks (${(ticks / seconds).toFixed(1)}/s) in ` +
            `${Date.now() - t0} ms wall; wait=${rg.isWait()} nonblack=${nonBlack(frame)} -> ${out}`);
if (!rg.alive || nonBlack(frame) < 1000) process.exitCode = 1;
