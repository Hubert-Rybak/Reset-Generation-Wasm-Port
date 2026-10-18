// Soak test: random input for a long stretch of simulated time; the engine
// must stay alive without aborting.  node tests/soak.mjs [minutes] [seed]
import { boot, runFor, AWT } from './common.mjs';

const minutes = parseFloat(process.argv[2] || '15');
let seed = parseInt(process.argv[3] || '12345', 10);
const rand = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);
const rg = await boot();
const keys = [AWT.UP, AWT.DOWN, AWT.LEFT, AWT.RIGHT, AWT.CTRL, AWT.CTRL, AWT.SHIFT];
const t0 = Date.now();
let ticks = 0, presses = 0;
for (let s = 0; s < minutes * 60; s++) {
  if (rand() < 0.7) {
    const k = keys[Math.floor(rand() * keys.length)];
    rg.keyDown(rg.now(), k); ticks += runFor(rg, 120); rg.keyUp(rg.now(), k); presses++;
    ticks += runFor(rg, 880);
  } else ticks += runFor(rg, 1000);
  if (s % 60 === 59) console.log(`${(s + 1) / 60} min: ${ticks} ticks, ${presses} presses, heap top 0x${rg.ex.rg_heap_top().toString(16)}, wait=${rg.isWait()}`);
  if (!rg.alive) { console.log('game exited at', s, 's'); break; }
}
console.log(`soak done: ${ticks} ticks in ${((Date.now() - t0) / 1000).toFixed(0)} s wall`);
for (const f of rg.files()) if (f.dirty || f.deleted) console.log(`  wrote ${f.name}: ${f.size} bytes${f.deleted ? ' (deleted)' : ''}`);
