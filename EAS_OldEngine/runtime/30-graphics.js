/* =========================================================================
 * 30-graphics.js -- android.graphics.*
 *
 * Bitmap  -> a canvas backed surface (with a lazily synchronised ARGB pixel
 *            buffer so getPixels/setPixels keep working)
 * Canvas  -> CanvasRenderingContext2D + an explicit affine matrix, which
 *            reproduces Android's Canvas transform/clip stack semantics.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, mangle = $rt.mangle;

  /* ------------------------------------------------------------- Bitmap */
  const Bitmap = def('android/graphics/Bitmap', null, {
    ctor() {
      this.w = 0; this.h = 0; this.cv = null; this.ctx = null;
      this.img = null; this.px = null;
      this.pxDirty = false; this.cvDirty = true;
      this.opaque = false; this.tints = null; this.dead = false;
    },
    m: {
      'getWidth()I': function () { return this.w; },
      'getHeight()I': function () { return this.h; },
      'getRowBytes()I': function () { return this.w * 4; },
      'isRecycled()Z': function () { return this.dead ? 1 : 0; },
      'recycle()V': function () {
        this.dead = true; this.cv = null; this.ctx = null;
        this.img = null; this.px = null; this.tints = null;
      },
      'eraseColor(I)V': function (c) {
        if (this.opaque) c |= 0xff000000;
        const ctx = this.ctx;
        flush(this);
        ctx.save();
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = 'copy';
        ctx.fillStyle = css(c);
        ctx.fillRect(0, 0, this.w, this.h);
        ctx.restore();
        this.cvDirty = true;
      },
      'getPixel(II)I': function (x, y) {
        const p = pixels(this);
        return abgr2argb(p[y * this.w + x]);
      },
      'setPixel(III)V': function (x, y, c) {
        const p = pixels(this);
        p[y * this.w + x] = argb2abgr(c);
        this.pxDirty = true;
      },
      'getPixels([IIIIIII)V': function (dst, offset, stride, x, y, w, h) {
        const p = pixels(this);
        for (let j = 0; j < h; j++) {
          const s = (y + j) * this.w + x;
          const d = offset + j * stride;
          for (let i = 0; i < w; i++) dst[d + i] = abgr2argb(p[s + i]);
        }
      },
      'setPixels([IIIIIII)V': function (src, offset, stride, x, y, w, h) {
        const p = pixels(this);
        for (let j = 0; j < h; j++) {
          const d = (y + j) * this.w + x;
          const s = offset + j * stride;
          for (let i = 0; i < w; i++) p[d + i] = argb2abgr(src[s + i]);
        }
        this.pxDirty = true;
      },
      'compress(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z':
        function (fmt, quality, out) {
          const p = pixels(this);
          const png = $img.encodePNG(p, this.w, this.h);
          $rt.invoke(out, 'write([B)V', [new Int8Array(png.buffer, png.byteOffset,
                                                       png.byteLength)]);
          return 1;
        },
      'copy(Landroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;':
        function (cfg) {
          const b = newBitmap(this.w, this.h, this.opaque);
          flush(this);
          b.ctx.drawImage(this.cv, 0, 0);
          b.cvDirty = true;
          return b;
        },
      'isMutable()Z': function () { return 1; },
      'getConfig()Landroid/graphics/Bitmap$Config;': function () {
        return $rt.classes['android/graphics/Bitmap$Config'].s_ARGB_8888;
      },
    },
    s: {
      'createBitmap(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;':
        function (w, h, cfg) {
          return newBitmap(w, h, cfg && cfg.$ename === 'RGB_565');
        },
      'createBitmap(Landroid/graphics/Bitmap;)Landroid/graphics/Bitmap;':
        function (src) {
          const b = newBitmap(src.w, src.h, src.opaque);
          flush(src);
          b.ctx.drawImage(src.cv, 0, 0);
          b.cvDirty = true;
          return b;
        },
      'createScaledBitmap(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;':
        function (src, w, h, filter) {
          const b = newBitmap(w, h, src.opaque);
          flush(src);
          b.ctx.imageSmoothingEnabled = !!filter;
          b.ctx.drawImage(src.cv, 0, 0, src.w, src.h, 0, 0, w, h);
          b.cvDirty = true;
          return b;
        },
      'createBitmap(Landroid/graphics/Bitmap;IIII)Landroid/graphics/Bitmap;':
        function (src, x, y, w, h) {
          const b = newBitmap(w, h, src.opaque);
          flush(src);
          b.ctx.drawImage(src.cv, x, y, w, h, 0, 0, w, h);
          b.cvDirty = true;
          return b;
        },
    },
  });

  function newBitmap(w, h, opaque) {
    w = Math.max(1, w | 0); h = Math.max(1, h | 0);
    const b = new Bitmap();
    b.w = w; b.h = h; b.opaque = !!opaque;
    b.cv = $gfx.create(w, h);
    b.ctx = b.cv.getContext('2d');
    if (opaque) {
      b.ctx.fillStyle = '#000000';
      b.ctx.fillRect(0, 0, w, h);
    }
    return b;
  }
  function pixels(b) {
    if (!b.px || b.cvDirty) {
      b.img = b.ctx.getImageData(0, 0, b.w, b.h);
      b.px = new Uint32Array(b.img.data.buffer, b.img.data.byteOffset,
                             b.w * b.h);
      b.cvDirty = false;
    }
    return b.px;
  }
  function flush(b) {
    if (b.pxDirty) {
      b.ctx.putImageData(b.img, 0, 0);
      b.pxDirty = false;
      b.cvDirty = false;
    }
  }
  function abgr2argb(v) {
    return ((v & 0xff000000) | ((v & 0xff) << 16) | (v & 0xff00) |
            ((v >>> 16) & 0xff)) | 0;
  }
  function argb2abgr(v) {
    return ((v & 0xff000000) | ((v & 0xff) << 16) | (v & 0xff00) |
            ((v >>> 16) & 0xff)) | 0;
  }
  function css(c) {
    const a = (c >>> 24) / 255;
    return 'rgba(' + ((c >> 16) & 255) + ',' + ((c >> 8) & 255) + ',' +
           (c & 255) + ',' + a + ')';
  }
  $rt.gfxNewBitmap = newBitmap;
  $rt.gfxFlush = flush;
  $rt.gfxPixels = pixels;

  /* ------------------------------------------------ BitmapFactory / enums */
  function enumClass(name, values) {
    const C = def(name, 'java/lang/Enum', {});
    values.forEach((v, i) => {
      const o = new C();
      o.$ename = v; o.$eord = i;
      if (!Object.prototype.hasOwnProperty.call(C, '$enums')) C.$enums = [];
      C.$enums.push(o);
      C['s_' + v] = o;
    });
    return C;
  }
  enumClass('android/graphics/Bitmap$Config',
            ['ALPHA_8', 'RGB_565', 'ARGB_4444', 'ARGB_8888']);
  enumClass('android/graphics/Bitmap$CompressFormat', ['JPEG', 'PNG', 'WEBP']);
  enumClass('android/graphics/Paint$Style', ['FILL', 'STROKE', 'FILL_AND_STROKE']);
  enumClass('android/graphics/Region$Op',
            ['DIFFERENCE', 'INTERSECT', 'UNION', 'XOR', 'REVERSE_DIFFERENCE',
             'REPLACE']);

  def('android/graphics/BitmapFactory', null, {
    s: {
      'decodeByteArray([BII)Landroid/graphics/Bitmap;': function (data, off, len) {
        return decodeBytes(data, off, len);
      },
      'decodeByteArray([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;':
        function (data, off, len) { return decodeBytes(data, off, len); },
      'decodeStream(Ljava/io/InputStream;)Landroid/graphics/Bitmap;': function (is) {
        const bos = new ($rt.classes['java/io/ByteArrayOutputStream'])();
        const buf = new Int8Array(4096);
        for (;;) {
          const n = $rt.invoke(is, 'read([B)I', [buf]);
          if (n <= 0) break;
          $rt.invoke(bos, 'write([BII)V', [buf, 0, n]);
        }
        const all = $rt.invoke(bos, 'toByteArray()[B', []);
        return decodeBytes(all, 0, all.length);
      },
    },
  });
  def('android/graphics/BitmapFactory$Options', null, {
    ctor() { this.f_inSampleSize = 1; this.f_inPreferredConfig = null;
             this.f_inJustDecodeBounds = 0; this.f_outWidth = 0; this.f_outHeight = 0; },
    m: { '<init>()V': function () { return this; } },
  });

  function decodeBytes(data, off, len) {
    const u = $rt.u8(data).subarray(off, off + len);
    let img = null;
    try {
      if (u[0] === 0x89 && u[1] === 0x50) img = $img.decodePNG(u);
      else if ($gfx.decodeSync) img = $gfx.decodeSync(u);
    } catch (e) {
      console.error('[BitmapFactory] decode failed', e);
      return null;
    }
    if (!img) {
      console.error('[BitmapFactory] unsupported image format: ' +
                    Array.from(u.subarray(0, 4)).map((x) => x.toString(16)).join(' '));
      return null;
    }
    const b = newBitmap(img.width, img.height, false);
    const id = b.ctx.createImageData(img.width, img.height);
    new Uint32Array(id.data.buffer).set(img.pixels);
    b.ctx.putImageData(id, 0, 0);
    b.cvDirty = true;
    return b;
  }

  /* -------------------------------------------------------------- Color */
  def('android/graphics/Color', null, {
    sf: { BLACK: 0xff000000 | 0, WHITE: -1, RED: 0xffff0000 | 0,
          GREEN: 0xff00ff00 | 0, BLUE: 0xff0000ff | 0, TRANSPARENT: 0 },
    s: {
      'argb(IIII)I': (a, r, g, b) => ((a << 24) | (r << 16) | (g << 8) | b) | 0,
      'rgb(III)I': (r, g, b) => (0xff000000 | (r << 16) | (g << 8) | b) | 0,
      'alpha(I)I': (c) => (c >>> 24),
      'red(I)I': (c) => (c >> 16) & 255,
      'green(I)I': (c) => (c >> 8) & 255,
      'blue(I)I': (c) => c & 255,
    },
  });

  /* --------------------------------------------------------- Rect/RectF */
  def('android/graphics/Rect', null, {
    ctor() { this.f_left = 0; this.f_top = 0; this.f_right = 0; this.f_bottom = 0; },
    m: {
      '<init>()V': function () { return this; },
      '<init>(IIII)V': function (l, t, r, b) {
        this.f_left = l; this.f_top = t; this.f_right = r; this.f_bottom = b;
        return this;
      },
      'set(IIII)V': function (l, t, r, b) {
        this.f_left = l; this.f_top = t; this.f_right = r; this.f_bottom = b;
      },
      'width()I': function () { return this.f_right - this.f_left; },
      'height()I': function () { return this.f_bottom - this.f_top; },
      'isEmpty()Z': function () {
        return (this.f_left >= this.f_right || this.f_top >= this.f_bottom) ? 1 : 0;
      },
      'contains(II)Z': function (x, y) {
        return (x >= this.f_left && x < this.f_right &&
                y >= this.f_top && y < this.f_bottom) ? 1 : 0;
      },
      'toString()Ljava/lang/String;': function () {
        return 'Rect(' + this.f_left + ', ' + this.f_top + ' - ' +
               this.f_right + ', ' + this.f_bottom + ')';
      },
    },
  });
  def('android/graphics/RectF', null, {
    ctor() { this.f_left = 0; this.f_top = 0; this.f_right = 0; this.f_bottom = 0; },
    m: {
      '<init>()V': function () { return this; },
      '<init>(FFFF)V': function (l, t, r, b) {
        this.f_left = l; this.f_top = t; this.f_right = r; this.f_bottom = b;
        return this;
      },
      'set(FFFF)V': function (l, t, r, b) {
        this.f_left = l; this.f_top = t; this.f_right = r; this.f_bottom = b;
      },
      'width()F': function () { return Math.fround(this.f_right - this.f_left); },
      'height()F': function () { return Math.fround(this.f_bottom - this.f_top); },
      'offset(FF)V': function (dx, dy) {
        this.f_left += dx; this.f_right += dx; this.f_top += dy; this.f_bottom += dy;
      },
    },
  });

  /* ------------------------------------------------------------- Matrix */
  const Matrix = def('android/graphics/Matrix', null, {
    ctor() { this.a = 1; this.b = 0; this.c = 0; this.d = 1; this.e = 0; this.f = 0; },
    m: {
      '<init>()V': function () { return this; },
      '<init>(Landroid/graphics/Matrix;)V': function (o) { copyM(o, this); return this; },
      'reset()V': function () {
        this.a = 1; this.b = 0; this.c = 0; this.d = 1; this.e = 0; this.f = 0;
      },
      'set(Landroid/graphics/Matrix;)V': function (o) { copyM(o, this); },
      'postTranslate(FF)Z': function (dx, dy) {
        this.e += dx; this.f += dy; return 1;
      },
      'postScale(FF)Z': function (sx, sy) {
        this.a *= sx; this.c *= sx; this.e *= sx;
        this.b *= sy; this.d *= sy; this.f *= sy;
        return 1;
      },
      'postScale(FFFF)Z': function (sx, sy, px, py) {
        this.e -= px; this.f -= py;
        this.a *= sx; this.c *= sx; this.e *= sx;
        this.b *= sy; this.d *= sy; this.f *= sy;
        this.e += px; this.f += py;
        return 1;
      },
      'postRotate(F)Z': function (deg) {
        const r = deg * Math.PI / 180, cs = Math.cos(r), sn = Math.sin(r);
        const a = this.a, b = this.b, c = this.c, d = this.d, e = this.e, f = this.f;
        this.a = cs * a - sn * b; this.c = cs * c - sn * d; this.e = cs * e - sn * f;
        this.b = sn * a + cs * b; this.d = sn * c + cs * d; this.f = sn * e + cs * f;
        return 1;
      },
      'postRotate(FFF)Z': function (deg, px, py) {
        this[mangle('postTranslate(FF)Z')](-px, -py);
        this[mangle('postRotate(F)Z')](deg);
        this[mangle('postTranslate(FF)Z')](px, py);
        return 1;
      },
      'preTranslate(FF)Z': function (dx, dy) {
        this.e += this.a * dx + this.c * dy;
        this.f += this.b * dx + this.d * dy;
        return 1;
      },
      'preScale(FF)Z': function (sx, sy) {
        this.a *= sx; this.b *= sx; this.c *= sy; this.d *= sy; return 1;
      },
      'setTranslate(FF)V': function (dx, dy) {
        this.a = 1; this.b = 0; this.c = 0; this.d = 1; this.e = dx; this.f = dy;
      },
      'setScale(FF)V': function (sx, sy) {
        this.a = sx; this.b = 0; this.c = 0; this.d = sy; this.e = 0; this.f = 0;
      },
      'toString()Ljava/lang/String;': function () {
        return 'Matrix{[' + this.a + ',' + this.c + ',' + this.e + '][' +
               this.b + ',' + this.d + ',' + this.f + ']}';
      },
    },
  });
  function copyM(s, d) {
    d.a = s.a; d.b = s.b; d.c = s.c; d.d = s.d; d.e = s.e; d.f = s.f;
  }

  /* ------------------------------------------------------- ColorFilter */
  def('android/graphics/ColorFilter', null, {});
  def('android/graphics/LightingColorFilter', 'android/graphics/ColorFilter', {
    ctor() { this.mul = 0xffffff; this.add = 0; },
    m: {
      '<init>(II)V': function (mul, add) {
        this.mul = mul; this.add = add; return this;
      },
    },
  });

  /* ----------------------------------------------------------- Typeface */
  const Typeface = def('android/graphics/Typeface', null, {
    ctor() { this.family = 'sans-serif'; this.style = 0; },
    m: {
      'getStyle()I': function () { return this.style; },
      'isBold()Z': function () { return this.style & 1 ? 1 : 0; },
      'isItalic()Z': function () { return this.style & 2 ? 1 : 0; },
    },
    s: {
      'create(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;':
        function (tf, style) { return mkTypeface(tf ? tf.family : 'sans-serif', style); },
      'create(Ljava/lang/String;I)Landroid/graphics/Typeface;':
        function (fam, style) { return mkTypeface(fam || 'sans-serif', style); },
      'defaultFromStyle(I)Landroid/graphics/Typeface;':
        function (style) { return mkTypeface('sans-serif', style); },
    },
  });
  function mkTypeface(family, style) {
    const t = new Typeface();
    t.family = family; t.style = style | 0;
    return t;
  }
  Typeface.s_DEFAULT = mkTypeface('sans-serif', 0);
  Typeface.s_DEFAULT_BOLD = mkTypeface('sans-serif', 1);
  Typeface.s_SANS_SERIF = mkTypeface('sans-serif', 0);
  Typeface.s_SERIF = mkTypeface('serif', 0);
  Typeface.s_MONOSPACE = mkTypeface('monospace', 0);
  Typeface.s_NORMAL = 0;
  Typeface.s_BOLD = 1;
  Typeface.s_ITALIC = 2;
  Typeface.s_BOLD_ITALIC = 3;

  /* -------------------------------------------------------------- Paint */
  const Paint = def('android/graphics/Paint', null, {
    ctor() {
      this.color = 0xff000000 | 0;
      this.style = 0;
      this.strokeWidth = 0;
      this.textSize = 12;
      this.textScaleX = 1;
      this.antiAlias = false;
      this.filterBitmap = false;
      this.typeface = Typeface.s_DEFAULT;
      this.colorFilter = null;
      this.textAlign = 0;
      this.$font = null;
    },
    m: {
      '<init>()V': function () { return this; },
      '<init>(I)V': function () { return this; },
      'setColor(I)V': function (c) { this.color = c | 0; },
      'getColor()I': function () { return this.color; },
      'setAlpha(I)V': function (a) {
        this.color = ((a & 255) << 24) | (this.color & 0xffffff);
      },
      'getAlpha()I': function () { return this.color >>> 24; },
      'setAntiAlias(Z)V': function (v) { this.antiAlias = !!v; },
      'setFilterBitmap(Z)V': function (v) { this.filterBitmap = !!v; },
      'setDither(Z)V': function () {},
      'setLinearText(Z)V': function () {},
      'setSubpixelText(Z)V': function () {},
      'setFakeBoldText(Z)V': function () {},
      'setStrokeWidth(F)V': function (w) { this.strokeWidth = w; },
      'getStrokeWidth()F': function () { return this.strokeWidth; },
      'setStyle(Landroid/graphics/Paint$Style;)V': function (s) {
        this.style = s ? s.$eord : 0;
      },
      'setTextSize(F)V': function (s) { this.textSize = s; this.$font = null; },
      'getTextSize()F': function () { return this.textSize; },
      'setTextScaleX(F)V': function (s) { this.textScaleX = s; },
      'getTextScaleX()F': function () { return this.textScaleX; },
      'setTypeface(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;':
        function (t) { this.typeface = t; this.$font = null; return t; },
      'getTypeface()Landroid/graphics/Typeface;': function () { return this.typeface; },
      'setColorFilter(Landroid/graphics/ColorFilter;)Landroid/graphics/ColorFilter;':
        function (f) { this.colorFilter = f; return f; },
      'getColorFilter()Landroid/graphics/ColorFilter;': function () {
        return this.colorFilter;
      },
      'setTextAlign(Landroid/graphics/Paint$Align;)V': function (a) {
        this.textAlign = a ? a.$eord : 0;
      },
      'measureText(Ljava/lang/String;)F': function (s) {
        return Math.fround(measure(this, s));
      },
      'measureText(Ljava/lang/String;II)F': function (s, a, b) {
        return Math.fround(measure(this, s.substring(a, b)));
      },
      'getTextWidths(Ljava/lang/String;[F)I': function (s, out) {
        const ctx = $gfx.measureCtx();
        ctx.font = fontOf(this);
        for (let i = 0; i < s.length; i++) {
          out[i] = Math.fround(ctx.measureText(s.charAt(i)).width * this.textScaleX);
        }
        return s.length;
      },
      'ascent()F': function () {
        const m = fontMetrics(this);
        return Math.fround(-m.ascent);
      },
      'descent()F': function () {
        const m = fontMetrics(this);
        return Math.fround(m.descent);
      },
      'getFontSpacing()F': function () {
        const m = fontMetrics(this);
        return Math.fround(m.ascent + m.descent);
      },
      'reset()V': function () {
        this.color = 0xff000000 | 0; this.style = 0; this.colorFilter = null;
      },
      'set(Landroid/graphics/Paint;)V': function (o) {
        this.color = o.color; this.style = o.style; this.textSize = o.textSize;
        this.typeface = o.typeface; this.textScaleX = o.textScaleX;
        this.antiAlias = o.antiAlias; this.colorFilter = o.colorFilter;
        this.$font = null;
      },
    },
  });
  enumClass('android/graphics/Paint$Align', ['LEFT', 'CENTER', 'RIGHT']);

  function fontOf(p) {
    if (!p.$font) {
      const st = p.typeface ? p.typeface.style : 0;
      p.$font = (st & 2 ? 'italic ' : '') + (st & 1 ? 'bold ' : '') +
                p.textSize.toFixed(2) + 'px ' +
                ((p.typeface && p.typeface.family) || 'sans-serif');
    }
    return p.$font;
  }
  function measure(p, s) {
    if (s === null || s === '') return 0;
    const ctx = $gfx.measureCtx();
    ctx.font = fontOf(p);
    return ctx.measureText(s).width * p.textScaleX;
  }
  const metricCache = Object.create(null);
  function fontMetrics(p) {
    const f = fontOf(p);
    let m = metricCache[f];
    if (!m) {
      const ctx = $gfx.measureCtx();
      ctx.font = f;
      let asc = p.textSize * 0.9, desc = p.textSize * 0.24;
      try {
        const tm = ctx.measureText('Hg');
        if (tm.fontBoundingBoxAscent) {
          asc = tm.fontBoundingBoxAscent;
          desc = tm.fontBoundingBoxDescent;
        }
      } catch (e) { /* keep defaults */ }
      m = { ascent: asc, descent: desc };
      metricCache[f] = m;
    }
    return m;
  }

  /* ------------------------------------------------------------- Canvas */
  const Canvas = def('android/graphics/Canvas', null, {
    ctor() {
      this.bmp = null; this.ctx = null;
      this.a = 1; this.b = 0; this.c = 0; this.d = 1; this.e = 0; this.f = 0;
      this.clipped = false;
      this.w = 0; this.h = 0;
    },
    m: {
      '<init>()V': function () { return this; },
      '<init>(Landroid/graphics/Bitmap;)V': function (bm) {
        this.bmp = bm; this.ctx = bm.ctx; this.w = bm.w; this.h = bm.h;
        return this;
      },
      'setBitmap(Landroid/graphics/Bitmap;)V': function (bm) {
        this.bmp = bm; this.ctx = bm.ctx; this.w = bm.w; this.h = bm.h;
      },
      'getWidth()I': function () { return this.w; },
      'getHeight()I': function () { return this.h; },
      'save()I': function () {
        (this.$stack || (this.$stack = [])).push([this.a, this.b, this.c,
                                                  this.d, this.e, this.f]);
        return this.$stack.length;
      },
      'restore()V': function () {
        const s = this.$stack && this.$stack.pop();
        if (s) { this.a = s[0]; this.b = s[1]; this.c = s[2];
                 this.d = s[3]; this.e = s[4]; this.f = s[5]; }
      },
      'translate(FF)V': function (dx, dy) {
        this.e += this.a * dx + this.c * dy;
        this.f += this.b * dx + this.d * dy;
      },
      'scale(FF)V': function (sx, sy) {
        this.a *= sx; this.b *= sx; this.c *= sy; this.d *= sy;
      },
      'scale(FFFF)V': function (sx, sy, px, py) {
        this[mangle('translate(FF)V')](px, py);
        this[mangle('scale(FF)V')](sx, sy);
        this[mangle('translate(FF)V')](-px, -py);
      },
      'rotate(F)V': function (deg) {
        const r = deg * Math.PI / 180, cs = Math.cos(r), sn = Math.sin(r);
        const a = this.a, b = this.b, c = this.c, d = this.d;
        this.a = a * cs + c * sn; this.b = b * cs + d * sn;
        this.c = -a * sn + c * cs; this.d = -b * sn + d * cs;
      },
      'getMatrix()Landroid/graphics/Matrix;': function () {
        const m = new Matrix();
        copyM(this, m);
        return m;
      },
      'getMatrix(Landroid/graphics/Matrix;)V': function (m) { copyM(this, m); },
      'setMatrix(Landroid/graphics/Matrix;)V': function (m) {
        if (m === null) { this.a = 1; this.b = 0; this.c = 0; this.d = 1; this.e = 0; this.f = 0; }
        else copyM(m, this);
      },
      'concat(Landroid/graphics/Matrix;)V': function (m) {
        const a = this.a * m.a + this.c * m.b;
        const b = this.b * m.a + this.d * m.b;
        const c = this.a * m.c + this.c * m.d;
        const d = this.b * m.c + this.d * m.d;
        const e = this.a * m.e + this.c * m.f + this.e;
        const f = this.b * m.e + this.d * m.f + this.f;
        this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; this.f = f;
      },
      'clipRect(FFFFLandroid/graphics/Region$Op;)Z': function (l, t, r, b, op) {
        const ctx = this.ctx;
        if (this.clipped) ctx.restore();
        ctx.save();
        ctx.setTransform(this.a, this.b, this.c, this.d, this.e, this.f);
        ctx.beginPath();
        ctx.rect(l, t, r - l, b - t);
        ctx.clip();
        this.clipped = true;
        return 1;
      },
      'clipRect(IIII)Z': function (l, t, r, b) {
        return this[mangle('clipRect(FFFFLandroid/graphics/Region$Op;)Z')](l, t, r, b, null);
      },
      'clipRect(FFFF)Z': function (l, t, r, b) {
        return this[mangle('clipRect(FFFFLandroid/graphics/Region$Op;)Z')](l, t, r, b, null);
      },
      'drawColor(I)V': function (c) {
        const ctx = this.ctx;
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.globalAlpha = 1;
        ctx.fillStyle = css(c);
        ctx.fillRect(0, 0, this.w, this.h);
        this.$dirty();
      },
      'drawRect(FFFFLandroid/graphics/Paint;)V': function (l, t, r, b, p) {
        const ctx = this.pre(p);
        if (p.style === 1) {
          ctx.lineWidth = p.strokeWidth || 1;
          ctx.strokeStyle = css(p.color);
          ctx.strokeRect(l, t, r - l, b - t);
        } else {
          ctx.fillStyle = css(p.color);
          ctx.fillRect(l, t, r - l, b - t);
          if (p.style === 2) {
            ctx.lineWidth = p.strokeWidth || 1;
            ctx.strokeStyle = css(p.color);
            ctx.strokeRect(l, t, r - l, b - t);
          }
        }
        this.$dirty();
      },
      'drawRect(Landroid/graphics/RectF;Landroid/graphics/Paint;)V': function (rc, p) {
        this[mangle('drawRect(FFFFLandroid/graphics/Paint;)V')](
          rc.f_left, rc.f_top, rc.f_right, rc.f_bottom, p);
      },
      'drawLine(FFFFLandroid/graphics/Paint;)V': function (x0, y0, x1, y1, p) {
        const ctx = this.pre(p);
        ctx.strokeStyle = css(p.color);
        ctx.lineWidth = p.strokeWidth || 1;
        ctx.beginPath();
        ctx.moveTo(x0, y0);
        ctx.lineTo(x1, y1);
        ctx.stroke();
        this.$dirty();
      },
      'drawArc(Landroid/graphics/RectF;FFZLandroid/graphics/Paint;)V':
        function (rc, start, sweep, useCenter, p) {
          const ctx = this.pre(p);
          const cx = (rc.f_left + rc.f_right) / 2, cy = (rc.f_top + rc.f_bottom) / 2;
          const rx = (rc.f_right - rc.f_left) / 2, ry = (rc.f_bottom - rc.f_top) / 2;
          ctx.beginPath();
          if (useCenter) ctx.moveTo(cx, cy);
          ctx.ellipse(cx, cy, Math.abs(rx), Math.abs(ry), 0,
                      start * Math.PI / 180, (start + sweep) * Math.PI / 180,
                      sweep < 0);
          if (useCenter) ctx.closePath();
          if (p.style === 1) {
            ctx.strokeStyle = css(p.color);
            ctx.lineWidth = p.strokeWidth || 1;
            ctx.stroke();
          } else {
            ctx.fillStyle = css(p.color);
            ctx.fill();
          }
          this.$dirty();
        },
      'drawCircle(FFFLandroid/graphics/Paint;)V': function (x, y, r, p) {
        const ctx = this.pre(p);
        ctx.beginPath();
        ctx.arc(x, y, r, 0, Math.PI * 2);
        if (p.style === 1) {
          ctx.strokeStyle = css(p.color);
          ctx.lineWidth = p.strokeWidth || 1;
          ctx.stroke();
        } else {
          ctx.fillStyle = css(p.color);
          ctx.fill();
        }
        this.$dirty();
      },
      'drawBitmap(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V':
        function (bm, x, y, p) {
          if (!bm || bm.dead) return;
          const src = source(bm, p);
          const ctx = this.pre(p);
          ctx.drawImage(src, x, y);
          this.$dirty();
        },
      'drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Matrix;Landroid/graphics/Paint;)V':
        function (bm, m, p) {
          if (!bm || bm.dead) return;
          const src = source(bm, p);
          const ctx = this.pre(p, m);
          ctx.drawImage(src, 0, 0);
          this.$dirty();
        },
      'drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V':
        function (bm, sr, dr, p) {
          if (!bm || bm.dead) return;
          const src = source(bm, p);
          const ctx = this.pre(p);
          if (sr === null) {
            ctx.drawImage(src, dr.f_left, dr.f_top,
                          dr.f_right - dr.f_left, dr.f_bottom - dr.f_top);
          } else {
            const sw = sr.f_right - sr.f_left, sh = sr.f_bottom - sr.f_top;
            if (sw <= 0 || sh <= 0) return;
            ctx.drawImage(src, sr.f_left, sr.f_top, sw, sh,
                          dr.f_left, dr.f_top,
                          dr.f_right - dr.f_left, dr.f_bottom - dr.f_top);
          }
          this.$dirty();
        },
      'drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/Rect;Landroid/graphics/Paint;)V':
        function (bm, sr, dr, p) {
          this[mangle('drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V')](bm, sr, dr, p);
        },
      'drawText(Ljava/lang/String;FFLandroid/graphics/Paint;)V':
        function (s, x, y, p) {
          if (s === null || s === '') return;
          const ctx = this.pre(p, null, p.textScaleX);
          ctx.fillStyle = css(p.color);
          ctx.font = fontOf(p);
          ctx.textBaseline = 'alphabetic';
          ctx.fillText(s, p.textScaleX !== 1 ? x / p.textScaleX : x, y);
          this.$dirty();
        },
      'drawText(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V':
        function (s, start, end, x, y, p) {
          this[mangle('drawText(Ljava/lang/String;FFLandroid/graphics/Paint;)V')](
            s.substring(start, end), x, y, p);
        },
      'drawText([CIIFFLandroid/graphics/Paint;)V':
        function (chars, start, count, x, y, p) {
          let s = '';
          for (let i = 0; i < count; i++) s += String.fromCharCode(chars[start + i]);
          this[mangle('drawText(Ljava/lang/String;FFLandroid/graphics/Paint;)V')](s, x, y, p);
        },
    },
  });

  Canvas.prototype.pre = function (p, extra, xscale) {
    const ctx = this.ctx;
    let a = this.a, b = this.b, c = this.c, d = this.d, e = this.e, f = this.f;
    if (extra) {
      const m = extra;
      const na = a * m.a + c * m.b;
      const nb = b * m.a + d * m.b;
      const nc = a * m.c + c * m.d;
      const nd = b * m.c + d * m.d;
      const ne = a * m.e + c * m.f + e;
      const nf = b * m.e + d * m.f + f;
      a = na; b = nb; c = nc; d = nd; e = ne; f = nf;
    }
    if (xscale && xscale !== 1) { a *= xscale; b *= xscale; }
    ctx.setTransform(a, b, c, d, e, f);
    ctx.globalAlpha = p ? (p.color >>> 24) / 255 : 1;
    ctx.imageSmoothingEnabled = p ? !!p.filterBitmap : false;
    return ctx;
  };
  Canvas.prototype.$dirty = function () {
    if (this.bmp) { this.bmp.cvDirty = true; this.bmp.pxDirty = false; }
  };

  /** returns the drawable source for a bitmap, applying a colour filter */
  function source(bm, p) {
    flush(bm);
    const cf = p && p.colorFilter;
    if (!cf) return bm.cv;
    const key = (cf.mul >>> 0) + '_' + (cf.add >>> 0);
    if (!bm.tints) bm.tints = new Map();
    let t = bm.tints.get(key);
    if (!t) {
      const px = pixels(bm);
      const cv = $gfx.create(bm.w, bm.h);
      const ctx = cv.getContext('2d');
      const id = ctx.createImageData(bm.w, bm.h);
      const out = new Uint32Array(id.data.buffer);
      const mr = (cf.mul >> 16) & 255, mg = (cf.mul >> 8) & 255, mb = cf.mul & 255;
      const ar = (cf.add >> 16) & 255, ag = (cf.add >> 8) & 255, ab = cf.add & 255;
      for (let i = 0; i < px.length; i++) {
        const v = px[i];
        const a = v & 0xff000000;
        let r = ((v & 255) * mr / 255 + ar) | 0;
        let g = (((v >> 8) & 255) * mg / 255 + ag) | 0;
        let bl = (((v >> 16) & 255) * mb / 255 + ab) | 0;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (bl > 255) bl = 255;
        out[i] = a | (bl << 16) | (g << 8) | r;
      }
      ctx.putImageData(id, 0, 0);
      t = cv;
      if (bm.tints.size > 12) bm.tints.clear();
      bm.tints.set(key, t);
    }
    return t;
  }
})($rt);
