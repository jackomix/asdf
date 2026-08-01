/* ============================================================================
 * boot.js — application loader + Android lifecycle orchestration.
 *
 *   1. downloads the untouched APK payload (classes.dex, assets, res/raw,
 *      resource table) prepared by tools/build_web.py
 *   2. stands up the web Dalvik VM + virtual device (js/host.js)
 *   3. instantiates the game's real entry point
 *      (net.kairosoft.android.frontier_en.Main, extends Activity) and drives
 *      its lifecycle: onCreate -> SurfaceHolder.surfaceCreated/surfaceChanged
 *      -> onResume, pump, onPause on tab-hide
 *   4. delivers MotionEvent/KeyEvent streams from the browser into the game's
 *      own handlers (untouched engine code decides what a touch means)
 * ========================================================================== */
'use strict';

(() => {

  const $ = (id) => document.getElementById(id);
  const setStatus = (txt, frac) => {
    const el = $('load-status');
    if (el) el.textContent = txt;
    const bar = $('load-bar-fill');
    if (bar && frac !== undefined) bar.style.width = Math.round(frac * 100) + '%';
  };

  /* ---------------------------------------------------------------- */
  /* log plumbing                                                      */
  /* ---------------------------------------------------------------- */
  const logBox = $('log-box');
  const logLines = [];
  const pushLog = (cls, args) => {
    const line = args.map((a) => {
      if (a instanceof Error) return a.message + '\n' + (a.stack || '');
      if (typeof a === 'object') { try { return JSON.stringify(a); } catch (e) { return String(a); } }
      return String(a);
    }).join(' ');
    logLines.push([cls, line]);
    if (logLines.length > 400) logLines.splice(0, logLines.length - 400);
    if (logBox && document.getElementById('log-panel').classList.contains('open')) {
      renderLog();
    }
    if (cls === 'err') {
      console.error(line);
    }
  };
  const renderLog = () => {
    if (!logBox) return;
    logBox.textContent = logLines.map(([c, l]) => (c === 'err' ? '!! ' : c === 'vm' ? 'vm ' : '·  ') + l).join('\n');
    logBox.scrollTop = logBox.scrollHeight;
  };

  /* ---------------------------------------------------------------- */
  /* loading                                                           */
  /* ---------------------------------------------------------------- */
  const fetchBytes = async (url, onErr) => {
    const r = await fetch(url);
    if (!r.ok) { onErr && onErr(url + ': HTTP ' + r.status); return null; }
    return new Uint8Array(await r.arrayBuffer());
  };

  async function loadAll() {
    setStatus('resource table…', 0.03);
    const [appJson, resJson, listJson] = await Promise.all([
      fetch('game/app.json').then((r) => r.json()),
      fetch('game/resources.json').then((r) => r.json()),
      fetch('game/filelist.json').then((r) => r.json()),
    ]);

    const apkTree = new Map();
    let done = 0;
    const total = listJson.length;
    const jobs = listJson.map(([path]) => async () => {
      const bytes = await fetchBytes('game/files/' + path, (m2) => pushLog('err', ['fetch failed', m2]));
      if (bytes) apkTree.set(path, bytes);
      done++;
      if ((done & 3) === 0 || done === total) setStatus('apk payload ' + done + '/' + total + '…', 0.05 + 0.75 * (done / total));
    });
    // modest parallelism
    const queue = jobs.slice();
    const workers = [];
    for (let i = 0; i < 6; i++) {
      workers.push((async () => { while (queue.length) { const j = queue.shift(); await j(); } })());
    }
    await Promise.all(workers);
    setStatus('linking runtime…', 0.84);
    return { appJson, resJson, apkTree };
  }

  /* ---------------------------------------------------------------- */
  /* VM + host construction                                            */
  /* ---------------------------------------------------------------- */
  let vm, host, booted = false, bootError = null;

  async function start() {
    let packet;
    try {
      packet = await loadAll();
    } catch (e) {
      setStatus('download failed: ' + e.message, 1);
      pushLog('err', ['load failed', e]);
      return;
    }

    vm = new VM({});
    vm.onLog = (...a) => pushLog('vm', a);
    vm.onError = (...a) => pushLog('err', a);
    vm.onUncaught = (thr, x) => {
      pushLog('err', ['uncaught in thread ' + thr.name, x.c.desc + (x.vmMsg ? ': ' + x.vmMsg : ''), (x.vmTrace || []).slice(0, 10).join('\n')]);
    };

    installCoreNatives(vm);

    const q = new URLSearchParams(location.search);
    host = new AndroidHost(vm, {
      appInfo: packet.appJson,
      resources: packet.resJson,
      canvasElement: $('screen'),
      width: q.get('w') ? parseInt(q.get('w'), 10) : 800,
      height: q.get('h') ? parseInt(q.get('h'), 10) : 480,
      density: q.get('density') ? parseFloat(q.get('density')) : 1.5,
    });
    host.apkTree = packet.apkTree;
    host.fsLoad();
    vm.hostReadResource = (name) => host.hostReadResource(name);

    installAndroidNatives(vm, host);

    setStatus('parsing classes.dex…', 0.9);
    await tick();
    const dexBytes = packet.apkTree.get('classes.dex');
    const dex = new DexFile(dexBytes);
    vm.loadDex(dex);

    /* quick self-check against a known method table size */
    pushLog('vm', [`dex: ${dex.stringIdsSize} strings, ${dex.methodIdsSize} method refs, ${dex.classDefsSize} classes`]);

    host.onFinish = () => {
      const el = document.createElement('div');
      el.className = 'finish-overlay';
      el.innerHTML = '<div>Epic Astro Story has closed.<br><span style="opacity:.7;font-size:13px">Reload the page to play again — your save data persists.</span></div>';
      document.getElementById('game-ui').appendChild(el);
      host.fsPersist();
    };
    host.onGameFinish = null;
    host.onDialogVisible = (vis) => { /* no-op */ };
    host.onResize = () => layout();

    setStatus('starting activity…', 0.96);
    await tick();

    try {
      bootGame();
    } catch (e) {
      bootError = e;
      pushLog('err', ['boot failed', e]);
      setStatus('boot failed — open the console (ⓘ) for details', 1);
      $('log-panel').classList.add('open');
      renderLog();
      return;
    }

    $('loading').classList.add('hidden');
    setTimeout(() => $('loading').remove(), 600);
    booted = true;
    pushLog('vm', ['boot ok — activity onCreate did ' + vm.stats.insns + ' insns']);
  }

  function bootGame() {
    const actDesc = 'L' + host.appInfo.mainActivity.replace(/\./g, '/') + ';';
    const activityCls = vm.requireClass(actDesc);
    vm.ensureInit(vm.mainThread, activityCls);
    const activity = vm.newObject(activityCls);
    host._activity = activity;
    // Android instantiates the activity: run its <init>()V (sets the
    // engine singleton, creates its Handler, DisplayMetrics, etc.)
    const ctor = activityCls.sigMap.get('<init>()V');
    if (ctor) vm.invokeSync(vm.mainThread, ctor, activity, []);

    // invoke Activity lifecycle through the game's own overrides:
    // onCreate(null) -> onStart() -> onResume()  (standard Android boot sequence;
    // the engine's "started" flag is raised in onStart)
    vm.runOnUi((mt) => {
      vm.call(mt, activity, 'onCreate(Landroid/os/Bundle;)V', [null]);
      vm.call(mt, activity, 'onStart()V', []);
      vm.call(mt, activity, 'onResume()V', []);
    });
    drainAll();
  }

  /* run the VM until quiescent (UI queue empties and threads block) */
  function drainAll() {
    for (let i = 0; i < 400; i++) {
      vm.pump(200000);
      host.pumpLoopers();
    }
  }

  function tick() { return new Promise((r) => setTimeout(r, 0)); }

  /* ---------------------------------------------------------------- */
  /* main loop                                                         */
  /* ---------------------------------------------------------------- */
  let lastFrame = performance.now(), fpsAcc = 0, fpsN = 0, fpsShown = 0;
  let surfaceDeliverAttempts = 0, surfaceDelivered = false;

  function frame(now) {
    requestAnimationFrame(frame);
    if (!booted) return;
    const dt = now - lastFrame;
    lastFrame = now;
    fpsAcc += dt; fpsN++;
    if (fpsAcc >= 500) { fpsShown = Math.round(1000 / (fpsAcc / fpsN)); fpsAcc = 0; fpsN = 0; updateHud(); }

    if (!surfaceDelivered) {
      if (host.deliverSurfaceLifecycle()) {
        surfaceDelivered = true;
        pushLog('vm', ['surface lifecycle delivered']);
      } else if (++surfaceDeliverAttempts > 120) {
        surfaceDelivered = true;
        pushLog('err', ['no SurfaceHolder callback ever registered — touch/input may misbehave']);
      }
    }

    try {
      vm.pump(3000000);
      host.pumpLoopers();
    } catch (e) {
      pushLog('err', ['pump error', e && e.stack || e]);
      if (e instanceof VMThrow) {
        pushLog('err', ['vm exception: ' + e.exc.c.desc + (e.exc.vmMsg ? ': ' + e.exc.vmMsg : ''), (e.exc.vmTrace || []).slice(0, 10).join('\n')]);
      }
    }
  }

  function updateHud() {
    const el = $('fps');
    if (!el) return;
    if (!debugOn()) { el.textContent = ''; return; }
    el.textContent = fpsShown + 'fps ' + (vm ? (vm.stats.insns / 1e6).toFixed(1) + 'M insn total' : '');
  }
  const debugOn = () => new URLSearchParams(location.search).get('debug') === '1';

  /* ---------------------------------------------------------------- */
  /* input                                                             */
  /* ---------------------------------------------------------------- */
  function canvasPoint(clientX, clientY) {
    const el = $('screen');
    const r = el.getBoundingClientRect();
    // CSS keeps aspect: compute inner fit box
    const scale = Math.min(r.width / host.displayWidth, r.height / host.displayHeight);
    const drawW = host.displayWidth * scale, drawH = host.displayHeight * scale;
    const padX = (r.width - drawW) / 2, padY = (r.height - drawH) / 2;
    const x = (clientX - r.left - padX) / scale;
    const y = (clientY - r.top - padY) / scale;
    return [Math.max(0, Math.min(host.displayWidth - 1, x)), Math.max(0, Math.min(host.displayHeight - 1, y))];
  }

  const touchState = { down: false, x: 0, y: 0 };

  function dispatchTouch(action, x, y) {
    if (!booted) return;
    host.unlockAudio();
    const ev = host.makeMotionEvent(action, x, y);
    vm.runOnUi((mt) => {
      let ok = false, err = null;
      const sv = host._surfaceView;
      if (sv) {
        try { vm.call(mt, sv, 'onTouchEvent(Landroid/view/MotionEvent;)Z', [ev]); ok = true; }
        catch (e) { err = e; }
      }
      if (!ok && host._activity) {
        try { vm.call(mt, host._activity, 'dispatchTouchEvent(Landroid/view/MotionEvent;)Z', [ev]); ok = true; }
        catch (e) { err = e; }
      }
      if (!ok && err && action === 0) pushLog('err', ['touch dispatch failed', err.message || String(err)]);
    });
  }

  function dispatchKey(action, keyCode, repeat) {
    if (!booted) return;
    host.unlockAudio();
    const ev = host.makeKeyEvent(action, keyCode, repeat || 0);
    vm.runOnUi((mt) => {
      let ok = false, lastErr = null;
      if (host._activity) {
        try { vm.call(mt, host._activity, 'dispatchKeyEvent(Landroid/view/KeyEvent;)Z', [ev]); ok = true; }
        catch (e) { lastErr = e; }
      }
      if (!ok && keyCode === 4/*KEYCODE_BACK*/) {
        try { vm.call(mt, host._activity, action === 0 ? 'onKeyDown(ILandroid/view/KeyEvent;)Z' : 'onKeyUp(ILandroid/view/KeyEvent;)Z', [keyCode, ev]); ok = true; }
        catch (e) { lastErr = e; }
      }
      if (!ok && lastErr) pushLog('err', ['key dispatch failed', lastErr.message || String(lastErr)]);
    });
  }

  function wireInput() {
    const stage = $('stage');
    const pt = (e) => canvasPoint(e.clientX, e.clientY);

    if (window.PointerEvent) {
      stage.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        const [x, y] = pt(e);
        touchState.down = true; touchState.x = x; touchState.y = y;
        dispatchTouch(0, x, y);
      });
      stage.addEventListener('pointermove', (e) => {
        if (!touchState.down) return;
        e.preventDefault();
        const [x, y] = pt(e);
        touchState.x = x; touchState.y = y;
        dispatchTouch(2, x, y);
      });
      const up = (e) => {
        if (!touchState.down) return;
        e.preventDefault();
        const [x, y] = pt(e);
        touchState.down = false;
        dispatchTouch(1, x, y);
      };
      stage.addEventListener('pointerup', up);
      stage.addEventListener('pointercancel', up);
    }

    /* mouse fallback for PointerEvent-less browsers */
    if (!window.PointerEvent) {
      let down = false;
      stage.addEventListener('mousedown', (e) => { down = true; const [x, y] = pt(e); dispatchTouch(0, x, y); });
      stage.addEventListener('mousemove', (e) => { if (down) { const [x, y] = pt(e); dispatchTouch(2, x, y); } });
      stage.addEventListener('mouseup', (e) => { if (down) { down = false; const [x, y] = pt(e); dispatchTouch(1, x, y); } });
    }

    /* touch events (iOS Safari pre-pointer) */
    if (!window.PointerEvent && 'ontouchstart' in window) {
      // already covered by mousedown fallback + touchstart below for reliability
      stage.addEventListener('touchstart', (e) => {
        e.preventDefault();
        const t = e.changedTouches[0];
        const [x, y] = pt(t.clientX, t.clientY);
        touchState.down = true;
        dispatchTouch(0, x, y);
      }, { passive: false });
      stage.addEventListener('touchmove', (e) => {
        if (!touchState.down) return;
        e.preventDefault();
        const t = e.changedTouches[0];
        const [x, y] = pt(t.clientX, t.clientY);
        dispatchTouch(2, x, y);
      }, { passive: false });
      stage.addEventListener('touchend', (e) => {
        if (!touchState.down) return;
        e.preventDefault();
        touchState.down = false;
        const t = e.changedTouches[0];
        const [x, y] = pt(t.clientX, t.clientY);
        dispatchTouch(1, x, y);
      }, { passive: false });
    }

    const KEYMAP = {
      Backspace: 4, Escape: 4,
      ArrowUp: 19, ArrowDown: 20, ArrowLeft: 21, ArrowRight: 22, Enter: 23, NumpadEnter: 23, Space: 62,
      F1: 82, // menu-ish
    };
    const keyRepeat = {};
    window.addEventListener('keydown', (e) => {
      const kc = KEYMAP[e.key] || KEYMAP[e.code];
      if (kc === undefined) return;
      e.preventDefault();
      host.unlockAudio();
      const rep = keyRepeat[e.key] ? 1 : 0;
      keyRepeat[e.key] = true;
      dispatchKey(0, kc, rep);
    });
    window.addEventListener('keyup', (e) => {
      const kc = KEYMAP[e.key] || KEYMAP[e.code];
      if (kc === undefined) return;
      e.preventDefault();
      keyRepeat[e.key] = false;
      dispatchKey(1, kc, 0);
    });
    window.addEventListener('pointerdown', () => host.unlockAudio(), { once: false });
    window.addEventListener('keydown', () => host.unlockAudio(), { once: false });
  }

  /* ---------------------------------------------------------------- */
  /* layout / lifecycle                                                */
  /* ---------------------------------------------------------------- */
  function layout() {
    const s = $('screen');
    const stage = $('stage');
    const sw = host ? host.displayWidth : 800, sh = host ? host.displayHeight : 480;
    const vw = stage.clientWidth, vh = stage.clientHeight;
    const scale = Math.min(vw / sw, vh / sh);
    s.style.width = Math.floor(sw * scale) + 'px';
    s.style.height = Math.floor(sh * scale) + 'px';
  }

  function wireLifecycle() {
    window.addEventListener('resize', layout);
    document.addEventListener('visibilitychange', () => {
      if (!booted) return;
      if (document.hidden) {
        vm.runOnUi((mt) => {
          safeLife('onPause()V');
        });
        host.fsPersist();
      } else {
        vm.runOnUi((mt) => {
          safeLife('onResume()V');
        });
      }
    });
    window.addEventListener('pagehide', () => {
      if (booted) {
        vm.runOnUi((mt) => { safeLife('onPause()V'); safeLife('onStop()V'); });
        host.fsPersist();
      }
    });
    window.addEventListener('beforeunload', () => { if (host) host.fsPersist(); });
  }
  function safeLife(sig) {
    if (!host._activity) return;
    try { vm.call(vm.mainThread, host._activity, sig); }
    catch (e) { pushLog('err', [sig + ' failed', e.message || String(e)]); }
    try { vm.pump(500000); host.pumpLoopers(); } catch (e2) { }
  }

  /* ---------------------------------------------------------------- */
  /* hud buttons                                                       */
  /* ---------------------------------------------------------------- */
  function wireHud() {
    const logBtn = $('log-btn');
    logBtn.addEventListener('click', () => {
      $('log-panel').classList.toggle('open');
      renderLog();
    });
    $('log-close').addEventListener('click', () => $('log-panel').classList.remove('open'));
    if (debugOn()) $('log-btn').style.display = 'flex';
  }

  /* ---------------------------------------------------------------- */
  document.addEventListener('DOMContentLoaded', () => {
    wireHud();
    wireInput();
    wireLifecycle();
    layout();
    requestAnimationFrame(frame);
    start();
  });

})();
