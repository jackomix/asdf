/* ============================================================================
 * tools/swcanvas.js — pure-JS software-rasterizing Canvas2D substitute so the
 * headless VM test can COMPOSITE the game's real draw calls into pixels and
 * dump PNGs for visual verification. Implements the subset of the 2D API the
 * AndroidHost uses: fillRect/drawImage(3/5/9-arg)/clearRect/strokeRect,
 * save/restore, transform/setTransform/translate/scale/rotate, beginPath+
 * rect+clip, createImageData/getImageData/putImageData, measureText/fillText
 * (block glyphs), set fillStyle/globalAlpha.
 * ========================================================================== */
'use strict';

function parseColor(s) {
  if (Array.isArray(s)) return s;
  if (typeof s !== 'string') return [0, 0, 0, 255];
  s = s.trim();
  if (s[0] === '#') {
    let h = s.slice(1);
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    const r = parseInt(h.slice(0, 2), 16), g = parseInt(h.slice(2, 4), 16), b = parseInt(h.slice(4, 6), 16);
    const a = h.length >= 8 ? parseInt(h.slice(6, 8), 16) : 255;
    return [r, g, b, a];
  }
  const m = s.match(/rgba?\(([^)]+)\)/);
  if (m) {
    const parts = m[1].split(',').map((x) => parseFloat(x));
    return [parts[0] | 0, parts[1] | 0, parts[2] | 0, parts.length > 3 ? Math.round(parts[3] * 255) : 255];
  }
  const named = { black: [0, 0, 0, 255], white: [255, 255, 255, 255], red: [255, 0, 0, 255], green: [0, 128, 0, 255], blue: [0, 0, 255, 255], transparent: [0, 0, 0, 0] };
  return named[s.toLowerCase()] || [255, 0, 255, 255];
}

class SWImageData {
  constructor(w, h) {
    this.width = w; this.height = h;
    this.data = new Uint8ClampedArray(w * h * 4);
  }
}

function mxMul(a, b) {
  // DOM 2D affine: [a,b,c,d,e,f] meaning x' = a*x + c*y + e ; y' = b*x + d*y + f
  return [
    a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
    a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
    a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5],
  ];
}
function mxInv(m) {
  const det = m[0] * m[3] - m[1] * m[2];
  if (!det) return null;
  const id = 1 / det;
  return [m[3] * id, -m[1] * id, -m[2] * id, m[0] * id, (m[2] * m[5] - m[3] * m[4]) * id, (m[1] * m[4] - m[0] * m[5]) * id];
}
const mxApply = (m, x, y) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]];

class SWContext2D {
  constructor(canvas) {
    this.canvas = canvas;
    this.fillStyle = '#000000';
    this.strokeStyle = '#000000';
    this.globalAlpha = 1;
    this.font = '10px sans-serif';
    this.textAlign = 'left';
    this.textBaseline = 'alphabetic';
    this.shadowColor = 'rgba(0,0,0,0)';
    this.shadowBlur = 0;
    this.lineWidth = 1;
    this.imageSmoothingEnabled = true;
    this._m = [1, 0, 0, 1, 0, 0];
    this._clip = [0, 0, canvas.width, canvas.height];
    this._stk = [];
    this._path = [];
  }

  /* ---------------- state ---------------- */
  save() { this._stk.push({ m: this._m.slice(), clip: this._clip.slice() }); }
  restore() { const s = this._stk.pop(); if (s) { this._m = s.m; this._clip = s.clip; } }
  transform(a, b, c, d, e, f) { this._m = mxMul(this._m, [a, b, c, d, e, f]); }
  setTransform(a, b, c, d, e, f) { this._m = [a, b, c, d, e, f]; }
  translate(x, y) { this.transform(1, 0, 0, 1, x, y); }
  scale(x, y) { this.transform(x, 0, 0, y === undefined ? x : y, 0, 0); }
  rotate(r) { const c = Math.cos(r), s = Math.sin(r); this.transform(c, s, -s, c, 0, 0); }

  /* ---------------- path/clip ---------------- */
  beginPath() { this._path = []; }
  rect(x, y, w, h) { this._path = [[x, y, w, h]]; }
  moveTo() { } lineTo() { } closePath() { } arc() { } ellipse() { }
  clip() {
    if (!this._path.length) return;
    const [x, y, w, h] = this._path[0];
    // conservative: bounding box of transformed rect (axis-aligned fast case)
    const cs = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]].map(([px, py]) => mxApply(this._m, px, py));
    const minX = Math.max(this._clip[0], Math.floor(Math.min(cs[0][0], cs[1][0], cs[2][0], cs[3][0])));
    const minY = Math.max(this._clip[1], Math.floor(Math.min(cs[0][1], cs[1][1], cs[2][1], cs[3][1])));
    const maxX = Math.min(this._clip[2], Math.ceil(Math.max(cs[0][0], cs[1][0], cs[2][0], cs[3][0])));
    const maxY = Math.min(this._clip[3], Math.ceil(Math.max(cs[0][1], cs[1][1], cs[2][1], cs[3][1])));
    this._clip = [minX, minY, Math.max(minX, maxX), Math.max(minY, maxY)];
  }

  /* ---------------- pixels ---------------- */
  createImageData(w, h) { return new SWImageData(w, h); }
  getImageData(x, y, w, h) {
    const c = this.canvas, id = new SWImageData(w, h);
    for (let j = 0; j < h; j++) {
      const sy = y + j;
      if (sy < 0 || sy >= c.height) continue;
      for (let i = 0; i < w; i++) {
        const sx = x + i;
        if (sx < 0 || sx >= c.width) continue;
        const so = (sy * c.width + sx) * 4, dof = (j * w + i) * 4;
        id.data[dof] = c._px[so]; id.data[dof + 1] = c._px[so + 1]; id.data[dof + 2] = c._px[so + 2]; id.data[dof + 3] = c._px[so + 3];
      }
    }
    return id;
  }
  putImageData(id, x, y) {
    const c = this.canvas;
    for (let j = 0; j < id.height; j++) {
      const dy = y + j;
      if (dy < 0 || dy >= c.height) continue;
      for (let i = 0; i < id.width; i++) {
        const dx = x + i;
        if (dx < 0 || dx >= c.width) continue;
        const so = (j * id.width + i) * 4, dof = (dy * c.width + dx) * 4;
        c._px[dof] = id.data[so]; c._px[dof + 1] = id.data[so + 1]; c._px[dof + 2] = id.data[so + 2]; c._px[dof + 3] = id.data[so + 3];
      }
    }
  }

  clearRect(x, y, w, h) { this._fill(x, y, w, h, [0, 0, 0, 0], true); }
  fillRect(x, y, w, h) { this._fill(x, y, w, h, parseColor(this.fillStyle), false); }
  strokeRect(x, y, w, h) {
    const c = parseColor(this.strokeStyle);
    this._fill(x, y, w, 1, c, false);
    this._fill(x, y + h - 1, w, 1, c, false);
    this._fill(x, y, 1, h, c, false);
    this._fill(x + w - 1, y, 1, h, c, false);
  }

  _fill(x, y, w, h, rgba, replace) {
    const c = this.canvas, cl = this._clip, gA = this.globalAlpha;
    const cs = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]].map(([px, py]) => mxApply(this._m, px, py));
    const minX = Math.max(cl[0], Math.floor(Math.min(cs[0][0], cs[1][0], cs[2][0], cs[3][0])));
    const minY = Math.max(cl[1], Math.floor(Math.min(cs[0][1], cs[1][1], cs[2][1], cs[3][1])));
    const maxX = Math.min(cl[2], Math.ceil(Math.max(cs[0][0], cs[1][0], cs[2][0], cs[3][0])));
    const maxY = Math.min(cl[3], Math.ceil(Math.max(cs[0][1], cs[1][1], cs[2][1], cs[3][1])));
    // note: rotations get bounding-box fill (good enough for rect ops which are unrotated in this engine)
    const a = rgba[3] * gA / 255;
    for (let yy = minY; yy < maxY; yy++) {
      if (yy < 0 || yy >= c.height) continue;
      for (let xx = minX; xx < maxX; xx++) {
        if (xx < 0 || xx >= c.width) continue;
        const o = (yy * c.width + xx) * 4;
        if (replace || a >= 1) { c._px[o] = rgba[0]; c._px[o + 1] = rgba[1]; c._px[o + 2] = rgba[2]; c._px[o + 3] = replace ? 0 : rgba[3]; if (replace) continue; }
        else {
          const ia = 1 - a;
          c._px[o] = rgba[0] * a + c._px[o] * ia;
          c._px[o + 1] = rgba[1] * a + c._px[o + 1] * ia;
          c._px[o + 2] = rgba[2] * a + c._px[o + 2] * ia;
          c._px[o + 3] = rgba[3] + c._px[o + 3] * ia;
        }
      }
    }
  }

  drawImage(img, ...a) {
    let sx, sy, sw, sh, dx, dy, dw, dh;
    if (a.length === 2) { sx = 0; sy = 0; sw = img.width; sh = img.height; [dx, dy] = a; dw = sw; dh = sh; }
    else if (a.length === 4) { sx = 0; sy = 0; sw = img.width; sh = img.height; [dx, dy, dw, dh] = a; }
    else if (a.length === 8) { [sx, sy, sw, sh, dx, dy, dw, dh] = a; }
    else return;
    const src = img;   // SWCanvas
    if (!src._px || sw <= 0 || sh <= 0 || dw === 0 || dh === 0) return;
    const c = this.canvas, cl = this._clip, gA = this.globalAlpha;
    // dst rect in user space -> device via matrix; general affine sampling
    const inv = mxInv(this._m);
    if (!inv) return;
    const cs = [[dx, dy], [dx + dw, dy], [dx + dw, dy + dh], [dx, dy + dh]].map(([px, py]) => mxApply(this._m, px, py));
    const minX = Math.max(cl[0], Math.floor(Math.min(cs[0][0], cs[1][0], cs[2][0], cs[3][0])));
    const minY = Math.max(cl[1], Math.floor(Math.min(cs[0][1], cs[1][1], cs[2][1], cs[3][1])));
    const maxX = Math.min(cl[2], Math.ceil(Math.max(cs[0][0], cs[1][0], cs[2][0], cs[3][0])));
    const maxY = Math.min(cl[3], Math.ceil(Math.max(cs[0][1], cs[1][1], cs[2][1], cs[3][1])));
    if (maxX <= minX || maxY <= minY) return;
    for (let yy = minY; yy < maxY; yy++) {
      if (yy < 0 || yy >= c.height) continue;
      for (let xx = minX; xx < maxX; xx++) {
        if (xx < 0 || xx >= c.width) continue;
        // device -> user space -> texture space
        const [ux, uy] = [inv[0] * xx + inv[2] * yy + inv[4], inv[1] * xx + inv[3] * yy + inv[5]];
        const tx = sx + ((ux - dx) / dw) * sw;
        const ty = sy + ((uy - dy) / dh) * sh;
        const itx = tx | 0, ity = ty | 0;
        if (itx < sx || itx >= sx + sw || ity < sy || ity >= sy + sh || itx < 0 || ity < 0 || itx >= src.width || ity >= src.height) continue;
        const so = (ity * src.width + itx) * 4;
        const sA = src._px[so + 3];
        if (sA === 0) continue;
        const dof = (yy * c.width + xx) * 4;
        const a = (sA / 255) * gA;
        if (a >= 1) {
          c._px[dof] = src._px[so]; c._px[dof + 1] = src._px[so + 1]; c._px[dof + 2] = src._px[so + 2]; c._px[dof + 3] = 255;
        } else {
          const ia = 1 - a;
          c._px[dof] = src._px[so] * a + c._px[dof] * ia;
          c._px[dof + 1] = src._px[so + 1] * a + c._px[dof + 1] * ia;
          c._px[dof + 2] = src._px[so + 2] * a + c._px[dof + 2] * ia;
          c._px[dof + 3] = sA + c._px[dof + 3] * ia;
        }
      }
    }
  }

  /* ---------------- text ---------------- */
  measureText(t) {
    const size = parseInt((this.font || '10px').match(/(\d+)/) ? [10, (this.font.match(/(\d+)/) || [0, 10])[1]][1] : '10', 10) || 10;
    return { width: (t ? String(t).length : 0) * size * 0.6, actualBoundingBoxAscent: size * 0.8, actualBoundingBoxDescent: size * 0.2 };
  }
  fillText(t, x, y) {
    // block-glyph rendering so text presence is visible in dumps
    t = String(t);
    const size = parseInt(((String(this.font)).match(/(\d+(?:\.\d+)?)px/) || [0, 12])[1], 10) || 12;
    const cw = size * 0.6, ch = size;
    let start = x;
    const w = cw * t.length;
    if (this.textAlign === 'center') start = x - w / 2;
    else if (this.textAlign === 'right') start = x - w;
    const col = parseColor(this.fillStyle);
    for (let i = 0; i < t.length; i++) {
      const chCode = t.charCodeAt(i);
      if (chCode === 32) continue;
      // light inner notch so glyphs read as noise-text, not solid bar
      this._fillChar(start + i * cw, y - ch * 0.8, cw * 0.85, ch * 0.9, col, chCode);
    }
  }
  _fillChar(x, y, w, h, rgba, seed) {
    const c = this.canvas, cl = this._clip;
    const x0 = Math.max(cl[0], Math.round(x)), y0 = Math.max(cl[1], Math.round(y));
    const x1 = Math.min(cl[2], Math.round(x + w)), y1 = Math.min(cl[3], Math.round(y + h));
    for (let yy = y0; yy < y1; yy++) {
      if (yy < 0 || yy >= c.height) continue;
      for (let xx = x0; xx < x1; xx++) {
        if (xx < 0 || xx >= c.width) continue;
        // pseudo-glyph perimeter + scattered inner pixels
        const edge = (xx === x0 || xx === x1 - 1 || yy === y0 || yy === y1 - 1);
        const inner = ((xx * 31 + yy * 17 + seed) & 7) < 2;
        if (!edge && !inner) continue;
        const o = (yy * c.width + xx) * 4;
        const a = rgba[3] / 255;
        c._px[o] = rgba[0] * a + c._px[o] * (1 - a);
        c._px[o + 1] = rgba[1] * a + c._px[o + 1] * (1 - a);
        c._px[o + 2] = rgba[2] * a + c._px[o + 2] * (1 - a);
        c._px[o + 3] = rgba[3] + c._px[o + 3] * (1 - a);
      }
    }
  }

  fill() { } stroke() { } toDataURL() { return 'data:image/png;base64,iVBORw0KGgo='; }
}

class SWCanvas {
  constructor() {
    this._w = 300; this._h = 150;
    this._px = new Uint8ClampedArray(this._w * this._h * 4);
    this.width = this._w; this.height = this._h;
    this.style = {};
    this._ctx = null;
    this.classList = { add: () => { }, remove: () => { }, toggle: () => { } };
  }
  get width() { return this._w; }
  set width(v) { v = v | 0; if (v <= 0) v = 1; if (v !== this._w) { this._w = v; this._alloc(); this._syncStyle(); } }
  get height() { return this._h; }
  set height(v) { v = v | 0; if (v <= 0) v = 1; if (v !== this._h) { this._h = v; this._alloc(); this._syncStyle(); } }
  _alloc() { this._px = new Uint8ClampedArray(this._w * this._h * 4); if (this._ctx) this._ctx._clip = [0, 0, this._w, this._h]; }
  _syncStyle() { this.style.width = this._w + 'px'; this.style.height = this._h + 'px'; }
  getContext(kind) {
    if (kind !== '2d') return null;
    if (!this._ctx) this._ctx = new SWContext2D(this);
    return this._ctx;
  }
  addEventListener() { } removeEventListener() { } remove() { }
  appendChild() { } getBoundingClientRect() { return { left: 0, top: 0, width: 800, height: 480 }; }
  toDataURL() { return 'data:image/png;base64,iVBORw0KGgo='; }
}

/* ---------------- PNG dump (Node zlib) ---------------- */
function dumpPng(canvas, file) {
  const zlib = require('zlib');
  const fs = require('fs');
  const { width: w, height: h, _px: px } = canvas;
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0; // filter 0
    for (let x = 0; x < w; x++) {
      const so = (y * w + x) * 4, dof = y * (w * 4 + 1) + 1 + x * 4;
      raw[dof] = px[so]; raw[dof + 1] = px[so + 1]; raw[dof + 2] = px[so + 2]; raw[dof + 3] = px[so + 3];
    }
  }
  const crcTable = [];
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    crcTable[n] = c >>> 0;
  }
  const crc32 = (buf) => {
    let c = 0xffffffff;
    for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
  fs.writeFileSync(file, png);
}

if (typeof module !== 'undefined') module.exports = { SWCanvas, SWContext2D, SWImageData, dumpPng };
