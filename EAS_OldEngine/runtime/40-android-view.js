/* =========================================================================
 * 40-android-view.js -- android.view.*, android.widget.*, android.text.*
 *
 * SurfaceView is the game screen: it owns the on-screen <canvas>, and its
 * SurfaceHolder hands out an android.graphics.Canvas bound to that canvas'
 * 2D context, which is exactly the contract the game's render loop uses
 * (lockCanvas / draw / unlockCanvasAndPost).
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  /* ------------------------------------------------------------- Display */
  const Display = def('android/view/Display', null, {
    ctor() { this.w = 480; this.h = 800; this.density = 1.5; this.rotation = 0; },
    m: {
      'getWidth()I': function () { return this.w; },
      'getHeight()I': function () { return this.h; },
      'getOrientation()I': function () { return this.rotation; },
      'getRotation()I': function () { return this.rotation; },
      'getMetrics(Landroid/util/DisplayMetrics;)V': function (dm) {
        dm.f_widthPixels = this.w;
        dm.f_heightPixels = this.h;
        dm.f_density = this.density;
        dm.f_densityDpi = Math.round(this.density * 160);
        dm.f_scaledDensity = this.density;
        dm.f_xdpi = this.density * 160;
        dm.f_ydpi = this.density * 160;
      },
    },
  });
  $rt.display = new Display();

  def('android/view/WindowManager', null, {
    m: {
      'getDefaultDisplay()Landroid/view/Display;': function () { return $rt.display; },
    },
    impl: ['android/view/ViewManager'],
  });
  iface('android/view/ViewManager');
  $rt.windowManager = new ($rt.classes['android/view/WindowManager'])();

  /* ---------------------------------------------------------------- View */
  const View = def('android/view/View', null, {
    ctor() {
      this.$el = null; this.$ctx = null;
      this.$w = 0; this.$h = 0; this.$vis = 0; this.$parent = null;
    },
    m: {
      '<init>(Landroid/content/Context;)V': function () { return this; },
      'getWidth()I': function () { return this.$w; },
      'getHeight()I': function () { return this.$h; },
      'setFocusable(Z)V': function () {},
      'setFocusableInTouchMode(Z)V': function () {},
      'setKeepScreenOn(Z)V': function () {},
      'requestFocus()Z': function () { return 1; },
      'invalidate()V': function () {},
      'postInvalidate()V': function () {},
      'setVisibility(I)V': function (v) {
        this.$vis = v;
        if (this.$el) this.$el.style.display = v === 0 ? '' : 'none';
      },
      'getVisibility()I': function () { return this.$vis; },
      'setBackgroundColor(I)V': function (c) {
        if (this.$el) {
          this.$el.style.background = 'rgba(' + ((c >> 16) & 255) + ',' +
            ((c >> 8) & 255) + ',' + (c & 255) + ',' + ((c >>> 24) / 255) + ')';
        }
      },
      'getWindowVisibleDisplayFrame(Landroid/graphics/Rect;)V': function (r) {
        r.f_left = 0;
        r.f_top = 0;
        r.f_right = $rt.display.w;
        r.f_bottom = $rt.display.h;
      },
      'getContext()Landroid/content/Context;': function () {
        return $rt.classes['kairo/android/ui/IApplication'] ?
          $rt.classes['kairo/android/ui/IApplication'].s_e : null;
      },
      'onTouchEvent(Landroid/view/MotionEvent;)Z': function () { return 0; },
      'onKeyDown(ILandroid/view/KeyEvent;)Z': function () { return 0; },
      'onKeyUp(ILandroid/view/KeyEvent;)Z': function () { return 0; },
    },
  });

  const ViewGroup = def('android/view/ViewGroup', 'android/view/View', {
    ctor() {
      /* superclass field init runs via super() */
      this.$children = [];
    },
    m: {
      '<init>(Landroid/content/Context;)V': function () { return this; },
      'addView(Landroid/view/View;)V': function (v) {
        this.$children.push(v);
        v.$parent = this;
        if (this.$el && v.$el) this.$el.appendChild(v.$el);
      },
      'removeView(Landroid/view/View;)V': function (v) {
        this.$children = this.$children.filter((c) => c !== v);
        if (this.$el && v.$el && v.$el.parentNode === this.$el) {
          this.$el.removeChild(v.$el);
        }
      },
      'getChildCount()I': function () { return this.$children.length; },
    },
    impl: ['android/view/ViewManager'],
  });

  def('android/widget/FrameLayout', 'android/view/ViewGroup', {
    ctor() {},
    m: {
      '<init>(Landroid/content/Context;)V': function () {
        if (typeof document !== 'undefined') {
          this.$el = document.createElement('div');
          this.$el.className = 'eas-frame';
        }
        return this;
      },
    },
  });

  /* -------------------------------------------------------------- Window */
  def('android/view/Window', null, {
    ctor() { this.$flags = 0; this.$decor = null; },
    m: {
      'addFlags(I)V': function (f) { this.$flags |= f; },
      'clearFlags(I)V': function (f) { this.$flags &= ~f; },
      'setFlags(II)V': function (f, m) { this.$flags = (this.$flags & ~m) | (f & m); },
      'requestFeature(I)Z': function () { return 1; },
      'setBackgroundDrawable(Landroid/graphics/drawable/Drawable;)V': function () {},
      'getDecorView()Landroid/view/View;': function () {
        if (!this.$decor) this.$decor = new View();
        return this.$decor;
      },
    },
  });

  /* ------------------------------------------------------------ KeyEvent */
  def('android/view/KeyEvent', null, {
    ctor() { this.$action = 0; this.$code = 0; this.$repeat = 0; },
    m: {
      '<init>(II)V': function (a, c) { this.$action = a; this.$code = c; return this; },
      'getAction()I': function () { return this.$action; },
      'getKeyCode()I': function () { return this.$code; },
      'getRepeatCount()I': function () { return this.$repeat; },
      'getUnicodeChar()I': function () { return 0; },
      'isShiftPressed()Z': function () { return 0; },
      'getDownTime()J': function () { return 0n; },
      'getEventTime()J': function () { return 0n; },
    },
    sf: { ACTION_DOWN: 0, ACTION_UP: 1, ACTION_MULTIPLE: 2 },
  });
  $rt.newKeyEvent = function (action, code, repeat) {
    const e = new ($rt.classes['android/view/KeyEvent'])();
    e.$action = action; e.$code = code; e.$repeat = repeat || 0;
    return e;
  };

  /* --------------------------------------------------------- MotionEvent */
  def('android/view/MotionEvent', null, {
    ctor() { this.$action = 0; this.$pts = []; },
    m: {
      'getAction()I': function () { return this.$action; },
      'getActionMasked()I': function () { return this.$action & 0xff; },
      'getActionIndex()I': function () { return (this.$action >> 8) & 0xff; },
      'getPointerCount()I': function () { return this.$pts.length; },
      'getPointerId(I)I': function (i) {
        const p = this.$pts[i];
        return p ? p.id : 0;
      },
      'findPointerIndex(I)I': function (id) {
        for (let i = 0; i < this.$pts.length; i++) {
          if (this.$pts[i].id === id) return i;
        }
        return -1;
      },
      'getX()F': function () { return this.$pts.length ? this.$pts[0].x : 0; },
      'getY()F': function () { return this.$pts.length ? this.$pts[0].y : 0; },
      'getX(I)F': function (i) { return this.$pts[i] ? this.$pts[i].x : 0; },
      'getY(I)F': function (i) { return this.$pts[i] ? this.$pts[i].y : 0; },
      'getPressure()F': function () { return 1; },
      'getEventTime()J': function () { return BigInt($rt.now()); },
      'recycle()V': function () {},
    },
    sf: {
      ACTION_DOWN: 0, ACTION_UP: 1, ACTION_MOVE: 2, ACTION_CANCEL: 3,
      ACTION_POINTER_DOWN: 5, ACTION_POINTER_UP: 6,
      ACTION_MASK: 255, ACTION_POINTER_INDEX_SHIFT: 8,
    },
  });
  $rt.newMotionEvent = function (action, pts) {
    const e = new ($rt.classes['android/view/MotionEvent'])();
    e.$action = action;
    e.$pts = pts;
    return e;
  };

  /* ------------------------------------------------------- SurfaceHolder */
  iface('android/view/SurfaceHolder$Callback');

  const SurfaceHolder = def('android/view/SurfaceHolder', null, {
    ctor() {
      this.$cbs = [];
      this.$view = null;
      this.$canvas = null;
      this.$created = false;
    },
    m: {
      'addCallback(Landroid/view/SurfaceHolder$Callback;)V': function (cb) {
        this.$cbs.push(cb);
        if (this.$created) fireCreated(this, cb);
      },
      'removeCallback(Landroid/view/SurfaceHolder$Callback;)V': function (cb) {
        this.$cbs = this.$cbs.filter((c) => c !== cb);
      },
      'setFixedSize(II)V': function () {},
      'setFormat(I)V': function () {},
      'setType(I)V': function () {},
      'getSurfaceFrame()Landroid/graphics/Rect;': function () {
        const r = new ($rt.classes['android/graphics/Rect'])();
        r.f_right = this.$view ? this.$view.$w : 0;
        r.f_bottom = this.$view ? this.$view.$h : 0;
        return r;
      },
      'lockCanvas()Landroid/graphics/Canvas;': function () {
        return lockCanvas(this);
      },
      'lockCanvas(Landroid/graphics/Rect;)Landroid/graphics/Canvas;': function () {
        return lockCanvas(this);
      },
      'unlockCanvasAndPost(Landroid/graphics/Canvas;)V': function (c) {
        if (!c) return;
        if (c.clipped) { c.ctx.restore(); c.clipped = false; }
        c.ctx.setTransform(1, 0, 0, 1, 0, 0);
        if (this.$view && this.$view.$present) this.$view.$present();
      },
    },
  });

  function lockCanvas(holder) {
    const v = holder.$view;
    if (!v || !v.$ctx) return null;
    let c = holder.$canvas;
    if (!c) {
      c = new ($rt.classes['android/graphics/Canvas'])();
      holder.$canvas = c;
    }
    c.ctx = v.$ctx;
    c.bmp = null;
    c.w = v.$w;
    c.h = v.$h;
    c.a = 1; c.b = 0; c.c = 0; c.d = 1; c.e = 0; c.f = 0;
    if (c.clipped) { c.ctx.restore(); c.clipped = false; }
    c.$stack = [];
    c.ctx.setTransform(1, 0, 0, 1, 0, 0);
    c.ctx.globalCompositeOperation = 'source-over';
    c.ctx.globalAlpha = 1;
    return c;
  }

  function fireCreated(holder, cb) {
    $rt.invoke(cb, 'surfaceCreated(Landroid/view/SurfaceHolder;)V', [holder]);
    $rt.invoke(cb, 'surfaceChanged(Landroid/view/SurfaceHolder;III)V',
               [holder, 4 /* RGBA_8888 */, holder.$view.$w, holder.$view.$h]);
  }

  /* --------------------------------------------------------- SurfaceView */
  def('android/view/SurfaceView', 'android/view/View', {
    ctor() { this.$holder = null; },
    m: {
      '<init>(Landroid/content/Context;)V': function () {
        const s = $host.surface;
        this.$w = s ? s.width : $rt.display.w;
        this.$h = s ? s.height : $rt.display.h;
        this.$el = s || null;
        this.$ctx = s ? s.getContext('2d', { alpha: false }) : null;
        if (this.$ctx) this.$ctx.imageSmoothingEnabled = false;
        const h = new SurfaceHolder();
        h.$view = this;
        h.$created = true;
        this.$holder = h;
        $host.gameView = this;
        return this;
      },
      'getHolder()Landroid/view/SurfaceHolder;': function () { return this.$holder; },
    },
  });

  /* ------------------------------------------------------ text / widgets */
  def('android/text/Editable', null, {
    ctor() { this.$owner = null; },
    m: {
      'toString()Ljava/lang/String;': function () {
        return this.$owner ? this.$owner.$value() : '';
      },
      'length()I': function () {
        return this.$owner ? this.$owner.$value().length : 0;
      },
      'clear()V': function () { if (this.$owner) this.$owner.$set(''); },
    },
    impl: ['java/lang/CharSequence'],
  });

  def('android/widget/TextView', 'android/view/View', {
    ctor() { this.$text = ''; },
    m: {
      '<init>(Landroid/content/Context;)V': function () { return this; },
      'setText(Ljava/lang/CharSequence;)V': function (t) { this.$set($rt.jToString(t)); },
      'getText()Landroid/text/Editable;': function () {
        const e = new ($rt.classes['android/text/Editable'])();
        e.$owner = this;
        return e;
      },
      'setTextSize(F)V': function () {},
      'setSingleLine()V': function () {},
    },
  });

  const EditText = def('android/widget/EditText', 'android/widget/TextView', {
    ctor() { this.$text = ''; },
    m: {
      '<init>(Landroid/content/Context;)V': function () {
        if (typeof document !== 'undefined') {
          this.$el = document.createElement('input');
          this.$el.type = 'text';
          this.$el.className = 'eas-edit';
        }
        return this;
      },
    },
  });
  EditText.prototype.$value = function () {
    return this.$el ? this.$el.value : this.$text;
  };
  EditText.prototype.$set = function (v) {
    this.$text = v;
    if (this.$el) this.$el.value = v;
  };
  $rt.classes['android/widget/TextView'].prototype.$value = function () {
    return this.$text;
  };
  $rt.classes['android/widget/TextView'].prototype.$set = function (v) {
    this.$text = v;
    if (this.$el) this.$el.textContent = v;
  };

  def('android/widget/Toast', null, {
    ctor() { this.$msg = ''; },
    m: {
      'show()V': function () {
        if (typeof document === 'undefined') { $host.log('[toast] ' + this.$msg); return; }
        const el = document.createElement('div');
        el.className = 'eas-toast';
        el.textContent = this.$msg;
        (($host.root) || document.body).appendChild(el);
        setTimeout(() => { if (el.parentNode) el.parentNode.removeChild(el); }, 2500);
      },
      'cancel()V': function () {},
      'setDuration(I)V': function () {},
    },
    s: {
      'makeText(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;':
        function (ctx, txt) {
          const t = new ($rt.classes['android/widget/Toast'])();
          t.$msg = $rt.jToString(txt);
          return t;
        },
    },
    sf: { LENGTH_SHORT: 0, LENGTH_LONG: 1 },
  });

  def('android/view/inputmethod/InputMethodManager', null, {
    m: {
      'showSoftInput(Landroid/view/View;I)Z': function (v) {
        if (v && v.$el && v.$el.focus) v.$el.focus();
        return 1;
      },
      'hideSoftInputFromWindow(Landroid/os/IBinder;I)Z': function () { return 1; },
      'toggleSoftInput(II)V': function () {},
    },
  });
  $rt.inputMethodManager =
    new ($rt.classes['android/view/inputmethod/InputMethodManager'])();
})($rt);
