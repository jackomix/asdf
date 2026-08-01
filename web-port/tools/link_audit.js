/* ============================================================================
 * tools/link_audit.js — static linkage audit: loads the VM + natives + DEX,
 * links every DEX class, then verifies that every method/field reference in
 * the DEX can be resolved (through DEX super chains into the native stubs).
 * Prints all unresolved references. Usage: node tools/link_audit.js
 * ========================================================================== */
'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');
const { DexFile } = require(path.join(ROOT, 'js', 'dex.js'));
const { VM } = require(path.join(ROOT, 'js', 'vm.js'));
const { installCoreNatives } = require(path.join(ROOT, 'js', 'natives-core.js'));
const { installAndroidNatives } = require(path.join(ROOT, 'js', 'natives-android.js'));
const { AndroidHost } = require(path.join(ROOT, 'js', 'host.js'));

/* minimal DOM stubs (copied from headless_test.js) */
const noop = () => { };
global.ImageData = class { constructor(w, h) { this.width = w; this.height = h; this.data = new Uint8ClampedArray(w * h * 4); } };
function stubCtx() {
  return new Proxy({
    canvas: null,
    measureText: (s) => ({ width: (s ? s.length : 0) * 6, actualBoundingBoxAscent: 9, actualBoundingBoxDescent: 2 }),
    createImageData: (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
    getImageData: (x, y, w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
  }, {
    get(t, k) { if (k in t) return t[k]; if (k === 'toDataURL') return () => 'data:,'; return noop; },
    set(t, k, v) { t[k] = v; return true; },
  });
}
function makeCanvas() { return { width: 300, height: 300, getContext: () => stubCtx(), style: {}, addEventListener: noop, getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 480 }) }; }
const els = new Map();
global.document = {
  createElement: (t) => t === 'canvas' ? makeCanvas() : { style: {}, appendChild: noop, addEventListener: noop, classList: { add: noop, remove: noop } },
  getElementById: (id) => { if (!els.has(id)) els.set(id, id === 'screen' ? makeCanvas() : { style: {}, addEventListener: noop, textContent: '', clientWidth: 1280, clientHeight: 720 }); return els.get(id); },
  addEventListener: noop, body: { appendChild: noop }, hidden: false,
};
global.window = { addEventListener: noop, innerWidth: 1280, innerHeight: 720 };
try { global.navigator = { userAgent: 'audit' }; } catch (e) { Object.defineProperty(globalThis, 'navigator', { value: { userAgent: 'audit' }, configurable: true }); }
global.location = { search: '', href: 'http://x/' };
global.performance = { now: () => Date.now() };
global.requestAnimationFrame = (cb) => setTimeout(cb, 0);
global.Audio = class { addEventListener() { } play() { return Promise.resolve(); } pause() { } };
global.localStorage = { _m: new Map(), getItem(k) { return this._m.has(k) ? this._m.get(k) : null; }, setItem(k, v) { this._m.set(k, String(v)); }, removeItem(k) { this._m.delete(k); } };
global.PNG = require(path.join(ROOT, 'js', 'png.js'));

const appJson = JSON.parse(fs.readFileSync(path.join(ROOT, 'game', 'app.json'), 'utf8'));
const resJson = JSON.parse(fs.readFileSync(path.join(ROOT, 'game', 'resources.json'), 'utf8'));
const apkTree = new Map();
for (const [p] of JSON.parse(fs.readFileSync(path.join(ROOT, 'game', 'filelist.json'), 'utf8'))) {
  apkTree.set(p, new Uint8Array(fs.readFileSync(path.join(ROOT, 'game', 'files', p))));
}

const vm = new VM({});
vm.onLog = () => { };
installCoreNatives(vm);
const host = new AndroidHost(vm, { appInfo: appJson, resources: resJson, canvasElement: document.getElementById('screen'), width: 800, height: 480, density: 1.5 });
host.apkTree = apkTree;
vm.hostReadResource = (nm) => host.hostReadResource(nm);
installAndroidNatives(vm, host);
const dex = new DexFile(apkTree.get('classes.dex'));
vm.loadDex(dex);

/* ---- link all dex classes ---- */
let linkErrs = 0;
for (const cd of dex.classDefs) {
  const desc = dex.typeDesc(cd.classIdx);
  try { vm.requireClass(desc); }
  catch (e) { linkErrs++; console.log('LINK FAIL ' + desc + ': ' + e.message); }
}
console.log('classes linked:', [...vm.classesByName.values()].filter((c) => !c.pending).length, 'link errors:', linkErrs);

/* ---- resolve every method ref ---- */
const methUnresolved = new Map(); // key -> count
for (let i = 0; i < dex.methodIdsSize; i++) {
  const ref = dex.methodRef(i);
  try {
    const cls = vm.requireClass(ref.classDesc);
    const sig = ref.name + ref.proto.desc;
    let found = cls.sigMap.get(sig);
    if (!found) {
      // natives with `missing` hook?
      if (cls.nativeDef && cls.nativeDef.missing) continue;
      methUnresolved.set(ref.classDesc + '->' + sig, 1);
    }
  } catch (e) {
    methUnresolved.set(ref.classDesc + '->' + ref.name + ' [class ' + e.message + ']', 1);
  }
}
console.log('\n=== unresolved method refs:', methUnresolved.size, '===');
for (const k of [...methUnresolved.keys()].sort()) console.log('  ' + k);

/* ---- resolve every field ref ---- */
const fldUnresolved = new Map();
for (let i = 0; i < dex.fieldIdsSize; i++) {
  const ref = dex.fieldRef(i);
  try {
    vm._resolveField(i);
  } catch (e) {
    fldUnresolved.set(ref.classDesc + '->' + ref.name + ' ' + ref.desc + ' [' + e.message.slice(0, 80) + ']', 1);
  }
}
console.log('\n=== unresolved field refs:', fldUnresolved.size, '===');
for (const k of [...fldUnresolved.keys()].sort()) console.log('  ' + k);
console.log('\nAUDIT DONE');
