/* =========================================================================
 * 10-lang.js -- java.lang.*  (Object, String, StringBuilder, System, Math,
 * Thread, boxed primitives, Class, Enum and the Throwable hierarchy)
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  /* ------------------------------------------------------------- Object */
  const O = $rt.JObject.prototype;
  O[mangle('<init>()V')] = function () { return this; };
  O[mangle('toString()')] = function () {
    return this.constructor.$name.replace(/\//g, '.') + '@' +
           ($rt.jHashCode(this) >>> 0).toString(16);
  };
  O[mangle('equals(Ljava/lang/Object;)')] = function (o) { return this === o ? 1 : 0; };
  O[mangle('hashCode()')] = function () {
    if (this.$id === undefined) this.$id = ($rt.strHash(String(Math.random())) | 0);
    return this.$id;
  };
  O[mangle('getClass()')] = function () { return $rt.getClass(this); };
  O[mangle('notify()')] = function () {};
  O[mangle('notifyAll()')] = function () {};
  O[mangle('wait()')] = function* () { yield { s: 1 }; };

  iface('java/lang/Runnable');
  iface('java/lang/Comparable');
  iface('java/lang/CharSequence');
  iface('java/lang/Cloneable');
  iface('java/lang/Iterable');

  /* ---------------------------------------------------------- Throwable */
  $rt.throwable('java/lang/Throwable', null);
  $rt.throwable('java/lang/Exception', 'java/lang/Throwable');
  $rt.throwable('java/lang/Error', 'java/lang/Throwable');
  $rt.throwable('java/lang/RuntimeException', 'java/lang/Exception');
  [
    ['java/lang/NullPointerException', 'java/lang/RuntimeException'],
    ['java/lang/ClassCastException', 'java/lang/RuntimeException'],
    ['java/lang/ArithmeticException', 'java/lang/RuntimeException'],
    ['java/lang/IllegalArgumentException', 'java/lang/RuntimeException'],
    ['java/lang/NumberFormatException', 'java/lang/IllegalArgumentException'],
    ['java/lang/IllegalStateException', 'java/lang/RuntimeException'],
    ['java/lang/UnsupportedOperationException', 'java/lang/RuntimeException'],
    ['java/lang/IndexOutOfBoundsException', 'java/lang/RuntimeException'],
    ['java/lang/ArrayIndexOutOfBoundsException', 'java/lang/IndexOutOfBoundsException'],
    ['java/lang/StringIndexOutOfBoundsException', 'java/lang/IndexOutOfBoundsException'],
    ['java/lang/NegativeArraySizeException', 'java/lang/RuntimeException'],
    ['java/lang/SecurityException', 'java/lang/RuntimeException'],
    ['java/lang/InterruptedException', 'java/lang/Exception'],
    ['java/lang/AbstractMethodError', 'java/lang/Error'],
    ['java/lang/OutOfMemoryError', 'java/lang/Error'],
    ['java/lang/NoClassDefFoundError', 'java/lang/Error'],
  ].forEach(([n, s]) => $rt.throwable(n, s));

  /* -------------------------------------------------------------- Class */
  def('java/lang/Class', null, {
    ctor() { this.$desc = 'Ljava/lang/Object;'; },
    m: {
      'getName()Ljava/lang/String;': function () {
        const d = this.$desc;
        const prim = { V: 'void', Z: 'boolean', B: 'byte', S: 'short', C: 'char',
                       I: 'int', J: 'long', F: 'float', D: 'double' };
        if (prim[d]) return prim[d];
        if (d.charAt(0) === '[') return d.replace(/\//g, '.');
        return d.slice(1, -1).replace(/\//g, '.');
      },
      'getSimpleName()Ljava/lang/String;': function () {
        const n = this.$desc.slice(1, -1);
        return n.substring(n.lastIndexOf('/') + 1);
      },
      'desiredAssertionStatus()Z': function () { return 0; },
      /* On Android the class loader resolves resources against the APK, so
       * "/res/raw/snd.inf" is the zip entry "res/raw/snd.inf".  The web build
       * registers every archive entry in $host.resources. */
      'getResourceAsStream(Ljava/lang/String;)Ljava/io/InputStream;':
        function (name) {
          if (name === null || name === undefined) return null;
          const b = $host.getResource($rt.jToString(name));
          if (!b) return null;
          const S = $rt.classes['java/io/ByteArrayInputStream'];
          const s = new S();
          s[mangle('<init>([B)V')](b);
          return s;
        },
      'toString()Ljava/lang/String;': function () {
        return 'class ' + this[mangle('getName()')]();
      },
    },
  });

  /* --------------------------------------------------------------- Enum */
  def('java/lang/Enum', null, {
    ctor() { this.$ename = null; this.$eord = 0; },
    m: {
      '<init>(Ljava/lang/String;I)V': function (n, o) {
        this.$ename = n; this.$eord = o;
        const C = this.constructor;
        if (!Object.prototype.hasOwnProperty.call(C, '$enums')) C.$enums = [];
        C.$enums.push(this);
        return this;
      },
      'name()Ljava/lang/String;': function () { return this.$ename; },
      'ordinal()I': function () { return this.$eord; },
      'toString()Ljava/lang/String;': function () { return this.$ename; },
      'equals(Ljava/lang/Object;)Z': function (o) { return this === o ? 1 : 0; },
      'compareTo(Ljava/lang/Object;)I': function (o) { return this.$eord - o.$eord; },
    },
    s: {
      'valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;':
        function (cls, name) {
          const C = $rt.classes[cls.$desc.slice(1, -1)];
          for (const e of (C && C.$enums) || []) if (e.$ename === name) return e;
          $rt.raise('java/lang/IllegalArgumentException', name);
        },
    },
  });

  /* ------------------------------------------------------------- String */
  const enc = new TextEncoder();
  function decode(bytes, charset) {
    const u8 = bytes instanceof Uint8Array ? bytes
             : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let cs = (charset || 'UTF-8').toLowerCase().replace(/^ms932$/, 'shift_jis');
    try { return new TextDecoder(cs).decode(u8); }
    catch (e) { return new TextDecoder('utf-8').decode(u8); }
  }

  function jformat(fmt, args) {
    let i = 0;
    return fmt.replace(/%(\d+\$)?([-+ 0,#]*)(\d+)?(?:\.(\d+))?([sdfxXcob%])/g,
      function (all, argn, flags, width, prec, conv) {
        if (conv === '%') return '%';
        let v = args[argn ? parseInt(argn) - 1 : i++];
        let s;
        switch (conv) {
          case 'd': {
            let n = typeof v === 'bigint' ? v : Math.trunc(Number($rt.unbox(v)));
            s = String(n);
            if (flags.indexOf(',') >= 0) s = s.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
            if (flags.indexOf('+') >= 0 && Number(n) >= 0) s = '+' + s;
            break;
          }
          case 'f': {
            const p = prec === undefined ? 6 : parseInt(prec);
            s = Number($rt.unbox(v)).toFixed(p);
            break;
          }
          case 'x': s = (Number($rt.unbox(v)) >>> 0).toString(16); break;
          case 'X': s = (Number($rt.unbox(v)) >>> 0).toString(16).toUpperCase(); break;
          case 'o': s = (Number($rt.unbox(v)) >>> 0).toString(8); break;
          case 'c': s = String.fromCharCode(Number($rt.unbox(v))); break;
          case 'b': s = v ? 'true' : 'false'; break;
          default: s = $rt.jToString(v);
        }
        if (width) {
          const w = parseInt(width);
          if (s.length < w) {
            const pad = (flags.indexOf('0') >= 0 && flags.indexOf('-') < 0) ? '0' : ' ';
            const fill = pad.repeat(w - s.length);
            s = flags.indexOf('-') >= 0 ? s + ' '.repeat(w - s.length)
              : (pad === '0' && /^[-+]/.test(s)
                  ? s[0] + fill + s.slice(1) : fill + s);
          }
        }
        return s;
      });
  }

  function ck(s) {
    if (s === null || s === undefined) $rt.raise('java/lang/NullPointerException');
    return s;
  }

  function charsToStr(c, off, len) {
    let out = '';
    for (let i = 0; i < len; i += 4096) {
      out += String.fromCharCode.apply(null,
        Array.prototype.slice.call(c, off + i, off + Math.min(len, i + 4096)));
    }
    return out;
  }

  const S = {
    'length()I': (s) => ck(s).length,
    'charAt(I)C': (s, i) => {
      if (i < 0 || i >= ck(s).length) {
        $rt.raise('java/lang/StringIndexOutOfBoundsException', '' + i);
      }
      return s.charCodeAt(i);
    },
    'isEmpty()Z': (s) => (ck(s).length === 0 ? 1 : 0),
    'equals(Ljava/lang/Object;)Z': (s, o) => (typeof o === 'string' && s === o) ? 1 : 0,
    'equalsIgnoreCase(Ljava/lang/String;)Z': (s, o) =>
      (typeof o === 'string' && s.toLowerCase() === o.toLowerCase()) ? 1 : 0,
    'compareTo(Ljava/lang/String;)I': (s, o) => {
      const n = Math.min(s.length, o.length);
      for (let i = 0; i < n; i++) {
        const d = s.charCodeAt(i) - o.charCodeAt(i);
        if (d) return d;
      }
      return s.length - o.length;
    },
    'compareTo(Ljava/lang/Object;)I': (s, o) => S['compareTo(Ljava/lang/String;)I'](s, o),
    'hashCode()I': (s) => $rt.strHash(s),
    'indexOf(I)I': (s, c) => s.indexOf(String.fromCharCode(c)),
    'indexOf(II)I': (s, c, f) => s.indexOf(String.fromCharCode(c), f),
    'indexOf(Ljava/lang/String;)I': (s, t) => s.indexOf(t),
    'indexOf(Ljava/lang/String;I)I': (s, t, f) => s.indexOf(t, f),
    'lastIndexOf(I)I': (s, c) => s.lastIndexOf(String.fromCharCode(c)),
    'lastIndexOf(II)I': (s, c, f) => s.lastIndexOf(String.fromCharCode(c), f),
    'lastIndexOf(Ljava/lang/String;)I': (s, t) => s.lastIndexOf(t),
    'substring(I)Ljava/lang/String;': (s, a) => {
      if (a < 0 || a > s.length) $rt.raise('java/lang/StringIndexOutOfBoundsException', '' + a);
      return s.substring(a);
    },
    'substring(II)Ljava/lang/String;': (s, a, b) => {
      if (a < 0 || b > s.length || a > b) {
        $rt.raise('java/lang/StringIndexOutOfBoundsException', a + ',' + b);
      }
      return s.substring(a, b);
    },
    'startsWith(Ljava/lang/String;)Z': (s, p) => (s.lastIndexOf(p, 0) === 0 ? 1 : 0),
    'startsWith(Ljava/lang/String;I)Z': (s, p, o) => (s.startsWith(p, o) ? 1 : 0),
    'endsWith(Ljava/lang/String;)Z': (s, p) => (s.endsWith(p) ? 1 : 0),
    'contains(Ljava/lang/CharSequence;)Z': (s, t) => (s.indexOf($rt.jToString(t)) >= 0 ? 1 : 0),
    'trim()Ljava/lang/String;': (s) => {
      let a = 0, b = s.length;
      while (a < b && s.charCodeAt(a) <= 0x20) a++;
      while (b > a && s.charCodeAt(b - 1) <= 0x20) b--;
      return s.substring(a, b);
    },
    'toLowerCase()Ljava/lang/String;': (s) => s.toLowerCase(),
    'toUpperCase()Ljava/lang/String;': (s) => s.toUpperCase(),
    'replace(CC)Ljava/lang/String;': (s, a, b) =>
      s.split(String.fromCharCode(a)).join(String.fromCharCode(b)),
    'replace(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;':
      (s, a, b) => s.split($rt.jToString(a)).join($rt.jToString(b)),
    'toString()Ljava/lang/String;': (s) => s,
    'toCharArray()[C': (s) => {
      const a = new Uint16Array(s.length);
      for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);
      return a;
    },
    'getBytes()[B': (s) => new Int8Array(enc.encode(s).buffer),
    'getBytes(Ljava/lang/String;)[B': (s) => new Int8Array(enc.encode(s).buffer),
    'concat(Ljava/lang/String;)Ljava/lang/String;': (s, o) => s + o,
    'matches(Ljava/lang/String;)Z': (s, r) => (new RegExp('^(?:' + r + ')$').test(s) ? 1 : 0),
    'split(Ljava/lang/String;)[Ljava/lang/String;': (s, r) => {
      const a = s.split(new RegExp(r));
      a.$t = '[Ljava/lang/String;';
      return a;
    },
    /* constructors -- `new String(...)` yields a primitive; the translator
     * assigns the result straight back into the receiver register. */
    '<init>()V': () => '',
    '<init>(Ljava/lang/String;)V': (s) => (s === null ? '' : s),
    '<init>([B)V': (b) => decode(b),
    '<init>([BLjava/lang/String;)V': (b, cs) => decode(b, cs),
    '<init>([BII)V': (b, o, n) => decode(b.subarray(o, o + n)),
    '<init>([BIILjava/lang/String;)V': (b, o, n, cs) => decode(b.subarray(o, o + n), cs),
    '<init>([C)V': (c) => charsToStr(c, 0, c.length),
    '<init>([CII)V': (c, o, n) => charsToStr(c, o, n),
    '<init>([BI)V': (b) => decode(b),
    // static
    'valueOf(I)Ljava/lang/String;': (v) => String(v),
    'valueOf(J)Ljava/lang/String;': (v) => String(v),
    'valueOf(C)Ljava/lang/String;': (v) => String.fromCharCode(v),
    'valueOf(Z)Ljava/lang/String;': (v) => (v ? 'true' : 'false'),
    'valueOf(F)Ljava/lang/String;': (v) => $rt.fstr(v),
    'valueOf(D)Ljava/lang/String;': (v) => $rt.fstr(v),
    'valueOf(Ljava/lang/Object;)Ljava/lang/String;': (v) => $rt.jToString(v),
    'format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;':
      (f, a) => jformat(f, a || []),
    'format(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;':
      (l, f, a) => jformat(f, a || []),
  };
  $rt.str = S;
  $rt.jformat = jformat;

  // java.lang.String is also instantiated with `new-instance` + <init>([B)V.
  // Those become a JObject whose $init returns the real primitive string, so
  // the translator's `$r = ...` assignment picks it up; to keep that working
  // the constructor methods return the string and the register is overwritten
  // by the following move-result-object... Dalvik has no move-result for
  // <init>, therefore we keep a mutable box instead.
  def('java/lang/String', null, {
    cls: class JString extends $rt.JObject {
      constructor() { super(); this.$s = ''; }
    },
    m: {
      '<init>([B)V': function (b) { this.$s = decode(b); return this; },
      '<init>([BLjava/lang/String;)V': function (b, cs) {
        this.$s = decode(b, cs); return this;
      },
      '<init>([BII)V': function (b, o, n) {
        this.$s = decode(b.subarray(o, o + n)); return this;
      },
      '<init>(Ljava/lang/String;)V': function (s) { this.$s = s; return this; },
      '<init>()V': function () { this.$s = ''; return this; },
      'toString()Ljava/lang/String;': function () { return this.$s; },
    },
    s: {
      'valueOf(I)Ljava/lang/String;': S['valueOf(I)Ljava/lang/String;'],
      'valueOf(J)Ljava/lang/String;': S['valueOf(J)Ljava/lang/String;'],
      'valueOf(C)Ljava/lang/String;': S['valueOf(C)Ljava/lang/String;'],
      'valueOf(Ljava/lang/Object;)Ljava/lang/String;':
        S['valueOf(Ljava/lang/Object;)Ljava/lang/String;'],
      'format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;':
        S['format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;'],
    },
  });

  /* ------------------------------------------------------- StringBuilder */
  const sbSpec = {
    ctor() { this.$s = ''; },
    m: {
      '<init>()V': function () { this.$s = ''; return this; },
      '<init>(Ljava/lang/String;)V': function (s) { this.$s = s; return this; },
      '<init>(I)V': function () { this.$s = ''; return this; },
      'append(Ljava/lang/String;)Ljava/lang/StringBuilder;':
        function (s) { this.$s += (s === null ? 'null' : s); return this; },
      'append(I)Ljava/lang/StringBuilder;': function (v) { this.$s += v; return this; },
      'append(J)Ljava/lang/StringBuilder;': function (v) { this.$s += v; return this; },
      'append(C)Ljava/lang/StringBuilder;':
        function (v) { this.$s += String.fromCharCode(v); return this; },
      'append(Z)Ljava/lang/StringBuilder;':
        function (v) { this.$s += (v ? 'true' : 'false'); return this; },
      'append(F)Ljava/lang/StringBuilder;': function (v) { this.$s += $rt.fstr(v); return this; },
      'append(D)Ljava/lang/StringBuilder;': function (v) { this.$s += $rt.fstr(v); return this; },
      'append(Ljava/lang/Object;)Ljava/lang/StringBuilder;':
        function (v) { this.$s += $rt.jToString(v); return this; },
      'append([C)Ljava/lang/StringBuilder;': function (v) {
        for (let i = 0; i < v.length; i++) this.$s += String.fromCharCode(v[i]);
        return this;
      },
      'length()I': function () { return this.$s.length; },
      'charAt(I)C': function (i) { return this.$s.charCodeAt(i); },
      'setLength(I)V': function (n) { this.$s = this.$s.substring(0, n); },
      'deleteCharAt(I)Ljava/lang/StringBuilder;': function (i) {
        this.$s = this.$s.slice(0, i) + this.$s.slice(i + 1); return this;
      },
      'insert(ILjava/lang/String;)Ljava/lang/StringBuilder;': function (i, s) {
        this.$s = this.$s.slice(0, i) + s + this.$s.slice(i); return this;
      },
      'reverse()Ljava/lang/StringBuilder;': function () {
        this.$s = this.$s.split('').reverse().join(''); return this;
      },
      'toString()Ljava/lang/String;': function () { return this.$s; },
    },
  };
  def('java/lang/StringBuilder', null, sbSpec);
  def('java/lang/StringBuffer', null, sbSpec);

  /* ------------------------------------------------------------- System */
  function arraycopy(src, sp, dst, dp, len) {
    if (src === null || dst === null) $rt.raise('java/lang/NullPointerException');
    if (len < 0 || sp < 0 || dp < 0 || sp + len > src.length || dp + len > dst.length) {
      $rt.raise('java/lang/ArrayIndexOutOfBoundsException', 'arraycopy');
    }
    if (ArrayBuffer.isView(src) && ArrayBuffer.isView(dst) &&
        src.constructor === dst.constructor) {
      dst.set(src.subarray(sp, sp + len), dp);
      return;
    }
    if (src === dst && dp > sp) {
      for (let i = len - 1; i >= 0; i--) dst[dp + i] = src[sp + i];
    } else {
      for (let i = 0; i < len; i++) dst[dp + i] = src[sp + i];
    }
  }
  $rt.arraycopy = arraycopy;

  def('java/lang/System', null, {
    s: {
      'currentTimeMillis()J': () => BigInt($rt.now()),
      'nanoTime()J': () => BigInt(Math.round($rt.now() * 1e6)),
      'arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V': arraycopy,
      'exit(I)V': (code) => { if ($rt.onExit) $rt.onExit(code); },
      'gc()V': () => {},
      'getProperty(Ljava/lang/String;)Ljava/lang/String;': (k) => {
        const props = {
          'os.name': 'Linux', 'java.vendor': 'dex2js',
          'line.separator': '\n', 'file.separator': '/',
          'microedition.platform': null,
        };
        return k in props ? props[k] : null;
      },
      'getenv(Ljava/lang/String;)Ljava/lang/String;': () => null,
      'identityHashCode(Ljava/lang/Object;)I': (o) => $rt.jHashCode(o),
    },
  });

  /* --------------------------------------------------------------- Math */
  def('java/lang/Math', null, {
    sf: { PI: Math.PI, E: Math.E },
    s: {
      'abs(I)I': (v) => (v < 0 ? (-v) | 0 : v),
      'abs(J)J': (v) => (v < 0n ? -v : v),
      'abs(F)F': (v) => Math.fround(Math.abs(v)),
      'abs(D)D': Math.abs,
      'min(II)I': (a, b) => (a < b ? a : b),
      'max(II)I': (a, b) => (a > b ? a : b),
      'min(JJ)J': (a, b) => (a < b ? a : b),
      'max(JJ)J': (a, b) => (a > b ? a : b),
      'min(FF)F': (a, b) => Math.fround(Math.min(a, b)),
      'max(FF)F': (a, b) => Math.fround(Math.max(a, b)),
      'min(DD)D': Math.min, 'max(DD)D': Math.max,
      'sqrt(D)D': Math.sqrt, 'sin(D)D': Math.sin, 'cos(D)D': Math.cos,
      'tan(D)D': Math.tan, 'atan2(DD)D': Math.atan2, 'pow(DD)D': Math.pow,
      'floor(D)D': Math.floor, 'ceil(D)D': Math.ceil,
      'log(D)D': Math.log, 'exp(D)D': Math.exp,
      'random()D': Math.random,
      'round(F)I': (v) => Math.floor(v + 0.5) | 0,
      'round(D)J': (v) => BigInt(Math.floor(v + 0.5)),
      'toRadians(D)D': (d) => d / 180 * Math.PI,
      'toDegrees(D)D': (r) => r * 180 / Math.PI,
    },
  });

  /* ------------------------------------------------------------- Thread */
  const Thread = def('java/lang/Thread', null, {
    ctor() { this.$runnable = null; this.$co = null; this.$name = 'Thread'; },
    m: {
      '<init>()V': function () { return this; },
      '<init>(Ljava/lang/Runnable;)V': function (r) { this.$runnable = r; return this; },
      '<init>(Ljava/lang/Runnable;Ljava/lang/String;)V': function (r, n) {
        this.$runnable = r; this.$name = n; return this;
      },
      'start()V': function () {
        const target = this.$runnable || this;
        const r = target[$rt.M.run]();
        if (r && typeof r.next === 'function') {
          this.$co = $rt.scheduler.spawn(r, this.$name);
          $rt.scheduler.kick();
        }
      },
      'run()V': function () {},
      'interrupt()V': function () { if (this.$co) this.$co.interrupt = true; },
      'isAlive()Z': function () { return this.$co && !this.$co.done ? 1 : 0; },
      'setPriority(I)V': function () {},
      'setDaemon(Z)V': function () {},
      'setName(Ljava/lang/String;)V': function (n) { this.$name = n; },
      'getName()Ljava/lang/String;': function () { return this.$name; },
      'join()V': function* () {
        while (this.$co && !this.$co.done) yield { s: 4 };
      },
    },
    s: {
      'sleep(J)V': function* (ms) { yield { s: Number(ms) }; },
      'sleep(JI)V': function* (ms) { yield { s: Number(ms) }; },
      'yield()V': function* () { yield { y: 1 }; },
      'currentThread()Ljava/lang/Thread;': () => $rt.mainThread,
    },
    impl: ['java/lang/Runnable'],
  });
  $rt.mainThread = new Thread();

  /* ---------------------------------------------------- boxed primitives */
  function box(name, opts) {
    return def(name, null, Object.assign({
      ctor() { this.$v = opts.zero; },
      m: Object.assign({
        'toString()Ljava/lang/String;': function () { return opts.str(this.$v); },
        'equals(Ljava/lang/Object;)Z': function (o) {
          return (o !== null && o !== undefined && o.$v !== undefined &&
                  o.$v === this.$v && o.constructor === this.constructor) ? 1 : 0;
        },
        'hashCode()I': function () { return Number(this.$v) | 0; },
        'compareTo(Ljava/lang/Object;)I': function (o) {
          return this.$v < o.$v ? -1 : (this.$v > o.$v ? 1 : 0);
        },
        'intValue()I': function () { return Number(this.$v) | 0; },
        'longValue()J': function () { return BigInt(this.$v); },
        'floatValue()F': function () { return Math.fround(Number(this.$v)); },
        'doubleValue()D': function () { return Number(this.$v); },
        'shortValue()S': function () { return (Number(this.$v) << 16) >> 16; },
        'byteValue()B': function () { return (Number(this.$v) << 24) >> 24; },
        'booleanValue()Z': function () { return this.$v ? 1 : 0; },
      }, opts.m || {}),
      s: opts.s || {},
      sf: opts.sf || {},
    }, {}));
  }

  function parseIntJ(s, radix) {
    if (s === null) $rt.raise('java/lang/NumberFormatException', 'null');
    const t = String(s).trim();
    const v = parseInt(t, radix || 10);
    if (Number.isNaN(v) || !/^[-+]?[0-9a-zA-Z]+$/.test(t)) {
      $rt.raise('java/lang/NumberFormatException', 'For input string: "' + s + '"');
    }
    return v | 0;
  }

  const Integer = box('java/lang/Integer', {
    zero: 0, str: (v) => String(v),
    m: { '<init>(I)V': function (v) { this.$v = v | 0; return this; },
         '<init>(Ljava/lang/String;)V': function (v) { this.$v = parseIntJ(v); return this; } },
    s: {
      'parseInt(Ljava/lang/String;)I': (s) => parseIntJ(s, 10),
      'parseInt(Ljava/lang/String;I)I': (s, r) => parseIntJ(s, r),
      'valueOf(I)Ljava/lang/Integer;': (v) => {
        const o = new (($rt.classes['java/lang/Integer']))(); o.$v = v | 0; return o;
      },
      'valueOf(Ljava/lang/String;)Ljava/lang/Integer;': (s) => {
        const o = new (($rt.classes['java/lang/Integer']))(); o.$v = parseIntJ(s); return o;
      },
      'toString(I)Ljava/lang/String;': (v) => String(v),
      'toHexString(I)Ljava/lang/String;': (v) => (v >>> 0).toString(16),
      'toBinaryString(I)Ljava/lang/String;': (v) => (v >>> 0).toString(2),
      'bitCount(I)I': (v) => { let c = 0; v |= 0; while (v) { c += v & 1; v >>>= 1; } return c; },
    },
    sf: { MAX_VALUE: 2147483647, MIN_VALUE: -2147483648 },
  });
  Integer.s_TYPE = $rt.classFor('I');

  const Long = box('java/lang/Long', {
    zero: 0n, str: (v) => String(v),
    m: { '<init>(J)V': function (v) { this.$v = v; return this; } },
    s: {
      'parseLong(Ljava/lang/String;)J': (s) => {
        try { return BigInt(String(s).trim()); }
        catch (e) { $rt.raise('java/lang/NumberFormatException', String(s)); }
      },
      'valueOf(J)Ljava/lang/Long;': (v) => {
        const o = new (($rt.classes['java/lang/Long']))(); o.$v = v; return o;
      },
      'toString(J)Ljava/lang/String;': (v) => String(v),
    },
    sf: { MAX_VALUE: 9223372036854775807n, MIN_VALUE: -9223372036854775808n },
  });
  Long.s_TYPE = $rt.classFor('J');

  const Short = box('java/lang/Short', {
    zero: 0, str: (v) => String(v),
    m: { '<init>(S)V': function (v) { this.$v = (v << 16) >> 16; return this; } },
    s: {
      'parseShort(Ljava/lang/String;)S': (s) => (parseIntJ(s, 10) << 16) >> 16,
      'valueOf(S)Ljava/lang/Short;': (v) => {
        const o = new (($rt.classes['java/lang/Short']))(); o.$v = v; return o;
      },
    },
  });
  Short.s_TYPE = $rt.classFor('S');

  const Byte = box('java/lang/Byte', {
    zero: 0, str: (v) => String(v),
    m: { '<init>(B)V': function (v) { this.$v = (v << 24) >> 24; return this; } },
    s: { 'parseByte(Ljava/lang/String;)B': (s) => (parseIntJ(s, 10) << 24) >> 24 },
  });
  Byte.s_TYPE = $rt.classFor('B');

  function fstr(v) {
    if (Number.isNaN(v)) return 'NaN';
    if (!Number.isFinite(v)) return v > 0 ? 'Infinity' : '-Infinity';
    if (Number.isInteger(v) && Math.abs(v) < 1e7) return v.toFixed(1);
    return String(v);
  }
  $rt.fstr = fstr;

  const Float = box('java/lang/Float', {
    zero: 0, str: fstr,
    m: { '<init>(F)V': function (v) { this.$v = Math.fround(v); return this; } },
    s: {
      'parseFloat(Ljava/lang/String;)F': (s) => {
        const v = parseFloat(String(s).trim());
        if (Number.isNaN(v) && !/^nan$/i.test(String(s).trim())) {
          $rt.raise('java/lang/NumberFormatException', String(s));
        }
        return Math.fround(v);
      },
      'floatToIntBits(F)I': (v) => {
        const b = new DataView(new ArrayBuffer(4));
        b.setFloat32(0, v); return b.getInt32(0);
      },
      'intBitsToFloat(I)F': (v) => {
        const b = new DataView(new ArrayBuffer(4));
        b.setInt32(0, v); return b.getFloat32(0);
      },
      'valueOf(F)Ljava/lang/Float;': (v) => {
        const o = new (($rt.classes['java/lang/Float']))(); o.$v = v; return o;
      },
      'toString(F)Ljava/lang/String;': fstr,
    },
  });
  Float.s_TYPE = $rt.classFor('F');

  const Double = box('java/lang/Double', {
    zero: 0, str: fstr,
    m: { '<init>(D)V': function (v) { this.$v = v; return this; } },
    s: { 'parseDouble(Ljava/lang/String;)D': (s) => parseFloat(String(s).trim()) },
  });
  Double.s_TYPE = $rt.classFor('D');

  const Boolean_ = box('java/lang/Boolean', {
    zero: 0, str: (v) => (v ? 'true' : 'false'),
    m: { '<init>(Z)V': function (v) { this.$v = v ? 1 : 0; return this; } },
    s: {
      'parseBoolean(Ljava/lang/String;)Z': (s) => (String(s).toLowerCase() === 'true' ? 1 : 0),
      'valueOf(Z)Ljava/lang/Boolean;': (v) => {
        const o = new (($rt.classes['java/lang/Boolean']))(); o.$v = v ? 1 : 0; return o;
      },
    },
  });
  Boolean_.s_TYPE = $rt.classFor('Z');

  def('java/lang/Character', null, {
    ctor() { this.$v = 0; },
    m: { '<init>(C)V': function (v) { this.$v = v; return this; },
         'charValue()C': function () { return this.$v; },
         'toString()Ljava/lang/String;': function () { return String.fromCharCode(this.$v); } },
    s: {
      'isDigit(C)Z': (c) => (c >= 48 && c <= 57 ? 1 : 0),
      'isLetter(C)Z': (c) => (/[a-zA-Z]/.test(String.fromCharCode(c)) ? 1 : 0),
      'isWhitespace(C)Z': (c) => (/\s/.test(String.fromCharCode(c)) ? 1 : 0),
      'toLowerCase(C)C': (c) => String.fromCharCode(c).toLowerCase().charCodeAt(0),
      'toUpperCase(C)C': (c) => String.fromCharCode(c).toUpperCase().charCodeAt(0),
      'valueOf(C)Ljava/lang/Character;': (v) => {
        const o = new (($rt.classes['java/lang/Character']))(); o.$v = v; return o;
      },
      'toString(C)Ljava/lang/String;': (c) => String.fromCharCode(c),
    },
  });
  $rt.classes['java/lang/Character'].s_TYPE = $rt.classFor('C');

  $rt.unbox = function (v) {
    if (v === null || v === undefined) return 0;
    if (typeof v === 'object' && v.$v !== undefined) return v.$v;
    return v;
  };

  /* ------------------------------------------------------- reflect.Array */
  def('java/lang/reflect/Array', null, {
    s: {
      'newInstance(Ljava/lang/Class;[I)Ljava/lang/Object;': function (cls, dims) {
        const desc = cls.$desc;
        function make(level) {
          const n = dims[level];
          if (level === dims.length - 1) {
            if (desc.length === 1 && 'ZBSCIJFD'.indexOf(desc) >= 0) {
              const T = { Z: Uint8Array, B: Int8Array, S: Int16Array,
                          C: Uint16Array, I: Int32Array, J: BigInt64Array,
                          F: Float32Array, D: Float64Array }[desc];
              return new T(n);
            }
            return $rt.arr.newObj(desc, n);
          }
          const a = $rt.arr.newObj('[', n);
          for (let i = 0; i < n; i++) a[i] = make(level + 1);
          return a;
        }
        return make(0);
      },
      'getLength(Ljava/lang/Object;)I': (a) => a.length,
    },
  });

  $rt.initNames();
})($rt);
