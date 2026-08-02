/* ============================================================================
 * tools/dom_stubs.js — minimal DOM/canvas/audio/localStorage/fetch stubs so
 * the web-port's UNMODIFIED browser scripts (see index.html <script> order)
 * can run under Node.js. Shared by tools/headless_test.js (fast VM boot) and
 * tools/boot_test.js (full browser-script boot via boot.js).
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const { SWCanvas, dumpPng } = require(path.join(__dirname, 'swcanvas.js'));

const noop = () => { };

function installDomStubs(ROOT) {
  const listeners = { document: {}, window: {} };   // event -> [fn]
  const elements = new Map();

  const mkClassList = () => {
    const set = new Set();
    return {
      add: (...c) => c.forEach((x) => set.add(x)),
      remove: (...c) => c.forEach((x) => set.delete(x)),
      toggle: (c) => (set.has(c) ? set.delete(c) : set.add(c)),
      contains: (c) => set.has(c),
      _set: set,
    };
  };

  const mkEl = (id) => {
    const el = {
      id: id || '', tagName: '', style: {}, classList: mkClassList(),
      _listeners: {},
      addEventListener(t, f) { (this._listeners[t] = this._listeners[t] || []).push(f); },
      removeEventListener: noop,
      appendChild: (c) => c, removeChild: noop, remove: noop,
      insertBefore: (c) => c,
      setAttribute: noop, focus: noop, click: noop, blur: noop,
      textContent: '', innerHTML: '', value: '',
      offsetWidth: 800, clientWidth: 1280, clientHeight: 720,
      width: 480, height: 320,
      getBoundingClientRect: () => ({ left: 0, top: 0, width: parseInt(process.env.W || '480', 10), height: parseInt(process.env.H || '320', 10) }),
    };
    return el;
  };

  const makeCanvasStub = () => {
    const sw = new SWCanvas();
    sw.id = '';
    sw._listeners = {};
    sw.addEventListener = (t, f) => { (sw._listeners[t] = sw._listeners[t] || []).push(f); };
    sw.removeEventListener = noop;
    sw.remove = noop;
    sw.classList = mkClassList();
    sw.getBoundingClientRect = () => ({ left: 0, top: 0, width: parseInt(process.env.W || '480', 10), height: parseInt(process.env.H || '320', 10) });
    return sw;
  };

  global.document = {
    createElement: (tag) => tag === 'canvas' ? makeCanvasStub() : mkEl(tag),
    createTextNode: (t) => ({ text: t }),
    getElementById: (id) => {
      if (!elements.has(id)) elements.set(id, id === 'screen' ? makeCanvasStub() : mkEl(id));
      return elements.get(id);
    },
    addEventListener: (t, f) => { (listeners.document[t] = listeners.document[t] || []).push(f); },
    removeEventListener: noop,
    body: mkEl('body'),
    hidden: false,
    fontMetricsElement: null,
  };
  global.window = {
    addEventListener: (t, f) => { (listeners.window[t] = listeners.window[t] || []).push(f); },
    removeEventListener: noop,
    innerWidth: 1280, innerHeight: 720,
    PointerEvent: undefined,
    open: noop,
    location: { search: '', hash: '', href: 'http://localhost/', protocol: 'http:', host: 'localhost' },
  };
  global.location = global.window.location;
  global.performance = { now: () => Date.now() };
  global.requestAnimationFrame = (cb) => setTimeout(() => cb(Date.now()), 0);
  global.localStorage = {
    _m: new Map(),
    getItem(k) { return this._m.has(k) ? this._m.get(k) : null; },
    setItem(k, v) { this._m.set(k, String(v)); },
    removeItem(k) { this._m.delete(k); },
  };
  global.__audioEls = [];
  global.Audio = class {
    constructor(url) {
      this.url = url; this.paused = true; this.loop = false; this.volume = 1;
      this.currentTime = 0; this.duration = 60; this.readyState = 4;
      this._listeners = {}; this.playCount = 0; this.pauseCount = 0; this.preload = '';
      global.__audioEls.push(this);
    }
    addEventListener(t, f) { this._listeners[t] = f; }
    play() { this.paused = false; this.playCount++; return Promise.resolve(); }
    pause() { this.paused = true; this.pauseCount++; }
    /* test helper: simulate the track playing to its natural end */
    simulateEnded() { const f = this._listeners['ended']; if (f) f(); }
    cloneNode() { const a = new global.Audio(this.url); return a; }
  };
  global.ImageData = class {
    constructor(w, h) { this.width = w; this.height = h; this.data = new Uint8ClampedArray(w * h * 4); }
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
      blob: async () => ({ arrayBuffer: async () => buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) }),
      text: async () => buf.toString('utf8'),
    };
  };
  global.setImmediate = global.setImmediate || ((f) => setTimeout(f, 0));
  try { global.navigator = { userAgent: 'node-headless', vibrate: null, maxTouchPoints: 0 }; }
  catch (e) { Object.defineProperty(globalThis, 'navigator', { value: { userAgent: 'node-headless', vibrate: null, maxTouchPoints: 0 }, configurable: true, writable: true }); }

  const fire = (target, type, ev) => {
    const list = target === 'document' ? listeners.document[type]
      : target === 'window' ? listeners.window[type]
        : (target._listeners && target._listeners[type]);
    if (list) for (const f of list.slice()) { try { f(ev || { preventDefault: noop, target }); } catch (e) { console.log('[fire ' + type + ' err]', e && e.message); } }
  };

  return { elements, listeners, fire, makeCanvasStub, dumpPng };
}

module.exports = { installDomStubs };
