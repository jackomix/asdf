/* ============================================================================
 * natives-android.js — android.* framework classes for the web Dalvik VM.
 *
 * This is the "framework jar" of the target device (API 4, Android 1.6):
 * the game's bytecode links against these descriptors. Each class is backed
 * by a real browser facility:
 *
 *   android.graphics.Bitmap  -> HTMLCanvasElement/2D context (ARGB premul,
 *                               same memory model as Android bitmaps)
 *   android.graphics.Canvas  -> the same 2D context (Matrix transforms map
 *                               1:1 onto ctx.transform; LightingColorFilter
 *                               via pixel remap slow-path)
 *   android.media.MediaPlayer -> HTMLAudioElement (+ autoplay unlock queue)
 *   android.view.SurfaceView  -> the on-page <canvas>; lockCanvas() returns
 *                                a Canvas over the visible framebuffer so the
 *                                game's original draw loop renders straight
 *                                to the screen without a blit pass
 *   android.os.Handler/Looper -> the VM's green-thread scheduler queues
 *   android.app.AlertDialog   -> DOM overlay (unchanged callbacks re-enter VM)
 *   android.content.res.*     -> the untouched APK asset tree
 * ========================================================================== */
'use strict';

function installAndroidNatives(vm, host) {

  /* ==================== android.graphics ==================== */

  const J = (s) => vm.newString(s);

  /* ---- Bitmap$Config / Bitmap$CompressFormat / Paint$Style / Region$Op enums */
  const mkEnum = (desc, names, extraClinit) => {
    const sfields = names.map((n) => ({ name: n, desc }));
    vm.registerNative({
      desc,
      superDesc: 'Ljava/lang/Enum;',
      sfields,
      methods: { '<init>(Ljava/lang/String;I)V': (vm2, thr, o, [nm, ord]) => { o._enumName = nm ? nm.js : ''; o._enumOrdinal = ord; } },
      clinit: (vm2, cls) => {
        cls._enumTable = {};
        names.forEach((n, i) => {
          const o = vm2.newObject(cls);
          o._enumName = n; o._enumOrdinal = i;
          cls._enumTable[n] = o;
          const f = cls.sfields.find((x) => x.name === n);
          if (f) cls.statics[f.slot] = o;
        });
        if (extraClinit) extraClinit(vm2, cls);
      },
    });
  };
  mkEnum('Landroid/graphics/Bitmap$Config;', ['ALPHA_8', 'RGB_565', 'ARGB_4444', 'ARGB_8888']);
  mkEnum('Landroid/graphics/Bitmap$CompressFormat;', ['JPEG', 'PNG', 'WEBP']);
  mkEnum('Landroid/graphics/Paint$Style;', ['FILL', 'STROKE', 'FILL_AND_STROKE']);
  mkEnum('Landroid/graphics/Paint$Align;', ['LEFT', 'CENTER', 'RIGHT']);
  mkEnum('Landroid/graphics/Region$Op;', ['DIFFERENCE', 'INTERSECT', 'UNION', 'XOR', 'REVERSE_DIFFERENCE', 'REPLACE']);
  mkEnum('Landroid/graphics/PorterDuff$Mode;', ['CLEAR', 'SRC', 'DST', 'SRC_OVER', 'DST_OVER', 'SRC_IN', 'DST_IN', 'SRC_OUT', 'DST_OUT', 'SRC_ATOP', 'DST_ATOP', 'XOR', 'DARKEN', 'LIGHTEN', 'MULTIPLY', 'SCREEN']);

  /* ---- android.graphics.Bitmap --------------------------------------- */
  const bmpFromCanvas = (canvas, cfgName) => {
    const bmp = vm.newObject(vm.requireClass('Landroid/graphics/Bitmap;'));
    bmp.el = canvas;
    bmp.w = canvas.width; bmp.h = canvas.height;
    bmp.config = cfgName || 'ARGB_8888';
    bmp.recycled = false;
    return bmp;
  };
  const newBitmap = (w, h, cfg) => {
    w = Math.max(1, w | 0); h = Math.max(1, h | 0);
    if (host.Offscreen) {
      const cv = host.makeCanvas(w, h);
      return bmpFromCanvas(cv, cfg);
    }
    const cv = host.makeCanvas(w, h);
    return bmpFromCanvas(cv, cfg);
  };

  vm.registerNative({
    desc: 'Landroid/graphics/Bitmap;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o.recycled = false; },
      'getWidth()I': (vm2, thr, o) => o.w | 0,
      'getHeight()I': (vm2, thr, o) => o.h | 0,
      'isRecycled()Z': (vm2, thr, o) => o.recycled ? 1 : 0,
      'recycle()V': (vm2, thr, o) => { o.recycled = true; o.el = null; o.w = 0; o.h = 0; },
      'recyclePixels()V': (vm2, thr, o) => { o.recycled = true; },
      'getConfig()Landroid/graphics/Bitmap$Config;': (vm2, thr, o) => {
        const cls = vm2.requireClass('Landroid/graphics/Bitmap$Config;');
        return (cls._enumTable || {})[o.config] || null;
      },
      'eraseColor(I)V': (vm2, thr, o, [color]) => {
        host.bmpErase(o, color | 0);
      },
      'getPixels([IIIIIII)V': (vm2, thr, o, [pixels, offset, stride, x, y, w, h]) => {
        host.bmpGetPixels(o, pixels.a, offset | 0, stride | 0, x | 0, y | 0, w | 0, h | 0);
      },
      'setPixels([IIIIIII)V': (vm2, thr, o, [pixels, offset, stride, x, y, w, h]) => {
        host.bmpSetPixels(o, pixels.a, offset | 0, stride | 0, x | 0, y | 0, w | 0, h | 0);
      },
      'getPixel(II)I': (vm2, thr, o, [x, y]) => host.bmpGetPixel(o, x | 0, y | 0) | 0,
      'setPixel(III)V': (vm2, thr, o, [x, y, c]) => host.bmpSetPixel(o, x | 0, y | 0, c | 0),
      'compress(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z': (vm2, thr, o, [fmt, q, stream]) => {
        if (!o.el) return 0;
        const mime = fmt && fmt._enumName === 'JPEG' ? 'image/jpeg' : 'image/png';
        const url = o.el.toDataURL(mime, Math.max(0, Math.min(100, q)) / 100);
        const b64 = url.slice(url.indexOf(',') + 1);
        const bin = atob(b64);
        const bytes = vm2.newArray('B', bin.length, thr);
        for (let i = 0; i < bin.length; i++) bytes.a[i] = (bin.charCodeAt(i) << 24) >> 24;
        vm2.call(thr, stream, 'write([B)V', [bytes]);
        return 1;
      },
      'createBitmap(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;': (vm2, thr, o, [w, h, cfg]) =>
        newBitmap(w, h, cfg && cfg._enumName || 'ARGB_8888'),
      'createBitmap([IIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;': (vm2, thr, o, [colors, w, h, cfg]) => {
        const bmp = newBitmap(w, h, cfg && cfg._enumName || 'ARGB_8888');
        host.bmpSetPixels(bmp, colors.a, 0, w, 0, 0, w, h);
        return bmp;
      },
      'createBitmap(Landroid/graphics/Bitmap;)Landroid/graphics/Bitmap;': (vm2, thr, o, [src]) => {
        const bmp = newBitmap(src.w, src.h, src.config);
        host.bmpBlit(bmp, src, 0, 0, src.w, src.h, 0, 0, src.w, src.h, null);
        return bmp;
      },
      'createBitmap(Landroid/graphics/Bitmap;IIII)Landroid/graphics/Bitmap;': (vm2, thr, o, [src, x, y, w, h]) => {
        const bmp = newBitmap(w, h, src.config);
        host.bmpBlit(bmp, src, x, y, w, h, 0, 0, w, h, null);
        return bmp;
      },
      'createScaledBitmap(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;': (vm2, thr, o, [src, w, h, filter]) => {
        const bmp = newBitmap(w, h, src.config);
        host.bmpBlit(bmp, src, 0, 0, src.w, src.h, 0, 0, w, h, null, !!filter);
        return bmp;
      },
      'copy(Landroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;': (vm2, thr, o, [cfg, mut]) => {
        const bmp = newBitmap(o.w, o.h, cfg && cfg._enumName || 'ARGB_8888');
        host.bmpBlit(bmp, o, 0, 0, o.w, o.h, 0, 0, o.w, o.h, null);
        return bmp;
      },
      'isMutable()Z': () => 1,
    },
    staticSigs: new Set([
      'createBitmap(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;',
      'createBitmap([IIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;',
      'createBitmap(Landroid/graphics/Bitmap;)Landroid/graphics/Bitmap;',
      'createBitmap(Landroid/graphics/Bitmap;IIII)Landroid/graphics/Bitmap;',
      'createScaledBitmap(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;',
    ]),
  });

  vm.registerNative({
    desc: 'Landroid/graphics/BitmapFactory;',
    methods: {
      '<init>()V': () => { },
      'decodeByteArray([BII)Landroid/graphics/Bitmap;': (vm2, thr, o, [data, off, len]) => {
        const bytes = Uint8Array.from(data.a.slice(off, off + len).map((b) => b & 0xff));
        const dec = host.decodeImageBytes(bytes);            // {w,h,rgba} via png.js
        if (!dec) throw new Error('[vm] BitmapFactory: unsupported image bytes');
        const bmp = host.bmpFromRGBA(dec.w, dec.h, dec.rgba);
        return bmp;
      },
      'decodeByteArray([B)Landroid/graphics/Bitmap;': (vm2, thr, o, [data]) => {
        const bytes = Uint8Array.from(data.a.map((b) => b & 0xff));
        const dec = host.decodeImageBytes(bytes);
        if (!dec) throw new Error('[vm] BitmapFactory: unsupported image bytes');
        const bmp = host.bmpFromRGBA(dec.w, dec.h, dec.rgba);
        return bmp;
      },
    },
    staticSigs: new Set(['decodeByteArray([BII)Landroid/graphics/Bitmap;', 'decodeByteArray([B)Landroid/graphics/Bitmap;']),
  });

  /* ---- android.graphics.Canvas --------------------------------------- */
  vm.registerNative({
    desc: 'Landroid/graphics/Canvas;',
    methods: {
      '<init>()V': (vm2, thr, o) => host.canvasInit(o, null),
      '<init>(Landroid/graphics/Bitmap;)V': (vm2, thr, o, [bmp]) => host.canvasInit(o, bmp),
      'translate(FF)V': (vm2, thr, o, [dx, dy]) => { host.canvasTranslate(o, dx, dy); },
      'scale(FF)V': (vm2, thr, o, [sx, sy]) => { host.canvasScale(o, sx, sy); },
      'scale(FFFF)V': (vm2, thr, o, [sx, sy, px, py]) => {
        host.canvasTranslate(o, px, py);
        host.canvasScale(o, sx, sy);
        host.canvasTranslate(o, -px, -py);
      },
      'rotate(F)V': (vm2, thr, o, [deg]) => { host.canvasRotate(o, deg); },
      'concat(Landroid/graphics/Matrix;)V': (vm2, thr, o, [mx]) => { host.canvasConcat(o, mx.m); },
      'getMatrix()Landroid/graphics/Matrix;': (vm2, thr, o) => host.newMatrixObj(o.matrix.slice()),
      'getMatrix(Landroid/graphics/Matrix;)V': (vm2, thr, o, [mx]) => host.matrixCopyInto(o.matrix, mx.m),
      'setMatrix(Landroid/graphics/Matrix;)V': (vm2, thr, o, [mx]) => { host.canvasSetMatrix(o, mx ? mx.m : null); },
      'save()I': (vm2, thr, o) => host.canvasSave(o),
      'restore()V': (vm2, thr, o) => host.canvasRestore(o),
      'restoreToCount(I)V': (vm2, thr, o, [n]) => host.canvasRestoreTo(o, n | 0),
      'clipRect(FFFFLandroid/graphics/Region$Op;)Z': (vm2, thr, o, [l, t, r, b, op]) =>
        host.canvasClipRect(o, l, t, r, b, op && op._enumName === 'REPLACE') ? 1 : 0,
      'clipRect(FFFF)Z': (vm2, thr, o, [l, t, r, b]) => host.canvasClipRect(o, l, t, r, b, false) ? 1 : 0,
      'clipRect(IIII)Z': (vm2, thr, o, [l, t, r, b]) => host.canvasClipRect(o, l, t, r, b, false) ? 1 : 0,
      'clipRect(Landroid/graphics/RectF;)Z': (vm2, thr, o, [rect]) =>
        host.canvasClipRect(o, host.rectFVals(rect)[0], host.rectFVals(rect)[1], host.rectFVals(rect)[2], host.rectFVals(rect)[3], false) ? 1 : 0,
      'clipRect(Landroid/graphics/Rect;)Z': (vm2, thr, o, [rect]) =>
        host.canvasClipRect(o, host.rectIvals(rect)[0], host.rectIvals(rect)[1], host.rectIvals(rect)[2], host.rectIvals(rect)[3], false) ? 1 : 0,
      'drawBitmap(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V': (vm2, thr, o, [bmp, x, y, paint]) =>
        host.canvasDrawBitmap(o, bmp, 0, 0, bmp.w, bmp.h, x, y, bmp.w, bmp.h, paint),
      'drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/Rect;Landroid/graphics/Paint;)V': (vm2, thr, o, [bmp, src, dst, paint]) => {
        const s = host.rectIvals(src), d = host.rectIvals(dst);
        host.canvasDrawBitmap(o, bmp, s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], paint);
      },
      'drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V': (vm2, thr, o, [bmp, src, dst, paint]) => {
        const s = host.rectIvals(src), d = host.rectFVals(dst);
        host.canvasDrawBitmap(o, bmp, s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], paint);
      },
      'drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Matrix;Landroid/graphics/Paint;)V': (vm2, thr, o, [bmp, mtx, paint]) =>
        host.canvasDrawBitmapMatrix(o, bmp, mtx.m, paint),
      'drawLine(FFFFLandroid/graphics/Paint;)V': (vm2, thr, o, [x0, y0, x1, y1, p]) => host.canvasDrawLine(o, x0, y0, x1, y1, p),
      'drawLines([FLandroid/graphics/Paint;)V': (vm2, thr, o, [pts, p]) => {
        for (let i = 0; i + 3 < pts.n; i += 4) host.canvasDrawLine(o, pts.a[i], pts.a[i + 1], pts.a[i + 2], pts.a[i + 3], p);
      },
      'drawRect(FFFFLandroid/graphics/Paint;)V': (vm2, thr, o, [l, t, r, b, p]) => host.canvasDrawRect(o, l, t, r, b, p),
      'drawRect(Landroid/graphics/Rect;Landroid/graphics/Paint;)V': (vm2, thr, o, [rect, p]) => {
        const v = host.rectIvals(rect); host.canvasDrawRect(o, v[0], v[1], v[2], v[3], p);
      },
      'drawRect(Landroid/graphics/RectF;Landroid/graphics/Paint;)V': (vm2, thr, o, [rect, p]) => {
        const v = host.rectFVals(rect); host.canvasDrawRect(o, v[0], v[1], v[2], v[3], p);
      },
      'drawArc(Landroid/graphics/RectF;FFZLandroid/graphics/Paint;)V': (vm2, thr, o, [oval, start, sweep, useCenter, p]) => {
        const v = host.rectFVals(oval);
        host.canvasDrawArc(o, v[0], v[1], v[2], v[3], start, sweep, useCenter !== 0, p);
      },
      'drawCircle(FFFLandroid/graphics/Paint;)V': (vm2, thr, o, [cx, cy, r, p]) => host.canvasDrawCircle(o, cx, cy, r, p),
      'drawText(Ljava/lang/String;FFLandroid/graphics/Paint;)V': (vm2, thr, o, [s, x, y, p]) => host.canvasDrawText(o, s ? s.js : '', x, y, p),
      'drawText(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V': (vm2, thr, o, [s, st, en, x, y, p]) =>
        host.canvasDrawText(o, (s ? s.js : '').substring(st | 0, en | 0), x, y, p),
      'drawColor(I)V': (vm2, thr, o, [c]) => host.canvasDrawColor(o, c | 0),
      'drawRGB(III)V': (vm2, thr, o, [r, g, b]) => host.canvasDrawColor(o, ((0xff << 24) | (r << 16) | (g << 8) | b) | 0),
      'drawARGB(IIII)V': (vm2, thr, o, [a, r, g, b]) => host.canvasDrawColor(o, ((a << 24) | (r << 16) | (g << 8) | b) | 0),
      'drawPoint(FFLandroid/graphics/Paint;)V': (vm2, thr, o, [x, y, p]) => host.canvasDrawRect(o, x, y, x + 1, y + 1, p),
      'drawOval(Landroid/graphics/RectF;Landroid/graphics/Paint;)V': (vm2, thr, o, [oval, p]) => {
        const v = host.rectFVals(oval); host.canvasDrawArc(o, v[0], v[1], v[2], v[3], 0, 360, false, p);
      },
    },
  });

  /* ---- android.graphics.Paint ---------------------------------------- */
  vm.registerNative({
    desc: 'Landroid/graphics/Paint;',
    methods: {
      '<init>()V': (vm2, thr, o) => { host.paintInit(o); },
      '<init>(I)V': (vm2, thr, o, [flags]) => { host.paintInit(o); },
      '<init>(Landroid/graphics/Paint;)V': (vm2, thr, o, [src]) => { host.paintInit(o, src); },
      'setColor(I)V': (vm2, thr, o, [c]) => { o._color = c | 0; },
      'getColor()I': (vm2, thr, o) => o._color | 0,
      'setAlpha(I)V': (vm2, thr, o, [a]) => { o._alpha = Math.max(0, Math.min(255, a | 0)); },
      'getAlpha()I': (vm2, thr, o) => o._alpha | 0,
      'setAntiAlias(Z)V': (vm2, thr, o, [v]) => { o._aa = !!v; },
      'isAntiAlias()Z': (vm2, thr, o) => o._aa ? 1 : 0,
      'setFilterBitmap(Z)V': (vm2, thr, o, [v]) => { o._filter = !!v; },
      'isFilterBitmap()Z': (vm2, thr, o) => o._filter ? 1 : 0,
      'setStyle(Landroid/graphics/Paint$Style;)V': (vm2, thr, o, [st]) => { o._style = st ? st._enumOrdinal : 0; },
      'getStyle()Landroid/graphics/Paint$Style;': (vm2, thr, o) => {
        const cls = vm2.requireClass('Landroid/graphics/Paint$Style;');
        return (cls._enumTable || {})[['FILL', 'STROKE', 'FILL_AND_STROKE'][o._style | 0]] || null;
      },
      'setStrokeWidth(F)V': (vm2, thr, o, [w]) => { o._strokeWidth = w; },
      'getStrokeWidth()F': (vm2, thr, o) => o._strokeWidth,
      'setTextSize(F)V': (vm2, thr, o, [s]) => { o._textSize = s; },
      'setLinearText(Z)V': () => { },
      'setSubpixelText(Z)V': () => { },
      'setFakeBoldText(Z)V': () => { },
      'getTextSize()F': (vm2, thr, o) => o._textSize,
      'setTextScaleX(F)V': (vm2, thr, o, [s]) => { o._textScaleX = s; },
      'setTextAlign(Landroid/graphics/Paint$Align;)V': (vm2, thr, o, [al]) => { o._align = al ? al._enumOrdinal : 0; },
      'setTypeface(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;': (vm2, thr, o, [tf]) => {
        const old = o._typeface || null;
        o._typeface = tf;
        return old;
      },
      'getTypeface()Landroid/graphics/Typeface;': (vm2, thr, o) => o._typeface || null,
      'setColorFilter(Landroid/graphics/ColorFilter;)Landroid/graphics/ColorFilter;': (vm2, thr, o, [cf]) => {
        const old = o._colorFilter || null;
        o._colorFilter = cf;
        return old;
      },
      'getColorFilter()Landroid/graphics/ColorFilter;': (vm2, thr, o) => o._colorFilter || null,
      'setShadowLayer(FFFI)V': (vm2, thr, o, [r, dx, dy, c]) => { o._shadow = { r, dx, dy, c }; },
      'measureText(Ljava/lang/String;)F': (vm2, thr, o, [s]) => host.paintMeasureText(o, s ? s.js : ''),
      'measureText(Ljava/lang/String;II)F': (vm2, thr, o, [s, st, en]) => host.paintMeasureText(o, (s ? s.js : '').substring(st | 0, en | 0)),
      'getTextWidths(Ljava/lang/String;[F)I': (vm2, thr, o, [s, widths]) => host.paintGetTextWidths(o, s ? s.js : '', widths.a, 0, (s ? s.js : '').length),
      'getTextWidths(Ljava/lang/String;II[F)I': (vm2, thr, o, [s, st, en, widths]) =>
        host.paintGetTextWidths(o, (s ? s.js : '').substring(st | 0, en | 0), widths.a, 0, (en | 0) - (st | 0)),
      'ascent()F': (vm2, thr, o) => host.paintAscent(o),
      'descent()F': (vm2, thr, o) => host.paintDescent(o),
      'getFontMetricsInt(Landroid/graphics/Paint$FontMetricsInt;)I': (vm2, thr, o, [fmi]) => host.paintFontMetrics(o, fmi),
      'reset()V': (vm2, thr, o) => { host.paintInit(o); },
    },
  });

  vm.registerNative({
    desc: 'Landroid/graphics/Paint$FontMetricsInt;',
    ifields: [
      { name: 'top', desc: 'I' }, { name: 'ascent', desc: 'I' }, { name: 'descent', desc: 'I' },
      { name: 'bottom', desc: 'I' }, { name: 'leading', desc: 'I' },
    ],
    methods: { '<init>()V': () => { } },
  });

  vm.registerNative({
    desc: 'Landroid/graphics/ColorFilter;',
    methods: { '<init>()V': () => { } },
  });
  vm.registerNative({
    desc: 'Landroid/graphics/LightingColorFilter;',
    superDesc: 'Landroid/graphics/ColorFilter;',
    methods: {
      '<init>(II)V': (vm2, thr, o, [mul, add]) => { o.mul = mul | 0; o.add = add | 0; },
    },
  });

  vm.registerNative({
    desc: 'Landroid/graphics/Typeface;',
    sfields: [],
    methods: {
      '<init>()V': (vm2, thr, o) => { o._family = 'sans-serif'; o._style = 0; },
      'create(Ljava/lang/String;I)Landroid/graphics/Typeface;': (vm2, thr, o, [name, style]) => {
        const t = vm2.newObject(vm2.requireClass('Landroid/graphics/Typeface;'));
        t._family = name ? (name.js.includes('mono') ? 'monospace' : 'sans-serif') : 'sans-serif';
        t._style = style | 0;
        return t;
      },
      'create(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;': (vm2, thr, o, [src, style]) => {
        const t = vm2.newObject(vm2.requireClass('Landroid/graphics/Typeface;'));
        t._family = src ? src._family : 'sans-serif';
        t._style = style | 0;
        return t;
      },
      'defaultFromStyle(I)Landroid/graphics/Typeface;': (vm2, thr, o, [style]) => {
        const t = vm2.newObject(vm2.requireClass('Landroid/graphics/Typeface;'));
        t._family = 'sans-serif'; t._style = style | 0;
        return t;
      },
      'getStyle()I': (vm2, thr, o) => o._style | 0,
      'isBold()Z': (vm2, thr, o) => (o._style & 1) ? 1 : 0,
      'isItalic()Z': (vm2, thr, o) => (o._style & 2) ? 1 : 0,
    },
    staticSigs: new Set(['create(Ljava/lang/String;I)Landroid/graphics/Typeface;', 'create(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;', 'defaultFromStyle(I)Landroid/graphics/Typeface;']),
    clinit: (vm2, cls) => {
      const mk = (fam, style) => { const t = vm2.newObject(cls); t._family = fam; t._style = style; return t; };
      const put = (name, v) => {
        let f = cls.sfields.find((x) => x.name === name);
        if (!f) { f = { name, desc: 'Landroid/graphics/Typeface;', slot: cls.statics.length, holder: cls, isStatic: true }; cls.sfields.push(f); cls.statics.push(v); }
        else cls.statics[f.slot] = v;
      };
      put('DEFAULT', mk('sans-serif', 0));
      put('DEFAULT_BOLD', mk('sans-serif', 1));
      put('SANS_SERIF', mk('sans-serif', 0));
      put('SERIF', mk('serif', 0));
      put('MONOSPACE', mk('monospace', 0));
    },
  });

  vm.registerNative({
    desc: 'Landroid/graphics/Color;',
    methods: {
      '<init>()V': () => { },
      'argb(IIII)I': (vm2, thr, o, [a, r, g, b]) => ((a << 24) | (r << 16) | (g << 8) | b) | 0,
      'rgb(III)I': (vm2, thr, o, [r, g, b]) => ((0xff << 24) | (r << 16) | (g << 8) | b) | 0,
      'alpha(I)I': (vm2, thr, o, [c]) => (c >>> 24) & 0xff,
      'red(I)I': (vm2, thr, o, [c]) => (c >>> 16) & 0xff,
      'green(I)I': (vm2, thr, o, [c]) => (c >>> 8) & 0xff,
      'blue(I)I': (vm2, thr, o, [c]) => c & 0xff,
      'parseColor(Ljava/lang/String;)I': (vm2, thr, o, [s]) => {
        let js = s.js;
        if (js.startsWith('#')) js = js.slice(1);
        if (js.length === 6) return ((0xff << 24) | parseInt(js, 16)) | 0;
        if (js.length === 8) return parseInt(js, 16) | 0;
        vm.throwNew(thr, 'Ljava/lang/IllegalArgumentException;', 'Unknown color');
      },
      'hsvToColor([F)I': (vm2, thr, o, [hsv]) => hsvToColorInt(hsv.a),
    },
    staticSigs: new Set(['argb(IIII)I', 'rgb(III)I', 'alpha(I)I', 'red(I)I', 'green(I)I', 'blue(I)I', 'parseColor(Ljava/lang/String;)I', 'hsvToColor([F)I']),
    sfields: [
      { name: 'BLACK', desc: 'I', value: 0xff000000 | 0 }, { name: 'DKGRAY', desc: 'I', value: 0xff444444 | 0 },
      { name: 'GRAY', desc: 'I', value: 0xff888888 | 0 }, { name: 'LTGRAY', desc: 'I', value: 0xffcccccc | 0 },
      { name: 'WHITE', desc: 'I', value: 0xffffffff | 0 }, { name: 'RED', desc: 'I', value: 0xffff0000 | 0 },
      { name: 'GREEN', desc: 'I', value: 0xff00ff00 | 0 }, { name: 'BLUE', desc: 'I', value: 0xff0000ff | 0 },
      { name: 'YELLOW', desc: 'I', value: 0xffffff00 | 0 }, { name: 'CYAN', desc: 'I', value: 0xff00ffff | 0 },
      { name: 'MAGENTA', desc: 'I', value: 0xffff00ff | 0 }, { name: 'TRANSPARENT', desc: 'I', value: 0 },
    ],
  });
  function hsvToColorInt(hsv) {
    let [h, s, v] = hsv;
    h = ((h % 360) + 360) % 360;
    const c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m0 = v - c;
    let r, g, b;
    if (h < 60) [r, g, b] = [c, x, 0]; else if (h < 120) [r, g, b] = [x, c, 0];
    else if (h < 180) [r, g, b] = [0, c, x]; else if (h < 240) [r, g, b] = [0, x, c];
    else if (h < 300) [r, g, b] = [x, 0, c]; else[r, g, b] = [c, 0, x];
    return ((0xff << 24) | (Math.round((r + m0) * 255) << 16) | (Math.round((g + m0) * 255) << 8) | Math.round((b + m0) * 255)) | 0;
  }

  /* ---- android.graphics.Matrix --------------------------------------- */
  vm.registerNative({
    desc: 'Landroid/graphics/Matrix;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o.m = [1, 0, 0, 0, 1, 0]; },   // [a(MSC_X), b(MSKX), c(MTRX), d(MSKY), e(MSC_Y), f(MTRY)]
      '<init>(Landroid/graphics/Matrix;)V': (vm2, thr, o, [src]) => { o.m = src ? src.m.slice() : [1, 0, 0, 0, 1, 0]; },
      'reset()V': (vm2, thr, o) => { o.m = [1, 0, 0, 0, 1, 0]; },
      'postScale(FF)Z': (vm2, thr, o, [sx, sy]) => { o.m = mxMul(o.m, [sx, 0, 0, 0, sy, 0]); return 1; },
      'postScale(FFFF)Z': (vm2, thr, o, [sx, sy, px, py]) => {
        o.m = mxMul(o.m, mxMul(mxMul([1, 0, px, 0, 1, py], [sx, 0, 0, 0, sy, 0]), [1, 0, -px, 0, 1, -py]));
        return 1;
      },
      'postTranslate(FF)Z': (vm2, thr, o, [dx, dy]) => { o.m = mxMul(o.m, [1, 0, dx, 0, 1, dy]); return 1; },
      'postRotate(F)Z': (vm2, thr, o, [deg]) => { o.m = mxMul(o.m, rotMx(deg)); return 1; },
      'postRotate(FFF)Z': (vm2, thr, o, [deg, px, py]) => {
        o.m = mxMul(o.m, mxMul(mxMul([1, 0, px, 0, 1, py], rotMx(deg)), [1, 0, -px, 0, 1, -py]));
        return 1;
      },
      'postConcat(Landroid/graphics/Matrix;)Z': (vm2, thr, o, [other]) => { o.m = mxMul(o.m, other.m); return 1; },
      'setConcat(Landroid/graphics/Matrix;Landroid/graphics/Matrix;)V': (vm2, thr, o, [a, b]) => { o.m = mxMul(a.m, b.m); },
      'preScale(FF)Z': (vm2, thr, o, [sx, sy]) => { o.m = mxMul([sx, 0, 0, 0, sy, 0], o.m); return 1; },
      'preTranslate(FF)Z': (vm2, thr, o, [dx, dy]) => { o.m = mxMul([1, 0, dx, 0, 1, dy], o.m); return 1; },
      'preRotate(F)Z': (vm2, thr, o, [deg]) => { o.m = mxMul(rotMx(deg), o.m); return 1; },
      'setScale(FF)V': (vm2, thr, o, [sx, sy]) => { o.m = [sx, 0, 0, 0, sy, 0]; },
      'setTranslate(FF)V': (vm2, thr, o, [dx, dy]) => { o.m = [1, 0, dx, 0, 1, dy]; },
      'setRotate(F)V': (vm2, thr, o, [deg]) => { o.m = rotMx(deg); },
      'mapPoints([F)V': (vm2, thr, o, [pts]) => {
        for (let i = 0; i + 1 < pts.n; i += 2) {
          const x = pts.a[i], y = pts.a[i + 1];
          pts.a[i] = o.m[0] * x + o.m[1] * y + o.m[2];
          pts.a[i + 1] = o.m[3] * x + o.m[4] * y + o.m[5];
        }
      },
      'invert(Landroid/graphics/Matrix;)Z': (vm2, thr, o, [inv]) => {
        const [a, b, c, d, e, f] = o.m;
        const det = a * e - b * d;
        if (!det) return 0;
        const idet = 1 / det;
        inv.m = [e * idet, -b * idet, (b * f - e * c) * idet, -d * idet, a * idet, (d * c - a * f) * idet];
        return 1;
      },
      'isIdentity()Z': (vm2, thr, o) => (o.m[0] === 1 && o.m[1] === 0 && o.m[2] === 0 && o.m[3] === 0 && o.m[4] === 1 && o.m[5] === 0) ? 1 : 0,
    },
  });
  function rotMx(deg) {
    const r = deg * Math.PI / 180, c = Math.cos(r), s = Math.sin(r);
    return [c, -s, 0, s, c, 0];
  }
  function mxMul(a, b) {  // returns a * b (apply b first, then a)
    return [
      a[0] * b[0] + a[1] * b[3],
      a[0] * b[1] + a[1] * b[4],
      a[0] * b[2] + a[1] * b[5] + a[2],
      a[3] * b[0] + a[4] * b[3],
      a[3] * b[1] + a[4] * b[4],
      a[3] * b[2] + a[4] * b[5] + a[5],
    ];
  }
  host.mxMul = mxMul;

  /* ---- android.graphics.Rect / RectF ---------------------------------- */
  vm.registerNative({
    desc: 'Landroid/graphics/Rect;',
    ifields: [{ name: 'left', desc: 'I' }, { name: 'top', desc: 'I' }, { name: 'right', desc: 'I' }, { name: 'bottom', desc: 'I' }],
    methods: {
      '<init>()V': () => { },
      '<init>(IIII)V': (vm2, thr, o, [l, t, r, b]) => { host.rectISet(o, l, t, r, b); },
      '<init>(Landroid/graphics/Rect;)V': (vm2, thr, o, [src]) => {
        const v = host.rectIvals(src); host.rectISet(o, v[0], v[1], v[2], v[3]);
      },
      'set(IIII)V': (vm2, thr, o, [l, t, r, b]) => host.rectISet(o, l, t, r, b),
      'set(Landroid/graphics/Rect;)V': (vm2, thr, o, [src]) => {
        const v = host.rectIvals(src); host.rectISet(o, v[0], v[1], v[2], v[3]);
      },
      'width()I': (vm2, thr, o) => { const v = host.rectIvals(o); return (v[2] - v[0]) | 0; },
      'height()I': (vm2, thr, o) => { const v = host.rectIvals(o); return (v[3] - v[1]) | 0; },
      'contains(II)Z': (vm2, thr, o, [x, y]) => {
        const v = host.rectIvals(o);
        return (x >= v[0] && x < v[2] && y >= v[1] && y < v[3]) ? 1 : 0;
      },
      'intersects(Landroid/graphics/Rect;)Z': (vm2, thr, o, [r]) => {
        const a = host.rectIvals(o), b = host.rectIvals(r);
        return (a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]) ? 1 : 0;
      },
      'intersect(Landroid/graphics/Rect;)Z': (vm2, thr, o, [r]) => {
        const a = host.rectIvals(o), b = host.rectIvals(r);
        if (a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]) {
          host.rectISet(o, Math.max(a[0], b[0]), Math.max(a[1], b[1]), Math.min(a[2], b[2]), Math.min(a[3], b[3]));
          return 1;
        }
        return 0;
      },
      'offset(II)V': (vm2, thr, o, [dx, dy]) => {
        const v = host.rectIvals(o);
        host.rectISet(o, v[0] + dx, v[1] + dy, v[2] + dx, v[3] + dy);
      },
      'isEmpty()Z': (vm2, thr, o) => {
        const v = host.rectIvals(o);
        return (v[0] >= v[2] || v[1] >= v[3]) ? 1 : 0;
      },
      'toString()Ljava/lang/String;': (vm2, thr, o) => {
        const v = host.rectIvals(o);
        return vm2.newString('Rect(' + v[0] + ', ' + v[1] + ' - ' + v[2] + ', ' + v[3] + ')');
      },
    },
  });
  vm.registerNative({
    desc: 'Landroid/graphics/RectF;',
    ifields: [{ name: 'left', desc: 'F' }, { name: 'top', desc: 'F' }, { name: 'right', desc: 'F' }, { name: 'bottom', desc: 'F' }],
    methods: {
      '<init>()V': () => { },
      '<init>(FFFF)V': (vm2, thr, o, [l, t, r, b]) => { host.rectFSet(o, l, t, r, b); },
      '<init>(Landroid/graphics/RectF;)V': (vm2, thr, o, [src]) => {
        const v = host.rectFVals(src); host.rectFSet(o, v[0], v[1], v[2], v[3]);
      },
      'set(FFFF)V': (vm2, thr, o, [l, t, r, b]) => host.rectFSet(o, l, t, r, b),
      'width()F': (vm2, thr, o) => { const v = host.rectFVals(o); return v[2] - v[0]; },
      'height()F': (vm2, thr, o) => { const v = host.rectFVals(o); return v[3] - v[1]; },
      'isEmpty()Z': (vm2, thr, o) => { const v = host.rectFVals(o); return (v[0] >= v[2] || v[1] >= v[3]) ? 1 : 0; },
    },
  });
  vm.registerNative({ desc: 'Landroid/graphics/Region;', methods: { '<init>()V': () => { } } });

  /* ==================== android.view ==================== */
  vm.registerNative({
    desc: 'Landroid/view/View;',
    ifields: [{ name: 'mLeft', desc: 'I' }, { name: 'mTop', desc: 'I' }],
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; },
      '<init>()V': () => { },
      'getWindowVisibleDisplayFrame(Landroid/graphics/Rect;)V': (vm2, thr, o, [rect]) => {
        host.rectISet(rect, 0, 0, host.displayWidth, host.displayHeight);
      },
      'getWidth()I': () => host.displayWidth,
      'getHeight()I': () => host.displayHeight,
      'getContext()Landroid/content/Context;': (vm2, thr, o) => o._context || null,
      'invalidate()V': () => { },
      'postInvalidate()V': () => { },
      'setVisibility(I)V': () => { },
      'isShown()Z': () => 1,
      'requestFocus()Z': () => 1,
      'setFocusable(Z)V': () => { },
      'setFocusableInTouchMode(Z)V': () => { },
      'setKeepScreenOn(Z)V': () => { },
      'setWillNotDraw(Z)V': () => { },
      'setClickable(Z)V': () => { },
      'setPressed(Z)V': () => { },
      'layout(IIII)V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Landroid/view/SurfaceView;',
    superDesc: 'Landroid/view/View;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => {
        o._context = ctx;
        host.surfaceViewCreated(o);
      },
      'getHolder()Landroid/view/SurfaceHolder;': (vm2, thr, o) => {
        if (!o._holder) o._holder = host.surfaceHolderFor(o);
        return o._holder;
      },
    },
  });

  vm.registerNative({
    desc: 'Landroid/view/SurfaceHolder;',
    accessFlags: 0x0601,
    methods: {},
  });

  /* surface holder impl class (native, concrete) */
  vm.registerNative({
    desc: 'Landroid/view/SurfaceHolderWeb;',
    interfaces: ['Landroid/view/SurfaceHolder;'],
    methods: {
      '<init>()V': (vm2, thr, o) => { o._callbacks = []; o._surface = null; },
      'addCallback(Landroid/view/SurfaceHolder$Callback;)V': (vm2, thr, o, [cb]) => {
        o._callbacks.push(cb);
      },
      'removeCallback(Landroid/view/SurfaceHolder$Callback;)V': (vm2, thr, o, [cb]) => {
        const i = o._callbacks.indexOf(cb);
        if (i >= 0) o._callbacks.splice(i, 1);
      },
      'lockCanvas()Landroid/graphics/Canvas;': (vm2, thr, o) => host.surfaceLockCanvas(o, null),
      'lockCanvas(Landroid/graphics/Rect;)Landroid/graphics/Canvas;': (vm2, thr, o, [dirty]) => host.surfaceLockCanvas(o, dirty),
      'unlockCanvasAndPost(Landroid/graphics/Canvas;)V': (vm2, thr, o, [canvas]) => host.surfaceUnlockPost(o, canvas),
      'getSurfaceFrame()Landroid/graphics/Rect;': (vm2, thr, o) => {
        const r = vm2.newObject(vm2.requireClass('Landroid/graphics/Rect;'));
        host.rectISet(r, 0, 0, host.displayWidth, host.displayHeight);
        return r;
      },
      'setFixedSize(II)V': (vm2, thr, o, [w, h]) => { host.surfaceSetFixedSize(o, w, h); },
      'setFormat(I)V': () => { },
      'setSizeFromLayout()V': () => { },
      'setType(I)V': () => { },
    },
  });

  vm.registerNative({ desc: 'Landroid/view/SurfaceHolder$Callback;', accessFlags: 0x0601, methods: {} });

  vm.registerNative({
    desc: 'Landroid/view/MotionEvent;',
    methods: {
      '<init>()V': () => { },
      'getAction()I': (vm2, thr, o) => o._action | 0,
      'getActionMasked()I': (vm2, thr, o) => (o._action & 0xff) | 0,
      'getX()F': (vm2, thr, o) => o._xs ? o._xs[0] : o._x,
      'getY()F': (vm2, thr, o) => o._ys ? o._ys[0] : o._y,
      'getX(I)F': (vm2, thr, o, [i]) => o._xs ? o._xs[i | 0] : o._x,
      'getY(I)F': (vm2, thr, o, [i]) => o._ys ? o._ys[i | 0] : o._y,
      'getPointerCount()I': (vm2, thr, o) => o._xs ? o._xs.length : 1,
      'getPointerId(I)I': (vm2, thr, o, [i]) => o._ids ? o._ids[i | 0] : 0,
      'getDownTime()J': (vm2, thr, o) => BigInt(Math.floor(o._downTime || 0)),
      'getEventTime()J': (vm2, thr, o) => BigInt(Math.floor(o._eventTime || 0)),
    },
  });
  vm.registerNative({
    desc: 'Landroid/view/KeyEvent;',
    sfields: [],
    methods: {
      '<init>()V': () => { },
      'getAction()I': (vm2, thr, o) => o._action | 0,
      'getKeyCode()I': (vm2, thr, o) => o._keyCode | 0,
      'getRepeatCount()I': (vm2, thr, o) => o._repeat | 0,
    },
    clinit: (vm2, cls) => {
      const put = (name, v) => {
        let f = cls.sfields.find((x) => x.name === name);
        if (!f) { f = { name, desc: 'I', slot: cls.statics.length, holder: cls, isStatic: true }; cls.sfields.push(f); cls.statics.push(v); }
        else cls.statics[f.slot] = v;
      };
      put('ACTION_DOWN', 0); put('ACTION_UP', 1);
      put('KEYCODE_BACK', 4); put('KEYCODE_DPAD_UP', 19); put('KEYCODE_DPAD_DOWN', 20);
      put('KEYCODE_DPAD_LEFT', 21); put('KEYCODE_DPAD_RIGHT', 22); put('KEYCODE_DPAD_CENTER', 23);
      put('KEYCODE_MENU', 82); put('KEYCODE_SEARCH', 84);
    },
  });

  vm.registerNative({
    desc: 'Landroid/view/Window;',
    methods: {
      '<init>()V': () => { },
      'addFlags(I)V': () => { },
      'clearFlags(I)V': () => { },
      'setFlags(II)V': () => { },
      'getDecorView()Landroid/view/View;': (vm2, thr) => vm2.newObject(vm2.requireClass('Landroid/view/View;')),
      'requestFeature(I)Z': () => 1,
    },
  });
  vm.registerNative({
    desc: 'Landroid/view/WindowManager;',
    accessFlags: 0x0601,
    methods: {
      'getDefaultDisplay()Landroid/view/Display;': (vm2, thr) => host.defaultDisplayObj(),
    },
  });
  vm.registerNative({
    desc: 'Landroid/view/WindowManagerImpl;',
    interfaces: ['Landroid/view/WindowManager;'],
    methods: {
      '<init>()V': () => { },
      'getDefaultDisplay()Landroid/view/Display;': (vm2, thr) => host.defaultDisplayObj(),
    },
  });
  vm.registerNative({
    desc: 'Landroid/view/Display;',
    methods: {
      '<init>()V': () => { },
      'getWidth()I': () => host.displayWidth,
      'getHeight()I': () => host.displayHeight,
      'getOrientation()I': () => host.orientation,
      'getRotation()I': () => host.orientation,
      'getMetrics(Landroid/util/DisplayMetrics;)V': (vm2, thr, o, [dm]) => {
        host.fillDisplayMetrics(dm);
      },
    },
  });

  /* ==================== android.util ==================== */
  vm.registerNative({
    desc: 'Landroid/util/DisplayMetrics;',
    ifields: [
      { name: 'widthPixels', desc: 'I' }, { name: 'heightPixels', desc: 'I' },
      { name: 'density', desc: 'F' }, { name: 'densityDpi', desc: 'I' },
      { name: 'scaledDensity', desc: 'F' }, { name: 'xdpi', desc: 'F' }, { name: 'ydpi', desc: 'F' },
    ],
    methods: {
      '<init>()V': () => { },
      'toString()Ljava/lang/String;': (vm2, thr, o) => vm2.newString('DisplayMetrics{density=1.0, width=' + host.displayWidth + ', height=' + host.displayHeight + '}'),
    },
  });
  vm.registerNative({
    desc: 'Landroid/util/Log;',
    methods: {
      '<init>()V': () => { },
      'println(ILjava/lang/String;Ljava/lang/String;)I': (vm2, thr, o, [prio, tag, msg]) => {
        vm2.onLog('[log]' + (tag ? ' ' + tag.js + ':' : '') + ' ' + (msg ? msg.js : ''));
        return 0;
      },
      'd(Ljava/lang/String;Ljava/lang/String;)I': (vm2, thr, o, [tag, msg]) => { vm2.onLog('[log.d] ' + tag.js + ': ' + (msg ? msg.js : '')); return 0; },
      'i(Ljava/lang/String;Ljava/lang/String;)I': (vm2, thr, o, [tag, msg]) => { vm2.onLog('[log.i] ' + tag.js + ': ' + (msg ? msg.js : '')); return 0; },
      'w(Ljava/lang/String;Ljava/lang/String;)I': (vm2, thr, o, [tag, msg]) => { vm2.onLog('[log.w] ' + tag.js + ': ' + (msg ? msg.js : '')); return 0; },
      'e(Ljava/lang/String;Ljava/lang/String;)I': (vm2, thr, o, [tag, msg]) => { vm2.onLog('[log.e] ' + tag.js + ': ' + (msg ? msg.js : '')); return 0; },
      'e(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I': (vm2, thr, o, [tag, msg, t]) => {
        vm2.onLog('[log.e] ' + tag.js + ': ' + (msg ? msg.js : '') + ' ' + (t && t.vmMsg ? t.vmMsg : ''));
        return 0;
      },
    },
    staticSigs: new Set(['println(ILjava/lang/String;Ljava/lang/String;)I', 'd(Ljava/lang/String;Ljava/lang/String;)I', 'i(Ljava/lang/String;Ljava/lang/String;)I', 'w(Ljava/lang/String;Ljava/lang/String;)I', 'e(Ljava/lang/String;Ljava/lang/String;)I', 'e(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I']),
  });

  /* ==================== android.os ==================== */
  vm.registerNative({
    desc: 'Landroid/os/Bundle;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._map = new Map(); },
      'putInt(Ljava/lang/String;I)V': (vm2, thr, o, [k, v]) => { o._map.set(k.js, v | 0); },
      'getInt(Ljava/lang/String;)I': (vm2, thr, o, [k]) => o._map.get(k.js) | 0,
      'getInt(Ljava/lang/String;I)I': (vm2, thr, o, [k, d]) => o._map.has(k.js) ? o._map.get(k.js) : d,
      'putString(Ljava/lang/String;Ljava/lang/String;)V': (vm2, thr, o, [k, v]) => { o._map.set(k.js, v); },
      'getString(Ljava/lang/String;)Ljava/lang/String;': (vm2, thr, o, [k]) => o._map.get(k.js) || null,
      'containsKey(Ljava/lang/String;)Z': (vm2, thr, o, [k]) => o._map.has(k.js) ? 1 : 0,
    },
  });

  vm.registerNative({
    desc: 'Landroid/os/Parcelable;',
    accessFlags: 0x0601,
    methods: {},
  });
  vm.registerNative({ desc: 'Landroid/os/IInterface;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Landroid/os/IBinder;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Landroid/os/DeadObjectException;', superDesc: 'Ljava/lang/RuntimeException;', methods: { '<init>()V': () => { } } });
  vm.registerNative({ desc: 'Landroid/os/RemoteException;', superDesc: 'Ljava/lang/Exception;', methods: { '<init>()V': () => { } } });

  vm.registerNative({
    desc: 'Landroid/os/Binder;',
    interfaces: ['Landroid/os/IBinder;'],
    methods: {
      '<init>()V': () => { },
      'queryLocalInterface(Ljava/lang/String;)Landroid/os/IInterface;': () => null,
      'transact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z': (vm2, thr, o, [code, data, reply, flags]) => {
        if (o._isLicenseService) return vm2.host.binderTransact(o, code | 0, data, reply, flags | 0);
        // local binder object (e.g. a dex ILicensingService*.Stub): deliver onTransact
        return vm2.call(thr, o, 'onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z', [code | 0, data, reply, flags | 0]);
      },
      'onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z': () => 0,
      'getInterfaceDescriptor()Ljava/lang/String;': (vm2, thr, o) => o._descriptor || null,
      'attachInterface(Landroid/os/IInterface;Ljava/lang/String;)V': (vm2, thr, o, [owner, descv]) => { o._descriptor = descv; },
    },
  });

  /* Parcels over JS arrays of typed cells — only what LVL/licensing flow needs */
  vm.registerNative({
    desc: 'Landroid/os/Parcel;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._cells = []; o._pos = 0; },
      'obtain()Landroid/os/Parcel;': (vm2, thr) => {
        const p = vm2.newObject(vm2.requireClass('Landroid/os/Parcel;'));
        p._cells = []; p._pos = 0;
        return p;
      },
      'recycle()V': (vm2, thr, o) => { o._cells = []; o._pos = 0; },
      'writeInt(I)V': (vm2, thr, o, [v]) => { o._cells.push(['i', v | 0]); },
      'readInt()I': (vm2, thr, o) => { const c = o._cells[o._pos++]; return c ? c[1] | 0 : 0; },
      'writeLong(J)V': (vm2, thr, o, [v]) => { o._cells.push(['J', v]); },
      'readLong()J': (vm2, thr, o) => { const c = o._cells[o._pos++]; return c ? c[1] : 0n; },
      'writeString(Ljava/lang/String;)V': (vm2, thr, o, [v]) => { o._cells.push(['s', v]); },
      'readString()Ljava/lang/String;': (vm2, thr, o) => { const c = o._cells[o._pos++]; return c ? c[1] : null; },
      'writeStrongBinder(Landroid/os/IBinder;)V': (vm2, thr, o, [v]) => { o._cells.push(['b', v]); },
      'readStrongBinder()Landroid/os/IBinder;': (vm2, thr, o) => { const c = o._cells[o._pos++]; return c ? c[1] : null; },
      'writeInterfaceToken(Ljava/lang/String;)V': (vm2, thr, o, [v]) => { o._cells.push(['t', v]); },
      'enforceInterface(Ljava/lang/String;)V': (vm2, thr, o, [v]) => { o._pos++; },
      'writeIntArray([I)V': () => { },
      'setDataPosition(I)V': (vm2, thr, o, [pos]) => { o._pos = pos | 0; },
      'dataAvail()I': (vm2, thr, o) => o._cells.length - o._pos,
    },
    staticSigs: new Set(['obtain()Landroid/os/Parcel;']),
  });

  vm.registerNative({
    desc: 'Landroid/os/Build;',
    sfields: [],
    methods: { '<init>()V': () => { } },
    clinit: (vm2, cls) => {
      const put = (name, v, desc) => {
        let f = cls.sfields.find((x) => x.name === name);
        if (!f) { f = { name, desc: desc || 'Ljava/lang/String;', slot: cls.statics.length, holder: cls, isStatic: true }; cls.sfields.push(f); cls.statics.push(v); }
        else cls.statics[f.slot] = v;
      };
      put('MODEL', vm2.newString('arena-web'));
      put('DEVICE', vm2.newString('arena-web'));
      put('PRODUCT', vm2.newString('full'));
    },
  });
  vm.registerNative({
    desc: 'Landroid/os/Build$VERSION;',
    sfields: [],
    methods: { '<init>()V': () => { } },
    clinit: (vm2, cls) => {
      const put = (name, v, desc) => {
        let f = cls.sfields.find((x) => x.name === name);
        if (!f) { f = { name, desc: desc || 'Ljava/lang/String;', slot: cls.statics.length, holder: cls, isStatic: true }; cls.sfields.push(f); cls.statics.push(v); }
        else cls.statics[f.slot] = v;
      };
      put('SDK_INT', 15, 'I');
      put('RELEASE', vm2.newString('4.0.3'));
      put('SDK', vm2.newString('15'));
    },
  });

  vm.registerNative({
    desc: 'Landroid/os/Environment;',
    methods: {
      '<init>()V': () => { },
      'getExternalStorageDirectory()Ljava/io/File;': (vm2, thr) => {
        const f = vm2.newObject(vm2.requireClass('Ljava/io/File;'));
        f.path = '/sdcard';
        return f;
      },
      'getExternalStorageState()Ljava/lang/String;': (vm2, thr) => vm2.newString('mounted'),
      'getDataDirectory()Ljava/io/File;': (vm2, thr) => {
        const f = vm2.newObject(vm2.requireClass('Ljava/io/File;'));
        f.path = '/data';
        return f;
      },
    },
    staticSigs: new Set(['getExternalStorageDirectory()Ljava/io/File;', 'getExternalStorageState()Ljava/lang/String;', 'getDataDirectory()Ljava/io/File;']),
  });

  /* ---- Handler/Looper machinery -------------------------------------- */
  vm.registerNative({
    desc: 'Landroid/os/MessageQueue;',
    methods: { '<init>()V': (vm2, thr, o) => { o._msgs = []; } },
  });
  vm.registerNative({
    desc: 'Landroid/os/Looper;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._msgs = []; o._thread = null; },
      'getMainLooper()Landroid/os/Looper;': (vm2, thr) => host.mainLooperObj(),
      'myLooper()Landroid/os/Looper;': (vm2, thr) => thr._looper || host.mainLooperObj(),
      'prepare()V': (vm2, thr) => {
        if (!thr._looper) thr._looper = host.newLooperObj(thr);
      },
      'loop()V': (vm2, thr) => {
        // cooperative: mark thread as looper-owned; scheduler drives messages
        thr._isLooperThread = true;
        thr.blockedUntil = vm2.now() + 1000;
      },
      'getQueue()Landroid/os/MessageQueue;': (vm2, thr, o) => {
        if (!o._queueObj) {
          o._queueObj = vm2.newObject(vm2.requireClass('Landroid/os/MessageQueue;'));
          o._queueObj._msgs = o._msgs;
        }
        return o._queueObj;
      },
      'getThread()Ljava/lang/Thread;': (vm2, thr, o) => o._thread || null,
    },
    staticSigs: new Set(['getMainLooper()Landroid/os/Looper;', 'myLooper()Landroid/os/Looper;', 'prepare()V', 'loop()V']),
  });

  vm.registerNative({
    desc: 'Landroid/os/Message;',
    methods: {
      '<init>()V': () => { },
      'obtain()Landroid/os/Message;': (vm2, thr) => vm2.newObject(vm2.requireClass('Landroid/os/Message;')),
    },
    staticSigs: new Set(['obtain()Landroid/os/Message;']),
  });

  vm.registerNative({
    desc: 'Landroid/os/Handler;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._looper = host.mainLooperObj(); },
      '<init>(Landroid/os/Looper;)V': (vm2, thr, o, [looper]) => { o._looper = looper; },
      'post(Ljava/lang/Runnable;)Z': (vm2, thr, o, [r]) => {
        host.looperPost(o._looper, { time: vm2.now(), r });
        return 1;
      },
      'postDelayed(Ljava/lang/Runnable;J)Z': (vm2, thr, o, [r, ms]) => {
        host.looperPost(o._looper, { time: vm2.now() + Number(ms), r });
        return 1;
      },
      'postAtTime(Ljava/lang/Runnable;J)Z': (vm2, thr, o, [r, ms]) => {
        host.looperPost(o._looper, { time: Number(ms), r });
        return 1;
      },
      'removeCallbacks(Ljava/lang/Runnable;)V': (vm2, thr, o, [r]) => { host.looperRemove(o._looper, r); },
      'obtainMessage()Landroid/os/Message;': (vm2, thr, o) => { const m2 = vm2.newObject(vm2.requireClass('Landroid/os/Message;')); m2._target = o; return m2; },
      'obtainMessage(I)Landroid/os/Message;': (vm2, thr, o, [what]) => { const m2 = vm2.newObject(vm2.requireClass('Landroid/os/Message;')); m2._target = o; m2._what = what | 0; return m2; },
      'sendMessage(Landroid/os/Message;)Z': (vm2, thr, o, [msg]) => { msg._target = o; host.looperPost(o._looper, { time: vm2.now(), msg, h: o }); return 1; },
      'sendMessageDelayed(Landroid/os/Message;J)Z': (vm2, thr, o, [msg, ms]) => { msg._target = o; host.looperPost(o._looper, { time: vm2.now() + Number(ms), msg, h: o }); return 1; },
      'removeMessages(I)V': (vm2, thr, o, [what]) => { host.looperRemoveMsgs(o._looper, o, what | 0); },
      'handleMessage(Landroid/os/Message;)V': () => { },
      'getLooper()Landroid/os/Looper;': (vm2, thr, o) => o._looper,
    },
  });

  vm.registerNative({
    desc: 'Landroid/os/HandlerThread;',
    superDesc: 'Ljava/lang/Thread;',
    methods: {
      '<init>(Ljava/lang/String;)V': (vm2, thr, o, [name]) => {
        o._name = name ? name.js : 'HandlerThread';
        o._looper = null;
        o._target = null;
      },
      '<init>(Ljava/lang/String;I)V': (vm2, thr, o, [name, prio]) => {
        o._name = name ? name.js : 'HandlerThread';
        o._looper = null; o._target = null;
      },
      'start()V': (vm2, thr, o) => {
        const looper = host.newLooperObj(null);
        o._looper = looper;
        const t = vm2.createThread(o._name, (vm3, thr3) => {
          thr3._looper = looper;
          looper._threadObj = o;
          thr3._isLooperThread = true;
          // register thread mapping so host can pump messages onto it
          thr3._handlerThreadObj = o;
        });
        o._vthread = t;
        looper._vthread = t;
        t.started = true;
      },
      'getLooper()Landroid/os/Looper;': (vm2, thr, o) => {
        if (!o._vthread) {
          // not started (engine may start it explicitly)
        }
        return o._looper;
      },
    },
  });

  /* ==================== android.app ==================== */
  vm.registerNative({
    desc: 'Landroid/app/Activity;',
    superDesc: 'Landroid/content/ContextWrapper;',
    methods: {
      '<init>()V': (vm2, thr, o) => { host.activityInit(o); },
      'onCreate(Landroid/os/Bundle;)V': () => { },
      'onStart()V': () => { },
      'onResume()V': () => { },
      'onPause()V': () => { },
      'onRestart()V': () => { },
      'onStop()V': () => { },
      'onDestroy()V': () => { },
      'onNewIntent(Landroid/content/Intent;)V': () => { },
      'onKeyDown(ILandroid/view/KeyEvent;)Z': () => 0,
      'onKeyUp(ILandroid/view/KeyEvent;)Z': () => 0,
      'dispatchKeyEvent(Landroid/view/KeyEvent;)Z': (vm2, thr, o, [ev]) => {
        const action = vm2.call(thr, ev, 'getAction()I');
        if (action === 0) return vm2.call(thr, o, 'onKeyDown(ILandroid/view/KeyEvent;)Z', [vm2.call(thr, ev, 'getKeyCode()I'), ev]);
        if (action === 1) return vm2.call(thr, o, 'onKeyUp(ILandroid/view/KeyEvent;)Z', [vm2.call(thr, ev, 'getKeyCode()I'), ev]);
        return 0;
      },
      'requestWindowFeature(I)Z': () => 1,
      'setContentView(Landroid/view/View;)V': (vm2, thr, o, [view]) => { host.setContentView(view); },
      'setContentView(I)V': (vm2, thr, o, [resid]) => { },
      'setTitle(Ljava/lang/CharSequence;)V': () => { },
      'setRequestedOrientation(I)V': (vm2, thr, o, [ori]) => { host.requestedOrientation = ori | 0; },
      'getRequestedOrientation()I': () => host.requestedOrientation | 0,
      'getWindowManager()Landroid/view/WindowManager;': (vm2, thr) => host.windowManagerObj(),
      'getWindow()Landroid/view/Window;': (vm2, thr) => host.windowObj(),
      'getApplication()Landroid/app/Application;': (vm2, thr) => host.applicationObj(),
      'getApplicationContext()Landroid/content/Context;': (vm2, thr, o) => o,
      'getIntent()Landroid/content/Intent;': (vm2, thr) => host.launchIntent(),
      'finish()V': () => { host.onFinish && host.onFinish(); },
      'runOnUiThread(Ljava/lang/Runnable;)V': (vm2, thr, o, [r]) => { vm2.runOnUi((mt) => { vm2.call(mt, r, 'run()V'); }); },
      'isFinishing()Z': () => 0,
      'getLocalClassName()Ljava/lang/String;': (vm2, thr) => vm2.newString(host.appInfo.mainActivity),
      'getResources()Landroid/content/res/Resources;': (vm2, thr) => host.resourcesObj(),
      'getAssets()Landroid/content/res/AssetManager;': (vm2, thr) => host.assetManagerObj(),
      'getPackageManager()Landroid/content/pm/PackageManager;': (vm2, thr) => host.packageManagerObj(),
      'getPackageName()Ljava/lang/String;': (vm2, thr) => vm2.newString(host.appInfo.package),
      'getContentResolver()Landroid/content/ContentResolver;': (vm2, thr) => host.contentResolverObj(),
      'getSystemService(Ljava/lang/String;)Ljava/lang/Object;': (vm2, thr, o, [name]) => host.getSystemService(name ? name.js : ''),
      'getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;': (vm2, thr, o, [name, mode]) => host.sharedPreferencesObj(name ? name.js : ''),
      'openFileOutput(Ljava/lang/String;I)Ljava/io/FileOutputStream;': (vm2, thr, o, [name, mode]) => {
        const fos = vm2.newObject(vm2.requireClass('Ljava/io/FileOutputStream;'));
        fos.path = '/data/data/' + host.appInfo.package + '/files/' + (name ? name.js : '');
        fos.buf = [];
        return fos;
      },
      'openFileInput(Ljava/lang/String;)Ljava/io/FileInputStream;': (vm2, thr, o, [name]) => {
        const fis = vm2.newObject(vm2.requireClass('Ljava/io/FileInputStream;'));
        const path = '/data/data/' + host.appInfo.package + '/files/' + (name ? name.js : '');
        const bytes = vm2.host.fsRead(path);
        if (!bytes) vm2.throwNew(thr, 'Ljava/io/FileNotFoundException;', path);
        fis.a = Array.from(bytes); fis.pos = 0; fis.n = fis.a.length;
        return fis;
      },
      'getFilesDir()Ljava/io/File;': (vm2, thr) => {
        const f = vm2.newObject(vm2.requireClass('Ljava/io/File;'));
        f.path = '/data/data/' + host.appInfo.package + '/files';
        return f;
      },
      'getPreferences(I)Landroid/content/SharedPreferences;': (vm2, thr, o, [mode]) => host.sharedPreferencesObj('default'),
      'fileList()[Ljava/lang/String;': (vm2, thr) => {
        const names = host.fsList('/data/data/' + host.appInfo.package + '/files') || [];
        const arr = vm2.newArray('[Ljava/lang/String;', names.length, thr);
        arr.a = names.map((n) => vm2.newString(n));
        return arr;
      },
      'hasWindowFocus()Z': () => 1,
      'isTaskRoot()Z': () => 1,
    },
  });

  vm.registerNative({
    desc: 'Landroid/app/Application;',
    superDesc: 'Landroid/content/ContextWrapper;',
    methods: {
      '<init>()V': (vm2, thr, o) => { host.activityInit(o); },
      'onCreate()V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Landroid/app/KeyguardManager;',
    methods: {
      '<init>()V': () => { },
      'inKeyguardRestrictedInputMode()Z': () => 0,
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/DialogInterface;',
    accessFlags: 0x0601,
    methods: {},
  });
  vm.registerNative({ desc: 'Landroid/content/DialogInterface$OnClickListener;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Landroid/content/DialogInterface$OnCancelListener;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Landroid/content/DialogInterface$OnKeyListener;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Landroid/content/DialogInterface$OnDismissListener;', accessFlags: 0x0601, methods: {} });

  vm.registerNative({
    desc: 'Landroid/app/Dialog;',
    interfaces: ['Landroid/content/DialogInterface;'],
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; },
      '<init>()V': () => { },
      'show()V': (vm2, thr, o) => { host.dialogShow(o); },
      'dismiss()V': (vm2, thr, o) => { host.dialogDismiss(o); },
      'cancel()V': (vm2, thr, o) => { host.dialogDismiss(o); },
      'isShowing()Z': (vm2, thr, o) => o._showing ? 1 : 0,
      'setTitle(Ljava/lang/CharSequence;)V': (vm2, thr, o, [t]) => { o._title = t; host.dialogSetTitle(o, t); },
      'requestWindowFeature(I)Z': () => 1,
      'setOnCancelListener(Landroid/content/DialogInterface$OnCancelListener;)V': (vm2, thr, o, [l]) => { o._onCancel = l; },
    },
  });

  vm.registerNative({
    desc: 'Landroid/app/AlertDialog;',
    superDesc: 'Landroid/app/Dialog;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; o._showing = false; },
      '<init>()V': () => { },
      'getListView()Landroid/widget/ListView;': (vm2, thr) => vm2.newObject(vm2.requireClass('Landroid/widget/ListView;')),
    },
  });

  vm.registerNative({
    desc: 'Landroid/app/AlertDialog$Builder;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => {
        o._ctx = ctx; o._cfg = { title: null, message: null, view: null, buttons: [], onCancel: null, onKey: null };
      },
      'setTitle(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [t]) => { o._cfg.title = t; return o; },
      'setTitle(I)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [res]) => { return o; },
      'setMessage(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [m]) => { o._cfg.message = m; return o; },
      'setMessage(I)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [res]) => { return o; },
      'setView(Landroid/view/View;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [v]) => { o._cfg.view = v; return o; },
      'setPositiveButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [t, l]) => {
        o._cfg.buttons.push({ which: -1, label: vm2._strOf(t), listener: l }); return o;
      },
      'setNegativeButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [t, l]) => {
        o._cfg.buttons.push({ which: -2, label: vm2._strOf(t), listener: l }); return o;
      },
      'setNeutralButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [t, l]) => {
        o._cfg.buttons.push({ which: -3, label: vm2._strOf(t), listener: l }); return o;
      },
      'setOnCancelListener(Landroid/content/DialogInterface$OnCancelListener;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [l]) => { o._cfg.onCancel = l; return o; },
      'setOnKeyListener(Landroid/content/DialogInterface$OnKeyListener;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [l]) => { o._cfg.onKey = l; return o; },
      'setCancelable(Z)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [c]) => { o._cfg.cancelable = !!c; return o; },
      'setItems([Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;': (vm2, thr, o, [items, l]) => {
        o._cfg.items = items ? items.a : []; o._cfg.itemListener = l; return o;
      },
      'show()Landroid/app/AlertDialog;': (vm2, thr, o) => {
        const dlg = vm2.newObject(vm2.requireClass('Landroid/app/AlertDialog;'));
        dlg._cfg = o._cfg;
        dlg._showing = true;
        host.dialogShow(dlg);
        return dlg;
      },
      'create()Landroid/app/AlertDialog;': (vm2, thr, o) => {
        const dlg = vm2.newObject(vm2.requireClass('Landroid/app/AlertDialog;'));
        dlg._cfg = o._cfg;
        dlg._showing = false;
        return dlg;
      },
    },
  });

  /* ==================== android.content ==================== */
  vm.registerNative({
    desc: 'Landroid/content/Context;',
    methods: {
      '<init>()V': () => { },
      'getAssets()Landroid/content/res/AssetManager;': (vm2, thr) => host.assetManagerObj(),
      'getResources()Landroid/content/res/Resources;': (vm2, thr) => host.resourcesObj(),
      'getPackageManager()Landroid/content/pm/PackageManager;': (vm2, thr) => host.packageManagerObj(),
      'getPackageName()Ljava/lang/String;': (vm2, thr) => vm2.newString(host.appInfo.package),
      'bindService(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z': (vm2, thr, o, [intent, conn, flags]) =>
        host.bindService(intent, conn, flags) ? 1 : 0,
      'unbindService(Landroid/content/ServiceConnection;)V': (vm2, thr, o, [conn]) => { host.unbindService(conn); },
      'getSystemService(Ljava/lang/String;)Ljava/lang/Object;': (vm2, thr, o, [name]) => host.getSystemService(name ? name.js : ''),
      'getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;': (vm2, thr, o, [name, mode]) => host.sharedPreferencesObj(name ? name.js : ''),
      'registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;': () => null,
      'unregisterReceiver(Landroid/content/BroadcastReceiver;)V': () => { },
      'startActivity(Landroid/content/Intent;)V': (vm2, thr, o, [intent]) => {
        // external intents (market:// etc.) — open in new tab when http
        const action = intent && intent._action ? intent._action : '';
        const uri = intent && intent._uri ? intent._uri : null;
        if (uri && /^https?:/i.test(uri)) { try { window.open(uri, '_blank'); } catch (e) { } }
      },
      'sendBroadcast(Landroid/content/Intent;)V': () => { },
      'getContentResolver()Landroid/content/ContentResolver;': (vm2, thr) => host.contentResolverObj(),
      'getMainLooper()Landroid/os/Looper;': (vm2, thr) => host.mainLooperObj(),
      'getApplicationContext()Landroid/content/Context;': (vm2, thr, o) => host.currentActivityObj(),
      'stopService(Landroid/content/Intent;)Z': () => 0,
      'startService(Landroid/content/Intent;)Landroid/content/ComponentName;': () => null,
      'checkCallingOrSelfPermission(Ljava/lang/String;)I': () => 0,
    },
    staticSigs: new Set(),
    sfields: [
      { name: 'AUDIO_SERVICE', desc: 'Ljava/lang/String;' },
      { name: 'WINDOW_SERVICE', desc: 'Ljava/lang/String;' },
      { name: 'KEYGUARD_SERVICE', desc: 'Ljava/lang/String;' },
      { name: 'VIBRATOR_SERVICE', desc: 'Ljava/lang/String;' },
      { name: 'POWER_SERVICE', desc: 'Ljava/lang/String;' },
      { name: 'LAYOUT_INFLATER_SERVICE', desc: 'Ljava/lang/String;' },
    ],
    clinit: (vm2, cls) => {
      const put = (name, v) => {
        const f = cls.sfields.find((x) => x.name === name);
        if (f) cls.statics[f.slot] = vm2.newString(v);
      };
      put('AUDIO_SERVICE', 'audio');
      put('WINDOW_SERVICE', 'window');
      put('KEYGUARD_SERVICE', 'keyguard');
      put('VIBRATOR_SERVICE', 'vibrator');
      put('POWER_SERVICE', 'power');
      put('LAYOUT_INFLATER_SERVICE', 'layout_inflater');
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/ContextWrapper;',
    superDesc: 'Landroid/content/Context;',
    methods: {
      '<init>()V': () => { },
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [base]) => { o._base = base; },
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/Intent;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._extras = new Map(); },
      '<init>(Ljava/lang/String;)V': (vm2, thr, o, [action]) => { o._action = action ? action.js : null; o._extras = new Map(); },
      '<init>(Ljava/lang/String;Landroid/net/Uri;)V': (vm2, thr, o, [action, uri]) => {
        o._action = action ? action.js : null;
        o._uri = uri && uri._uri !== undefined ? uri._uri : null;
        o._extras = new Map();
      },
      '<init>(Landroid/content/Context;Ljava/lang/Class;)V': (vm2, thr, o, [ctx, cls]) => { o._extras = new Map(); o._cls = cls; },
      'getAction()Ljava/lang/String;': (vm2, thr, o) => o._action ? vm2.newString(o._action) : null,
      'setAction(Ljava/lang/String;)Landroid/content/Intent;': (vm2, thr, o, [a]) => { o._action = a ? a.js : null; return o; },
      'getIntExtra(Ljava/lang/String;I)I': (vm2, thr, o, [k, def]) => o._extras.has(k.js) ? o._extras.get(k.js) : def,
      'putExtra(Ljava/lang/String;I)Landroid/content/Intent;': (vm2, thr, o, [k, v]) => { o._extras.set(k.js, v | 0); return o; },
      'putExtra(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;': (vm2, thr, o, [k, v]) => { o._extras.set(k.js, v); return o; },
      'getStringExtra(Ljava/lang/String;)Ljava/lang/String;': (vm2, thr, o, [k]) => o._extras.get(k.js) || null,
      'setFlags(I)Landroid/content/Intent;': (vm2, thr, o, [f]) => { o._flags = f | 0; return o; },
      'getFlags()I': (vm2, thr, o) => o._flags | 0,
      'setClassName(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;': (vm2, thr, o) => o,
      'addCategory(Ljava/lang/String;)Landroid/content/Intent;': (vm2, thr, o) => o,
      'getData()Landroid/net/Uri;': (vm2, thr, o) => {
        if (o._uri === null || o._uri === undefined) return null;
        const u = vm2.newObject(vm2.requireClass('Landroid/net/Uri;'));
        u._uri = o._uri;
        return u;
      },
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/IntentFilter;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._actions = []; },
      '<init>(Ljava/lang/String;)V': (vm2, thr, o, [a]) => { o._actions = [a ? a.js : '']; },
      'addAction(Ljava/lang/String;)V': (vm2, thr, o, [a]) => { o._actions.push(a ? a.js : ''); },
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/BroadcastReceiver;',
    methods: {
      '<init>()V': () => { },
      'onReceive(Landroid/content/Context;Landroid/content/Intent;)V': () => { },
    },
  });

  vm.registerNative({ desc: 'Landroid/content/ServiceConnection;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({
    desc: 'Landroid/content/ComponentName;',
    methods: {
      '<init>(Ljava/lang/String;Ljava/lang/String;)V': (vm2, thr, o, [pkg, cls]) => { o._pkg = pkg ? pkg.js : ''; o._cls = cls ? cls.js : ''; },
      '<init>()V': () => { },
      'getPackageName()Ljava/lang/String;': (vm2, thr, o) => vm2.newString(o._pkg || ''),
      'getClassName()Ljava/lang/String;': (vm2, thr, o) => vm2.newString(o._cls || ''),
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/ContentResolver;',
    methods: { '<init>()V': () => { } },
  });

  /* ---- SharedPreferences (backed by localStorage) --------------------- */
  vm.registerNative({
    desc: 'Landroid/content/SharedPreferences;',
    accessFlags: 0x0601,
    methods: {},
  });
  vm.registerNative({
    desc: 'Landroid/content/SharedPreferences$Editor;',
    accessFlags: 0x0601,
    methods: {},
  });
  vm.registerNative({
    desc: 'Landroid/content/SharedPreferencesWeb;',
    interfaces: ['Landroid/content/SharedPreferences;'],
    methods: {
      '<init>()V': (vm2, thr, o) => { o._map = new Map(); o._name = ''; },
      'getString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;': (vm2, thr, o, [k, def]) =>
        o._map.has(k.js) ? vm2.newString(o._map.get(k.js)) : def,
      'getInt(Ljava/lang/String;I)I': (vm2, thr, o, [k, def]) => o._map.has(k.js) ? o._map.get(k.js) : def,
      'getLong(Ljava/lang/String;J)J': (vm2, thr, o, [k, def]) => o._map.has(k.js) ? o._map.get(k.js) : def,
      'getBoolean(Ljava/lang/String;Z)Z': (vm2, thr, o, [k, def]) => o._map.has(k.js) ? o._map.get(k.js) : def,
      'contains(Ljava/lang/String;)Z': (vm2, thr, o, [k]) => o._map.has(k.js) ? 1 : 0,
      'edit()Landroid/content/SharedPreferences$Editor;': (vm2, thr, o) => host.prefsEditorObj(o),
    },
  });
  vm.registerNative({
    desc: 'Landroid/content/SharedPreferencesEditorWeb;',
    interfaces: ['Landroid/content/SharedPreferences$Editor;'],
    methods: {
      '<init>()V': (vm2, thr, o) => { o._dirty = new Map(); },
      'putString(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;': (vm2, thr, o, [k, v]) => { o._dirty.set(k.js, { t: 's', v: v ? v.js : null }); return o; },
      'putInt(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;': (vm2, thr, o, [k, v]) => { o._dirty.set(k.js, { t: 'i', v: v | 0 }); return o; },
      'putLong(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;': (vm2, thr, o, [k, v]) => { o._dirty.set(k.js, { t: 'J', v }); return o; },
      'putBoolean(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;': (vm2, thr, o, [k, v]) => { o._dirty.set(k.js, { t: 'z', v: v ? 1 : 0 }); return o; },
      'remove(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;': (vm2, thr, o, [k]) => { o._dirty.set(k.js, { t: 'rm' }); return o; },
      'clear()Landroid/content/SharedPreferences$Editor;': (vm2, thr, o) => { o._dirty.set('*', { t: 'clear' }); return o; },
      'commit()Z': (vm2, thr, o) => { host.prefsCommit(o); return 1; },
      'apply()V': (vm2, thr, o) => { host.prefsCommit(o); },
    },
  });

  /* ==================== android.content.pm ==================== */
  vm.registerNative({
    desc: 'Landroid/content/pm/PackageManager;',
    methods: {
      '<init>()V': () => { },
      'getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;': (vm2, thr, o, [pkg, flags]) => {
        const pi = vm2.newObject(vm2.requireClass('Landroid/content/pm/PackageInfo;'));
        host.setField(pi, 'versionCode', 'I', host.appInfo.versionCode | 0);
        host.setField(pi, 'versionName', 'Ljava/lang/String;', vm2.newString(host.appInfo.versionName));
        return pi;
      },
    },
  });
  vm.registerNative({
    desc: 'Landroid/content/pm/PackageInfo;',
    ifields: [{ name: 'versionCode', desc: 'I' }, { name: 'versionName', desc: 'Ljava/lang/String;' }, { name: 'packageName', desc: 'Ljava/lang/String;' }],
    methods: { '<init>()V': () => { } },
  });
  vm.registerNative({
    desc: 'Landroid/content/pm/ApplicationInfo;',
    ifields: [{ name: 'packageName', desc: 'Ljava/lang/String;' }, { name: 'sourceDir', desc: 'Ljava/lang/String;' }],
    methods: { '<init>()V': () => { } },
  });

  /* ==================== android.content.res ==================== */
  vm.registerNative({
    desc: 'Landroid/content/res/AssetManager;',
    methods: {
      '<init>()V': () => { },
      'open(Ljava/lang/String;)Ljava/io/InputStream;': (vm2, thr, o, [name]) => {
        const bytes = host.assetBytes(name ? name.js : '');
        if (!bytes) vm2.throwNew(thr, 'Ljava/io/FileNotFoundException;', name ? name.js : '');
        const is = vm2.newObject(vm2.requireClass('Ljava/io/ByteArrayInputStream;'));
        is.a = Array.from(bytes); is.pos = 0; is.n = is.a.length;
        return is;
      },
      'list(Ljava/lang/String;)[Ljava/lang/String;': (vm2, thr, o, [path]) => {
        const names = host.assetList(path ? path.js : '');
        const arr = vm2.newArray('Ljava/lang/String;', names.length, thr);
        for (let i = 0; i < names.length; i++) arr.a[i] = vm2.newString(names[i]);
        return arr;
      },
      'close()V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Landroid/content/res/Resources;',
    methods: {
      '<init>()V': () => { },
      'openRawResourceFd(I)Landroid/content/res/AssetFileDescriptor;': (vm2, thr, o, [resid]) => {
        const info = host.rawResource(resid | 0);
        if (!info) vm2.throwNew(thr, 'Landroid/content/res/Resources$NotFoundException;', 'Resource ID #0x' + (resid >>> 0).toString(16));
        const afd = vm2.newObject(vm2.requireClass('Landroid/content/res/AssetFileDescriptor;'));
        afd._bytes = info.bytes;
        afd._name = info.name;
        return afd;
      },
      'openRawResource(I)Ljava/io/InputStream;': (vm2, thr, o, [resid]) => {
        const info = host.rawResource(resid | 0);
        if (!info) vm2.throwNew(thr, 'Landroid/content/res/Resources$NotFoundException;', 'Resource ID #0x' + (resid >>> 0).toString(16));
        const is = vm2.newObject(vm2.requireClass('Ljava/io/ByteArrayInputStream;'));
        is.a = Array.from(info.bytes); is.pos = 0; is.n = is.a.length;
        return is;
      },
      'getIdentifier(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I': (vm2, thr, o, [name, defType, defPkg]) => {
        return host.getIdentifier(name ? name.js : '', defType ? defType.js : '', defPkg ? defPkg.js : '') | 0;
      },
      'getString(I)Ljava/lang/String;': (vm2, thr, o, [resid]) => {
        return vm2.newString(host.getStringResource(resid | 0) || '');
      },
      'getAssets()Landroid/content/res/AssetManager;': (vm2, thr) => host.assetManagerObj(),
      'getResourceName(I)Ljava/lang/String;': (vm2, thr, o, [resid]) => {
        const r = host.rawResource(resid | 0);
        return vm2.newString(r ? host.appInfo.package + ':raw/' + r.name : '');
      },
      'getText(I)Ljava/lang/CharSequence;': (vm2, thr, o, [resid]) => vm2.newString(host.getStringResource(resid | 0) || ''),
    },
  });
  vm.registerNative({ desc: 'Landroid/content/res/Resources$NotFoundException;', superDesc: 'Ljava/lang/RuntimeException;', methods: { '<init>()V': () => { }, '<init>(Ljava/lang/String;)V': (vm2, thr, o, [m]) => { o.vmMsg = m ? m.js : null; } } });

  vm.registerNative({
    desc: 'Landroid/content/res/AssetFileDescriptor;',
    methods: {
      '<init>()V': () => { },
      'getStartOffset()J': () => 0n,
      'getLength()J': (vm2, thr, o) => BigInt(o._bytes ? o._bytes.length : -1),
      'getDeclaredLength()J': (vm2, thr, o) => BigInt(o._bytes ? o._bytes.length : -1),
      'createInputStream()Ljava/io/FileInputStream;': (vm2, thr, o) => {
        const fis = vm2.newObject(vm2.requireClass('Ljava/io/FileInputStream;'));
        fis.a = o._bytes ? Array.from(o._bytes) : [];
        fis.pos = 0; fis.n = fis.a.length;
        return fis;
      },
      'createOutputStream()Ljava/io/FileOutputStream;': () => null,
      'getFileDescriptor()Ljava/io/FileDescriptor;': (vm2, thr) => vm2.newObject(vm2.requireClass('Ljava/io/FileDescriptor;')),
      'close()V': () => { },
    },
  });
  vm.registerNative({
    desc: 'Ljava/io/FileDescriptor;',
    methods: { '<init>()V': () => { }, 'valid()Z': () => 1 },
  });

  /* ==================== android.media ==================== */
  vm.registerNative({
    desc: 'Landroid/media/MediaPlayer;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._player = null; o._resid = 0; o._looping = false; },
      'create(Landroid/content/Context;I)Landroid/media/MediaPlayer;': (vm2, thr, o, [ctx, resid]) => {
        const mp = vm2.newObject(vm2.requireClass('Landroid/media/MediaPlayer;'));
        mp._resid = resid | 0;
        mp._player = host.mediaCreate(resid | 0);
        mp._looping = false;
        return mp;
      },
      'start()V': (vm2, thr, o) => { host.mediaStart(o); },
      'stop()V': (vm2, thr, o) => { host.mediaStop(o); },
      'pause()V': (vm2, thr, o) => { host.mediaPause(o); },
      'release()V': (vm2, thr, o) => { host.mediaRelease(o); },
      'reset()V': (vm2, thr, o) => { host.mediaReset(o); },
      'seekTo(I)V': (vm2, thr, o, [ms]) => { host.mediaSeek(o, ms | 0); },
      'setVolume(FF)V': (vm2, thr, o, [l, r]) => { host.mediaSetVolume(o, (l + r) / 2); },
      'setLooping(Z)V': (vm2, thr, o, [l]) => { o._looping = !!l; host.mediaSetLooping(o, !!l); },
      'isPlaying()Z': (vm2, thr, o) => host.mediaIsPlaying(o) ? 1 : 0,
      'getCurrentPosition()I': (vm2, thr, o) => host.mediaGetPosition(o) | 0,
      'getDuration()I': (vm2, thr, o) => host.mediaGetDuration(o) | 0,
      'isLooping()Z': (vm2, thr, o) => o._looping ? 1 : 0,
      'setOnCompletionListener(Landroid/media/MediaPlayer$OnCompletionListener;)V': (vm2, thr, o, [l]) => {
        o._onCompletion = l;
      },
      'setAudioStreamType(I)V': () => { },
      'prepare()V': () => { },
      'prepareAsync()V': () => { },
      'setDataSource(Ljava/lang/String;)V': () => { },
      'startAsync()V': () => { },
    },
    staticSigs: new Set(['create(Landroid/content/Context;I)Landroid/media/MediaPlayer;']),
  });
  vm.registerNative({ desc: 'Landroid/media/MediaPlayer$OnCompletionListener;', accessFlags: 0x0601, methods: {} });

  vm.registerNative({
    desc: 'Landroid/media/AudioManager;',
    methods: {
      '<init>()V': () => { },
      'getStreamMaxVolume(I)I': () => 15,
      'getStreamVolume(I)I': () => 10,
      'setStreamVolume(III)V': () => { },
      'getRingerMode()I': () => 2,
      'getMusicVolume()I': () => 10,
      'setMusicVolume(I)V': () => { },
      'getSoundEffectVolume()I': () => 10,
      'setSoundEffectVolume(I)V': () => { },
      'playSoundEffect(I)V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Landroid/media/JetPlayer;',
    methods: {
      '<init>()V': () => { },
      'getJetPlayer()Landroid/media/JetPlayer;': (vm2, thr) => vm2.newObject(vm2.requireClass('Landroid/media/JetPlayer;')),
      'loadJetFile(Landroid/content/res/AssetFileDescriptor;)Z': () => 1,
      'queueJetSegment(IIIIII)Z': () => 1,
      'queueJetSegment(IIIIIB)Z': () => 1,
      'queueJetSegmentMuteArray(IIIIII[IZ)Z': () => 1,
      'play()Z': () => 1,
      'pause()Z': () => 1,
      'clearQueue()Z': () => 1,
      'closeJetFile()Z': () => 1,
      'release()V': () => { },
      'setMuteArray([IZ)Z': () => 1,
      'setTrackMuteArray([IZ)Z': () => 1,
      'setMuteFlag(IZZ)Z': () => 1,
    },
    staticSigs: new Set(['getJetPlayer()Landroid/media/JetPlayer;']),
  });

  /* ==================== android.net.Uri ==================== */
  vm.registerNative({
    desc: 'Landroid/net/Uri;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._uri = null; },
      'parse(Ljava/lang/String;)Landroid/net/Uri;': (vm2, thr, o, [u]) => {
        const x = vm2.newObject(vm2.requireClass('Landroid/net/Uri;'));
        x._uri = u ? u.js : '';
        return x;
      },
      'toString()Ljava/lang/String;': (vm2, thr, o) => vm2.newString(o._uri || ''),
      'getScheme()Ljava/lang/String;': (vm2, thr, o) => {
        const m2 = (o._uri || '').match(/^([a-z]+):/i);
        return vm2.newString(m2 ? m2[1] : '');
      },
      'getHost()Ljava/lang/String;': (vm2, thr, o) => {
        const m2 = (o._uri || '').match(/^[a-z]+:\/\/([^/]+)/i);
        return vm2.newString(m2 ? m2[1] : '');
      },
    },
    staticSigs: new Set(['parse(Ljava/lang/String;)Landroid/net/Uri;']),
  });

  /* ==================== android.webkit ==================== */
  vm.registerNative({
    desc: 'Landroid/webkit/WebSettings;',
    methods: {
      '<init>()V': (vm2, thr, o) => { o._ua = 'Mozilla/5.0 (Linux; U; Android 2.3.4; en-us) AppleWebKit/533.1 (KHTML, like Gecko) Version/4.0 Mobile Safari/533.1'; },
      /* <init> may never run when the object came from WebView.<init> (host-side
       * alloc) — always fall back to the stock UA like Android does */
      'getUserAgentString()Ljava/lang/String;': (vm2, thr, o) => vm2.newString(o._ua || 'Mozilla/5.0 (Linux; U; Android 2.3.4; en-us) AppleWebKit/533.1 (KHTML, like Gecko) Version/4.0 Mobile Safari/533.1'),
      'setUserAgentString(Ljava/lang/String;)V': (vm2, thr, o, [ua]) => { o._ua = ua ? ua.js : o._ua; },
      'setJavaScriptEnabled(Z)V': () => { },
    },
  });
  vm.registerNative({
    desc: 'Landroid/webkit/WebView;',
    superDesc: 'Landroid/view/View;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => {
        o._context = ctx;
        o._settings = vm2.newObject(vm2.requireClass('Landroid/webkit/WebSettings;'));
      },
      'getSettings()Landroid/webkit/WebSettings;': (vm2, thr, o) => o._settings,
      'loadUrl(Ljava/lang/String;)V': () => { },
      'loadData(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V': () => { },
    },
  });

  /* ==================== android.widget ==================== */
  vm.registerNative({
    desc: 'Landroid/widget/Toast;',
    methods: {
      '<init>(Landroid/content/Context;)V': () => { },
      'makeText(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;': (vm2, thr, o, [ctx, text, dur]) => {
        const t = vm2.newObject(vm2.requireClass('Landroid/widget/Toast;'));
        t._text = vm2._strOf(text);
        t._duration = dur | 0;
        return t;
      },
      'makeText(Landroid/content/Context;II)Landroid/widget/Toast;': (vm2, thr, o, [ctx, resid, dur]) => {
        const t = vm2.newObject(vm2.requireClass('Landroid/widget/Toast;'));
        t._text = host.getStringResource(resid | 0) || '';
        t._duration = dur | 0;
        return t;
      },
      'show()V': (vm2, thr, o) => { host.showToast(o._text, o._duration); },
      'cancel()V': () => { },
      'setGravity(III)V': () => { },
    },
    staticSigs: new Set(['makeText(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;', 'makeText(Landroid/content/Context;II)Landroid/widget/Toast;']),
  });

  vm.registerNative({
    desc: 'Landroid/text/Editable;',
    accessFlags: 0x0601,
    interfaces: ['Ljava/lang/CharSequence;'],
    methods: {},
  });
  vm.registerNative({
    desc: 'Landroid/text/SpannableStringBuilder;',
    interfaces: ['Landroid/text/Editable;'],
    methods: {
      '<init>()V': (vm2, thr, o) => { o.js = ''; },
      'toString()Ljava/lang/String;': (vm2, thr, o) => vm2.newString(o.js || ''),
      'length()I': (vm2, thr, o) => (o.js || '').length,
      'charAt(I)C': (vm2, thr, o, [i]) => (o.js || '').charCodeAt(i),
    },
  });

  vm.registerNative({
    desc: 'Landroid/widget/TextView;',
    superDesc: 'Landroid/view/View;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; o._text = ''; },
      'setText(Ljava/lang/CharSequence;)V': (vm2, thr, o, [t]) => { o._text = vm2._strOf(t); },
      'getText()Ljava/lang/CharSequence;': (vm2, thr, o) => vm2.newString(o._text || ''),
    },
  });

  vm.registerNative({
    desc: 'Landroid/widget/EditText;',
    superDesc: 'Landroid/widget/TextView;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => {
        o._context = ctx;
        const ed = vm2.newObject(vm2.requireClass('Landroid/text/SpannableStringBuilder;'));
        ed.js = '';
        o._editable = ed;
      },
      'getText()Landroid/text/Editable;': (vm2, thr, o) => o._editable,
      'setText(Ljava/lang/CharSequence;)V': (vm2, thr, o, [t]) => { o._editable.js = vm2._strOf(t); },
      'setHint(Ljava/lang/CharSequence;)V': () => { },
      'setMaxLines(I)V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Landroid/widget/FrameLayout;',
    superDesc: 'Landroid/view/View;',
    methods: {
      '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; o._children = []; },
      'addView(Landroid/view/View;)V': (vm2, thr, o, [v]) => { o._children.push(v); if (v.c.desc === 'Landroid/view/SurfaceView;') host.setContentView(v); },
    },
  });
  vm.registerNative({
    desc: 'Landroid/widget/ListView;',
    superDesc: 'Landroid/view/View;',
    methods: { '<init>()V': () => { }, '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; } },
  });

  /* ==================== android.text.TextUtils ==================== */
  vm.registerNative({
    desc: 'Landroid/text/TextUtils;',
    methods: {
      '<init>()V': () => { },
      'isEmpty(Ljava/lang/CharSequence;)Z': (vm2, thr, o, [s]) => {
        return (s === null || s === 0 || vm2._strOf(s).length === 0) ? 1 : 0;
      },
      'join(Ljava/lang/CharSequence;Ljava/lang/Iterable;)Ljava/lang/String;': (vm2, thr, o, [delim, tokens]) => {
        const parts = [];
        const it = vm2.call(thr, tokens, 'iterator()Ljava/util/Iterator;');
        while (vm2.call(thr, it, 'hasNext()Z')) {
          parts.push(vm2._strOf(vm2.call(thr, it, 'next()Ljava/lang/Object;')));
        }
        return vm2.newString(parts.join(delim ? vm2._strOf(delim) : ''));
      },
      'join(Ljava/lang/CharSequence;[Ljava/lang/Object;)Ljava/lang/String;': (vm2, thr, o, [delim, tokens]) => {
        const parts = [];
        for (let i = 0; i < tokens.n; i++) parts.push(vm2._strOf(tokens.a[i]));
        return vm2.newString(parts.join(delim ? vm2._strOf(delim) : ''));
      },
      'split(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;': (vm2, thr, o, [text, expression]) => {
        const rx = expression.js;
        // TextUtils.split(text, expression): the expression is a JAVA regex filtered
        // through Pattern semantics; SimpleStringSplitter path handles the common
        // single-char case. \Q..\E quoting handled here.
        let jsRe = rx.replace(/\\Q([\s\S]*?)\\E/g, (m2, g) => g.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
        const parts = text.js.split(new RegExp(jsRe, 'g'));
        const arr = vm2.newArray('Ljava/lang/String;', parts.length, thr);
        for (let i = 0; i < parts.length; i++) arr.a[i] = vm2.newString(parts[i]);
        return arr;
      },
      'split(Ljava/lang/String;Ljava/util/regex/Pattern;)[Ljava/lang/String;': (vm2, thr, o, [text, pattern]) => {
        const parts = pattern._regex ? text.js.split(pattern._regex) : [text.js];
        const arr = vm2.newArray('Ljava/lang/String;', parts.length, thr);
        for (let i = 0; i < parts.length; i++) arr.a[i] = vm2.newString(parts[i]);
        return arr;
      },
    },
    staticSigs: new Set(['isEmpty(Ljava/lang/CharSequence;)Z', 'join(Ljava/lang/CharSequence;Ljava/lang/Iterable;)Ljava/lang/String;', 'join(Ljava/lang/CharSequence;[Ljava/lang/Object;)Ljava/lang/String;', 'split(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;', 'split(Ljava/lang/String;Ljava/util/regex/Pattern;)[Ljava/lang/String;']),
  });

  vm.registerNative({
    desc: 'Landroid/text/TextUtils$StringSplitter;',
    accessFlags: 0x0601,
    interfaces: ['Ljava/lang/Iterable;'],
    methods: {},
  });
  vm.registerNative({
    desc: 'Landroid/text/TextUtils$SimpleStringSplitter;',
    interfaces: ['Landroid/text/TextUtils$StringSplitter;', 'Ljava/util/Iterator;'],
    methods: {
      '<init>(C)V': (vm2, thr, o, [delim]) => { o._delim = String.fromCharCode(delim); o._str = ''; o._pos = 0; },
      'setString(Ljava/lang/String;)V': (vm2, thr, o, [s]) => { o._str = s ? s.js : ''; o._pos = 0; },
      'iterator()Ljava/util/Iterator;': (vm2, thr, o) => o,
      'hasNext()Z': (vm2, thr, o) => (o._pos < o._str.length) ? 1 : 0,
      'next()Ljava/lang/String;': (vm2, thr, o) => {
        const nl = o._str.indexOf(o._delim, o._pos);
        if (nl < 0) {
          const res = o._str.substring(o._pos);
          o._pos = o._str.length;
          return vm2.newString(res);
        }
        const res = o._str.substring(o._pos, nl);
        o._pos = nl + 1;
        return vm2.newString(res);
      },
      'next()Ljava/lang/Object;': (vm2, thr, o) => o.c.sigMap.get('next()Ljava/lang/String;').native(vm2, thr, o, []),
      'remove()V': (vm2, thr) => { vm2.throwNew(thr, 'Ljava/lang/UnsupportedOperationException;', ''); },
    },
  });
  vm.registerNative({ desc: 'Ljava/lang/Iterable;', accessFlags: 0x0601, methods: {} });

  /* ==================== android.provider.Settings ==================== */
  vm.registerNative({
    desc: 'Landroid/provider/Settings$System;',
    methods: {
      '<init>()V': () => { },
      'getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;': (vm2, thr, o, [cr, name]) => {
        const n = name ? name.js : '';
        if (n === 'android_id') return vm2.newString('d34db33fc0ffee01');
        return null;
      },
    },
    staticSigs: new Set(['getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;']),
  });

  /* ==================== android.view.inputmethod ==================== */
  vm.registerNative({
    desc: 'Landroid/view/inputmethod/InputMethodManager;',
    methods: {
      '<init>()V': () => { },
      'showSoftInput(Landroid/view/View;I)Z': () => 0,
      'hideSoftInputFromWindow(Landroid/os/IBinder;I)Z': () => 0,
    },
  });
  vm.registerNative({ desc: 'Landroid/os/ResultReceiver;', methods: { '<init>()V': () => { } } });

  /* ==================== android.telephony (defensive) ==================== */
  vm.registerNative({
    desc: 'Landroid/telephony/TelephonyManager;',
    methods: {
      '<init>()V': () => { },
      'getDeviceId()Ljava/lang/String;': () => null,
      'getLine1Number()Ljava/lang/String;': () => null,
      'getNetworkOperatorName()Ljava/lang/String;': (vm2, thr) => vm2.newString(''),
      'getSimCountryIso()Ljava/lang/String;': (vm2, thr) => vm2.newString('us'),
    },
  });

  /* ==================== remaining service stubs ==================== */
  vm.registerNative({
    desc: 'Landroid/view/LayoutInflater;',
    methods: {
      '<init>()V': () => { },
      'inflate(ILandroid/view/ViewGroup;Z)Landroid/view/View;': () => null,
      'inflate(ILandroid/view/ViewGroup;)Landroid/view/View;': () => null,
    },
  });
  vm.registerNative({ desc: 'Landroid/view/ViewGroup;', superDesc: 'Landroid/view/View;', methods: { '<init>(Landroid/content/Context;)V': (vm2, thr, o, [ctx]) => { o._context = ctx; } } });

  vm.registerNative({
    desc: 'Landroid/os/Vibrator;',
    methods: {
      '<init>()V': () => { },
      'vibrate(J)V': (vm2, thr, o, [ms]) => { try { if (navigator.vibrate) navigator.vibrate(Number(ms)); } catch (e) { } },
      'cancel()V': () => { },
      'hasVibrator()Z': () => 1,
    },
  });

  vm.registerNative({
    desc: 'Landroid/os/PowerManager;',
    methods: {
      '<init>()V': () => { },
      'newWakeLock(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;': (vm2, thr, o, [flags, tag]) => {
        const wl = vm2.newObject(vm2.requireClass('Landroid/os/PowerManager$WakeLock;'));
        wl._held = false;
        return wl;
      },
      'isScreenOn()Z': () => 1,
    },
  });
  vm.registerNative({
    desc: 'Landroid/os/PowerManager$WakeLock;',
    methods: {
      '<init>()V': () => { },
      'acquire()V': (vm2, thr, o) => { o._held = true; },
      'acquire(J)V': (vm2, thr, o, [t]) => { o._held = true; },
      'release()V': (vm2, thr, o) => { o._held = false; },
      'isHeld()Z': (vm2, thr, o) => o._held ? 1 : 0,
      'setReferenceCounted(Z)V': () => { },
    },
  });
  vm.registerNative({
    desc: 'Landroid/media/SoundPool;',
    methods: {
      '<init>(III)V': (vm2, thr, o, [max, st, q]) => { o._sounds = new Map(); o._next = 1; },
      'load(Landroid/content/res/AssetFileDescriptor;I)I': (vm2, thr, o, [afd, prio]) => {
        const id = o._next++;
        o._sounds.set(id, afd);
        return id;
      },
      'load(Landroid/content/Context;II)I': (vm2, thr, o, [ctx, resid, prio]) => {
        const id = o._next++;
        o._sounds.set(id, { _resid: resid | 0 });
        return id;
      },
      'play(IFFIIF)I': (vm2, thr, o, [id, lv, rv, prio, loop, rate]) => host.soundPoolPlay(o, id, lv, loop),
      'stop(I)V': () => { },
      'unload(I)Z': () => 1,
      'release()V': () => { },
      'setVolume(IFF)V': () => { },
    },
  });

}

if (typeof module !== 'undefined') module.exports = { installAndroidNatives };
