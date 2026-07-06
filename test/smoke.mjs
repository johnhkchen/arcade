// test/smoke.mjs — headless boot test for every arcade WASM cartridge.
//
// The arcade's oracle. raylib cartridges compile fine and then crash at runtime
// (bad emscripten flag, missing WebGL, 404 asset, unhandled promise rejection) —
// none of which the compiler catches. This loads each dist/<game>/index.html in
// headless Chromium (WebGL via SwiftShader), watches the first few seconds of
// startup for ANY uncaught error / console error / failed request, and confirms a
// live, sized WebGL canvas. Exit 1 if any cartridge fails. Run: `npm test`.
import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFile, readdir, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { join, extname, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const DIST = join(ROOT, 'dist');
const PORT = 8781;
const BOOT_WAIT_MS = 4000;

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
  '.css': 'text/css', '.png': 'image/png', '.json': 'application/json',
};

function serveDist() {
  return new Promise((resolve) => {
    const server = createServer(async (req, res) => {
      try {
        let p = decodeURIComponent(req.url.split('?')[0]);
        if (p.endsWith('/')) p += 'index.html';
        const fp = join(DIST, p);
        const buf = await readFile(fp);
        res.writeHead(200, { 'content-type': MIME[extname(fp)] || 'application/octet-stream' });
        res.end(buf);
      } catch {
        res.writeHead(404); res.end('404');
      }
    });
    server.listen(PORT, () => resolve(server));
  });
}

async function findCartridges() {
  if (!existsSync(DIST)) return [];
  const out = [];
  for (const name of await readdir(DIST)) {
    const dir = join(DIST, name);
    if ((await stat(dir)).isDirectory() && existsSync(join(dir, 'index.html'))) out.push(name);
  }
  return out.sort();
}

async function bootTest(browser, name) {
  const page = await browser.newPage();
  const errors = [];
  const push = (s) => errors.push(s.split('\n')[0]);

  // Surface unhandled promise rejections (the TextDecoder crash is one of these).
  await page.addInitScript(() => {
    window.addEventListener('unhandledrejection', (e) => {
      console.error('unhandledrejection: ' + ((e.reason && e.reason.message) || e.reason));
    });
  });
  page.on('pageerror', (e) => push('pageerror: ' + e.message));
  page.on('console', (m) => { if (m.type() === 'error') push('console.error: ' + m.text()); });
  page.on('requestfailed', (r) => push('requestfailed: ' + r.url()));
  page.on('response', (r) => { if (r.status() >= 400) push(`http ${r.status()}: ${r.url()}`); });

  try {
    await page.goto(`http://localhost:${PORT}/${name}/`, { waitUntil: 'load', timeout: 15000 });
    await page.waitForTimeout(BOOT_WAIT_MS);
    const gl = await page.evaluate(() => {
      const c = document.querySelector('canvas');
      if (!c) return { canvas: false };
      const ctx = c.getContext('webgl2') || c.getContext('webgl');
      return { canvas: true, gl: !!ctx, w: c.width, h: c.height };
    });
    if (!gl.canvas) push('no <canvas> element');
    else if (!gl.gl) push('no WebGL context');
    else if (!gl.w || !gl.h) push(`canvas has zero size (${gl.w}x${gl.h})`);
  } catch (e) {
    push('load failed: ' + e.message);
  }
  await page.close();
  return [...new Set(errors)];
}

const server = await serveDist();
const browser = await chromium.launch({
  headless: true,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
});

const games = await findCartridges();
if (games.length === 0) {
  console.error('no cartridges in dist/ — run ./build.sh <game> first');
  await browser.close(); server.close(); process.exit(2);
}

let failed = 0;
for (const name of games) {
  const errs = await bootTest(browser, name);
  if (errs.length === 0) {
    console.log(`PASS  ${name}`);
  } else {
    failed++;
    console.log(`FAIL  ${name}`);
    errs.slice(0, 6).forEach((e) => console.log('        ' + e));
  }
}

await browser.close();
server.close();
console.log(`\n${games.length - failed}/${games.length} cartridges booted cleanly`);
process.exit(failed ? 1 : 0);
