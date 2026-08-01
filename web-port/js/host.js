/* ============================================================================
 * host.js — the virtual Android device.
 *
 * Everything the shimmed framework classes touch outside the VM: the display
 * (a <canvas> the game renders into directly via lockCanvas), sound output
 * (HTMLAudio with the autoplay-unlock dance), the telephony-grade sandboxed
 * filesystem (localStorage UPS + read-only APK tree), DOM dialogs for
 * AlertDialog.Builder, touch/keyboard event delivery, and the
 * licensing-service stub that answers checkLicense locally (the Play Store
 * does not exist in this runtime; see README for the exact protocol niceties).
 * ========================================================================== */
'use strict';

class AndroidHost {
  constructor(vm, opts) {
    this.vm = vm;
    opts = opts || {};
    this.appInfo = opts.appInfo || { package: 'net.kairosoft.android.frontier_en', versionCode: 2, versionName: '1.0.1', mainActivity: 'net.kairosoft.android.frontier_en.Main' };
    this.resources = opts.resources || { raw: {}, string: {}, values: {} };

    /* ---------- display geometry ----------
     * The engine renders 1:1 pixels: on a 480x320 surface (this title's native
     * HVGA resolution) its sprite offsets and canvas-text coordinates line up
     * exactly as on the original device. Larger sizes work too via ?w=&h= but
     * letterbox the 480-wide content. */
    const q = new URLSearchParams(location.search);
    this.displayWidth = parseInt(q.get('w') || opts.width || 480, 10);
    this.displayHeight = parseInt(q.get('h') || opts.height || 320, 10);
    this.density = parseFloat(q.get('density') || opts.density || 1.0);
    this.orientation = this.displayWidth >= this.displayHeight ? 2 : 1; // landscape default
    this.requestedOrientation = 0;

    this.screenEl = opts.canvasElement;
    this.screenEl.width = this.displayWidth;
    this.screenEl.height = this.displayHeight;
    this.screenCtx = this.screenEl.getContext('2d');
    this.screenCtx.imageSmoothingEnabled = true;
    this.frameCount = 0;

    /* ---------- apk content ---------- */
    this.apkTree = new Map();           // path -> Uint8Array
    /* ---------- virtual filesystem (user data) ---------- */
    this.vfs = new Map();               // path -> Uint8Array
    this.vfsDirty = false;

    /* ---------- media ---------- */
    this.audioUnlocked = false;
    this._players = new Set();
    this._unlockPending = [];

    /* ---------- surfaces ---------- */
    this._surfaceView = null;
    this._holdLock = null;

    /* ---------- dialogs/toast ---------- */
    this._dialogEl = null;
    this._dialogObj = null;

    /* ---------- loopers ---------- */
    this._mainLooper = null;

    /* ---------- system services ---------- */
    this._services = new Map();

    /* ---------- misc ---------- */
    this._activity = null;
    this._measureCanvas = null;

    vm.host = this;
  }

  /* ================================================================== */
  /* VM-field convenience                                                */
  /* ================================================================== */
  findField(obj, name) {
    const arr = obj.c.ifields;
    for (let i = arr.length - 1; i >= 0; i--) if (arr[i].name === name) return arr[i];
    return null;
  }
  getField(obj, name) {
    const f = this.findField(obj, name);
    return f ? obj.f[f.slot] : null;
  }
  setField(obj, name, desc, v) {
    const arr = obj.c.ifields;
    for (let i = arr.length - 1; i >= 0; i--) {
      if (arr[i].name === name && (!desc || arr[i].desc === desc)) { obj.f[arr[i].slot] = v; return; }
    }
    throw new Error('[host] no field ' + name + ' ' + (desc || '') + ' on ' + obj.c.desc);
  }
  rectIvals(r) { return [this.getField(r, 'left') | 0, this.getField(r, 'top') | 0, this.getField(r, 'right') | 0, this.getField(r, 'bottom') | 0]; }
  rectISet(r, l, t, rr, b) {
    this.setField(r, 'left', 'I', l | 0); this.setField(r, 'top', 'I', t | 0);
    this.setField(r, 'right', 'I', rr | 0); this.setField(r, 'bottom', 'I', b | 0);
  }
  rectFVals(r) { return [this.getField(r, 'left'), this.getField(r, 'top'), this.getField(r, 'right'), this.getField(r, 'bottom')]; }
  rectFSet(r, l, t, rr, b) {
    this.setField(r, 'left', 'F', Math.fround(l)); this.setField(r, 'top', 'F', Math.fround(t));
    this.setField(r, 'right', 'F', Math.fround(rr)); this.setField(r, 'bottom', 'F', Math.fround(b));
  }

  /* ================================================================== */
  /* canvases / bitmaps                                                  */
  /* ================================================================== */
  makeCanvas(w, h) {
    const el = document.createElement('canvas');
    el.width = Math.max(1, w | 0);
    el.height = Math.max(1, h | 0);
    return el;
  }
  _ctx(bmp) {
    if (!bmp._cx) {
      bmp._cx = bmp.el.getContext('2d');
    }
    return bmp._cx;
  }
  bmpFromRGBA(w, h, rgba) {
    const el = this.makeCanvas(w, h);
    const cx = el.getContext('2d');
    const id = new ImageData(w, h);
    id.data.set(rgba);
    cx.putImageData(id, 0, 0);
    const cls = this.vm.requireClass('Landroid/graphics/Bitmap;');
    const bmp = this.vm.newObject(cls);
    bmp.el = el; bmp.w = w; bmp.h = h; bmp.config = 'ARGB_8888'; bmp.recycled = false;
    return bmp;
  }
  decodeImageBytes(bytes) {
    try {
      if (PNG.isPNG(bytes)) return PNG.decode(bytes);
      return null;
    } catch (e) {
      this.vm.onError('[host] image decode failed: ' + e.message);
      throw e;
    }
  }
  bmpErase(bmp, color) {
    const cx = this._ctx(bmp);
    cx.save();
    cx.setTransform(1, 0, 0, 1, 0, 0);
    cx.globalCompositeOperation = 'source-over';
    cx.globalAlpha = 1;
    cx.fillStyle = cssColor(color);
    cx.fillRect(0, 0, bmp.w, bmp.h);
    cx.restore();
  }
  bmpGetPixel(bmp, x, y) {
    const cx = this._ctx(bmp);
    const id = cx.getImageData(x, y, 1, 1);
    const d = id.data;
    return ((d[3] << 24) | (d[0] << 16) | (d[1] << 8) | d[2]) | 0;
  }
  bmpSetPixel(bmp, x, y, c) {
    const cx = this._ctx(bmp);
    const id = cx.createImageData(1, 1);
    const d = id.data;
    d[0] = (c >>> 16) & 0xff; d[1] = (c >>> 8) & 0xff; d[2] = c & 0xff; d[3] = (c >>> 24) & 0xff;
    cx.putImageData(id, x, y);
  }
  bmpGetPixels(bmp, out, offset, stride, x, y, w, h) {
    const cx = this._ctx(bmp);
    const id = cx.getImageData(x, y, w, h);
    const d = id.data;
    for (let row = 0; row < h; row++) {
      for (let col = 0; col < w; col++) {
        const si = (row * w + col) * 4;
        out[offset + row * stride + col] = ((d[si + 3] << 24) | (d[si] << 16) | (d[si + 1] << 8) | d[si + 2]) | 0;
      }
    }
  }
  bmpSetPixels(bmp, src, offset, stride, x, y, w, h) {
    const cx = this._ctx(bmp);
    const id = cx.createImageData(w, h);
    const d = id.data;
    for (let row = 0; row < h; row++) {
      for (let col = 0; col < w; col++) {
        const c = src[offset + row * stride + col] | 0;
        const si = (row * w + col) * 4;
        d[si] = (c >>> 16) & 0xff; d[si + 1] = (c >>> 8) & 0xff; d[si + 2] = c & 0xff; d[si + 3] = (c >>> 24) & 0xff;
      }
    }
    cx.putImageData(id, x, y);
  }
  /** plain blit with optional scaling (used by createScaledBitmap/copies) */
  bmpBlit(dst, src, sx, sy, sw, sh, dx, dy, dw, dh, paint, filter) {
    const cx = this._ctx(dst);
    cx.save();
    cx.imageSmoothingEnabled = filter === undefined ? true : !!filter;
    cx.drawImage(src.el, sx, sy, sw, sh, dx, dy, dw, dh);
    cx.restore();
  }

  /* ================================================================== */
  /* Canvas API state                                                    */
  /* ================================================================== */
  canvasInit(o, bmp) {
    o.bitmap = bmp;
    o.matrix = [1, 0, 0, 0, 1, 0];
    o._clip = null;              // [l,t,r,b] in device space
    o._stack = [];
  }
  _applyState(o, cxw) {
    // applies clip + matrix of canvas `o` onto 2d ctx `cxw`
    cxw.save();
    if (o._clip) {
      cxw.beginPath();
      cxw.rect(o._clip[0], o._clip[1], o._clip[2] - o._clip[0] + 1, o._clip[3] - o._clip[1] + 1);
      cxw.clip();
    }
    const m = o.matrix;
    cxw.transform(m[0], m[3], m[1], m[4], m[2], m[5]);
  }
  canvasSave(o) {
    o._stack.push({ matrix: o.matrix.slice(), clip: o._clip ? o._clip.slice() : null });
    return o._stack.length;
  }
  canvasRestore(o) {
    const st = o._stack.pop();
    if (st) { o.matrix = st.matrix; o._clip = st.clip; }
  }
  canvasRestoreTo(o, n) {
    while (o._stack.length >= n && o._stack.length > 0) this.canvasRestore(o);
  }
  canvasTranslate(o, dx, dy) { o.matrix = this.mxMul(o.matrix, [1, 0, dx, 0, 1, dy]); }
  canvasScale(o, sx, sy) { o.matrix = this.mxMul(o.matrix, [sx, 0, 0, 0, sy, 0]); }
  canvasRotate(o, deg) {
    const r = deg * Math.PI / 180, c = Math.cos(r), s = Math.sin(r);
    o.matrix = this.mxMul(o.matrix, [c, -s, 0, s, c, 0]);
  }
  canvasConcat(o, m) { o.matrix = this.mxMul(o.matrix, m); }
  canvasSetMatrix(o, m) { o.matrix = m ? m.slice() : [1, 0, 0, 0, 1, 0]; }
  matrixCopyInto(src, dstMx) { for (let i = 0; i < 6; i++) dstMx[i] = src[i]; }
  newMatrixObj(m) {
    const o = this.vm.newObject(this.vm.requireClass('Landroid/graphics/Matrix;'));
    o.m = m.slice();
    return o;
  }
  canvasClipRect(o, l, t, r, b, replace) {
    // transform (l,t)-(r,b) by current matrix → device-space rect
    const m = o.matrix;
    const pts = [
      [m[0] * l + m[1] * t + m[2], m[3] * l + m[4] * t + m[5]],
      [m[0] * r + m[1] * b + m[2], m[3] * r + m[4] * b + m[5]],
    ];
    const x0 = Math.min(pts[0][0], pts[1][0]), y0 = Math.min(pts[0][1], pts[1][1]);
    const x1 = Math.max(pts[0][0], pts[1][0]), y1 = Math.max(pts[0][1], pts[1][1]);
    if (replace || !o._clip) o._clip = [Math.floor(x0), Math.floor(y0), Math.ceil(x1), Math.ceil(y1)];
    else {
      o._clip = [
        Math.max(o._clip[0], Math.floor(x0)), Math.max(o._clip[1], Math.floor(y0)),
        Math.min(o._clip[2], Math.ceil(x1)), Math.min(o._clip[3], Math.ceil(y1)),
      ];
    }
    if (o._clip[2] < o._clip[0] || o._clip[3] < o._clip[1]) o._clip = [0, 0, -1, -1];
    return true;
  }
  _paintDrawPrep(o, cxw, paint) {
    this._applyState(o, cxw);
    if (paint) {
      cxw.globalAlpha = (paint._alpha !== undefined ? paint._alpha : 255) / 255;
      cxw.imageSmoothingEnabled = paint._filter !== false;
      if (paint._shadow) {
        const s = paint._shadow;
        cxw.shadowColor = cssColor(s.c);
        cxw.shadowBlur = s.r;
        cxw.shadowOffsetX = s.dx;
        cxw.shadowOffsetY = s.dy;
      }
    }
  }
  canvasDrawBitmap(o, bmp, sx, sy, sw, sh, dx, dy, dw, dh, paint) {
    if (!bmp || bmp.recycled || !bmp.el) return;
    const cxw = this._ctx(o.bitmap);
    this._paintDrawPrep(o, cxw, paint);
    try {
      if (paint && paint._colorFilter && paint._colorFilter.mul !== undefined) {
        // LightingColorFilter slow path: remap through a scratch canvas
        const tmp = this.makeCanvas(sw, sh);
        const tc = tmp.getContext('2d');
        tc.drawImage(bmp.el, sx, sy, sw, sh, 0, 0, sw, sh);
        const id = tc.getImageData(0, 0, sw, sh);
        const cf = paint._colorFilter;
        const mul = cf.mul | 0, add = cf.add | 0;
        const mr = ((mul >>> 16) & 0xff) / 255, mg = ((mul >>> 8) & 0xff) / 255, mb = (mul & 0xff) / 255;
        const ar = (add >>> 16) & 0xff, ag = (add >>> 8) & 0xff, ab = add & 0xff;
        const d = id.data;
        for (let i = 0; i < d.length; i += 4) {
          d[i] = Math.min(255, d[i] * mr + ar);
          d[i + 1] = Math.min(255, d[i + 1] * mg + ag);
          d[i + 2] = Math.min(255, d[i + 2] * mb + ab);
        }
        tc.putImageData(id, 0, 0);
        cxw.drawImage(tmp, 0, 0, sw, sh, dx, dy, dw, dh);
      } else {
        cxw.drawImage(bmp.el, sx, sy, sw, sh, dx, dy, dw, dh);
      }
    } finally {
      cxw.restore();
    }
  }
  canvasDrawBitmapMatrix(o, bmp, m, paint) {
    if (!bmp || bmp.recycled || !bmp.el) return;
    const cxw = this._ctx(o.bitmap);
    this._paintDrawPrep(o, cxw, paint);
    try {
      cxw.transform(m[0], m[3], m[1], m[4], m[2], m[5]);
      cxw.drawImage(bmp.el, 0, 0);
    } finally { cxw.restore(); }
  }
  canvasDrawRect(o, l, t, r, b, paint) {
    const cxw = this._ctx(o.bitmap);
    this._paintDrawPrep(o, cxw, paint);
    try {
      const style = paint ? paint._style : 0;
      const wa = Math.max(0, r - l), ha = Math.max(0, b - t);
      cxw.fillStyle = cssColor(paint ? paint._color : 0xff000000);
      cxw.strokeStyle = cxw.fillStyle;
      cxw.lineWidth = paint ? (paint._strokeWidth || 1) : 1;
      if (style === 0 || style === 2) cxw.fillRect(l, t, wa, ha);
      if (style === 1 || style === 2) cxw.strokeRect(l, t, wa, ha);
    } finally { cxw.restore(); }
  }
  canvasDrawLine(o, x0, y0, x1, y1, paint) {
    const cxw = this._ctx(o.bitmap);
    this._paintDrawPrep(o, cxw, paint);
    try {
      cxw.strokeStyle = cssColor(paint ? paint._color : 0xff000000);
      cxw.lineWidth = paint ? (paint._strokeWidth || 1) : 1;
      cxw.beginPath();
      cxw.moveTo(x0, y0);
      cxw.lineTo(x1, y1);
      cxw.stroke();
    } finally { cxw.restore(); }
  }
  canvasDrawArc(o, l, t, r, b, startDeg, sweepDeg, useCenter, paint) {
    const cxw = this._ctx(o.bitmap);
    this._paintDrawPrep(o, cxw, paint);
    try {
      const cx = (l + r) / 2, cy = (t + b) / 2;
      const rx = Math.abs(r - l) / 2, ry = Math.abs(b - t) / 2;
      const a0 = startDeg * Math.PI / 180, a1 = (startDeg + sweepDeg) * Math.PI / 180;
      cxw.beginPath();
      cxw.ellipse(cx, cy, Math.max(rx, 0.01), Math.max(ry, 0.01), 0, a0, a1, sweepDeg < 0);
      if (useCenter) cxw.lineTo(cx, cy);
      cxw.closePath();
      const style = paint ? paint._style : 0;
      cxw.fillStyle = cssColor(paint ? paint._color : 0xff000000);
      cxw.strokeStyle = cxw.fillStyle;
      cxw.lineWidth = paint ? (paint._strokeWidth || 1) : 1;
      if (style === 0 || style === 2) cxw.fill();
      if (style === 1 || style === 2) cxw.stroke();
    } finally { cxw.restore(); }
  }
  canvasDrawCircle(o, x, y, r, paint) {
    this.canvasDrawArc(o, x - r, y - r, x + r, y + r, 0, 360, false, paint);
  }
  canvasDrawColor(o, color) {
    const cxw = this._ctx(o.bitmap);
    cxw.save();
    cxw.setTransform(1, 0, 0, 1, 0, 0);
    if (o._clip) {
      cxw.beginPath();
      cxw.rect(o._clip[0], o._clip[1], o._clip[2] - o._clip[0] + 1, o._clip[3] - o._clip[1] + 1);
      cxw.clip();
    }
    cxw.fillStyle = cssColor(color);
    cxw.fillRect(0, 0, o.bitmap.w, o.bitmap.h);
    cxw.restore();
  }
  canvasDrawText(o, text, x, y, paint) {
    const cxw = this._ctx(o.bitmap);
    this._paintDrawPrep(o, cxw, paint);
    try {
      cxw.font = this._paintFont(paint);
      cxw.fillStyle = cssColor(paint ? paint._color : 0xff000000);
      cxw.textBaseline = 'alphabetic';
      cxw.textAlign = 'left';
      const sx = paint && paint._textScaleX !== undefined ? paint._textScaleX : 1;
      if (sx !== 1) { cxw.save(); cxw.transform(sx, 0, 0, 1, 0, 0); }
      cxw.fillText(text, x, y);
      if (sx !== 1) cxw.restore();
    } finally { cxw.restore(); }
  }
  _paintFont(paint) {
    if (!paint) return '12px sans-serif';
    const tf = paint._typeface;
    const fam = tf ? tf._family : 'sans-serif';
    const style = tf ? tf._style : 0;
    const parts = [];
    if (style & 2) parts.push('italic');
    if (style & 1) parts.push('bold');
    parts.push((paint._textSize || 12) + 'px');
    parts.push(fam + ', sans-serif');
    return parts.join(' ');
  }
  paintMeasureText(paint, s) {
    const cx = this._measureCtx();
    cx.font = this._paintFont(paint);
    const sx = paint && paint._textScaleX !== undefined ? paint._textScaleX : 1;
    return cx.measureText(s).width * sx;
  }
  paintGetTextWidths(paint, s, widths, start, count) {
    const cx = this._measureCtx();
    cx.font = this._paintFont(paint);
    const sx = paint && paint._textScaleX !== undefined ? paint._textScaleX : 1;
    const n = Math.min(s.length, start + count) - start;
    for (let i = 0; i < n; i++) widths[start + i] = cx.measureText(s[i]).width * sx;
    return n;
  }
  paintAscent(paint) {
    const cx = this._measureCtx();
    cx.font = this._paintFont(paint);
    const m = cx.measureText('Hg');
    if (m.actualBoundingBoxAscent !== undefined) return -m.actualBoundingBoxAscent;
    return -((paint ? paint._textSize : 12) * 0.85);
  }
  paintDescent(paint) {
    const cx = this._measureCtx();
    cx.font = this._paintFont(paint);
    const m = cx.measureText('Hg');
    if (m.actualBoundingBoxDescent !== undefined) return m.actualBoundingBoxDescent;
    return (paint ? paint._textSize : 12) * 0.15;
  }
  paintFontMetrics(paint, fmi) {
    try {
      const a = this.paintAscent(paint), d = this.paintDescent(paint);
      this.setField(fmi, 'top', 'I', Math.ceil(a * 1.2));
      this.setField(fmi, 'ascent', 'I', Math.ceil(a));
      this.setField(fmi, 'descent', 'I', Math.ceil(d));
      this.setField(fmi, 'bottom', 'I', Math.ceil(d * 1.2));
      this.setField(fmi, 'leading', 'I', 0);
      return Math.ceil(d - a);
    } catch (e) { return 0; }
  }
  paintInit(o, src) {
    o._color = src ? src._color : 0xff000000 | 0;
    o._alpha = src ? src._alpha : 255;
    o._aa = src ? src._aa : false;
    o._filter = src ? src._filter : true;
    o._style = src ? src._style : 0;
    o._strokeWidth = src ? src._strokeWidth : 0;
    o._textSize = src ? src._textSize : 12;
    o._textScaleX = src ? src._textScaleX : 1;
    o._align = src ? src._align : 0;
    o._typeface = src ? src._typeface : null;
    o._colorFilter = src ? src._colorFilter : null;
    o._shadow = src ? src._shadow : null;
  }
  _measureCtx() {
    if (!this._measureCanvas) this._measureCanvas = this.makeCanvas(4, 4);
    return this._measureCanvas.getContext('2d');
  }
  mxMul(a, b) {
    return [
      a[0] * b[0] + a[1] * b[3],
      a[0] * b[1] + a[1] * b[4],
      a[0] * b[2] + a[1] * b[5] + a[2],
      a[3] * b[0] + a[4] * b[3],
      a[3] * b[1] + a[4] * b[4],
      a[3] * b[2] + a[4] * b[5] + a[5],
    ];
  }

  /* ================================================================== */
  /* surfaces                                                            */
  /* ================================================================== */
  surfaceViewCreated(view) {
    this._surfaceView = view;
  }
  surfaceHolderFor(view) {
    if (!view._holderObj) {
      const vm = this.vm;
      view._holderObj = vm.newObject(vm.requireClass('Landroid/view/SurfaceHolderWeb;'));
      view._holderObj._callbacks = [];
      view._holderObj._view = view;
    }
    return view._holderObj;
  }
  surfaceLockCanvas(holder, dirty) {
    const vm = this.vm;
    const canvas = vm.newObject(vm.requireClass('Landroid/graphics/Canvas;'));
    this.canvasInit(canvas, this.screenBitmap());
    const cx = this.screenCtx;
    cx.save();
    return canvas;
  }
  surfaceUnlockPost(holder, canvas) {
    this.screenCtx.restore();
    this.frameCount++;
  }
  surfaceSetFixedSize(holder, w, h) {
    // the game asks for its native resolution; adopt it when no ?w/h= override
    const q = new URLSearchParams(location.search);
    if (!q.get('w') && !q.get('h') && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
      this.setDisplaySize(w, h);
    }
  }
  setDisplaySize(w, h) {
    this.displayWidth = w | 0;
    this.displayHeight = h | 0;
    this.orientation = w >= h ? 2 : 1;
    this.screenEl.width = this.displayWidth;
    this.screenEl.height = this.displayHeight;
    if (this._screenBitmapNative) {
      this._screenBitmapNative.el = this.screenEl;
      this._screenBitmapNative.w = this.displayWidth;
      this._screenBitmapNative.h = this.displayHeight;
      this._screenBitmapNative._cx = this.screenCtx;
    }
    this.onResize && this.onResize(this.displayWidth, this.displayHeight);
  }
  screenBitmap() {
    if (!this._screenBitmapNative) {
      const vm = this.vm;
      const bmp = vm.newObject(vm.requireClass('Landroid/graphics/Bitmap;'));
      bmp.el = this.screenEl;
      bmp.w = this.displayWidth; bmp.h = this.displayHeight;
      bmp.config = 'ARGB_8888'; bmp.recycled = false;
      bmp._cx = this.screenCtx;
      this._screenBitmapNative = bmp;
    }
    return this._screenBitmapNative;
  }
  /** deliver surfaceCreated/surfaceChanged after boot */
  deliverSurfaceLifecycle() {
    const vm = this.vm;
    const holder = this._surfaceView && this._surfaceView._holderObj;
    if (!holder || !holder._callbacks.length) return false;
    for (const cb of holder._callbacks.slice()) {
      vm.runOnUi((mt) => {
        try {
          vm.call(mt, cb, 'surfaceCreated(Landroid/view/SurfaceHolder;)V', [holder]);
          vm.call(mt, cb, 'surfaceChanged(Landroid/view/SurfaceHolder;III)V', [holder, 1, this.displayWidth, this.displayHeight]);
        } catch (e) {
          vm.onError('[host] surface lifecycle error: ' + (e && e.message));
        }
      });
    }
    return true;
  }

  setContentView(view) { /* engine content view tracking */
    this._contentView = view;
    if (view && view.c && (view.c.desc === 'Landroid/view/SurfaceView;' || view.c.desc.endsWith('/SurfaceView;'))) {
      this._surfaceView = view;
    }
  }

  /* ================================================================== */
  /* media                                                               */
  /* ================================================================== */
  mediaCreate(resid) {
    const r = this.rawResource(resid);
    if (!r) {
      this.vm.onError('[media] no raw resource for id 0x' + (resid >>> 0).toString(16));
      return null;
    }
    const p = {
      resid, name: r.name,
      el: null, url: r.url,
      playing: false, volume: 1, loop: false, pos: 0,
    };
    this._players.add(p);
    return p;
  }
  _playerEl(mp) {
    const p = mp._player;
    if (!p) return null;
    if (!p.el) {
      p.el = new Audio(p.url);
      p.el.preload = 'auto';
      p.el.volume = p.volume;
      p.el.loop = p.loop;
      p.el.addEventListener('ended', () => {
        p.playing = false;
        if (mp._onCompletion) {
          const vm = this.vm;
          vm.runOnUi((mt) => {
            try { vm.call(mt, mp._onCompletion, 'onCompletion(Landroid/media/MediaPlayer;)V', [mp]); }
            catch (e) { vm.onError('[media] onCompletion error ' + (e && e.message)); }
          });
        }
      });
    }
    return p.el;
  }
  mediaStart(mp) {
    const el = this._playerEl(mp);
    if (!el) return;
    const p = mp._player;
    p.playing = true;
    el.volume = Math.max(0, Math.min(1, p.volume));
    el.loop = p.loop;
    if (!this.audioUnlocked) {
      this._unlockPending.push(() => this.mediaStart(mp));
      return;
    }
    if (el.readyState >= 2) { el.currentTime = el.currentTime || 0; }
    const pr = el.play();
    if (pr && pr.catch) pr.catch(() => { });
  }
  mediaStop(mp) {
    const p = mp._player; if (!p) return;
    p.playing = false;
    const el = p.el; if (!el) return;
    try { el.pause(); el.currentTime = 0; } catch (e) { }
  }
  mediaPause(mp) {
    const p = mp._player; if (!p) return;
    p.playing = false;
    const el = p.el; if (!el) return;
    try { el.pause(); } catch (e) { }
  }
  mediaRelease(mp) {
    this.mediaStop(mp);
    if (mp._player) { this._players.delete(mp._player); mp._player = null; }
  }
  mediaReset(mp) { this.mediaStop(mp); }
  mediaSeek(mp, ms) {
    const el = this._playerEl(mp); if (!el) return;
    try { el.currentTime = ms / 1000; } catch (e) { }
  }
  mediaSetVolume(mp, v) {
    const p = mp._player; if (!p) return;
    p.volume = Math.max(0, Math.min(1, v));
    if (p.el) p.el.volume = p.volume;
  }
  mediaSetLooping(mp, l) {
    const p = mp._player; if (!p) return;
    p.loop = l;
    if (p.el) p.el.loop = l;
  }
  mediaIsPlaying(mp) {
    return !!(mp._player && mp._player.playing && mp._player.el && !mp._player.el.paused);
  }
  mediaGetPosition(mp) {
    const el = mp._player && mp._player.el;
    return el ? el.currentTime * 1000 : 0;
  }
  mediaGetDuration(mp) {
    const el = mp._player && mp._player.el;
    return el && isFinite(el.duration) ? el.duration * 1000 : 0;
  }
  unlockAudio() {
    if (this.audioUnlocked) return;
    this.audioUnlocked = true;
    const pend = this._unlockPending.splice(0);
    for (const f of pend) { try { f(); } catch (e) { } }
  }
  soundPoolPlay(sp, id, lv, loop) {
    const s = sp._sounds.get(id);
    if (!s) return 0;
    let url = null;
    if (s._resid !== undefined) {
      const r = this.rawResource(s._resid);
      url = r ? r.url : null;
    } else if (s._bytes) {
      url = URL.createObjectURL(new Blob([s._bytes]));
    }
    if (!url) return 0;
    const el = new Audio(url);
    el.volume = Math.max(0, Math.min(1, lv));
    el.loop = loop !== 0;
    if (this.audioUnlocked) { const pr = el.play(); if (pr && pr.catch) pr.catch(() => { }); }
    else this._unlockPending.push(() => { const pr = el.play(); if (pr && pr.catch) pr.catch(() => { }); });
    return id;
  }

  /* ================================================================== */
  /* assets & resources                                                  */
  /* ================================================================== */
  loadApkTree(listing) {
    // listing: [[path, size], ...] with entries fetched by boot loader
    // boot.js fills this.apkTree directly.
  }
  assetBytes(name) {
    const key = 'assets/' + name;
    return this.apkTree.get(key) || null;
  }
  assetList(path) {
    const pref = 'assets/' + (path ? path.replace(/\/+$/, '') + '/' : '');
    const out = new Set();
    for (const key of this.apkTree.keys()) {
      if (key.startsWith(pref)) {
        const rest = key.slice(pref.length);
        out.add(rest.split('/')[0]);
      }
    }
    return Array.from(out);
  }
  hostReadResource(name) {
    let n = name || '';
    if (n.startsWith('/')) n = n.slice(1);
    return this.apkTree.get(n) || this.apkTree.get('assets/' + n) || this.apkTree.get('res/raw/' + n) || null;
  }
  rawResource(resid) {
    if (!this._rawTable) {
      this._rawTable = new Map();
      const raw = this.resources.raw || {};
      for (const hexId in raw) {
        const id = parseInt(hexId, 16);
        const name = raw[hexId];
        // files: res/raw/<name>.<ext> — find in apk tree
        let found = null, ext = '';
        for (const key of this.apkTree.keys()) {
          if (key.startsWith('res/raw/' + name + '.')) { found = key; ext = key.slice(key.lastIndexOf('.') + 1); break; }
        }
        const bytes = found ? this.apkTree.get(found) : null;
        this._rawTable.set(id, { id, name, bytes, path: found, url: found ? 'game/files/' + found : null, ext });
      }
    }
    return this._rawTable.get(resid) || null;
  }
  getIdentifier(name, defType, defPkg) {
    const tbls = { raw: this.resources.raw || {}, string: this.resources.string || {}, layout: this.resources.layout || {}, drawable: this.resources.drawable || {} };
    const tryTypes = defType ? [defType] : Object.keys(tbls);
    for (const t of tryTypes) {
      const tbl = tbls[t] || {};
      for (const hexId in tbl) if (tbl[hexId] === name) return parseInt(hexId, 16) | 0;
    }
    return 0;
  }
  getStringResource(resid) {
    const name = (this.resources.string || {})['0x' + (resid >>> 0).toString(16)];
    if (name && this.resources.values && this.resources.values[name] !== undefined) return this.resources.values[name];
    return null;
  }

  /* ================================================================== */
  /* virtual filesystem                                                  */
  /* ================================================================== */
  fsNormalize(p) { return p; }
  fsRead(p) {
    if (this.vfs.has(p)) return this.vfs.get(p);
    return null;
  }
  fsWrite(p, bytes) {
    this.vfs.set(p, bytes);
    this.vfsDirty = true;
    this.fsPersistSoon();
  }
  fsExists(p) {
    if (this.vfs.has(p)) return true;
    const pref = p.endsWith('/') ? p : p + '/';
    for (const k of this.vfs.keys()) if (k.startsWith(pref)) return true;
    if (p === '/sdcard' || p === '/data' || p === '/' || p.startsWith('/data/data')) return true;
    return false;
  }
  fsIsDir(p) {
    const pref = p.endsWith('/') ? p : p + '/';
    for (const k of this.vfs.keys()) if (k.startsWith(pref)) return true;
    return p === '/' || p === '/sdcard' || p === '/data' || p.startsWith('/data/data');
  }
  fsMkdirs(p) { /* implicit dirs: noop */ }
  fsSize(p) { const b = this.vfs.get(p); return b ? b.length : 0; }
  fsDelete(p) { this.vfs.delete(p); this.vfsDirty = true; this.fsPersistSoon(); }
  fsList(p) {
    const pref = p.endsWith('/') ? p : p + '/';
    const out = new Set();
    for (const k of this.vfs.keys()) {
      if (k.startsWith(pref)) out.add(k.slice(pref.length).split('/')[0]);
    }
    return Array.from(out);
  }
  fsLoad() {
    try {
      const raw = localStorage.getItem('eas.vfs');
      if (!raw) return;
      const obj = JSON.parse(raw);
      for (const k in obj) {
        const bin = atob(obj[k]);
        const bytes = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
        this.vfs.set(k, bytes);
      }
    } catch (e) { this.vm.onError('[vfs] load failed: ' + e.message); }
  }
  fsPersistSoon() {
    if (this._persistTimer) return;
    this._persistTimer = setTimeout(() => {
      this._persistTimer = null;
      this.fsPersist();
    }, 400);
  }
  fsPersist() {
    if (!this.vfsDirty) return;
    try {
      const obj = {};
      for (const [k, v] of this.vfs) {
        let s = '';
        for (let i = 0; i < v.length; i += 8192) s += String.fromCharCode.apply(null, v.subarray(i, i + 8192));
        obj[k] = btoa(s);
      }
      localStorage.setItem('eas.vfs', JSON.stringify(obj));
      this.vfsDirty = false;
    } catch (e) { this.vm.onError('[vfs] persist failed: ' + e.message); }
  }

  /* ================================================================== */
  /* loopers                                                             */
  /* ================================================================== */
  mainLooperObj() {
    if (!this._mainLooper) {
      const vm = this.vm;
      this._mainLooper = vm.newObject(vm.requireClass('Landroid/os/Looper;'));
      this._mainLooper._msgs = [];
      this._mainLooper._thread = null;   // main thread
    }
    return this._mainLooper;
  }
  newLooperObj(thr) {
    const vm = this.vm;
    const looper = vm.newObject(vm.requireClass('Landroid/os/Looper;'));
    looper._msgs = [];
    looper._thread = thr || null;
    this.trackLooper(looper);
    return looper;
  }
  looperPost(looper, msg) {
    if (!looper) looper = this.mainLooperObj();
    looper._msgs.push(msg);
    if (looper._vthread) looper._vthread.blockedUntil = 0;
  }
  looperRemove(looper, r) {
    if (!looper) return;
    looper._msgs = looper._msgs.filter((m2) => m2.r !== r);
  }
  looperRemoveMsgs(looper, h, what) {
    if (!looper) return;
    looper._msgs = looper._msgs.filter((m2) => !(m2.msg && m2.h === h && (what === undefined || m2.msg._what === what)));
  }
  pumpLoopers() {
    const vm = this.vm;
    // deliver due messages for all live loopers. Called once per rAF.
    const deliver = (looper) => {
      if (!looper || !looper._msgs || !looper._msgs.length) return;
      const now = vm.now();
      const due = [];
      looper._msgs = looper._msgs.filter((m2) => {
        if (m2.time <= now) { due.push(m2); return false; }
        return true;
      });
      for (const m2 of due) {
        const thr = looper._vthread || vm.mainThread;
        try {
          if (m2.msg) vm.call(thr, m2.h, 'handleMessage(Landroid/os/Message;)V', [m2.msg]);
          else vm.call(thr, m2.r, 'run()V');
        }
        catch (e) { vm.onError('[looper] message delivery threw: ' + (e && e.message)); }
      }
    };
    deliver(this._mainLooper);
    for (const looper of this._extraLoopers || []) deliver(looper);
  }
  trackLooper(looper) {
    if (!looper) return;
    if (!this._extraLoopers) this._extraLoopers = [];
    if (looper !== this._mainLooper && !this._extraLoopers.includes(looper)) this._extraLoopers.push(looper);
  }

  /* ================================================================== */
  /* dialogs / toasts / UI                                               */
  /* ================================================================== */
  dialogShow(dlg) {
    this.dialogDismissExisting();
    this._dialogObj = dlg;
    dlg._showing = true;
    const cfg = dlg._cfg || {};
    const vm = this.vm;
    const wrap = document.createElement('div');
    wrap.className = 'dlg-shadow';
    const box = document.createElement('div');
    box.className = 'dlg-box';
    if (cfg.title) {
      const h = document.createElement('div');
      h.className = 'dlg-title';
      h.textContent = this.vm._strOf(cfg.title);
      box.appendChild(h);
    }
    if (cfg.message) {
      const p = document.createElement('div');
      p.className = 'dlg-msg';
      p.textContent = this.vm._strOf(cfg.message);
      box.appendChild(p);
    }
    let viewInput = null;
    if (cfg.view) {
      // the only View used with AlertDialog in this engine is an EditText
      const ed = cfg.view;
      if (ed._editable !== undefined) {
        viewInput = document.createElement('input');
        viewInput.className = 'dlg-input';
        viewInput.value = ed._editable.js || '';
        viewInput.maxLength = 64;
        box.appendChild(viewInput);
        viewInput.addEventListener('input', () => { ed._editable.js = viewInput.value; });
      }
    }
    if (cfg.items && cfg.items.length) {
      const list = document.createElement('div');
      list.className = 'dlg-list';
      cfg.items.forEach((it, idx) => {
        const row = document.createElement('div');
        row.className = 'dlg-item';
        row.textContent = this.vm._strOf(it);
        row.onclick = () => {
          this.dialogDismiss(dlg);
          if (cfg.itemListener) {
            vm.runOnUi((mt) => {
              try { vm.call(mt, cfg.itemListener, 'onClick(Landroid/content/DialogInterface;I)V', [dlg, idx]); }
              catch (e) { vm.onError('[dlg] items onClick: ' + (e && e.message)); }
            });
          }
        };
        list.appendChild(row);
      });
      box.appendChild(list);
    }
    const btns = document.createElement('div');
    btns.className = 'dlg-btns';
    for (const b of cfg.buttons || []) {
      const btn = document.createElement('button');
      btn.className = 'dlg-btn';
      btn.textContent = b.label || 'OK';
      btn.onclick = () => {
        if (viewInput && cfg.view && cfg.view._editable) cfg.view._editable.js = viewInput.value;
        this.dialogDismiss(dlg);
        if (b.listener && b.listener !== 0) {
          vm.runOnUi((mt) => {
            try { vm.call(mt, b.listener, 'onClick(Landroid/content/DialogInterface;I)V', [dlg, b.which]); }
            catch (e) { vm.onError('[dlg] onClick: ' + (e && e.message)); }
          });
        }
      };
      btns.appendChild(btn);
    }
    box.appendChild(btns);
    wrap.appendChild(box);
    if (cfg.onCancel && (cfg.cancelable !== false)) {
      wrap.addEventListener('click', (e) => {
        if (e.target !== wrap) return;
        this.dialogDismiss(dlg);
        vm.runOnUi((mt) => {
          try { vm.call(mt, cfg.onCancel, 'onCancel(Landroid/content/DialogInterface;)V', [dlg]); }
          catch (e2) { vm.onError('[dlg] onCancel: ' + (e2 && e2.message)); }
        });
      });
    }
    document.getElementById('game-ui').appendChild(wrap);
    this._dialogEl = wrap;
    if (viewInput) setTimeout(() => viewInput.focus(), 60);
    this.onDialogVisible && this.onDialogVisible(true);
  }
  dialogDismiss(dlg) {
    if (dlg) dlg._showing = false;
    this.dialogDismissExisting();
  }
  dialogDismissExisting() {
    if (this._dialogEl) {
      this._dialogEl.remove();
      this._dialogEl = null;
    }
    if (this._dialogObj) {
      this._dialogObj._showing = false;
      this._dialogObj = null;
    }
    this.onDialogVisible && this.onDialogVisible(false);
  }
  dialogSetTitle(dlg, t) { /* title set pre-show in our flow */ }
  showToast(text, dur) {
    const t = document.createElement('div');
    t.className = 'toast';
    t.textContent = text;
    document.getElementById('game-ui').appendChild(t);
    setTimeout(() => { t.classList.add('show'); }, 10);
    setTimeout(() => { t.classList.remove('show'); setTimeout(() => t.remove(), 400); }, dur === 1 ? 3300 : 1800);
  }

  /* ================================================================== */
  /* system services, activity context                                   */
  /* ================================================================== */
  activityInit(activityObj) {
    if (!this._activity) this._activity = activityObj;
  }
  currentActivityObj() {
    if (!this._activity) {
      this._activity = this.vm.newObject(this.vm.requireClass('Landroid/app/Activity;'));
    }
    return this._activity;
  }
  getSystemService(name) {
    if (this._services.has(name)) return this._services.get(name);
    const vm = this.vm;
    let svc = null;
    switch (name) {
      case 'window':
        svc = this.windowManagerObj();
        break;
      case 'audio':
        svc = vm.newObject(vm.requireClass('Landroid/media/AudioManager;'));
        break;
      case 'keyguard':
        svc = vm.newObject(vm.requireClass('Landroid/app/KeyguardManager;'));
        break;
      case 'vibrator':
        svc = vm.newObject(vm.requireClass('Landroid/os/Vibrator;'));
        break;
      case 'power':
        svc = vm.newObject(vm.requireClass('Landroid/os/PowerManager;'));
        break;
      case 'layout_inflater':
        svc = vm.newObject(vm.requireClass('Landroid/view/LayoutInflater;'));
        break;
      case 'phone':
        svc = vm.newObject(vm.requireClass('Landroid/telephony/TelephonyManager;'));
        break;
      default: {
        const cls = vm.findClass('Landroid/webkit/WebView;');
        svc = null;
      }
    }
    this._services.set(name, svc);
    return svc;
  }
  windowManagerObj() {
    if (!this._wm) this._wm = this.vm.newObject(this.vm.requireClass('Landroid/view/WindowManagerImpl;'));
    return this._wm;
  }
  windowObj() {
    if (!this._win) this._win = this.vm.newObject(this.vm.requireClass('Landroid/view/Window;'));
    return this._win;
  }
  applicationObj() {
    if (!this._app) this._app = this.vm.newObject(this.vm.requireClass('Landroid/app/Application;'));
    return this._app;
  }
  assetManagerObj() {
    if (!this._am) this._am = this.vm.newObject(this.vm.requireClass('Landroid/content/res/AssetManager;'));
    return this._am;
  }
  resourcesObj() {
    if (!this._res) this._res = this.vm.newObject(this.vm.requireClass('Landroid/content/res/Resources;'));
    return this._res;
  }
  packageManagerObj() {
    if (!this._pm) this._pm = this.vm.newObject(this.vm.requireClass('Landroid/content/pm/PackageManager;'));
    return this._pm;
  }
  contentResolverObj() {
    if (!this._cr) this._cr = this.vm.newObject(this.vm.requireClass('Landroid/content/ContentResolver;'));
    return this._cr;
  }
  defaultDisplayObj() {
    if (!this._display) this._display = this.vm.newObject(this.vm.requireClass('Landroid/view/Display;'));
    return this._display;
  }
  fillDisplayMetrics(dm) {
    this.setField(dm, 'widthPixels', 'I', this.displayWidth | 0);
    this.setField(dm, 'heightPixels', 'I', this.displayHeight | 0);
    this.setField(dm, 'density', 'F', Math.fround(this.density));
    this.setField(dm, 'densityDpi', 'I', Math.round(this.density * 160) | 0);
    this.setField(dm, 'scaledDensity', 'F', Math.fround(this.density));
    this.setField(dm, 'xdpi', 'F', Math.fround(this.density * 160));
    this.setField(dm, 'ydpi', 'F', Math.fround(this.density * 160));
  }
  launchIntent() {
    if (!this._intent) {
      this._intent = this.vm.newObject(this.vm.requireClass('Landroid/content/Intent;'));
      this._intent._action = 'android.intent.action.MAIN';
      this._intent._extras = new Map();
    }
    return this._intent;
  }

  /* ================================================================== */
  /* shared preferences                                                  */
  /* ================================================================== */
  sharedPreferencesObj(name) {
    this._prefObjs = this._prefObjs || new Map();
    if (this._prefObjs.has(name)) return this._prefObjs.get(name);
    const vm = this.vm;
    const o = vm.newObject(vm.requireClass('Landroid/content/SharedPreferencesWeb;'));
    o._map = new Map();
    o._name = name;
    // load persisted
    try {
      const raw = localStorage.getItem('eas.prefs.' + name);
      if (raw) {
        const obj = JSON.parse(raw);
        for (const k in obj) o._map.set(k, obj[k]);
      }
    } catch (e) { }
    this._prefObjs.set(name, o);
    return o;
  }
  prefsEditorObj(spObj) {
    const vm = this.vm;
    const ed = vm.newObject(vm.requireClass('Landroid/content/SharedPreferencesEditorWeb;'));
    ed._dirty = new Map();
    ed._sp = spObj;
    return ed;
  }
  prefsCommit(ed) {
    const sp = ed._sp;
    if (!sp) return;
    for (const [k, op] of ed._dirty) {
      if (k === '*' && op.t === 'clear') { sp._map.clear(); continue; }
      if (op.t === 'rm') { sp._map.delete(k); continue; }
      if (op.t === 'J') { sp._map.set(k, op.v); continue; }   // long — store as bigint? persist as string
      sp._map.set(k, op.v);
    }
    ed._dirty.clear();
    try {
      const obj = {};
      for (const [k, v] of sp._map) obj[k] = (typeof v === 'bigint') ? Number(v) : v;
      localStorage.setItem('eas.prefs.' + sp._name, JSON.stringify(obj));
    } catch (e) { }
  }

  /* ================================================================== */
  /* licensing binder (local Google Play Licensing stub)                 */
  /* ================================================================== */
  bindService(intent, conn, flags) {
    const action = intent && intent._action ? intent._action : '';
    if (action.includes('licensing') || action.includes('ILicensingService')) {
      this._licensingConn = conn;
      const vm = this.vm;
      // deliver onServiceConnected on the UI thread, like the Play Store process
      vm.runOnUi((mt) => {
        try {
          const binder = vm.newObject(vm.requireClass('Landroid/os/Binder;'));
          binder._isLicenseService = true;
          const cn = vm.newObject(vm.requireClass('Landroid/content/ComponentName;'));
          cn._pkg = 'com.android.vending'; cn._cls = 'com.android.vending.licensing.LicensingService';
          vm.call(mt, conn, 'onServiceConnected(Landroid/content/ComponentName;Landroid/os/IBinder;)V', [cn, binder]);
        } catch (e) {
          vm.onError('[licence] onServiceConnected delivery failed: ' + (e && e.message));
        }
      });
      return true;
    }
    return false;
  }
  unbindService(conn) { if (this._licensingConn === conn) this._licensingConn = null; }

  /** called by the native android.os.Binder.transact when target is our stub */
  binderTransact(binder, code, data, reply, flags) {
    if (!binder._isLicenseService) return 0;
    if (code !== 1) return 0;
    // Proxy.checkLicense wrote: interface token, long nonce, package string, strong binder(listener)
    data._pos = 0;
    data._pos++;                                    // interface token cell
    const nonceCell = data._cells[data._pos++];
    const pkgCell = data._cells[data._pos++];       // package name string
    const listenerCell = data._cells[data._pos++];
    const listener = listenerCell ? listenerCell[1] : null;
    if (!listener) return 0;
    const nonce = nonceCell ? nonceCell[1] : 0n;
    const vm = this.vm;
    const pkg = this.appInfo.package;
    const vc = this.appInfo.versionCode | 0;
    // LVL response (old v1 protocol, LicenseValidator in dex requires >= 6 '|' fields):
    //   responseCode | nonce | packageName | versionCode | userId | timestamp
    const signedData = '0|' + nonce.toString() + '|' + pkg + '|' + vc + '|webport-user|' + Math.floor(this.vm.now()).toString();
    vm.runOnUi((mt) => {
      try {
        // obfuscated ILicenseResultListener.verifyLicense -> l.a(I,Ljava/lang/String;Ljava/lang/String;)V
        vm.call(mt, listener, 'a(ILjava/lang/String;Ljava/lang/String;)V',
          [0, vm.newString(signedData), vm.newString('WEBPORT')]);
      } catch (e) {
        vm.onError('[licence] verifyLicense delivery failed: ' + (e && e.message));
      }
    });
    return 1;
  }

  /* ================================================================== */
  /* input                                                               */
  /* ================================================================== */
  makeMotionEvent(action, x, y, extra) {
    const vm = this.vm;
    const ev = vm.newObject(vm.requireClass('Landroid/view/MotionEvent;'));
    ev._action = action | 0;
    ev._x = x; ev._y = y;
    if (extra) {
      ev._xs = extra.xs; ev._ys = extra.ys; ev._ids = extra.ids;
    }
    ev._downTime = vm.now(); ev._eventTime = vm.now();
    return ev;
  }
  makeKeyEvent(action, keyCode, repeat) {
    const vm = this.vm;
    const ev = vm.newObject(vm.requireClass('Landroid/view/KeyEvent;'));
    ev._action = action | 0;
    ev._keyCode = keyCode | 0;
    ev._repeat = repeat | 0;
    return ev;
  }

  onFinish() {
    this.onGameFinish && this.onGameFinish();
  }
  onExit(code) {
    this.onFinish();
  }
}

function cssColor(c) {
  c = c | 0;
  const a = ((c >>> 24) & 0xff) / 255;
  return 'rgba(' + ((c >>> 16) & 0xff) + ',' + ((c >>> 8) & 0xff) + ',' + (c & 0xff) + ',' + a.toFixed(3) + ')';
}

if (typeof module !== 'undefined') module.exports = { AndroidHost };
