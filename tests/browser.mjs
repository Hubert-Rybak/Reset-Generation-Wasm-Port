// Drive the web frontend in headless Chromium: desktop and phone layouts.
//   node tests/browser.mjs                 # serves dist/ locally
//   node tests/browser.mjs https://...     # tests a deployed copy
import fs from 'fs';
import http from 'http';
import path from 'path';
import { chromium, devices } from 'playwright-core';
import { ROOT } from './common.mjs';

const OUT = path.join(ROOT, 'build');
const CHROME = process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';

function serve(dir) {
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
                  '.json': 'application/json', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
  const server = http.createServer((req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.join(dir, rel);
    if (!file.startsWith(dir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

const failures = [];
const check = (ok, what) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failures.push(what); };

async function canvasInfo(page) {
  return page.evaluate(() => {
    const c = document.querySelector('#game'), d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let nb = 0; for (let i = 0; i < d.length; i += 4) if (d[i] | d[i + 1] | d[i + 2]) nb++;
    const r = c.getBoundingClientRect();
    return { nonblack: nb, w: r.width, h: r.height, hash: [...d.filter((_, i) => i % 97 === 0)].join(',').length + ':' + d.reduce((a, b) => (a * 31 + b) >>> 0, 7) };
  });
}
const visible = (page, sel) => page.evaluate((s) => {
  const e = document.querySelector(s); if (!e) return false;
  const r = e.getBoundingClientRect(), st = getComputedStyle(e);
  return st.display !== 'none' && st.visibility !== 'hidden' && r.width > 20 && r.height > 20 &&
         r.right <= innerWidth + 1 && r.bottom <= innerHeight + 1 && r.left >= -1 && r.top >= -1;
}, sel);
const rect = (page, sel) => page.evaluate((s) => { const r = document.querySelector(s).getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; }, sel);
const overlaps = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;

async function scenario(browser, url, name, ctxOpts, mobile) {
  console.log(`[${name}]`);
  const ctx = await browser.newContext(ctxOpts);
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(url, { waitUntil: 'domcontentloaded' });

  if (mobile) {
    // Controls must be on screen as soon as the page has loaded.
    check(await visible(page, '#dpadArea'), 'D-pad visible on load');
    check(await visible(page, '.pad.sel') && await visible(page, '.pad.back'), 'SELECT/BACK visible on load');
  } else {
    check(!(await visible(page, '#dpadArea')), 'no touch controls on desktop');
  }
  await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 180000 });
  check(true, 'assets loaded, engine booted');
  await page.screenshot({ path: path.join(OUT, `pw_${name}_ready.png`) });
  if (mobile) await page.tap('#play'); else await page.click('#play');
  await page.waitForTimeout(1500);
  await page.waitForTimeout(16000);       // intro
  const info = await canvasInfo(page);
  check(info.nonblack > 20000, `game renders (nonblack=${info.nonblack}, canvas ${info.w.toFixed(0)}x${info.h.toFixed(0)})`);
  if (mobile) {
    const cv = await rect(page, '#game'), dp = await rect(page, '#dpadArea'), sel = await rect(page, '.pad.sel');
    check(!overlaps(cv, dp) && !overlaps(cv, sel), 'controls do not cover the game screen');
    check(cv.w >= 240, 'game screen at least 1x');
  }
  await page.screenshot({ path: path.join(OUT, `pw_${name}_intro.png`) });

  // Select through the title menu with the on-screen button (or Ctrl).
  const press = async (key, sel) => {
    if (mobile) {
      const r = await rect(page, sel);
      const x = r.x + r.w / 2, y = r.y + r.h / 2;
      const cdp = await ctx.newCDPSession(page);
      await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
      await page.waitForTimeout(150);
      await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
      await cdp.detach();
    } else {
      await page.keyboard.down(key); await page.waitForTimeout(150); await page.keyboard.up(key);
    }
    await page.waitForTimeout(600);
  };
  const before = (await canvasInfo(page)).hash;
  await press('Control', '.pad.sel');
  await page.waitForTimeout(1500);
  const after = (await canvasInfo(page)).hash;
  check(before !== after, 'select changes the screen');
  await page.screenshot({ path: path.join(OUT, `pw_${name}_menu.png`) });
  await press('Control', '.pad.sel'); await page.waitForTimeout(2000);
  await press('ArrowDown', '#dpadArea .d'); await page.waitForTimeout(800);
  await page.screenshot({ path: path.join(OUT, `pw_${name}_submenu.png`) });

  if (!mobile) {
    // Saves: leaving the Settings screen writes settings.cfg; it must reach
    // IndexedDB and come back after a reload.
    await press('Shift'); await page.waitForTimeout(1500);           // back to the title menu
    await press('ArrowUp'); await press('ArrowUp'); await press('Control'); await page.waitForTimeout(2000);
    await page.screenshot({ path: path.join(OUT, `pw_${name}_settings.png`) });
    await press('Shift'); await page.waitForTimeout(3500);
    const stored = await page.evaluate(() => new Promise((res) => {
      const r = indexedDB.open('resetgen', 1);
      r.onsuccess = () => { const g = r.result.transaction('saves').objectStore('saves').get('settings.cfg'); g.onsuccess = () => res(g.result ? g.result.length : -1); };
      r.onerror = () => res(-2);
    }));
    check(stored > 0, `settings.cfg persisted to IndexedDB (${stored} bytes)`);
    await page.reload({ waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 180000 });
    const restored = await page.evaluate(() => window.rg.files().find((f) => f.name === 'settings.cfg' && !f.deleted)?.size ?? -1);
    check(restored === stored, `settings.cfg restored after reload (${restored} bytes)`);
    await page.click('#play');
    await page.waitForTimeout(3000);
  }

  const rate = await page.evaluate(async () => {
    const r = window.rg; let n = 0; const orig = r.tick.bind(r);
    r.tick = () => { n++; return orig(); };
    await new Promise((res) => setTimeout(res, 3000));
    r.tick = orig; return n / 3;
  });
  check(rate > 26 && rate < 34, `engine runs at ~30 ticks/s (${rate.toFixed(1)})`);
  check(errors.length === 0, `no page errors${errors.length ? ': ' + errors.join(' | ') : ''}`);
  await ctx.close();
}

const arg = process.argv[2];
let server = null, url = arg;
if (!url) {
  server = await serve(path.join(ROOT, 'dist'));
  url = `http://127.0.0.1:${server.address().port}/`;
}
const browser = await chromium.launch({ executablePath: CHROME, args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'] });
try {
  await scenario(browser, url, 'desktop', { viewport: { width: 1280, height: 800 } }, false);
  await scenario(browser, url, 'phone', { ...devices['iPhone 13'], defaultBrowserType: undefined }, true);
  await scenario(browser, url, 'phone_landscape', { ...devices['iPhone 13 landscape'], defaultBrowserType: undefined }, true);
} finally {
  await browser.close();
  if (server) server.close();
}
console.log(failures.length ? `${failures.length} check(s) failed` : 'all checks passed');
process.exitCode = failures.length ? 1 : 0;
