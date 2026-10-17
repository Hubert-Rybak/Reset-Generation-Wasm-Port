// Drive the browser frontend in headless Chromium: load, start, play, screenshot.
import { chromium } from 'playwright-core';

const URL = process.argv[2] || 'http://localhost:8099/';
const EXE = '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';

const browser = await chromium.launch({ executablePath: EXE, args: ['--no-sandbox'] });
const page = await browser.newContext({ viewport: { width: 420, height: 680 } }).then(c => c.newPage());
page.on('console', m => { const t = m.text(); if (!t.startsWith('[rg]')) console.log('PAGE:', t); });
page.on('pageerror', e => console.log('PAGEERR:', e.message));

await page.goto(URL, { waitUntil: 'domcontentloaded' });
// wait for the Play button to enable (assets loaded)
await page.waitForFunction(() => { const b = document.querySelector('#start'); return b && !b.disabled; }, { timeout: 120000 });
console.log('assets loaded');
await page.click('#start');

async function run(ms) { await page.waitForTimeout(ms); }
async function key(code, holdMs = 120) {
  await page.evaluate((c) => window.dispatchEvent(new KeyboardEvent('keydown', { key: c.key, keyCode: c.kc })), code);
  await run(holdMs);
  await page.evaluate((c) => window.dispatchEvent(new KeyboardEvent('keyup', { key: c.key, keyCode: c.kc })), code);
  await run(80);
}
const CTRL = { key: 'Control', kc: 17 }, DOWN = { key: 'ArrowDown', kc: 40 },
      UP = { key: 'ArrowUp', kc: 38 }, SHIFT = { key: 'Shift', kc: 16 };

await run(6000);                       // intro/menu
await page.screenshot({ path: 'build/pw_menu.png' });
await key(CTRL); await run(1500);      // select single-player
await page.screenshot({ path: 'build/pw_1.png' });
await key(DOWN); await key(CTRL); await run(2500);
await page.screenshot({ path: 'build/pw_2.png' });
await run(2000);
await page.screenshot({ path: 'build/pw_3.png' });

// capture whether canvas has non-trivial content + any abort
const info = await page.evaluate(() => {
  const c = document.querySelector('#screen');
  const g = c.getContext('2d'); const d = g.getImageData(0, 0, c.width, c.height).data;
  let nb = 0; for (let i = 0; i < d.length; i += 4) if (d[i] | d[i+1] | d[i+2]) nb++;
  return { w: c.width, h: c.height, nonblack: nb, status: document.querySelector('#status')?.textContent };
});
console.log('canvas info', JSON.stringify(info));
await browser.close();
