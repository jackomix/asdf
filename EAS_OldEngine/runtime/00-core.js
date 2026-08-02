/* =========================================================================
 * 00-core.js -- object model, class linker, arrays, exceptions, scheduler
 *
 * The translated Dalvik code (dex-classes.js) talks to the outside world
 * exclusively through the `$rt` object defined here.  Method names are
 * mangled by tools/dex2js.py; `$DEXMETA.methodSignatures` maps a Java
 * signature ("setColor(I)") onto the mangled JS property name, which lets
 * the hand written classes below stay readable.
 * ========================================================================= */
'use strict';

var $rt = (function () {

  const classes = Object.create(null);
  const MSIG = (typeof $DEXMETA !== 'undefined' && $DEXMETA.methodSignatures) || {};
  let mangleCounter = 0;

  /** "setColor(I)V" | "setColor(I)"  ->  mangled JS property name */
  function mangle(sig) {
    const i = sig.indexOf(')');
    const key = i < 0 ? sig : sig.slice(0, i + 1);
    let m = MSIG[key];
    if (m === undefined) {
      // never referenced by the application: give it a private slot
      m = '$unused$' + (mangleCounter++);
      MSIG[key] = m;
    }
    return m;
  }

  function need(name) {
    const c = classes[name];
    if (!c) throw new Error('[dex2js] missing runtime class: ' + name);
    return c;
  }

  /* ---------------------------------------------------------------- base */

  class JObject {
    constructor() {}
  }
  JObject.$name = 'java/lang/Object';
  JObject.$super = null;
  JObject.$types = ['java/lang/Object'];
  classes['java/lang/Object'] = JObject;

  /** Build the transitive type-set used by instanceof / catch matching. */
  function link(C) {
    const set = new Set();
    const push = (n) => {
      if (!n || set.has(n)) return;
      set.add(n);
      const K = classes[n];
      if (K && K !== C) {
        if (K.$typeSet) { K.$typeSet.forEach((x) => set.add(x)); }
        else {
          (K.$types || []).forEach(push);
          push(K.$super);
        }
      }
    };
    (C.$types || [C.$name]).forEach(push);
    push(C.$super);
    set.add('java/lang/Object');
    C.$typeSet = set;
    return C;
  }

  /**
   * Define a runtime (android/java) class.
   *   name : registry name, e.g. 'android/graphics/Paint'
   *   ext  : superclass registry name
   *   spec : { ctor, m:{sig:fn}, s:{sig:fn}, sf:{NAME:value}, impl:[names] }
   */
  function def(name, ext, spec) {
    spec = spec || {};
    const Super = ext ? need(ext) : JObject;
    const ctor = spec.ctor;
    const C = spec.cls || class extends Super {
      constructor() { super(); if (ctor) ctor.call(this); }
    };
    if (spec.cls && ctor) throw new Error('cls+ctor');
    Object.defineProperty(C, 'name', { value: name.replace(/[/$]/g, '_') });
    C.$name = name;
    C.$super = ext || (name === 'java/lang/Object' ? null : 'java/lang/Object');
    C.$types = [name].concat(spec.impl || []);
    for (const sig in spec.m || {}) C.prototype[mangle(sig)] = spec.m[sig];
    for (const sig in spec.s || {}) C[mangle(sig)] = spec.s[sig];
    for (const f in spec.sf || {}) C['s_' + f] = spec.sf[f];
    classes[name] = C;
    link(C);
    if (spec.init) spec.init(C);
    return C;
  }

  /** Declare an interface (no members, only used for instanceof). */
  function iface(name, impl) {
    return def(name, null, { impl: impl || [] });
  }

  /* --------------------------------------------------------- exceptions */

  function isInstance(o, type) {
    if (o === null || o === undefined) return false;
    let name;
    if (type.charCodeAt(0) === 76 /* L */) name = type.slice(1, -1);
    else if (type.charCodeAt(0) === 91 /* [ */) {
      // array type: accept any array for Object[]-ish checks
      return isArray(o);
    } else name = type;
    if (typeof o === 'string') {
      return name === 'java/lang/String' || name === 'java/lang/Object' ||
             name === 'java/lang/CharSequence' || name === 'java/lang/Comparable';
    }
    if (typeof o === 'number' || typeof o === 'bigint') return false;
    const C = o.constructor;
    if (C && C.$typeSet) return C.$typeSet.has(name);
    return name === 'java/lang/Object';
  }

  function isArray(o) {
    return Array.isArray(o) || ArrayBuffer.isView(o);
  }

  function throwable(name, ext, extraSpec) {
    const spec = Object.assign({
      ctor() { this.f_detailMessage = null; this.$stack = null; },
      m: {
        '<init>()V': function () { return this; },
        '<init>(Ljava/lang/String;)V': function (s) { this.f_detailMessage = s; return this; },
        '<init>(Ljava/lang/Throwable;)V': function (t) {
          this.f_detailMessage = t === null ? null : jToString(t);
          this.$cause = t; return this;
        },
        'getMessage()Ljava/lang/String;': function () { return this.f_detailMessage; },
        'toString()Ljava/lang/String;': function () {
          const n = this.constructor.$name.replace(/\//g, '.');
          return this.f_detailMessage === null ? n : n + ': ' + this.f_detailMessage;
        },
        'printStackTrace()V': function () {
          console.warn('[java] ' + this[M.toString]() + (this.$stack ? '\n' + this.$stack : ''));
        },
        'getStackTrace()[Ljava/lang/StackTraceElement;': function () { return []; },
        'fillInStackTrace()Ljava/lang/Throwable;': function () { return this; },
      },
    }, extraSpec || {});
    return def(name, ext, spec);
  }

  /** Convert a native JS error into the closest java exception. */
  function jt(e) {
    if (e && e.constructor && e.constructor.$typeSet &&
        e.constructor.$typeSet.has('java/lang/Throwable')) return e;
    let name = 'java/lang/RuntimeException';
    if (e instanceof TypeError &&
        /null|undefined|not a function|of undefined/.test(e.message || '')) {
      name = 'java/lang/NullPointerException';
    } else if (e instanceof RangeError) {
      name = 'java/lang/ArrayIndexOutOfBoundsException';
    }
    if ($rt.trace) console.warn('[dex2js] native error -> ' + name, e);
    const C = classes[name];
    const o = new C();
    o.f_detailMessage = String((e && e.message) || e);
    o.$stack = e && e.stack;
    o.$js = e;
    return o;
  }

  function raise(name, msg) {
    const o = new (need(name))();
    o.f_detailMessage = msg === undefined ? null : msg;
    o.$stack = new Error().stack;
    throw o;
  }

  /* -------------------------------------------------------------- arrays */

  const arr = {
    newObj(elDesc, n) {
      if (n < 0) raise('java/lang/NegativeArraySizeException', '' + n);
      const a = new Array(n);
      for (let i = 0; i < n; i++) a[i] = null;
      a.$t = '[' + elDesc;
      return a;
    },
    obj(desc, values) {
      const a = values.slice();
      a.$t = desc;
      return a;
    },
    fill(a, b64, width) {
      const raw = b64ToBytes(b64);
      const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
      const n = raw.length / width;
      for (let i = 0; i < n; i++) {
        switch (width) {
          case 1: a[i] = dv.getInt8(i); break;
          case 2: a[i] = dv.getInt16(i * 2, true); break;
          case 4: a[i] = dv.getInt32(i * 4, true); break;
          case 8: a[i] = dv.getBigInt64(i * 8, true); break;
        }
      }
      if (a instanceof Float32Array) {
        for (let i = 0; i < n; i++) a[i] = dv.getFloat32(i * 4, true);
      } else if (a instanceof Uint16Array || a instanceof Uint8Array) {
        for (let i = 0; i < n; i++) {
          a[i] = width === 1 ? dv.getUint8(i) : dv.getUint16(i * 2, true);
        }
      }
      return a;
    },
    clone(a) {
      if (ArrayBuffer.isView(a)) return a.slice();
      const c = a.slice();
      c.$t = a.$t;
      return c;
    },
    ck(i, a) {
      if (i < 0 || i >= a.length) {
        raise('java/lang/ArrayIndexOutOfBoundsException', '' + i);
      }
      return i;
    },
  };

  function b64ToBytes(b64) {
    if (typeof atob === 'function') {
      const bin = atob(b64);
      const out = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
      return out;
    }
    return new Uint8Array(Buffer.from(b64, 'base64'));
  }

  function bytesToB64(u8) {
    if (typeof btoa === 'function') {
      let s = '';
      for (let i = 0; i < u8.length; i += 0x8000) {
        s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
      }
      return btoa(s);
    }
    return Buffer.from(u8).toString('base64');
  }

  /* ------------------------------------------------------ numeric helpers */

  function idiv(a, b) {
    if (b === 0) raise('java/lang/ArithmeticException', '/ by zero');
    return (a / b) | 0;
  }
  function irem(a, b) {
    if (b === 0) raise('java/lang/ArithmeticException', '/ by zero');
    return a % b;
  }
  function ldiv(a, b) {
    if (b === 0n) raise('java/lang/ArithmeticException', '/ by zero');
    return BigInt.asIntN(64, a / b);
  }
  function lrem(a, b) {
    if (b === 0n) raise('java/lang/ArithmeticException', '/ by zero');
    return BigInt.asIntN(64, a % b);
  }
  function lushr(a, b) {
    return BigInt.asIntN(64, BigInt.asUintN(64, a) >> (b & 63n));
  }
  function f2i(x) {
    if (Number.isNaN(x)) return 0;
    if (x >= 2147483647) return 2147483647;
    if (x <= -2147483648) return -2147483648;
    return x | 0;
  }
  function f2l(x) {
    if (Number.isNaN(x)) return 0n;
    if (x >= 9223372036854775807) return 9223372036854775807n;
    if (x <= -9223372036854775808) return -9223372036854775808n;
    return BigInt(Math.trunc(x));
  }
  function cmpf(a, b, nanv) {
    if (Number.isNaN(a) || Number.isNaN(b)) return nanv;
    return a > b ? 1 : (a < b ? -1 : 0);
  }

  /* --------------------------------------------------------- Object glue */

  const M = {};   // frequently used mangled names, filled by initNames()

  function jToString(o) {
    if (o === null || o === undefined) return 'null';
    if (typeof o === 'string') return o;
    if (typeof o === 'number') return String(o);
    if (typeof o === 'bigint') return String(o);
    const f = o[M.toString];
    if (f) return f.call(o);
    return (o.constructor.$name || 'object').replace(/\//g, '.') + '@1';
  }

  function jEquals(a, b) {
    if (a === null || a === undefined) return b === null || b === undefined ? 1 : 0;
    if (typeof a === 'string') return a === b ? 1 : 0;
    const f = a[M.equals];
    if (f) return f.call(a, b);
    return a === b ? 1 : 0;
  }

  function jHashCode(o) {
    if (o === null || o === undefined) return 0;
    if (typeof o === 'string') return strHash(o);
    const f = o[M.hashCode];
    if (f) return f.call(o);
    if (o.$id === undefined) { o.$id = (idc = (idc + 0x9e3779b1) | 0); }
    return o.$id;
  }
  let idc = 12345;

  function strHash(s) {
    let h = 0;
    for (let i = 0; i < s.length; i++) h = (Math.imul(31, h) + s.charCodeAt(i)) | 0;
    return h;
  }

  /* ------------------------------------------------------------- classes */

  const classObjects = Object.create(null);
  function classFor(desc) {
    let c = classObjects[desc];
    if (!c) {
      c = new (need('java/lang/Class'))();
      c.$desc = desc;
      classObjects[desc] = c;
    }
    return c;
  }
  function getClass(o) {
    if (typeof o === 'string') return classFor('Ljava/lang/String;');
    if (Array.isArray(o) || ArrayBuffer.isView(o)) {
      return classFor(o.$t || '[I');
    }
    return classFor('L' + o.constructor.$name + ';');
  }

  /* ----------------------------------------------------------- scheduler */

  const scheduler = {
    threads: [],
    now: 0,
    budgetMs: 12,
    /** wall clock; overridable so the head-less test can run a fast,
     *  deterministic virtual clock instead of real time.  Everything that
     *  observes time (System.currentTimeMillis, SystemClock, MotionEvent
     *  timestamps) goes through $rt.now() which defers to this. */
    clock: () => Date.now(),
    count() { let n = 0; for (const t of this.threads) if (!t.done) n++; return n; },
    spawn(gen, name) {
      const t = { gen, name: name || 'thread', wake: 0, done: false,
                  interrupt: false, value: undefined };
      this.threads.push(t);
      return t;
    },
    /** run one thread until it blocks (or the time budget is used up) */
    step(t) {
      const start = this.clock();
      for (;;) {
        let r;
        try {
          if (t.interrupt) {
            t.interrupt = false;
            r = t.gen.throw(mkThrowable('java/lang/InterruptedException'));
          } else {
            r = t.gen.next(t.value);
          }
        } catch (e) {
          t.done = true;
          const je = jt(e);
          console.error('[' + t.name + '] uncaught ' +
                        jToString(je), je.$stack || e);
          if ($rt.onCrash) $rt.onCrash(je);
          return;
        }
        t.value = undefined;
        if (r.done) { t.done = true; return; }
        const y = r.value;
        if (y && y.s !== undefined) { t.wake = this.clock() + y.s; return; }
        if (y && y.p !== undefined) {          // await a promise
          t.wake = Infinity;
          y.p.then((v) => { t.value = v; t.wake = 0; scheduler.kick(); },
                   (e) => { t.interruptError = e; t.wake = 0; scheduler.kick(); });
          return;
        }
        // Thread.yield(): keep going while we have budget
        if (this.clock() - start > this.budgetMs) { t.wake = 0; return; }
      }
    },
    /** callbacks run at the start of every scheduler tick; used by the
     *  scripted-input API so a synthetic touch can be held for N frames
     *  without depending on real wall-clock timers. */
    onTick: [],
    tick() {
      const now = this.clock();
      for (let i = this.onTick.length - 1; i >= 0; i--) {
        if (this.onTick[i]() === false) this.onTick.splice(i, 1);
      }
      let alive = false;
      for (let i = 0; i < this.threads.length; i++) {
        const t = this.threads[i];
        if (t.done) continue;
        alive = true;
        if (t.wake <= now) this.step(t);
      }
      if (!alive) return Infinity;
      let next = Infinity;
      for (const t of this.threads) if (!t.done && t.wake < next) next = t.wake;
      return next;
    },
    kick() { if (this._kick) this._kick(); },
  };

  /** Dalvik reference equality where a `const/4 v,0` may still be a raw 0.
   *  (`null == 0` must be true, `0 == 0` and `a === b` must stay true.) */
  function same(a, b) {
    if (a === b) return true;
    return (a === null || a === undefined || a === 0) &&
           (b === null || b === undefined || b === 0);
  }

  function mkThrowable(name, msg) {
    const o = new (need(name))();
    o.f_detailMessage = msg === undefined ? null : msg;
    return o;
  }

  /** Call a (possibly generator) java method from native code. */
  function invoke(obj, sig, args, name) {
    const f = obj[mangle(sig)];
    if (!f) throw new Error('no method ' + sig + ' on ' +
                            (obj && obj.constructor && obj.constructor.$name));
    const r = f.apply(obj, args || []);
    if (r && typeof r.next === 'function' && typeof r[Symbol.iterator] === 'function') {
      scheduler.spawn(r, name || sig);
      scheduler.kick();
      return undefined;
    }
    return r;
  }

  /* ------------------------------------------------------------- exports */

  const rt = {
    classes, need, def, iface, link, mangle, M, same,
    now: () => scheduler.clock(),
    JObject, throwable, jt, raise, mkThrowable,
    iof: isInstance, isArray,
    arr, b64ToBytes, bytesToB64,
    idiv, irem, ldiv, lrem, lushr, f2i, f2l, cmpf,
    jToString, jEquals, jHashCode, strHash,
    classFor, getClass,
    scheduler, invoke,
    trace: false,
    abstract() { raise('java/lang/AbstractMethodError'); },
    unsupported(what) { raise('java/lang/UnsupportedOperationException', what); },
    cast(o, type) {
      if (o !== null && o !== undefined && !isInstance(o, type)) {
        raise('java/lang/ClassCastException', type);
      }
      return o;
    },
    initNames() {
      M.toString = mangle('toString()');
      M.equals = mangle('equals(Ljava/lang/Object;)');
      M.hashCode = mangle('hashCode()');
      M.run = mangle('run()');
      M.compareTo = mangle('compareTo(Ljava/lang/Object;)');
    },
  };
  rt.obj = {
    'toString()Ljava/lang/String;': jToString,
    'equals(Ljava/lang/Object;)Z': jEquals,
    'hashCode()I': jHashCode,
    'getClass()Ljava/lang/Class;': getClass,
    'asBinder()Landroid/os/IBinder;': (o) => o[mangle('asBinder()')](),
  };
  return rt;
})();

if (typeof module !== 'undefined') module.exports = $rt;
