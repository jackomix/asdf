/* =========================================================================
 * 60-boot.js -- the bridge: DOM surface, asset preload, input, main loop
 *
 * This is the only file that knows about the browser.  It builds the screen,
 * loads the APK payload (assets/*.dat, res/raw/*.ogg), boots the translated
 * activity net.kairosoft.android.frontier_en.Main exactly the way Android
 * would (onCreate -> onStart -> onResume) and then pumps the cooperative
 * scheduler from requestAnimationFrame.
 * ========================================================================= */
'use strict';

var $EAS = (function () {
  const CONFIG = {
    width: 480,          // device pixels handed to the game (DisplayMetrics)
    height: 800,
    density: 1.5,
    base: '',            // where assets/ and raw/ live, relative to the page
  };

  let root = null, canvas = null, statusEl = null;
  let activity = null, started = false;

  /* --------------------------------------------------------------- DOM */
  const CSS = `
  html,body{margin:0;padding:0;height:100%;background:#0b0d12;overflow:hidden;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;}
  #eas-root{position:fixed;inset:0;display:flex;align-items:center;
    justify-content:center;}
  #eas-stage{position:relative;background:#000;box-shadow:0 0 40px #000a;
    image-rendering:pixelated;}
  #eas-stage canvas{display:block;width:100%;height:100%;
    image-rendering:pixelated;image-rendering:crisp-edges;touch-action:none;}
  .eas-frame{position:absolute;inset:0;}
  #eas-status{position:fixed;inset:0;display:flex;flex-direction:column;
    align-items:center;justify-content:center;color:#9fb4d8;font-size:14px;
    letter-spacing:.08em;gap:14px;z-index:40;background:#0b0d12;}
  #eas-status .bar{width:220px;height:3px;background:#1d2430;overflow:hidden;}
  #eas-status .bar i{display:block;height:100%;width:0;background:#5c8fd6;
    transition:width .15s linear;}
  #eas-docbtn{position:fixed;right:12px;bottom:12px;z-index:60;
    background:#161b26;color:#9fb4d8;border:1px solid #2a3446;border-radius:6px;
    font-size:11px;letter-spacing:.1em;padding:7px 10px;cursor:pointer;
    opacity:.65;transition:opacity .15s;}
  #eas-docbtn:hover{opacity:1;}
  #eas-docs{position:fixed;inset:0;z-index:70;background:#000c;display:none;
    align-items:center;justify-content:center;padding:24px;}
  #eas-docs.open{display:flex;}
  #eas-docs .panel{background:#0f131b;color:#c8d4e6;max-width:900px;
    max-height:100%;overflow:auto;border:1px solid #2a3446;border-radius:10px;
    padding:22px 26px;font-size:13px;line-height:1.6;}
  #eas-docs h2{margin:0 0 4px;font-size:15px;color:#8fb4ee;letter-spacing:.06em;}
  #eas-docs h3{margin:18px 0 6px;font-size:13px;color:#7fd6a8;}
  #eas-docs code,#eas-docs pre{font-family:ui-monospace,Menlo,Consolas,monospace;
    font-size:12px;color:#e0c98f;}
  #eas-docs pre{background:#0a0d13;border:1px solid #202839;border-radius:6px;
    padding:10px 12px;overflow:auto;white-space:pre;}
  #eas-docs .close{float:right;cursor:pointer;color:#65748d;border:0;
    background:none;font-size:16px;}
  .eas-dialog-backdrop{position:fixed;inset:0;background:#000a;z-index:50;
    display:flex;align-items:center;justify-content:center;}
  .eas-dialog{background:#20242c;color:#e6ecf5;border-radius:8px;padding:16px;
    min-width:260px;box-shadow:0 10px 40px #000b;}
  .eas-dialog-title{font-weight:600;margin-bottom:8px;}
  .eas-dialog-msg{margin-bottom:10px;white-space:pre-wrap;}
  .eas-dialog-row{display:flex;gap:8px;justify-content:flex-end;margin-top:12px;}
  .eas-dialog-row button{background:#38414f;color:#e6ecf5;border:0;
    border-radius:5px;padding:7px 14px;cursor:pointer;}
  .eas-edit{width:100%;box-sizing:border-box;font-size:16px;padding:7px 8px;
    border-radius:5px;border:1px solid #4a5566;background:#151a21;color:#fff;}
  .eas-toast{position:fixed;left:50%;bottom:60px;transform:translateX(-50%);
    background:#000c;color:#fff;padding:8px 14px;border-radius:16px;z-index:55;}
  #eas-crash{position:fixed;inset:0;z-index:80;background:#160a0a;color:#ffb4b4;
    padding:24px;font-family:ui-monospace,monospace;font-size:12px;
    white-space:pre-wrap;overflow:auto;display:none;}
  `;

  function buildDOM() {
    const style = document.createElement('style');
    style.textContent = CSS;
    document.head.appendChild(style);

    root = document.getElementById('eas-root');
    if (!root) {
      root = document.createElement('div');
      root.id = 'eas-root';
      document.body.appendChild(root);
    }

    const stage = document.createElement('div');
    stage.id = 'eas-stage';
    root.appendChild(stage);

    canvas = document.createElement('canvas');
    canvas.width = CONFIG.width;
    canvas.height = CONFIG.height;
    stage.appendChild(canvas);

    statusEl = document.createElement('div');
    statusEl.id = 'eas-status';
    statusEl.innerHTML = '<div>LOADING classes.dex PAYLOAD</div>' +
                         '<div class="bar"><i></i></div><div class="pct"></div>';
    document.body.appendChild(statusEl);

    $host.root = stage;
    $host.surface = canvas;
    $host.attachContentView = function (view) {
      if (view && view.$el && view.$el.parentNode !== stage) stage.appendChild(view.$el);
    };

    layout();
    window.addEventListener('resize', layout);
    buildDocsButton();
    return stage;
  }

  function layout() {
    const stage = document.getElementById('eas-stage');
    if (!stage) return;
    const s = Math.max(0.25, Math.min(window.innerWidth / CONFIG.width,
                                      window.innerHeight / CONFIG.height));
    stage.style.width = Math.round(CONFIG.width * s) + 'px';
    stage.style.height = Math.round(CONFIG.height * s) + 'px';
  }

  function progress(pct, text) {
    if (!statusEl) return;
    statusEl.querySelector('.bar i').style.width = Math.round(pct * 100) + '%';
    if (text) statusEl.querySelector('div').textContent = text;
    statusEl.querySelector('.pct').textContent = Math.round(pct * 100) + '%';
  }

  /* ------------------------------------------------------------- assets */
  async function loadPayload() {
    const mres = await fetch(CONFIG.base + 'assets/manifest.json');
    if (!mres.ok) throw new Error('assets/manifest.json missing - run build.sh');
    const man = await mres.json();
    const total = man.assets.length + man.raw.length;
    let done = 0;

    for (const name of man.assets) {
      const r = await fetch(CONFIG.base + 'assets/' + name);
      const b = new Uint8Array(await r.arrayBuffer());
      $host.putAsset(name, b);
      $host.putResource('assets/' + name, b);
      progress(++done / total, 'UNPACKING assets/' + name);
    }

    for (const name of man.raw) {
      const r = await fetch(CONFIG.base + 'raw/' + name);
      const ab = await r.arrayBuffer();
      const b = new Uint8Array(ab);
      const base = name.replace(/\.[^.]+$/, '');
      $host.addRaw(base, b);
      $host.putAsset(name, b);
      /* res/raw is also reachable as an APK class-path entry, which is how
       * kairo/android/c/h opens the "/res/raw" sound package. */
      $host.putResource('res/raw/' + name, b);
      if (/\.ogg$/.test(name)) $host.audio.put(base, ab);
      progress(++done / total, 'UNPACKING res/raw/' + name);
    }
  }

  /* -------------------------------------------------------------- input */
  const KEYMAP = {
    ArrowUp: 19, ArrowDown: 20, ArrowLeft: 21, ArrowRight: 22,
    Enter: 66, NumpadEnter: 66, Space: 62, Escape: 4, Backspace: 4,
    KeyM: 82, Tab: 61, ShiftLeft: 59, ShiftRight: 60,
    Digit0: 7, Digit1: 8, Digit2: 9, Digit3: 10, Digit4: 11,
    Digit5: 12, Digit6: 13, Digit7: 14, Digit8: 15, Digit9: 16,
  };

  function bindInput() {
    const pointers = new Map();

    const toLocal = (ev) => {
      const r = canvas.getBoundingClientRect();
      return {
        x: (ev.clientX - r.left) * (CONFIG.width / r.width),
        y: (ev.clientY - r.top) * (CONFIG.height / r.height),
      };
    };

    const send = (action) => {
      const gv = $host.gameView;
      if (!gv) return;
      const pts = [];
      pointers.forEach((p, id) => pts.push({ id, x: p.x, y: p.y }));
      const me = $rt.newMotionEvent(action, pts);
      try {
        $rt.invoke(gv, 'onTouchEvent(Landroid/view/MotionEvent;)Z', [me]);
      } catch (e) { console.error('[input]', e); }
      $rt.scheduler.kick();
    };

    canvas.addEventListener('pointerdown', (ev) => {
      ev.preventDefault();
      unlockAudio();
      canvas.setPointerCapture(ev.pointerId);
      const p = toLocal(ev);
      const first = pointers.size === 0;
      pointers.set(ev.pointerId, p);
      send(first ? 0 : (5 | ((pointers.size - 1) << 8)));
    });
    canvas.addEventListener('pointermove', (ev) => {
      if (!pointers.has(ev.pointerId)) return;
      ev.preventDefault();
      pointers.set(ev.pointerId, toLocal(ev));
      send(2);
    });
    const up = (ev) => {
      if (!pointers.has(ev.pointerId)) return;
      ev.preventDefault();
      pointers.set(ev.pointerId, toLocal(ev));
      const last = pointers.size === 1;
      const idx = Array.from(pointers.keys()).indexOf(ev.pointerId);
      send(last ? 1 : (6 | (idx << 8)));
      pointers.delete(ev.pointerId);
    };
    canvas.addEventListener('pointerup', up);
    canvas.addEventListener('pointercancel', up);
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());

    const held = Object.create(null);
    window.addEventListener('keydown', (ev) => {
      const code = KEYMAP[ev.code];
      if (code === undefined) return;
      if (document.activeElement && document.activeElement.tagName === 'INPUT') return;
      ev.preventDefault();
      unlockAudio();
      const repeat = held[code] ? 1 : 0;
      held[code] = 1;
      dispatchKey(0, code, repeat);
    });
    window.addEventListener('keyup', (ev) => {
      const code = KEYMAP[ev.code];
      if (code === undefined) return;
      if (document.activeElement && document.activeElement.tagName === 'INPUT') return;
      ev.preventDefault();
      held[code] = 0;
      dispatchKey(1, code, 0);
    });
  }

  function dispatchKey(action, code, repeat) {
    if (!activity) return;
    const ev = $rt.newKeyEvent(action, code, repeat);
    try {
      $rt.invoke(activity, 'dispatchKeyEvent(Landroid/view/KeyEvent;)Z', [ev]);
    } catch (e) { console.error('[key]', e); }
    $rt.scheduler.kick();
  }

  function unlockAudio() {
    if (!$host.audio.unlocked) $host.audio.resume();
  }

  /* ---------------------------------------------------------- main loop */
  let rafId = 0;
  function pump() {
    rafId = requestAnimationFrame(pump);
    try {
      $rt.scheduler.tick();
    } catch (e) {
      console.error('[scheduler]', e);
    }
  }

  function crash(err) {
    let el = document.getElementById('eas-crash');
    if (!el) {
      el = document.createElement('div');
      el.id = 'eas-crash';
      document.body.appendChild(el);
    }
    el.style.display = 'block';
    el.textContent = 'The translated VM raised an uncaught exception:\n\n' +
      (err && err.$stack ? $rt.jToString(err) + '\n' + err.$stack
                         : (err && err.stack) || String(err));
  }

  /* ------------------------------------------------------------- docs UI */
  function buildDocsButton() {
    const btn = document.createElement('button');
    btn.id = 'eas-docbtn';
    btn.textContent = 'AI DOCS';
    btn.title = 'How this port was produced (for AI agents)';
    document.body.appendChild(btn);

    const modal = document.createElement('div');
    modal.id = 'eas-docs';
    modal.innerHTML = '<div class="panel">' + DOCS + '</div>';
    document.body.appendChild(modal);

    btn.onclick = () => modal.classList.add('open');
    modal.onclick = (e) => { if (e.target === modal) modal.classList.remove('open'); };
    modal.querySelector('.panel').insertAdjacentHTML('afterbegin',
      '<button class="close" title="close">&#10005;</button>');
    modal.querySelector('.close').onclick = () => modal.classList.remove('open');
  }

  const DOCS = `
<h2>AI PORTING DOCUMENTATION &mdash; Android APK &rarr; Web, 1:1 recompile</h2>
<p>This page is <b>not</b> a re-implementation. It is the original
<code>classes.dex</code> of the APK, decompiled and recompiled to JavaScript
ahead-of-time, executing on a hand-written Java/Android runtime. Reproduce it
for any other Dalvik application with the steps below.</p>

<h3>0. Inputs</h3>
<pre>base64.txt            base64 of the APK (decode -&gt; identical bytes)
Epic_Astro_Story-NTU1NTM3.apk
sha256(apk) = 7f185d18b6e470862ea9166fda69a9328c2c694dafae5b0c5325a218c2bbe2af</pre>

<h3>1. Unpack the APK (no external tooling required)</h3>
<pre>base64 -d base64.txt &gt; app.apk        # or use the .apk directly
unzip -o app.apk -d build/apk         # classes.dex, assets/, res/, resources.arsc</pre>

<h3>2. Parse the DEX &mdash; <code>tools/dexlib.py</code></h3>
<p>A dependency-free DEX 035 reader: header, string_ids, type_ids, proto_ids,
field_ids, method_ids, class_defs, class_data, code_items, try/catch tables,
encoded values, and a full Dalvik disassembler covering every opcode
(<code>packed-switch</code>, <code>sparse-switch</code>,
<code>fill-array-data</code> payloads included).</p>
<pre>python3 tools/dexdump.py build/apk/classes.dex &gt; build/full.smali</pre>

<h3>3. Recompile Dalvik &rarr; JavaScript &mdash; <code>tools/dex2js.py</code></h3>
<p>An AOT translator, one JS class per DEX class, one JS method per DEX method.
It preserves the semantics the JVM/Dalvik guarantees:</p>
<pre>registers        -&gt; plain let-locals (v0..vN), SSA-free, 1:1 with the bytecode
int arithmetic   -&gt; (x|0), Math.imul, &gt;&gt;&gt;0 for unsigned shifts
long arithmetic  -&gt; BigInt with BigInt.asIntN(64, ..)
float            -&gt; Math.fround, double -&gt; native number
control flow     -&gt; basic blocks in a switch($p) inside a labelled for(;;)
try/catch        -&gt; JS try/catch + the DEX handler table, rethrow on no match
monitor-enter/exit, check-cast, instance-of, array bounds -&gt; runtime helpers
Thread.sleep / yield / wait -&gt; generator functions (yield {s:ms})
virtual dispatch -&gt; JS prototype chain, names mangled through dex-meta.js</pre>
<pre>python3 tools/dex2js.py build/apk/classes.dex -o web/dex
  =&gt; web/dex/dex-classes.js   (all 139 classes, 1881 methods)
  =&gt; web/dex/dex-meta.js      (signature -&gt; mangled-name table)</pre>
<p>Blocking methods are found by a union-find pass over the virtual-dispatch
groups: any method that can reach <code>Thread.sleep</code> becomes a
generator, and every caller of it becomes one too, so a blocking Java thread
becomes a coroutine that the scheduler resumes.</p>

<h3>4. The runtime &mdash; <code>runtime/*.js</code> (load in this order)</h3>
<pre>dex/dex-meta.js       signature table (must load before the runtime)
00-core.js            object model, class linker, arrays, exceptions, scheduler
01-host.js            host abstraction ($gfx canvas factory, $host assets)
05-image.js           inflate + PNG decode/encode (pure JS, synchronous)
10-lang.js            java.lang.* (String/StringBuilder/Math/Thread/boxes/Class)
20-util.js            java.util.* (Vector/HashMap/Random with the exact JDK LCG)
25-io.js              java.io.* over a localStorage-backed virtual filesystem
30-graphics.js        android.graphics.* (Bitmap/Canvas/Paint/Matrix on 2D ctx)
35-android-content.js Context/Activity/AssetManager/Resources/Intent/AlertDialog
40-android-view.js    View/SurfaceView/SurfaceHolder/KeyEvent/MotionEvent/widgets
45-android-os.js      Handler/Looper/Binder/Parcel/Build/TextUtils/Log/WebView
50-android-media.js   MediaPlayer &rarr; WebAudio, AudioManager, JetPlayer
dex/dex-classes.js    the translated application
55-licensing.js       Play-licensing (LVL) service emulation
60-boot.js            surface, asset preload, input, requestAnimationFrame loop</pre>

<h3>5. Boot sequence (mirrors Android)</h3>
<pre>fetch assets/*.dat + res/raw/*.ogg   (synchronous AssetManager needs them first)
$rt.initNames()
new Main()            -&gt; IApplication.&lt;init&gt; -&gt; Activity.&lt;init&gt;
Main.onCreate(null)   -&gt; creates the GameView, starts the game Thread
Main.onStart(); Main.onResume()
requestAnimationFrame -&gt; $rt.scheduler.tick()  (resumes every ready coroutine)</pre>

<h3>6. Mapping table used for this application</h3>
<pre>SurfaceHolder.lockCanvas()  -&gt; android.graphics.Canvas bound to the &lt;canvas&gt; 2D ctx
Bitmap                      -&gt; offscreen canvas + lazily synced ARGB Uint32Array
MediaPlayer.create(ctx,id)  -&gt; AudioBufferSourceNode over a decoded res/raw ogg
Resources.getIdentifier     -&gt; synthetic 0x7f04xxxx ids assigned at preload time
openFileOutput / fileList   -&gt; virtual FS persisted in localStorage (save games)
HttpURLConnection           -&gt; always throws IOException (device is "offline")
com.android.vending.licensing-&gt; LICENSED response injected at kairo.android.h.b.a()
Class.getResourceAsStream   -&gt; every APK entry is also registered as a classpath
                               resource ($host.putResource), because the engine
                               loads xls.dat/com.dat/... through kairo.android.c.h
AlertDialog + EditText      -&gt; DOM overlay; this is the game's name prompt, the
                               modal loop in kairo.android.b.c spins the frame
                               pump until a button listener fires</pre>

<h3>7. Rebuild &amp; verify</h3>
<pre>./build.sh                         # unzip -&gt; dex2js -&gt; copy assets -&gt; emit web/
node tools/smoke.js                # head-less boot: runs the VM with no browser
node tools/render.js 200 build/f   # render N frames to PNG through node-canvas
node tools/ascii-frame.js f/0.png  # eyeball a frame as luminance ASCII art
node tools/drive.js \              # scripted play using the game's own hit tables
  "wait:60;tap:0;wait:30;input:Terra;tap:2;wait:80;text;targets"
python3 -m http.server -d web 8080</pre>
<p>The render/drive tools need <code>@napi-rs/canvas</code>
(<code>NODE_PATH=&lt;dir&gt;/node_modules</code>); the browser build has no such
dependency.</p>

<h3>8. Porting the same pipeline to another target</h3>
<p>Everything above <code>runtime/30-graphics.js</code> is platform neutral.
To retarget (desktop, native, another engine) keep
<code>tools/dexlib.py</code>, <code>tools/dex2js.py</code>,
<code>00/05/10/20/25</code> and replace only
<code>01-host.js</code>, <code>30/35/40/45/50</code> and
<code>60-boot.js</code> with bindings for the new surface, input and audio
APIs. The translated <code>dex-classes.js</code> never changes.</p>
`;

  /* ---------------------------------------------------------------- boot */
  async function boot(opts) {
    Object.assign(CONFIG, opts || {});
    if (started) return;
    started = true;

    buildDOM();
    $rt.initNames();
    $rt.onCrash = crash;

    try {
      await loadPayload();
    } catch (e) {
      progress(0, 'FAILED: ' + e.message);
      throw e;
    }
    progress(1, 'STARTING VM');

    $rt.display.w = CONFIG.width;
    $rt.display.h = CONFIG.height;
    $rt.display.density = CONFIG.density;

    if ($rt.installLicensePatch) $rt.installLicensePatch();

    const Main = $rt.classes['net/kairosoft/android/frontier_en/Main'];
    if (!Main) throw new Error('translated Main class missing');

    activity = new Main();
    $rt.invoke(activity, '<init>()V', []);
    $host.activity = activity;

    bindInput();

    $rt.invoke(activity, 'onCreate(Landroid/os/Bundle;)V', [null]);
    $rt.invoke(activity, 'onStart()V', []);
    $rt.invoke(activity, 'onResume()V', []);

    document.addEventListener('visibilitychange', () => {
      try {
        if (document.hidden) $rt.invoke(activity, 'onPause()V', []);
        else {
          $rt.invoke(activity, 'onResume()V', []);
          unlockAudio();
        }
      } catch (e) { console.error(e); }
    });
    window.addEventListener('pointerdown', unlockAudio, { once: true });
    window.addEventListener('keydown', unlockAudio, { once: true });

    statusEl.style.display = 'none';
    pump();
  }

  /* ------------------------------------------------- scripted input API
   * Used by tools/render.js to drive the game without a pointing device;
   * also handy from the browser console.                                */
  function touch(action, x, y) {
    const gv = $host.gameView;
    if (!gv) return;
    $rt.invoke(gv, 'onTouchEvent(Landroid/view/MotionEvent;)Z',
               [$rt.newMotionEvent(action, [{ id: 0, x: x, y: y }])]);
    $rt.scheduler.kick();
  }

  /* A press is held for `holdFrames` scheduler ticks (default 2) so the
   * engine samples the DOWN state at least once before the UP arrives,
   * exactly like a real finger on a 20 Hz input loop. */
  function tap(x, y, holdFrames) {
    let n = Math.max(1, holdFrames === undefined ? 2 : holdFrames | 0);
    touch(0, x, y);
    $rt.scheduler.onTick.push(function () {
      if (--n > 0) { touch(2, x, y); return true; }
      touch(1, x, y);
      return false;
    });
  }

  function key(code) {
    dispatchKey(0, code, 0);
    dispatchKey(1, code, 0);
  }

  return { boot, CONFIG, docs: DOCS, tap, key, touch,
           get activity() { return activity; },
           get host() { return $host; } };
})();

if (typeof module !== 'undefined') module.exports = $EAS;
