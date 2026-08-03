/* =========================================================================
 * 35-android-content.js -- android.content.*, android.content.res.*,
 *                          android.app.*, android.net.Uri
 *
 * The Activity is the process: it owns the asset manager, the resource
 * table, the private file directory and the system services.  The game's
 * kairo.android.ui.IApplication extends it directly.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  /* ------------------------------------------------------- AssetManager */
  def('android/content/res/AssetManager', null, {
    m: {
      'open(Ljava/lang/String;)Ljava/io/InputStream;': function (path) {
        const data = $host.getAsset(String(path));
        if (!data) $rt.raise('java/io/FileNotFoundException', String(path));
        const is = new ($rt.classes['java/io/ByteArrayInputStream'])();
        is.$b = data;
        is.$p = 0;
        return is;
      },
      'open(Ljava/lang/String;I)Ljava/io/InputStream;': function (path) {
        return this[mangle('open(Ljava/lang/String;)Ljava/io/InputStream;')](path);
      },
      'list(Ljava/lang/String;)[Ljava/lang/String;': function (dir) {
        const pre = dir ? String(dir).replace(/\/$/, '') + '/' : '';
        const out = [];
        for (const k in $host.assets) {
          if (k.indexOf(pre) === 0) out.push(k.slice(pre.length));
        }
        return $rt.arr.obj('[Ljava/lang/String;', out);
      },
      'close()V': function () {},
    },
  });

  /* -------------------------------------------------- AssetFileDescriptor */
  def('android/content/res/AssetFileDescriptor', null, {
    ctor() { this.$raw = null; },
    m: {
      'close()V': function () {},
      'getLength()J': function () {
        return BigInt(this.$raw && this.$raw.bytes ? this.$raw.bytes.length : 0);
      },
      'getStartOffset()J': function () { return 0n; },
    },
  });

  /* ----------------------------------------------------------- Resources */
  def('android/content/res/Resources', null, {
    m: {
      'getIdentifier(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I':
        function (name, type, pkg) {
          if (String(type) !== 'raw') return 0;
          return $host.rawId(String(name));
        },
      'openRawResourceFd(I)Landroid/content/res/AssetFileDescriptor;': function (id) {
        const raw = $host.rawById[id];
        if (!raw) $rt.raise('java/io/FileNotFoundException', 'res id ' + id);
        const fd = new ($rt.classes['android/content/res/AssetFileDescriptor'])();
        fd.$raw = raw;
        return fd;
      },
      'openRawResource(I)Ljava/io/InputStream;': function (id) {
        const raw = $host.rawById[id];
        if (!raw || !raw.bytes) $rt.raise('java/io/FileNotFoundException', 'res id ' + id);
        const is = new ($rt.classes['java/io/ByteArrayInputStream'])();
        is.$b = raw.bytes;
        is.$p = 0;
        return is;
      },
      'getString(I)Ljava/lang/String;': function () { return ''; },
      'getDisplayMetrics()Landroid/util/DisplayMetrics;': function () {
        const dm = new ($rt.classes['android/util/DisplayMetrics'])();
        $rt.invoke($rt.display, 'getMetrics(Landroid/util/DisplayMetrics;)V', [dm]);
        return dm;
      },
    },
  });

  /* ------------------------------------------------------ package manager */
  def('android/content/pm/PackageInfo', null, {
    ctor() {
      this.f_versionCode = 1;
      this.f_versionName = '1.0';
      this.f_packageName = 'net.kairosoft.android.frontier_en';
    },
    m: { '<init>()V': function () { return this; } },
  });

  def('android/content/pm/PackageManager', null, {
    m: {
      'getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;':
        function () {
          return new ($rt.classes['android/content/pm/PackageInfo'])();
        },
    },
  });

  /* ---------------------------------------------------------------- Uri */
  def('android/net/Uri', null, {
    ctor() { this.$s = ''; },
    m: {
      'toString()Ljava/lang/String;': function () { return this.$s; },
      'getScheme()Ljava/lang/String;': function () {
        const i = this.$s.indexOf(':');
        return i < 0 ? null : this.$s.slice(0, i);
      },
    },
    s: {
      'parse(Ljava/lang/String;)Landroid/net/Uri;': function (s) {
        const u = new ($rt.classes['android/net/Uri'])();
        u.$s = String(s);
        return u;
      },
    },
  });

  /* ------------------------------------------------------------- Intent */
  def('android/content/Intent', null, {
    ctor() { this.$action = null; this.$data = null; this.$flags = 0;
             this.$extras = Object.create(null); },
    m: {
      '<init>()V': function () { return this; },
      '<init>(Ljava/lang/String;)V': function (a) { this.$action = a; return this; },
      '<init>(Ljava/lang/String;Landroid/net/Uri;)V': function (a, u) {
        this.$action = a; this.$data = u; return this;
      },
      'getAction()Ljava/lang/String;': function () { return this.$action; },
      'getData()Landroid/net/Uri;': function () { return this.$data; },
      'setFlags(I)Landroid/content/Intent;': function (f) { this.$flags = f; return this; },
      'addFlags(I)Landroid/content/Intent;': function (f) { this.$flags |= f; return this; },
      'putExtra(Ljava/lang/String;I)Landroid/content/Intent;': function (k, v) {
        this.$extras[k] = v; return this;
      },
      'getIntExtra(Ljava/lang/String;I)I': function (k, d) {
        const v = this.$extras[k];
        return v === undefined ? d : v | 0;
      },
      'getStringExtra(Ljava/lang/String;)Ljava/lang/String;': function (k) {
        const v = this.$extras[k];
        return v === undefined ? null : v;
      },
    },
  });

  def('android/content/IntentFilter', null, {
    ctor() { this.$actions = []; },
    m: {
      '<init>()V': function () { return this; },
      '<init>(Ljava/lang/String;)V': function (a) { this.$actions = [a]; return this; },
      'addAction(Ljava/lang/String;)V': function (a) { this.$actions.push(a); },
    },
  });

  def('android/content/BroadcastReceiver', null, {
    m: {
      '<init>()V': function () { return this; },
      'onReceive(Landroid/content/Context;Landroid/content/Intent;)V': function () {},
    },
  });

  def('android/content/ComponentName', null, {
    ctor() { this.$pkg = ''; this.$cls = ''; },
    m: {
      '<init>(Ljava/lang/String;Ljava/lang/String;)V': function (p, c) {
        this.$pkg = p; this.$cls = c; return this;
      },
      'getClassName()Ljava/lang/String;': function () { return this.$cls; },
      'getPackageName()Ljava/lang/String;': function () { return this.$pkg; },
    },
  });

  def('android/content/ContentResolver', null, {});

  iface('android/content/ServiceConnection');
  iface('android/content/DialogInterface');
  iface('android/content/DialogInterface$OnClickListener');
  iface('android/content/DialogInterface$OnCancelListener');
  iface('android/content/DialogInterface$OnKeyListener');
  iface('android/content/DialogInterface$OnDismissListener');

  /* ------------------------------------------------------------ Context */
  const Context = def('android/content/Context', null, {
    ctor() {
      this.$assets = null;
      this.$res = null;
      this.$cr = null;
      this.$pm = null;
      this.$receivers = [];
    },
    m: {
      '<init>()V': function () { return this; },
      'getApplicationContext()Landroid/content/Context;': function () { return this; },
      'getBaseContext()Landroid/content/Context;': function () { return this; },
      'getPackageName()Ljava/lang/String;': function () {
        return 'net.kairosoft.android.frontier_en';
      },
      'getAssets()Landroid/content/res/AssetManager;': function () {
        if (!this.$assets) {
          this.$assets = new ($rt.classes['android/content/res/AssetManager'])();
        }
        return this.$assets;
      },
      'getResources()Landroid/content/res/Resources;': function () {
        if (!this.$res) this.$res = new ($rt.classes['android/content/res/Resources'])();
        return this.$res;
      },
      'getContentResolver()Landroid/content/ContentResolver;': function () {
        if (!this.$cr) this.$cr = new ($rt.classes['android/content/ContentResolver'])();
        return this.$cr;
      },
      'getPackageManager()Landroid/content/pm/PackageManager;': function () {
        if (!this.$pm) this.$pm = new ($rt.classes['android/content/pm/PackageManager'])();
        return this.$pm;
      },
      'getSystemService(Ljava/lang/String;)Ljava/lang/Object;': function (name) {
        switch (String(name)) {
          case 'window': return $rt.windowManager;
          case 'audio': return $rt.audioManager;
          case 'keyguard': return $rt.keyguardManager;
          case 'input_method': return $rt.inputMethodManager;
          default: return null;
        }
      },
      'getFilesDir()Ljava/io/File;': function () {
        const f = new ($rt.classes['java/io/File'])();
        f.$path = 'files';
        return f;
      },
      'fileList()[Ljava/lang/String;': function () {
        return $rt.arr.obj('[Ljava/lang/String;', $rt.fs.list('files'));
      },
      'openFileInput(Ljava/lang/String;)Ljava/io/FileInputStream;': function (n) {
        const is = new ($rt.classes['java/io/FileInputStream'])();
        const d = $rt.fs.read('files/' + n);
        if (d === null) $rt.raise('java/io/FileNotFoundException', String(n));
        is.$b = d; is.$p = 0;
        return is;
      },
      'openFileOutput(Ljava/lang/String;I)Ljava/io/FileOutputStream;': function (n, mode) {
        const os = new ($rt.classes['java/io/FileOutputStream'])();
        os.$path = 'files/' + n;
        os.$n = 0;
        if ((mode & 0x8000) !== 0) {           // MODE_APPEND
          const old = $rt.fs.read(os.$path);
          if (old) { os.$buf = old.slice(); os.$n = old.length; }
        }
        return os;
      },
      'deleteFile(Ljava/lang/String;)Z': function (n) {
        $rt.fs.remove('files/' + n);
        return 1;
      },
      'registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;':
        function (r, f) {
          this.$receivers.push([r, f]);
          return null;
        },
      'unregisterReceiver(Landroid/content/BroadcastReceiver;)V': function (r) {
        this.$receivers = this.$receivers.filter((e) => e[0] !== r);
      },
      'sendBroadcast(Landroid/content/Intent;)V': function (i) {
        for (const e of this.$receivers) {
          $rt.invoke(e[0],
                     'onReceive(Landroid/content/Context;Landroid/content/Intent;)V',
                     [this, i]);
        }
      },
      'startActivity(Landroid/content/Intent;)V': function (i) {
        const url = i && i.$data ? i.$data.$s : null;
        if (url && typeof window !== 'undefined' && window.open) {
          if (/^market:\/\//.test(url)) {
            window.open('https://play.google.com/store/apps/details?id=' +
                        url.replace(/^market:\/\/details\?id=/, ''), '_blank');
          } else {
            window.open(url, '_blank');
          }
        } else {
          $host.log('[Context] startActivity ' + url);
        }
      },
      'bindService(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z':
        function () { return 0; },   // no Android services exist in a browser
      'unbindService(Landroid/content/ServiceConnection;)V': function () {},
    },
  });

  /* ----------------------------------------------------------- Activity */
  def('android/app/Activity', 'android/content/Context', {
    ctor() {
      /* superclass field init runs via super() */
      this.$assets = null; this.$res = null; this.$cr = null; this.$pm = null;
      this.$receivers = [];
      this.$orientation = -1;
      this.$window = null;
      this.$content = null;
      this.$focus = true;
    },
    m: {
      '<init>()V': function () { return this; },
      'onCreate(Landroid/os/Bundle;)V': function () {},
      'onStart()V': function () {},
      'onResume()V': function () {},
      'onPause()V': function () {},
      'onStop()V': function () {},
      'onRestart()V': function () {},
      'onDestroy()V': function () {},
      'onKeyDown(ILandroid/view/KeyEvent;)Z': function () { return 0; },
      'onKeyUp(ILandroid/view/KeyEvent;)Z': function () { return 0; },
      'dispatchKeyEvent(Landroid/view/KeyEvent;)Z': function () { return 1; },
      'requestWindowFeature(I)Z': function () { return 1; },
      'setContentView(Landroid/view/View;)V': function (v) {
        this.$content = v;
        if ($host.attachContentView) $host.attachContentView(v);
      },
      'getWindow()Landroid/view/Window;': function () {
        if (!this.$window) this.$window = new ($rt.classes['android/view/Window'])();
        return this.$window;
      },
      'getWindowManager()Landroid/view/WindowManager;': function () {
        return $rt.windowManager;
      },
      'getRequestedOrientation()I': function () { return this.$orientation; },
      'setRequestedOrientation(I)V': function (o) { this.$orientation = o; },
      'hasWindowFocus()Z': function () { return this.$focus ? 1 : 0; },
      'finish()V': function () { $host.log('[Activity] finish()'); },
      'isFinishing()Z': function () { return 0; },
      'runOnUiThread(Ljava/lang/Runnable;)V': function (r) {
        $rt.invoke(r, 'run()V', []);
      },
      'showDialog(I)V': function () {},
    },
  });

  def('android/app/KeyguardManager', null, {
    m: { 'inKeyguardRestrictedInputMode()Z': function () { return 0; } },
  });
  $rt.keyguardManager = new ($rt.classes['android/app/KeyguardManager'])();

  /* -------------------------------------------------------- AlertDialog */
  const AlertDialog = def('android/app/AlertDialog', null, {
    ctor() { this.$spec = null; this.$el = null; },
    m: {
      'dismiss()V': function () { closeDialog(this, false); },
      'cancel()V': function () { closeDialog(this, true); },
      'isShowing()Z': function () { return this.$el ? 1 : 0; },
    },
    impl: ['android/content/DialogInterface'],
  });

  def('android/app/AlertDialog$Builder', null, {
    ctor() {
      this.$s = { title: null, message: null, view: null, buttons: [],
                  onCancel: null, onKey: null, cancelable: true };
    },
    m: {
      '<init>(Landroid/content/Context;)V': function () { return this; },
      'setTitle(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;':
        function (t) { this.$s.title = $rt.jToString(t); return this; },
      'setMessage(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;':
        function (t) { this.$s.message = $rt.jToString(t); return this; },
      'setView(Landroid/view/View;)Landroid/app/AlertDialog$Builder;':
        function (v) { this.$s.view = v; return this; },
      'setCancelable(Z)Landroid/app/AlertDialog$Builder;':
        function (c) { this.$s.cancelable = !!c; return this; },
      'setPositiveButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;':
        function (t, l) { this.$s.buttons.push([$rt.jToString(t), l, -1]); return this; },
      'setNegativeButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;':
        function (t, l) { this.$s.buttons.push([$rt.jToString(t), l, -2]); return this; },
      'setNeutralButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;':
        function (t, l) { this.$s.buttons.push([$rt.jToString(t), l, -3]); return this; },
      'setOnCancelListener(Landroid/content/DialogInterface$OnCancelListener;)Landroid/app/AlertDialog$Builder;':
        function (l) { this.$s.onCancel = l; return this; },
      'setOnKeyListener(Landroid/content/DialogInterface$OnKeyListener;)Landroid/app/AlertDialog$Builder;':
        function (l) { this.$s.onKey = l; return this; },
      'create()Landroid/app/AlertDialog;': function () {
        const d = new AlertDialog();
        d.$spec = this.$s;
        return d;
      },
      'show()Landroid/app/AlertDialog;': function () {
        const d = new AlertDialog();
        d.$spec = this.$s;
        showDialog(d);
        return d;
      },
    },
  });

  function showDialog(d) {
    const s = d.$spec;
    /* A host hook answers the dialog without a user: that is how a scripted
     * session (tools/drive.js, tests) types into the game's name prompt.
     * Nothing installs it in the browser, where the DOM dialog below runs. */
    if (typeof $host.onDialog === 'function') { $host.onDialog(d, s); return; }
    if (typeof document === 'undefined') {         // head-less: cancel at once
      closeDialog(d, true);
      return;
    }
    const back = document.createElement('div');
    back.className = 'eas-dialog-backdrop';
    const box = document.createElement('div');
    box.className = 'eas-dialog';
    if (s.title) {
      const h = document.createElement('div');
      h.className = 'eas-dialog-title';
      h.textContent = s.title;
      box.appendChild(h);
    }
    if (s.message) {
      const p = document.createElement('div');
      p.className = 'eas-dialog-msg';
      p.textContent = s.message;
      box.appendChild(p);
    }
    if (s.view && s.view.$el) box.appendChild(s.view.$el);
    const row = document.createElement('div');
    row.className = 'eas-dialog-row';
    if (s.buttons.length === 0) {
      const b = document.createElement('button');
      b.textContent = 'OK';
      b.onclick = () => closeDialog(d, true);
      row.appendChild(b);
    } else {
      for (const [label, listener, which] of s.buttons) {
        const b = document.createElement('button');
        b.textContent = label;
        b.onclick = () => {
          closeDialog(d, false);
          if (listener) {
            $rt.invoke(listener,
                       'onClick(Landroid/content/DialogInterface;I)V', [d, which]);
          }
        };
        row.appendChild(b);
      }
    }
    box.appendChild(row);
    back.appendChild(box);
    (($host.root) || document.body).appendChild(back);
    d.$el = back;
    if (s.view && s.view.$el && s.view.$el.focus) {
      setTimeout(() => { try { s.view.$el.focus(); s.view.$el.select(); } catch (e) {} }, 0);
    }
    back.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') { ev.preventDefault(); closeDialog(d, true); }
    });
  }

  function closeDialog(d, cancelled) {
    if (d.$el && d.$el.parentNode) d.$el.parentNode.removeChild(d.$el);
    d.$el = null;
    const s = d.$spec;
    if (cancelled && s && s.onCancel) {
      $rt.invoke(s.onCancel, 'onCancel(Landroid/content/DialogInterface;)V', [d]);
    }
  }

  /** Press button #which of a live dialog (index into $spec.buttons). */
  function answerDialog(d, which) {
    const s = d.$spec;
    const b = s && s.buttons[which | 0];
    if (!b) { closeDialog(d, true); return; }
    closeDialog(d, false);
    if (b[1]) {
      $rt.invoke(b[1], 'onClick(Landroid/content/DialogInterface;I)V', [d, b[2]]);
    }
  }

  $rt.showDialog = showDialog;
  $rt.answerDialog = answerDialog;
})($rt);
