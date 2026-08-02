#!/usr/bin/env node
/* =============================================================================
 * tools/inspect-frames.js -- numeric description of rendered frames, so the
 * port can be verified from a terminal (no display, no browser).
 *
 *   NODE_PATH=/tmp/w/node_modules node tools/inspect-frames.js build/frames
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');

let napi = null;
for (const p of ['@napi-rs/canvas', '/tmp/w/node_modules/@napi-rs/canvas']) {
  try { napi = require(p); break; } catch (e) { /* next */ }
}
if (!napi) { console.error('needs @napi-rs/canvas'); process.exit(2); }

const dir = process.argv[2] || 'build/frames';

(async () => {
  for (const f of fs.readdirSync(dir).sort()) {
    if (!f.endsWith('.png')) continue;
    const img = await napi.loadImage(fs.readFileSync(path.join(dir, f)));
    const cv = napi.createCanvas(img.width, img.height);
    const c = cv.getContext('2d');
    c.drawImage(img, 0, 0);
    const d = c.getImageData(0, 0, img.width, img.height).data;
    const hist = new Map();
    let nonblack = 0;
    for (let i = 0; i < d.length; i += 4) {
      const k = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
      hist.set(k, (hist.get(k) || 0) + 1);
      if (k !== 0) nonblack++;
    }
    const px = d.length / 4;
    const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4)
      .map(([k, v]) => '#' + k.toString(16).padStart(6, '0') + ':' + (100 * v / px).toFixed(1) + '%');
    console.log(f.padEnd(18), img.width + 'x' + img.height,
      'colors=' + String(hist.size).padStart(6),
      'nonblack=' + (100 * nonblack / px).toFixed(1) + '%', top.join(' '));
  }
})();
