#!/usr/bin/env node
/* =============================================================================
 * tools/ascii-frame.js -- render a PNG frame as luminance ASCII art so the
 * output of the port can be eyeballed from a terminal / CI log.
 *
 *   NODE_PATH=/tmp/w/node_modules node tools/ascii-frame.js build/frames/frame-0060.png [cols]
 * ========================================================================== */
'use strict';

const fs = require('fs');

let napi = null;
for (const p of ['@napi-rs/canvas', '/tmp/w/node_modules/@napi-rs/canvas']) {
  try { napi = require(p); break; } catch (e) { /* next */ }
}
if (!napi) { console.error('needs @napi-rs/canvas'); process.exit(2); }

const RAMP = ' .:-=+*#%@';
const file = process.argv[2];
const COLS = parseInt(process.argv[3] || '78', 10);
/* optional crop:  x,y,w,h  in source pixels */
const CROP = (process.argv[4] || '').split(',').map(Number);

(async () => {
  const img = await napi.loadImage(fs.readFileSync(file));
  const sx = CROP.length === 4 ? CROP[0] : 0;
  const sy = CROP.length === 4 ? CROP[1] : 0;
  const sw = CROP.length === 4 ? CROP[2] : img.width;
  const sh = CROP.length === 4 ? CROP[3] : img.height;
  const rows = Math.max(1, Math.round(COLS * (sh / sw) / 2.1));
  const cv = napi.createCanvas(COLS, rows);
  const c = cv.getContext('2d');
  c.drawImage(img, sx, sy, sw, sh, 0, 0, COLS, rows);
  const d = c.getImageData(0, 0, COLS, rows).data;
  let min = 255, max = 0;
  const lum = new Float64Array(COLS * rows);
  for (let i = 0, p = 0; i < d.length; i += 4, p++) {
    const l = 0.2126 * d[i] + 0.7152 * d[i + 1] + 0.0722 * d[i + 2];
    lum[p] = l;
    if (l < min) min = l;
    if (l > max) max = l;
  }
  const span = Math.max(1, max - min);
  let out = '+' + '-'.repeat(COLS) + '+\n';
  for (let y = 0; y < rows; y++) {
    let line = '|';
    for (let x = 0; x < COLS; x++) {
      const v = (lum[y * COLS + x] - min) / span;
      line += RAMP[Math.min(RAMP.length - 1, Math.floor(v * RAMP.length))];
    }
    out += line + '|\n';
  }
  out += '+' + '-'.repeat(COLS) + '+';
  console.log(file + '  ' + img.width + 'x' + img.height + '  lum ' +
              min.toFixed(0) + '..' + max.toFixed(0));
  console.log(out);
})();
