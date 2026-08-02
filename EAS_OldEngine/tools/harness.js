/* =============================================================================
 * tools/harness.js -- shared head-less host used by tools/smoke.js and
 * tools/render.js.  Builds the minimal DOM the boot bridge expects, wires a
 * canvas factory (mock or a real rasteriser) and loads exactly the four
 * scripts web/index.html loads, in the same order.
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '..');
const WEB = path.join(ROOT, 'web');

/* -------------------------------------------------------- recording ctx */
const DRAW = new Set(['drawImage', 'fillRect', 'fillText', 'fill', 'stroke',
  'strokeRect', 'putImageData']);

function mockCanvasFactory(stats) {
  return function mockCanvas(w, h) {
    const cv = { width: w | 0 || 1, height: h | 0 || 1, style: {}, $mock: true };
    let c2d = null;
    cv.getContext = () => (c2d || (c2d = mockCtx(cv, stats)));
    cv.getBoundingClientRect = () =>
      ({ left: 0, top: 0, width: cv.width, height: cv.height });
    cv.addEventListener = () => {};
    cv.removeEventListener = () => {};
    cv.setPointerCapture = () => {};
    cv.appendChild = () => {};
    return cv;
  };
}

function mockCtx(cv, stats) {
  const ctx = {
    canvas: cv,
    fillStyle: '#000', strokeStyle: '#000', font: '10px sans-serif',
    globalAlpha: 1, globalCompositeOperation: 'source-over',
    lineWidth: 1, textBaseline: 'alphabetic', imageSmoothingEnabled: true,
    measureText: (s) => ({
      width: String(s).length * 6,
      actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2,
    }),
    getImageData: (x, y, w, h) => ({
      width: Math.max(1, w | 0), height: Math.max(1, h | 0),
      data: new Uint8ClampedArray(Math.max(1, (w | 0) * (h | 0)) * 4),
    }),
    createImageData: (w, h) => ({
      width: Math.max(1, w | 0), height: Math.max(1, h | 0),
      data: new Uint8ClampedArray(Math.max(1, (w | 0) * (h | 0)) * 4),
    }),
  };
  for (const m of ['save', 'restore', 'setTransform', 'beginPath', 'closePath',
    'moveTo', 'lineTo', 'rect', 'arc', 'ellipse', 'clip', 'fill', 'stroke',
    'fillRect', 'strokeRect', 'fillText', 'drawImage', 'putImageData',
    'translate', 'scale', 'rotate', 'transform', 'clearRect']) {
    ctx[m] = function () { if (DRAW.has(m)) stats.drawOps++; };
  }
  return ctx;
}

/* ------------------------------------------------------------------ DOM */
function mkEl(tag, byId) {
  const el = {
    tagName: String(tag).toUpperCase(),
    style: {}, dataset: {}, children: [], parentNode: null,
    className: '', textContent: '', innerHTML: '', value: '',
    classList: { add() {}, remove() {}, toggle() {}, contains: () => false },
    appendChild(c) { c.parentNode = el; el.children.push(c); return c; },
    removeChild(c) {
      const i = el.children.indexOf(c);
      if (i >= 0) el.children.splice(i, 1);
      c.parentNode = null; return c;
    },
    remove() { if (el.parentNode) el.parentNode.removeChild(el); },
    insertAdjacentHTML() {},
    setAttribute() {}, getAttribute: () => null, removeAttribute() {},
    addEventListener() {}, removeEventListener() {},
    focus() {}, blur() {}, click() {},
    querySelector: () => mkEl('div', byId),
    querySelectorAll: () => [],
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 480, height: 800 }),
    select() {}, setSelectionRange() {},
  };
  let _id = '';
  Object.defineProperty(el, 'id', {
    get: () => _id,
    set: (v) => { _id = v; byId[v] = el; },
  });
  return el;
}

/**
 * @param {object} opts
 *   canvasFactory  (w,h) -> canvas   (defaults to the recording mock)
 * @returns { sandbox, stats, load(), boot() }
 */
function createHost(opts) {
  opts = opts || {};
  const stats = { drawOps: 0, errors: [] };
  const byId = Object.create(null);
  const canvasFactory = opts.canvasFactory || mockCanvasFactory(stats);

  const document = {
    head: mkEl('head', byId),
    body: mkEl('body', byId),
    hidden: false,
    activeElement: null,
    createElement(tag) {
      if (String(tag).toLowerCase() === 'canvas') return canvasFactory(1, 1);
      return mkEl(tag, byId);
    },
    createTextNode: (t) => ({ textContent: t }),
    getElementById: (id) => byId[id] || null,
    querySelector: () => null,
    addEventListener() {}, removeEventListener() {},
  };

  const sandbox = {
    console,
    document,
    navigator: {
      userAgent: 'Mozilla/5.0 (Linux; Android 4.0.4; Build/IMM76D) ' +
                 'AppleWebKit/534.30 (KHTML, like Gecko) Version/4.0 Mobile Safari/534.30',
      language: 'en-US',
    },
    location: { href: 'file://' + WEB + '/index.html' },
    performance: { now: () => Date.now() },
    setTimeout, clearTimeout, setInterval, clearInterval, setImmediate,
    queueMicrotask,
    Uint8Array, Uint8ClampedArray, Uint16Array, Int8Array, Int16Array,
    Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array,
    ArrayBuffer, DataView, TextDecoder, TextEncoder,
    Math, JSON, Date, Promise, BigInt, Map, Set, WeakMap, WeakSet, Error,
    RegExp, Number, String, Boolean, Object, Array, Symbol, Proxy, Reflect,
    isNaN, isFinite, parseInt, parseFloat,
    requestAnimationFrame(fn) { return setTimeout(() => fn(Date.now()), 8); },
    cancelAnimationFrame(t) { clearTimeout(t); },
    atob: (s) => Buffer.from(s, 'base64').toString('binary'),
    btoa: (s) => Buffer.from(s, 'binary').toString('base64'),
    localStorage: (() => {
      const m = Object.create(null);
      return {
        getItem: (k) => (k in m ? m[k] : null),
        setItem: (k, v) => { m[k] = String(v); },
        removeItem: (k) => { delete m[k]; },
        key: (i) => (Object.keys(m)[i] ?? null),
        get length() { return Object.keys(m).length; },
        clear() { for (const k of Object.keys(m)) delete m[k]; },
      };
    })(),
    async fetch(url) {
      const p = path.join(WEB, String(url).replace(/^\.\//, ''));
      if (!fs.existsSync(p)) return { ok: false, status: 404 };
      const buf = fs.readFileSync(p);
      return {
        ok: true, status: 200,
        async arrayBuffer() {
          return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
        },
        async text() { return buf.toString('utf8'); },
        async json() { return JSON.parse(buf.toString('utf8')); },
      };
    },
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.self = sandbox;
  sandbox.innerWidth = 480;
  sandbox.innerHeight = 800;
  sandbox.addEventListener = () => {};
  sandbox.removeEventListener = () => {};
  sandbox.$HOST_CANVAS = canvasFactory;
  if (opts.extend) opts.extend(sandbox);

  vm.createContext(sandbox);

  const FILES = ['dex/dex-meta.js', 'runtime.js', 'dex/dex-classes.js', 'boot.js'];

  function load() {
    for (const f of FILES) {
      const p = path.join(WEB, f);
      if (!fs.existsSync(p)) {
        throw new Error('missing web/' + f + ' -- run ./build.sh first');
      }
      vm.runInContext(fs.readFileSync(p, 'utf8'), sandbox, { filename: p });
    }
    return sandbox;
  }

  return { sandbox, stats, load, byId, FILES, WEB, ROOT };
}

module.exports = { createHost, mockCanvasFactory, WEB, ROOT };
