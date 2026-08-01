/* ============================================================================
 * tools/headless_test.js — boots the web Dalvik VM under Node.js with a
 * stubbed DOM/canvas/audio so engine-boot crashes (linkage errors, opcode
 * bugs, missing natives) surface in seconds without a browser.
 *
 *   node tools/headless_test.js [million_instructions_budget]
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');
const { DexFile } = require(path.join(ROOT, 'js', 'dex.js'));
const { VM, VMThrow } = require(path.join(ROOT, 'js', 'vm.js'));
const { installCoreNatives } = require(path.join(ROOT, 'js', 'natives-core.js'));
const { installAndroidNatives } = require(path.join(ROOT, 'js', 'natives-android.js'));
const { AndroidHost } = require(path.join(ROOT, 'js', 'host.js'));

/* ------------------------------------------------------------------- */
/* DOM / browser stubs (only what our code touches)                    */
/* ------------------------------------------------------------------- */
const noop = () => { };
function stubCtx() {
  return new Proxy({
    canvas: null,
    save: noop, restore: noop, beginPath: noop, rect: noop, clip: noop,
    transform: noop, setTransform: noop, translate: noop, scale: noop, rotate: noop,
    drawImage: noop,
    fillRect: noop, strokeRect: noop, clearRect: noop,
    moveTo: noop, lineTo: noop, stroke: noop, fill: noop, closePath: noop,
    ellipse: noop, arc: noop,
    createImageData: (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
    getImageData: (x, y, w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
    putImageData: noop,
    measureText: (s) => ({ width: (s ? s.length : 0) * 6, actualBoundingBoxAscent: 9, actualBoundingBoxDescent: 2 }),
    fillText: noop,
    toDataURL: null,
  }, {
    get(t, k) {
      if (k in t) return t[k];
      if (k === 'toDataURL') return () => 'data:image/png;base64,iVBORw0KGgo=';
      return undefined;
    },
    set(t, k, v) { t[k] = v; return true; },
  });
}
const { SWCanvas, dumpPng } = require(path.join(__dirname, 'swcanvas.js'));
function makeCanvasStub() { return new SWCanvas(); }
global.ImageData = class {
  constructor(w, h) { this.width = w; this.height = h; this.data = new Uint8ClampedArray(w * h * 4); }
};
const _elements = new Map();
const _draftEl = (id) => {
  const el = {
    id, style: {}, classList: { add: noop, remove: noop, toggle: noop, contains: () => false },
    addEventListener: noop, removeEventListener: noop,
    appendChild: noop, removeChild: noop, remove: noop,
    textContent: '', innerHTML: '', offsetWidth: 800, clientWidth: 1280, clientHeight: 720,
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 480 }),
    click: noop, focus: noop,
  };
  return el;
};
global.document = {
  createElement: (tag) => tag === 'canvas' ? makeCanvasStub() : _draftEl(tag),
  createTextNode: (t) => ({ text: t }),
  getElementById: (id) => {
    if (!_elements.has(id)) _elements.set(id, id === 'screen' ? makeCanvasStub() : _draftEl(id));
    return _elements.get(id);
  },
  addEventListener: noop,
  removeEventListener: noop,
  body: _draftEl('body'),
  hidden: false,
  fontMetricsElement: null,
};
global.window = {
  addEventListener: noop, removeEventListener: noop,
  innerWidth: 1280, innerHeight: 720,
  PointerEvent: undefined,
  open: noop,
};
try { global.navigator = { userAgent: 'node-headless', vibrate: null }; }
catch (e) { Object.defineProperty(globalThis, 'navigator', { value: { userAgent: 'node-headless', vibrate: null }, configurable: true, writable: true }); }
global.location = { search: '', hash: '', href: 'http://localhost/', protocol: 'http:', host: 'localhost' };
global.performance = { now: () => Date.now() };
global.requestAnimationFrame = (cb) => setTimeout(() => cb(Date.now()), 0);
global.localStorage = {
  _m: new Map(),
  getItem(k) { return this._m.has(k) ? this._m.get(k) : null; },
  setItem(k, v) { this._m.set(k, String(v)); },
  removeItem(k) { this._m.delete(k); },
};
global.Audio = class {
  constructor(url) { this.url = url; this.paused = true; this.loop = false; this.volume = 1; this.currentTime = 0; this.duration = 60; this.readyState = 4; this._listeners = {}; }
  addEventListener(t, f) { this._listeners[t] = f; }
  play() { this.paused = false; return Promise.resolve(); }
  pause() { this.paused = true; }
  cloneNode() { const a = new Audio(this.url); return a; }
};
global.atob = (b64) => Buffer.from(b64, 'base64').toString('binary');
global.btoa = (bin) => Buffer.from(bin, 'binary').toString('base64');
global.URLSearchParams = URLSearchParams;
global.fetch = async (url) => {
  const p = path.join(ROOT, url);
  const buf = fs.readFileSync(p);
  return {
    ok: true, status: 200,
    arrayBuffer: async () => buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength),
    json: async () => JSON.parse(buf.toString('utf8')),
  };
};
global.setImmediate = global.setImmediate || ((f) => setTimeout(f, 0));

/* ------------------------------------------------------------------- */
/* boot                                                                */
/* ------------------------------------------------------------------- */
const BUDGET_MILLIONS = parseInt(process.argv[2] || '60', 10);
const DUMP_DIR = process.env.FRAMEDUMP || null;   // e.g. FRAMEDUMP=/tmp/frames node tools/headless_test.js
let _dumped = 0;

async function main() {
  console.log('== headless boot ==');
  const appJson = JSON.parse(fs.readFileSync(path.join(ROOT, 'game', 'app.json'), 'utf8'));
  const resJson = JSON.parse(fs.readFileSync(path.join(ROOT, 'game', 'resources.json'), 'utf8'));
  const listJson = JSON.parse(fs.readFileSync(path.join(ROOT, 'game', 'filelist.json'), 'utf8'));

  const apkTree = new Map();
  for (const [p] of listJson) {
    apkTree.set(p, new Uint8Array(fs.readFileSync(path.join(ROOT, 'game', 'files', p))));
  }
  console.log('apk files:', apkTree.size);

  const logAll = (prefix) => (...a) => {
    const line = a.map((x) => (x instanceof Error ? x.message + '\n' + x.stack : String(x))).join(' ');
    console.log(prefix + line);
  };

  const vm = new VM({});
  vm.onLog = logAll('[vm] ');
  vm.onError = logAll('[err] ');
  vm.onUncaught = (thr, x) => {
    console.log('[uncaught] thread=' + thr.name + ' ' + x.c.desc + (x.vmMsg ? ': ' + x.vmMsg : '') + (x._faultM ? ' fault@ ' + x._faultM.fullName() + ' off=0x' + x._faultPc.toString(16) : ''));
    if (x.vmTrace) console.log(x.vmTrace.slice(0, 10).join('\n'));
  };

  installCoreNatives(vm);
  global.PNG = require(path.join(ROOT, 'js', 'png.js'));

  const host = new AndroidHost(vm, {
    appInfo: appJson,
    resources: resJson,
    canvasElement: document.getElementById('screen'),
    width: 800, height: 480, density: 1.5,
  });
  host.apkTree = apkTree;
  host.fsLoad();
  vm.hostReadResource = (nm) => host.hostReadResource(nm);
  if (process.env.AUTODLG !== '0') {
    const origDialogShow = host.dialogShow.bind(host);
    host.dialogShow = (dlg) => {
      origDialogShow(dlg);
      const cfg = dlg._cfg || {};
      const b = (cfg.buttons || [])[0];
      const vm2 = host.vm;
      // simulate clicking the first (usually only) dialog button a bit later
      vm2.uiQueue.push(() => {
        host.dialogDismiss(dlg);
        if (b && b.listener && b.listener !== 0) {
          vm2.call(vm2.mainThread, b.listener, 'onClick(Landroid/content/DialogInterface;I)V', [dlg, b.which]);
        } else if (cfg.onCancel && cfg.cancelable !== false) {
          vm2.call(vm2.mainThread, cfg.onCancel, 'onCancel(Landroid/content/DialogInterface;)V', [dlg]);
        }
      });
    };
  }

  installAndroidNatives(vm, host);

  const dexBytes = apkTree.get('classes.dex');
  const dex = new DexFile(dexBytes);
  vm.loadDex(dex);
  /* count presents + locks + renderer calls */
  {
    const wrap1 = (clsD, sig, name) => {
      const cls = vm.requireClass(clsD);
      const m2 = cls.sigMap.get(sig);
      if (!m2 || !m2.native) { console.log('[probe] cannot wrap', clsD, sig); return; }
      const orig = m2.native;
      host['_n_' + name] = 0;
      m2.native = (vm2, thr, o, args) => { host['_n_' + name]++; return orig(vm2, thr, o, args); };
    };
    wrap1('Landroid/view/SurfaceHolderWeb;', 'lockCanvas()Landroid/graphics/Canvas;', 'lock');
    wrap1('Landroid/view/SurfaceHolderWeb;', 'unlockCanvasAndPost(Landroid/graphics/Canvas;)V', 'unlock');
  }
  /* count canvas ops hitting the screen bitmap */
  {
    const kc = vm.requireClass('Landroid/graphics/Canvas;');
    host._drawOps = 0; host._screenOps = 0;
    for (const m2 of kc.methods) {
      if (!m2.native || !/draw/.test(m2.sig)) continue;
      const orig = m2.native;
      m2.native = (vm2, thr, o, args) => {
        host._drawOps++;
        if (o.bitmap === host._screenBitmapNative) host._screenOps++;
        return orig(vm2, thr, o, args);
      };
    }
  }

  /* per-method dispatch counters for the render path (env MCOUNTS=1; adds overhead) */
  const mcounts = new Map();
  if (process.env.MCOUNTS === '1') {
    const origResolve = VM.prototype._resolveInvoke;
    VM.prototype._resolveInvoke = function (thr, op, refIdx, callerMethod, receiver) {
      const rs = origResolve.call(this, thr, op, refIdx, callerMethod, receiver);
      const nm = rs.m.fullName();
      if (nm.indexOf('kairo/android') >= 0 || nm.indexOf('Lc/a;') >= 0) mcounts.set(nm, (mcounts.get(nm) || 0) + 1);
      return rs;
    };
  }
  globalThis.__mcounts = mcounts;

  /* log every thrown (even internally caught) VM exception, deduped */
  if (process.env.THROWLOG !== '0') {
    const seen = new Map();
    vm.onThrow = (exc, thr, m, pc) => {
      try {
        const key = exc.c.desc + '@' + (exc._faultM ? exc._faultM.fullName() : '?') + ':' + (exc._faultPc | 0).toString(16);
        const n = (seen.get(key) || 0) + 1;
        seen.set(key, n);
        if (n > 3) return;
        let dbg = '';
        try {
          const dc = vm.findClass('Lkairo/a/a/a;');
          if (dc && dc.statics) {
            const g = (nm) => { const f = dc.sfields.find((x) => x.name === nm); return f ? dc.statics[f.slot] : '?'; };
            dbg = ` a/a/a.f,g,h=${g('f')},${g('g')},${g('h')}`;
          }
        } catch (_) { }
        console.log(`[throw#${n}] ${exc.c.desc}${exc.vmMsg ? ': ' + exc.vmMsg : ''}${dbg}`);
        console.log((exc.vmTrace || []).slice(0, 10).join('\n'));
      } catch (_) { }
    };
  }
  const actDesc = 'L' + appJson.mainActivity.replace(/\./g, '/') + ';';
  console.log('boot activity:', actDesc);

  const activityCls = vm.requireClass(actDesc);
  let bootErr = null;
  try {
    vm.ensureInit(vm.mainThread, activityCls);
    const activity = vm.newObject(activityCls);
    host._activity = activity;
    const ctor = activityCls.sigMap.get('<init>()V');
    if (ctor) vm.invokeSync(vm.mainThread, ctor, activity, []);
    vm.call(vm.mainThread, activity, 'onCreate(Landroid/os/Bundle;)V', [null]);
    vm.call(vm.mainThread, activity, 'onStart()V', []);
    vm.call(vm.mainThread, activity, 'onResume()V', []);
  } catch (e) {
    bootErr = e;
  }
  if (bootErr) {
    if (bootErr instanceof VMThrow) {
      const x = bootErr.exc;
      console.log('BOOT VM EXCEPTION: ' + x.c.desc + (x.vmMsg ? ': ' + x.vmMsg : '') + (x._faultM ? ' fault@ ' + x._faultM.fullName() + ' off=0x' + x._faultPc.toString(16) : ''));
      console.log((x.vmTrace || []).slice(0, 16).join('\n'));
    } else {
      console.log('BOOT ERROR: ' + bootErr.message + '\n' + (bootErr.stack || ''));
    }
    process.exit(2);
  }

  /* pump loop — virtual clock. MUST stay continuous with the real-time stamps the
     * game captured during boot (e.g. Lc/a.<init> ->b J): starting at an arbitrary
     * origin makes (now - b) negative forever and the frame-pacing loop in
     * Lc/a.d()V never exits. Advance one frame quantum (~50ms = 1000/21fps) per pump. */
  console.log('== pump ==');
  const VSTEP = parseFloat(process.env.VSTEP || '50');
  vm.virtualTime = Date.now();
  const VBASE = vm.virtualTime;
  const maxInsns = BUDGET_MILLIONS * 1e6;
  const maxFrames = parseInt(process.argv[3] || '1000000000', 10);
  let last = Date.now();
  let errs = 0;
  let delivered = false;
  let frames = 0;
  /* scripted touches: env TOUCH="frame:x,y;frame:x,y;..." in physical canvas px */
  const touchScript = (process.env.TOUCH || '').split(';').filter(Boolean).map((s) => {
    const parts = s.split(':'); const xy = parts[1].split(',');
    return { at: parseInt(parts[0], 10), x: Number(xy[0]), y: Number(xy[1]), done: false };
  });
  const pendingUps = [];
  const injectTouch = (action, x, y) => {
    const ev = host.makeMotionEvent(action, x, y);
    vm.runOnUi((mt) => {
      try { vm.call(mt, host._surfaceView, 'onTouchEvent(Landroid/view/MotionEvent;)Z', [ev]); }
      catch (e) { console.log('[touch] dispatch err: ' + (e && e.message)); }
    });
  };
  while (vm.stats.insns < maxInsns && frames < maxFrames) {
    vm.virtualTime += VSTEP;
    for (const t of touchScript) {
      if (!t.done && t.at === frames) {
        t.done = true;
        console.log(`[touch] DOWN @${t.x},${t.y} frame=${frames}`);
        injectTouch(0, t.x, t.y);
        pendingUps.push({ upAt: frames + 2, x: t.x, y: t.y, done: false });
      }
    }
    for (const u of pendingUps) {
      if (!u.done && u.upAt <= frames) {
        u.done = true;
        console.log(`[touch] UP @${u.x},${u.y} frame=${frames}`);
        injectTouch(1, u.x, u.y);
      }
    }
    frames++;
    if (DUMP_DIR && frames % (parseInt(process.env.DUMPEVERY || '60', 10)) === 0) {
      try {
        const cn = document.getElementById('screen');
        const out = path.join(DUMP_DIR, 'frame_' + String(frames).padStart(5, '0') + '.png');
        fs.mkdirSync(DUMP_DIR, { recursive: true });
        dumpPng(cn, out);
        _dumped++;
      } catch (e) { console.log('[dump] failed: ' + e.message); }
    }
    if (!delivered) { delivered = host.deliverSurfaceLifecycle() || delivered; if (delivered) console.log('[stat] surface lifecycle delivered at frame ' + frames); }
    try {
      /* budget ~one frame of real work; the frame-pacing spin (Lc/a.d) would
       * otherwise burn the whole remainder sleeping until the clock advances */
      vm.pump(parseInt(process.env.PUMPMAX || '400000', 10));
      host.pumpLoopers();
    } catch (e) {
      errs++;
      if (e instanceof VMThrow) {
        const x = e.exc;
        console.log('PUMP VM EXCEPTION: ' + x.c.desc + (x.vmMsg ? ': ' + x.vmMsg : '') + (x._faultM ? ' fault@ ' + x._faultM.fullName() + ' off=0x' + x._faultPc.toString(16) : ''));
        console.log((x.vmTrace || []).slice(0, 16).join('\n'));
      } else {
        console.log('PUMP ERROR: ' + e.message);
        console.log((e.stack || '').split('\n').slice(0, 14).join('\n'));
      }
      if (errs >= 3) { console.log('too many errors, abort'); process.exit(3); }
    }
    if (frames % 20 === 0 && process.env.TOS === '1') {
      const t91 = vm.threads.find((t) => t !== vm.mainThread && !t.dead && t.frames.length);
      if (t91) {
        const top = t91.frames.slice(-3).map((f) => f.m.fullName() + '@' + (f.pc | 0).toString(16)).join(' <- ');
        console.log(`  [tos f${frames}] ${top} | blocked=${t91.blockedUntil > vm.now()}`);
      }
    }
    if (frames % 20 === 0) {
      try {
        const lCls = vm.requireClass('Lkairo/android/ui/l;');
        const lObj = lCls.statics[lCls.sfields.find((f) => f.name === 'b').slot];
        const caCls = vm.requireClass('Lc/a;');
        const getter = caCls.sigMap.get('a()Lc/a;');
        const ca = vm.invokeSync(vm.mainThread, getter, null, []);
        const dump = {};
        for (const f of ca.c.ifields) {
          const v = ca.f[f.slot];
          dump[f.name + ':' + f.desc] = (v !== null && typeof v === 'object') ? (v.c ? '[ref ' + v.c.simpleName + ']' : String(v)) : String(v);
        }
        console.log('[probe] l.i(renderer)=', !!vm.invokeSync(vm.mainThread, lObj.c.sigMap.get('l()Lkairo/android/a/a;'), lObj, []), 'l=', JSON.stringify(dump));
      } catch (e) { console.log('[probe] err', e.message, e.stack ? e.stack.split('\n')[1] : ''); }
    }
    if (frames % 60 === 0 && frames >= 540 || frames % 600 === 0) {
      const tinfo = vm.threads.map((t) => `${t.name}:${t.dead ? 'X' : 'R'}${t.blockedUntil > vm.now() ? '(z)' : ''}f${t.frames.length}`).join(' ');
      console.log(`[stat] frame=${frames} simT=${((vm.virtualTime - VBASE) / 1000).toFixed(1)}s lock=${host._n_lock} unlock=${host._n_unlock} drawOps=${host._drawOps} screenOps=${host._screenOps} presents=${host.frameCount} threads=[${tinfo}]`);
      if (frames % 240 === 0) {
        const top = [...mcounts.entries()].filter(([k]) => /ui\/c;|\/l;->d\(\)V|a\/a;->a\(Z\)V|g\/b;->h\(\)V|IApplication;->b\(I\)V|c\/a;->d\(\)V/.test(k)).map(([k, v]) => k + '=' + v).join(' ');
        console.log('  [mcounts] ' + top);
      }
      for (const th of vm.threads) {
        if (th.frames.length) {
          const stk = th.frames.map((f) => '      ' + f.m.fullName() + ' @' + (f.pc | 0).toString(16)).join('\n');
          console.log('  stack(' + th.name + '):\n' + stk);
        }
      }
    }
  }
  console.log('== budget reached ==');
  console.log(`insns=${vm.stats.insns} nativeInvokes=${vm.stats.nativeInvokes} objects=${vm.stats.objects}`);
  const cls = [...vm.classesByName.values()].filter((c) => !c.pending);
  console.log('linked classes:', cls.length);
  console.log('HEADLESS OK');
}

main().catch((e) => { console.log('FATAL', e && e.stack || e); process.exit(1); });
