/* =========================================================================
 * 01-host.js -- host abstraction ($gfx / $host)
 *
 * Everything the translated Dalvik code needs from the *outside* world
 * (a drawing surface, an asset blob store, an audio device, a clock) is
 * funnelled through these two objects so the same runtime can be driven by
 * a browser DOM or by a head-less harness during the build's smoke test.
 * ========================================================================= */
'use strict';

var $gfx = (function () {
  const hasDOM = (typeof document !== 'undefined' && !!document.createElement);

  function create(w, h) {
    w = Math.max(1, w | 0);
    h = Math.max(1, h | 0);
    if (typeof globalThis !== 'undefined' && globalThis.$HOST_CANVAS) {
      return globalThis.$HOST_CANVAS(w, h);
    }
    if (hasDOM) {
      const c = document.createElement('canvas');
      c.width = w; c.height = h;
      return c;
    }
    if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
    throw new Error('[host] no canvas implementation available');
  }

  let mctx = null;
  function measureCtx() {
    if (!mctx) mctx = create(8, 8).getContext('2d');
    return mctx;
  }

  return {
    create, measureCtx,
    /* PNG is decoded by $img (05-image.js); no other format is used by the
     * game, so a synchronous fallback decoder is not required. */
    decodeSync: null,
    hasDOM,
  };
})();

var $host = (function () {
  /* ------------------------------------------------------------- assets */
  /* name -> Uint8Array. Filled by the boot code before the VM starts, so
   * that AssetManager.open() can stay synchronous exactly like on Android. */
  const assets = Object.create(null);

  /* -------------------------------------------------- classpath entries */
  /* Full APK zip-entry path -> Uint8Array, e.g. "res/raw/snd.inf".
   * kairo/android/c/h loads packages whose name starts with '/' through
   * Class.getResourceAsStream(), which on Android resolves against the APK
   * itself, so the whole archive has to be addressable by entry name. */
  const resources = Object.create(null);

  /* ------------------------------------------------------ raw resources */
  /* android resource id  <->  res/raw file name */
  const rawById = Object.create(null);
  const rawByName = Object.create(null);
  let nextRawId = 0x7f040000;

  function addRaw(name, bytes) {
    const id = nextRawId++;
    rawById[id] = { name, bytes: bytes || null, url: null };
    rawByName[name] = id;
    return id;
  }

  return {
    assets,
    putAsset(name, u8) { assets[name] = u8; },
    getAsset(name) { return assets[name] || null; },

    resources,
    putResource(path, u8) {
      resources[String(path).replace(/^\/+/, '')] = u8;
    },
    getResource(path) {
      let p = String(path).replace(/^\/+/, '');
      if (resources[p]) return resources[p];
      /* the game also asks for "res/raw//res/raw.dat" style doubled paths */
      p = p.replace(/\/{2,}/g, '/');
      return resources[p] || null;
    },

    rawById, rawByName, addRaw,
    rawId(name) {
      const id = rawByName[name];
      return id === undefined ? 0 : id;
    },

    /* audio device, installed by 50-android-media.js / boot */
    audio: null,
    /* the visible surface, installed by boot */
    surface: null,
    /* user agent reported to the game through WebView.getSettings() */
    userAgent: (typeof navigator !== 'undefined' && navigator.userAgent) ||
               'Mozilla/5.0 (Linux; Android 4.0.4; Build/IMM76D) AppleWebKit/534.30' +
               ' (KHTML, like Gecko) Version/4.0 Mobile Safari/534.30',
    locale: (typeof navigator !== 'undefined' && navigator.language) || 'en-US',
    log(...a) { if (typeof console !== 'undefined') console.log(...a); },
  };
})();

if (typeof module !== 'undefined') module.exports = { $gfx, $host };
