#!/usr/bin/env node
/* =============================================================================
 * tools/render.js -- run the recompiled game on a real rasteriser and write
 * PNG screenshots, so the port can be verified without a browser.
 *
 *   npm i @napi-rs/canvas          (dev dependency, not shipped in web/)
 *   node tools/render.js [frames] [outdir]
 *
 * Optionally drives touch input:  --tap <frame>:<x>,<y>  (repeatable)
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const { createHost, ROOT } = require('./harness');

let napi = null;
for (const p of ['@napi-rs/canvas', path.join('/tmp/w/node_modules', '@napi-rs/canvas')]) {
  try { napi = require(p); break; } catch (e) { /* try next */ }
}
if (!napi) {
  console.error('tools/render.js needs @napi-rs/canvas:  npm i @napi-rs/canvas');
  process.exit(2);
}

const argv = process.argv.slice(2);
const taps = [];
const rest = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--tap') {
    const m = /^(\d+):(-?\d+),(-?\d+)$/.exec(argv[++i] || '');
    if (m) taps.push({ f: +m[1], x: +m[2], y: +m[3] });
  } else if (argv[i] === '--key') {
    const m = /^(\d+):(\d+)$/.exec(argv[++i] || '');
    if (m) taps.push({ f: +m[1], key: +m[2] });
  } else rest.push(argv[i]);
}
const FRAMES = parseInt(rest[0] || '600', 10);
const OUT = path.resolve(rest[1] || path.join(ROOT, 'build', 'frames'));
const SHOTS = (rest[2] || '').split(',').filter(Boolean).map(Number);

fs.mkdirSync(OUT, { recursive: true });

function canvasFactory(w, h) {
  const cv = napi.createCanvas(Math.max(1, w | 0), Math.max(1, h | 0));
  cv.style = {};
  cv.addEventListener = () => {};
  cv.removeEventListener = () => {};
  cv.setPointerCapture = () => {};
  cv.appendChild = () => {};
  cv.getBoundingClientRect = () =>
    ({ left: 0, top: 0, width: cv.width, height: cv.height });
  return cv;
}

const host = createHost({
  canvasFactory,
  extend(sb) { sb.Path2D = napi.Path2D; sb.ImageData = napi.ImageData; },
});
const { sandbox } = host;
host.load();

(async () => {
  const $EAS = sandbox.$EAS;
  const $rt = sandbox.$rt;
  const $host = sandbox.$host;

  await $EAS.boot();
  console.log('booted');

  const surface = $host.surface;
  let vnow = Date.now();
  $rt.scheduler.clock = () => vnow;

  const want = SHOTS.length ? new Set(SHOTS)
    : new Set([1, 10, 30, 60, 120, 200, 300, 450, FRAMES - 1].filter((n) => n < FRAMES));

  for (let f = 0; f < FRAMES; f++) {
    vnow += 50;
    for (const t of taps) {
      if (t.f !== f) continue;
      if (t.key !== undefined) {
        $EAS.key(t.key);
      } else {
        $EAS.tap(t.x, t.y);
      }
    }
    $rt.scheduler.tick();
    if (f % 8 === 0) await new Promise((r) => setImmediate(r));
    if (want.has(f)) {
      const p = path.join(OUT, 'frame-' + String(f).padStart(4, '0') + '.png');
      fs.writeFileSync(p, surface.toBuffer('image/png'));
      console.log('wrote ' + path.relative(ROOT, p));
    }
  }
  console.log('done');
  process.exit(0);
})().catch((e) => {
  const $rt = sandbox.$rt;
  console.error((e && e.$stack) ? ($rt.jToString(e) + '\n' + e.$stack) : e);
  process.exit(1);
});
