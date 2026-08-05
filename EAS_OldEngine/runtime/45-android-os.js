/* =========================================================================
 * 45-android-os.js -- android.os.*, android.util.*, android.text.TextUtils,
 *                     android.provider.Settings, android.webkit.*
 *
 * Handler / Looper messages are dispatched through the same cooperative
 * scheduler that runs the game thread, so posted Runnables interleave with
 * Thread.sleep() exactly like on a real (single core) device.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  /* ------------------------------------------------------------- Bundle */
  def('android/os/Bundle', null, {
    ctor() { this.$m = Object.create(null); },
    m: {
      '<init>()V': function () { return this; },
      'putInt(Ljava/lang/String;I)V': function (k, v) { this.$m[k] = v; },
      'getInt(Ljava/lang/String;)I': function (k) { return this.$m[k] | 0; },
      'putString(Ljava/lang/String;Ljava/lang/String;)V': function (k, v) { this.$m[k] = v; },
      'getString(Ljava/lang/String;)Ljava/lang/String;': function (k) {
        const v = this.$m[k];
        return v === undefined ? null : v;
      },
    },
  });

  /* ------------------------------------------------------------ Message */
  const Message = def('android/os/Message', null, {
    ctor() {
      this.f_what = 0; this.f_arg1 = 0; this.f_arg2 = 0; this.f_obj = null;
      this.$target = null; this.$when = 0; this.$thread = null;
    },
    m: {
      '<init>()V': function () { return this; },
      'sendToTarget()V': function () {
        if (this.$target) {
          $rt.invoke(this.$target, 'sendMessage(Landroid/os/Message;)Z', [this]);
        }
      },
      'recycle()V': function () {},
    },
    s: {
      'obtain()Landroid/os/Message;': function () { return new Message(); },
    },
  });

  /* -------------------------------------------------------------- Looper */
  def('android/os/Looper', null, {
    ctor() { this.$name = 'looper'; },
    m: { 'quit()V': function () {}, 'getThread()Ljava/lang/Thread;': function () { return $rt.mainThread; } },
    s: {
      'getMainLooper()Landroid/os/Looper;': function () { return mainLooper; },
      'myLooper()Landroid/os/Looper;': function () { return mainLooper; },
      'prepare()V': function () {},
      'loop()V': function () {},
    },
  });
  const mainLooper = new ($rt.classes['android/os/Looper'])();

  /* ------------------------------------------------------------- Handler */
  def('android/os/Handler', null, {
    ctor() { this.$pending = []; },
    m: {
      '<init>()V': function () { this.$pending = []; return this; },
      '<init>(Landroid/os/Looper;)V': function () { this.$pending = []; return this; },
      '<init>(Landroid/os/Handler$Callback;)V': function (cb) {
        this.$pending = []; this.$cb = cb; return this;
      },
      'getLooper()Landroid/os/Looper;': function () { return mainLooper; },
      'handleMessage(Landroid/os/Message;)V': function () {},
      'post(Ljava/lang/Runnable;)Z': function (r) {
        return schedule(this, { run: r }, 0);
      },
      'postDelayed(Ljava/lang/Runnable;J)Z': function (r, d) {
        return schedule(this, { run: r }, Number(d));
      },
      'postAtTime(Ljava/lang/Runnable;J)Z': function (r) {
        return schedule(this, { run: r }, 0);
      },
      'removeCallbacks(Ljava/lang/Runnable;)V': function (r) {
        drop(this, (e) => e.run === r);
      },
      'removeMessages(I)V': function (what) {
        drop(this, (e) => e.msg && e.msg.f_what === what);
      },
      'removeCallbacksAndMessages(Ljava/lang/Object;)V': function () {
        drop(this, () => true);
      },
      'obtainMessage(I)Landroid/os/Message;': function (what) {
        const m = new Message();
        m.f_what = what;
        m.$target = this;
        return m;
      },
      'obtainMessage(III)Landroid/os/Message;': function (what, a1, a2) {
        const m = new Message();
        m.f_what = what; m.f_arg1 = a1; m.f_arg2 = a2; m.$target = this;
        return m;
      },
      'obtainMessage(IILjava/lang/Object;)Landroid/os/Message;': function (what, a1, o) {
        const m = new Message();
        m.f_what = what; m.f_arg1 = a1; m.f_obj = o; m.$target = this;
        return m;
      },
      'sendMessage(Landroid/os/Message;)Z': function (m) {
        return schedule(this, { msg: m }, 0);
      },
      'sendMessageDelayed(Landroid/os/Message;J)Z': function (m, d) {
        return schedule(this, { msg: m }, Number(d));
      },
      'sendEmptyMessage(I)Z': function (what) {
        const m = new Message();
        m.f_what = what;
        return schedule(this, { msg: m }, 0);
      },
      'sendEmptyMessageDelayed(IJ)Z': function (what, d) {
        const m = new Message();
        m.f_what = what;
        return schedule(this, { msg: m }, Number(d));
      },
      'hasMessages(I)Z': function (what) {
        return this.$pending.some((e) => e.msg && e.msg.f_what === what) ? 1 : 0;
      },
    },
  });
  iface('android/os/Handler$Callback');

  function schedule(handler, entry, delay) {
    handler.$pending.push(entry);
    const self = handler;
    entry.thread = $rt.scheduler.spawn((function* () {
      if (delay > 0) yield { s: delay };
      else yield { s: 0 };
      const i = self.$pending.indexOf(entry);
      if (i < 0) return;                       // removed before it ran
      self.$pending.splice(i, 1);
      if (entry.run) {
        const g = entry.run[$rt.M.run]();
        if (g && typeof g.next === 'function') yield* g;
      } else if (entry.msg) {
        const g = self[mangle('handleMessage(Landroid/os/Message;)V')](entry.msg);
        if (g && typeof g.next === 'function') yield* g;
      }
    })(), 'handler');
    $rt.scheduler.kick();
    return 1;
  }

  function drop(handler, pred) {
    handler.$pending = handler.$pending.filter((e) => {
      if (!pred(e)) return true;
      if (e.thread) e.thread.done = true;
      return false;
    });
  }

  def('android/os/HandlerThread', 'java/lang/Thread', {
    ctor() { this.$name = 'HandlerThread'; this.$looper = mainLooper; },
    m: {
      '<init>(Ljava/lang/String;)V': function (n) { this.$name = n; return this; },
      '<init>(Ljava/lang/String;I)V': function (n) { this.$name = n; return this; },
      'start()V': function () {},
      'getLooper()Landroid/os/Looper;': function () { return this.$looper; },
      'quit()Z': function () { return 1; },
    },
  });

  /* ----------------------------------------------------- Binder / Parcel */
  iface('android/os/IInterface');
  iface('android/os/IBinder');

  def('android/os/Parcel', null, {
    ctor() { this.$d = []; this.$p = 0; },
    m: {
      'recycle()V': function () { this.$d.length = 0; this.$p = 0; },
      'setDataPosition(I)V': function (p) { this.$p = p; },
      'dataSize()I': function () { return this.$d.length; },
      'writeInt(I)V': function (v) { this.$d.push(v | 0); },
      'writeLong(J)V': function (v) { this.$d.push(v); },
      'writeFloat(F)V': function (v) { this.$d.push(v); },
      'writeString(Ljava/lang/String;)V': function (v) { this.$d.push(v); },
      'writeStrongBinder(Landroid/os/IBinder;)V': function (v) { this.$d.push(v); },
      'writeInterfaceToken(Ljava/lang/String;)V': function (v) { this.$d.push(v); },
      'writeNoException()V': function () { this.$d.push(0); },
      'readException()V': function () { this.$p++; },
      'enforceInterface(Ljava/lang/String;)V': function () { this.$p++; },
      'readInt()I': function () { return this.$d[this.$p++] | 0; },
      'readLong()J': function () {
        const v = this.$d[this.$p++];
        return typeof v === 'bigint' ? v : BigInt(v | 0);
      },
      'readFloat()F': function () { return +this.$d[this.$p++]; },
      'readString()Ljava/lang/String;': function () {
        const v = this.$d[this.$p++];
        return v === undefined ? null : v;
      },
      'readStrongBinder()Landroid/os/IBinder;': function () {
        const v = this.$d[this.$p++];
        return v === undefined ? null : v;
      },
    },
    s: { 'obtain()Landroid/os/Parcel;': function () { return new ($rt.classes['android/os/Parcel'])(); } },
  });

  def('android/os/Binder', null, {
    ctor() { this.$owner = null; this.$descriptor = null; },
    m: {
      '<init>()V': function () { return this; },
      'attachInterface(Landroid/os/IInterface;Ljava/lang/String;)V': function (o, d) {
        this.$owner = o; this.$descriptor = d;
      },
      'queryLocalInterface(Ljava/lang/String;)Landroid/os/IInterface;': function (d) {
        return this.$descriptor === d ? this.$owner : null;
      },
      'onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z': function () { return 0; },
      'transact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z': function (c, i, o, f) {
        return $rt.invoke(this, 'onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z',
                          [c, i, o, f]);
      },
      'asBinder()Landroid/os/IBinder;': function () { return this; },
      'isBinderAlive()Z': function () { return 1; },
      'pingBinder()Z': function () { return 1; },
    },
    impl: ['android/os/IBinder'],
  });

  /* --------------------------------------------------------- Build / env */
  def('android/os/Build', null, {
    sf: {
      MODEL: 'Web', MANUFACTURER: 'Kairosoft', DEVICE: 'browser',
      PRODUCT: 'dex2js', BRAND: 'generic', ID: 'WEB', DISPLAY: 'web',
    },
  });
  def('android/os/Build$VERSION', null, {
    sf: { RELEASE: '4.0.4', SDK_INT: 15, SDK: '15', INCREMENTAL: '1' },
  });

  def('android/os/Environment', null, {
    s: {
      'getExternalStorageDirectory()Ljava/io/File;': function () {
        const f = new ($rt.classes['java/io/File'])();
        f.$path = 'sdcard';
        return f;
      },
      'getExternalStorageState()Ljava/lang/String;': function () { return 'mounted'; },
    },
    sf: { MEDIA_MOUNTED: 'mounted' },
  });

  def('android/os/SystemClock', null, {
    s: {
      'uptimeMillis()J': function () { return BigInt(Math.round(performanceNow())); },
      'elapsedRealtime()J': function () { return BigInt(Math.round(performanceNow())); },
      'sleep(J)V': function* (ms) { yield { s: Number(ms) }; },
    },
  });
  function performanceNow() { return $rt.now(); }

  /* -------------------------------------------------------- android.util */
  def('android/util/DisplayMetrics', null, {
    ctor() {
      this.f_density = 1.5; this.f_densityDpi = 240; this.f_scaledDensity = 1.5;
      this.f_widthPixels = 0; this.f_heightPixels = 0;
      this.f_xdpi = 240; this.f_ydpi = 240;
    },
    m: { '<init>()V': function () { return this; } },
  });

  def('android/util/Log', null, {
    s: {
      'println(ILjava/lang/String;Ljava/lang/String;)I': function (p, tag, msg) {
        $host.log('[' + tag + '] ' + msg);
        return 0;
      },
      'v(Ljava/lang/String;Ljava/lang/String;)I': logf,
      'd(Ljava/lang/String;Ljava/lang/String;)I': logf,
      'i(Ljava/lang/String;Ljava/lang/String;)I': logf,
      'w(Ljava/lang/String;Ljava/lang/String;)I': logf,
      'e(Ljava/lang/String;Ljava/lang/String;)I': logf,
    },
  });
  function logf(tag, msg) { $host.log('[' + tag + '] ' + msg); return 0; }

  /* -------------------------------------------------------- android.text */
  def('android/text/TextUtils', null, {
    s: {
      'isEmpty(Ljava/lang/CharSequence;)Z': function (s) {
        return (s === null || s === undefined || $rt.jToString(s).length === 0) ? 1 : 0;
      },
      'join(Ljava/lang/CharSequence;[Ljava/lang/Object;)Ljava/lang/String;':
        function (sep, parts) {
          const d = $rt.jToString(sep);
          const out = [];
          for (let i = 0; i < parts.length; i++) out.push($rt.jToString(parts[i]));
          return out.join(d);
        },
      'split(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;':
        function (s, re) {
          if (s === null || s === '') return $rt.arr.obj('[Ljava/lang/String;', []);
          return $rt.arr.obj('[Ljava/lang/String;', String(s).split(new RegExp(re)));
        },
    },
  });
  iface('android/text/TextUtils$StringSplitter', ['java/lang/Iterable']);

  def('android/text/TextUtils$SimpleStringSplitter', null, {
    ctor() { this.$c = ','; this.$parts = []; },
    m: {
      '<init>(C)V': function (c) {
        this.$c = String.fromCharCode(c);
        return this;
      },
      'setString(Ljava/lang/String;)V': function (s) {
        this.$parts = (s === null || s === '') ? [] : String(s).split(this.$c);
      },
      'iterator()Ljava/util/Iterator;': function () {
        const parts = this.$parts.slice();
        return $rt.mkIterator(() => parts);
      },
      'hasNext()Z': function () { return this.$parts.length ? 1 : 0; },
      'next()Ljava/lang/Object;': function () { return this.$parts.shift(); },
    },
    impl: ['android/text/TextUtils$StringSplitter', 'java/util/Iterator',
           'java/lang/Iterable'],
  });

  /* ---------------------------------------------------- Settings.System */
  def('android/provider/Settings$System', null, {
    s: {
      'getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;':
        function (cr, key) {
          if (String(key) === 'android_id') return $rt.androidId();
          return null;
        },
    },
    sf: { ANDROID_ID: 'android_id' },
  });
  def('android/provider/Settings$Secure', null, {
    s: {
      'getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;':
        function (cr, key) {
          if (String(key) === 'android_id') return $rt.androidId();
          return null;
        },
    },
    sf: { ANDROID_ID: 'android_id' },
  });
  $rt.androidId = function () {
    let id = null;
    try {
      if (typeof localStorage !== 'undefined') id = localStorage.getItem('eas.android_id');
    } catch (e) { /* ignore */ }
    if (!id) {
      id = '';
      for (let i = 0; i < 16; i++) id += (Math.random() * 16 | 0).toString(16);
      try {
        if (typeof localStorage !== 'undefined') localStorage.setItem('eas.android_id', id);
      } catch (e) { /* ignore */ }
    }
    return id;
  };

  /* ------------------------------------------------------ android.webkit */
  def('android/webkit/WebSettings', null, {
    m: {
      'getUserAgentString()Ljava/lang/String;': function () { return $host.userAgent; },
      'setUserAgentString(Ljava/lang/String;)V': function (s) { $host.userAgent = s; },
      'setJavaScriptEnabled(Z)V': function () {},
    },
  });
  def('android/webkit/WebView', 'android/view/View', {
    ctor() { this.$settings = null; },
    m: {
      '<init>(Landroid/content/Context;)V': function () { return this; },
      'getSettings()Landroid/webkit/WebSettings;': function () {
        if (!this.$settings) {
          this.$settings = new ($rt.classes['android/webkit/WebSettings'])();
        }
        return this.$settings;
      },
      'destroy()V': function () {},
      'loadUrl(Ljava/lang/String;)V': function () {},
    },
  });
})($rt);
