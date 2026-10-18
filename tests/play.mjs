// Scripted playthrough: intro -> main menu -> Single-Player -> the tutorial
// level's cutscene, board and dialog; writes a filmstrip to build/playthrough.png.
import path from 'path';
import { boot, runFor, press, nonBlack, writePNG, strip, ROOT, AWT } from './common.mjs';

const rg = await boot();
const sel = rg.selectKey() || AWT.CTRL;
const shots = [];
const snap = (label) => { const f = rg.frame(); shots.push(f); console.log(label.padEnd(24), 'nonblack', nonBlack(f)); };

runFor(rg, 17000); snap('intro');
press(rg, sel); runFor(rg, 2000); snap('main menu');
press(rg, sel); runFor(rg, 4000); snap('single-player menu');
for (let i = 0; i < 4; i++) { press(rg, sel); runFor(rg, 4000); snap(`tutorial ${i + 1}`); }
runFor(rg, 7000); snap('board');
press(rg, AWT.RIGHT); press(rg, AWT.DOWN); press(rg, sel); runFor(rg, 4000); snap('move 1');
press(rg, AWT.LEFT); press(rg, sel); runFor(rg, 4000); snap('move 2');

const s = strip(shots);
const out = path.join(ROOT, 'build', 'playthrough.png');
writePNG(out, s.w, s.h, s.rgba);
console.log(`wrote ${out}: ${shots.length} frames; select=${sel} back=${rg.backKey()}; heap top 0x${rg.ex.rg_heap_top().toString(16)}`);
if (!rg.alive) process.exitCode = 1;
