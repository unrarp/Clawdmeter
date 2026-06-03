/**
 * Stage 2: Headless frame capture via Playwright/Chromium.
 *
 * Reads .cache/manifest.json, pauses CSS animations, steps currentTime
 * across one loop period to get deterministic frames, writes PNGs to
 * .cache/frames/<name>/f00.png … f19.png.
 *
 * Playwright resolution (in order):
 *   1. require('playwright') — works when playwright is installed locally
 *      or in tools/node_modules.
 *   2. Dynamic import of the path in the PLAYWRIGHT_DIR env var.
 *   If neither resolves, exits with a clear error: install playwright
 *   (npm i playwright) or set PLAYWRIGHT_DIR to its directory.
 *
 * Chromium executable resolution (Linux only):
 *   Scans $HOME/.cache/ms-playwright/chromium-NNNN/chrome-linux64/chrome (newest
 *   match, excludes headless_shell variants). The chrome-linux64/chrome path is
 *   Linux-specific; macOS/Windows users must modify resolveChrome() manually.
 */
import { createRequire } from 'module';
import { readFileSync, mkdirSync, readdirSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { homedir } from 'os';

const __dirname = dirname(fileURLToPath(import.meta.url));
const require    = createRequire(import.meta.url);

// --- Playwright resolution ---
async function loadPlaywright() {
  // 1. Try local require (works if installed in tools/node_modules or globally via npm)
  try {
    return require('playwright');
  } catch (_) {}

  // 2. Honor PLAYWRIGHT_DIR env var
  const pwEnv = process.env.PLAYWRIGHT_DIR;
  if (pwEnv) {
    try {
      const mod = await import(join(pwEnv, 'playwright'));
      return mod;
    } catch (_) {}
    try {
      return require(join(pwEnv, 'playwright'));
    } catch (_) {}
  }

  throw new Error(
    'Cannot locate Playwright. Install it (`npm i playwright` in tools/) or ' +
    'set the PLAYWRIGHT_DIR env var to the directory containing playwright.'
  );
}

// --- Chromium executable resolution (Linux-only) ---
// Assumes chrome-linux64/chrome path — macOS/Windows users must modify this.
function resolveChrome() {
  const base = join(homedir(), '.cache', 'ms-playwright');
  const dirs = readdirSync(base)
    .filter(d => d.startsWith('chromium-') && !d.includes('headless'))
    .sort();
  const dir = dirs.pop();
  if (!dir) throw new Error(
    `No chromium-* dir found under ${base}. Run: npx playwright install chromium`
  );
  return join(base, dir, 'chrome-linux64', 'chrome');
}

// --- main ---
const CACHE_DIR = join(__dirname, '.cache');
const man = JSON.parse(readFileSync(join(CACHE_DIR, 'manifest.json'), 'utf8'));
const N = man.frames, T = man.period_ms;

const pw = await loadPlaywright();
const { chromium } = pw.default ?? pw;

const exe = resolveChrome();
console.log(`Using Chromium: ${exe}`);

const browser = await chromium.launch({ executablePath: exe });

for (const item of man.items) {
  const outdir = join(CACHE_DIR, 'frames', item.name);
  mkdirSync(outdir, { recursive: true });
  const page = await browser.newPage({
    viewport: { width: man.capture_px, height: man.capture_px },
    deviceScaleFactor: 1,
  });
  await page.goto('file://' + item.html);
  await page.waitForTimeout(200);
  const box = page.locator('#box');
  for (let i = 0; i < N; i++) {
    const tms = (i / N) * T;
    await page.evaluate((tms) => {
      for (const a of document.getAnimations()) {
        try { a.pause(); a.currentTime = tms; } catch (e) {}
      }
    }, tms);
    await page.waitForTimeout(30);
    await box.screenshot({
      path: join(outdir, `f${String(i).padStart(2, '0')}.png`),
      omitBackground: true,
    });
  }
  await page.close();
  console.log(`captured ${item.name}: ${N} frames -> ${outdir}`);
}

await browser.close();
console.log('done');
