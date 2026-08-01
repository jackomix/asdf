/* ============================================================================
 * natives-core.js — java.* framework classes (the part a device supplies from
 * core.jar / the JDK). These are the "native bindings" of the web Dalvik VM:
 * host-implemented classes the game's original bytecode links against.
 *
 * Semantics copied from java.lang / java.util / java.io 1.5 (Android API 4).
 * ========================================================================== */
'use strict';

function installCoreNatives(vm) {

  /* ---------------- java.lang.Object ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/Object;',
    superDesc: null,
    methods: {
      '<init>()V': (vm, thr, o) => { },
      'getClass()Ljava/lang/Class;': (vm, thr, o) => vm.classObject(o.c.isArray ? o.c.desc : o.c.desc),
      'toString()Ljava/lang/String;': (vm, thr, o) => {
        const hex = (o.id >>> 0).toString(16);
        return vm.newString((o.c.simpleName) + '@' + hex);
      },
      'equals(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => (o === x) ? 1 : 0,
      'hashCode()I': (vm, thr, o) => o.id | 0,
      'clone()Ljava/lang/Object;': (vm, thr, o) => {
        // arrays: shallow copy
        if (o.c && o.c.isArray) {
          const c = { c: o.c, a: o.a.slice(), n: o.n, et: o.et, id: 0 };
          c.id = vm._nextObjId ? vm._nextObjId() : (vm.stats.objects++, vm.stats.objects);
          vm.stats.objects++;
          return c;
        }
        const c2 = vm.newObject(o.c);
        c2.f = o.f.slice();
        return c2;
      },
      'notify()V': () => { },
      'notifyAll()V': () => { },
      'wait()V': () => { },
      'wait(J)V': () => { },
      'wait(JI)V': () => { },
    },
  });

  /* ---------------- java.lang.Class ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/Class;',
    methods: {
      '<init>()V': () => { },
      'getName()Ljava/lang/String;': (vm, thr, o) => {
        let d = o.typeDesc;
        let n = d;
        if (d[0] === 'L') n = d.slice(1, -1).replace(/\//g, '.');
        else if (d[0] === '[') n = d.replace(/\//g, '.');
        return vm.newString(n);
      },
      'desiredAssertionStatus()Z': () => 0,
      'isInterface()Z': (vm, thr, o) => {
        const t = vm.vmTypeOf(o);
        return (t && t.isInterface) ? 1 : 0;
      },
      'getResourceAsStream(Ljava/lang/String;)Ljava/io/InputStream;': (vm, thr, o, [nameObj]) => {
        const name = vm.jstr(nameObj);
        const bytes = vm.hostReadResource(name);
        if (!bytes) return null;
        const stream = vm.newObject(vm.requireClass('Ljava/io/ByteArrayInputStream;'));
        stream.a = Array.from(bytes); stream.pos = 0; stream.n = bytes.length;
        stream.c_stream_type = 'bais';
        return stream;
      },
      'getNameNative()Ljava/lang/String;': (vm, thr, o) => {
        return vm.newString(o.typeDesc);
      },
    },
  });

  /* ---------------- java.lang.String ---------------- */
  const S = (f) => f;
  vm.registerNative({
    desc: 'Ljava/lang/String;',
    interfaces: ['Ljava/lang/CharSequence;', 'Ljava/lang/Comparable;', 'Ljava/io/Serializable;'],
    methods: {
      '<init>()V': (vm, thr, o) => { o.js = ''; },
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [s]) => { o.js = s ? s.js : 'null'; },
      '<init>([B)V': (vm, thr, o, [b]) => { o.js = utf8Decode(b ? b.a : []); },
      '<init>([BLjava/lang/String;)V': (vm, thr, o, [b, charset]) => { o.js = utf8Decode(b ? b.a : []); },
      '<init>([BII)V': (vm, thr, o, [b, off, len]) => { o.js = utf8Decode((b ? b.a : []).slice(off, off + len)); },
      '<init>(Ljava/lang/StringBuilder;)V': (vm, thr, o, [sb]) => { o.js = sb ? sb.js : 'null'; },
      '<init>([C)V': (vm, thr, o, [c]) => { o.js = String.fromCharCode.apply(null, c ? c.a : []); },
      '<init>([CII)V': (vm, thr, o, [c, off, len]) => { o.js = String.fromCharCode.apply(null, (c ? c.a : []).slice(off, off + len)); },
      'length()I': (vm, thr, o) => o.js.length,
      'charAt(I)C': (vm, thr, o, [i]) => {
        if (i < 0 || i >= o.js.length) vm.throwNew(thr, 'Ljava/lang/StringIndexOutOfBoundsException;', '' + i);
        return o.js.charCodeAt(i);
      },
      'equals(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => (x && x.c && x.c.desc === 'Ljava/lang/String;' && x.js === o.js) ? 1 : 0,
      'equalsIgnoreCase(Ljava/lang/String;)Z': (vm, thr, o, [x]) => (x && o.js.toLowerCase() === x.js.toLowerCase()) ? 1 : 0,
      'hashCode()I': (vm, thr, o) => {
        if (o.hash !== 0) return o.hash | 0;
        let h = 0;
        for (let i = 0; i < o.js.length; i++) h = (Math.imul(h, 31) + o.js.charCodeAt(i)) | 0;
        o.hash = h; return h | 0;
      },
      'compareTo(Ljava/lang/String;)I': (vm, thr, o, [x]) => o.js < x.js ? -1 : o.js > x.js ? 1 : 0,
      'contains(Ljava/lang/CharSequence;)Z': (vm, thr, o, [x]) => o.js.indexOf(strOf(x)) >= 0 ? 1 : 0,
      'indexOf(I)I': (vm, thr, o, [c]) => o.js.indexOf(String.fromCharCode(c)),
      'indexOf(II)I': (vm, thr, o, [c, from]) => o.js.indexOf(String.fromCharCode(c), from),
      'indexOf(Ljava/lang/String;)I': (vm, thr, o, [s]) => o.js.indexOf(s.js),
      'indexOf(Ljava/lang/String;I)I': (vm, thr, o, [s, from]) => o.js.indexOf(s.js, from),
      'lastIndexOf(I)I': (vm, thr, o, [c]) => o.js.lastIndexOf(String.fromCharCode(c)),
      'lastIndexOf(II)I': (vm, thr, o, [c, from]) => {
        const i = o.js.lastIndexOf(String.fromCharCode(c), from);
        return i | 0;
      },
      'startsWith(Ljava/lang/String;)Z': (vm, thr, o, [s]) => o.js.startsWith(s.js) ? 1 : 0,
      'startsWith(Ljava/lang/String;I)Z': (vm, thr, o, [s, off]) => o.js.substr(off, s.js.length) === s.js ? 1 : 0,
      'endsWith(Ljava/lang/String;)Z': (vm, thr, o, [s]) => o.js.endsWith(s.js) ? 1 : 0,
      'isEmpty()Z': (vm, thr, o) => o.js.length === 0 ? 1 : 0,
      'substring(I)Ljava/lang/String;': (vm, thr, o, [i]) => {
        if (i < 0 || i > o.js.length) vm.throwNew(thr, 'Ljava/lang/StringIndexOutOfBoundsException;', '' + i);
        return vm.newString(o.js.substring(i));
      },
      'substring(II)Ljava/lang/String;': (vm, thr, o, [a, b]) => {
        if (a < 0 || b > o.js.length || a > b) vm.throwNew(thr, 'Ljava/lang/StringIndexOutOfBoundsException;', a + ',' + b);
        return vm.newString(o.js.substring(a, b));
      },
      'toLowerCase()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.js.toLowerCase()),
      'toUpperCase()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.js.toUpperCase()),
      'trim()Ljava/lang/String;': (vm, thr, o) => {
        // Java trim removes chars <= ' ' from both ends
        const s = o.js;
        let a = 0, b = s.length;
        while (a < b && s.charCodeAt(a) <= 0x20) a++;
        while (b > a && s.charCodeAt(b - 1) <= 0x20) b--;
        return vm.newString(s.substring(a, b));
      },
      'replace(CC)Ljava/lang/String;': (vm, thr, o, [a, b]) => {
        const ca = String.fromCharCode(a), cb = String.fromCharCode(b);
        return vm.newString(o.js.split(ca).join(cb));
      },
      'replace(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;': (vm, thr, o, [a, b]) => {
        return vm.newString(o.js.split(strOf(a)).join(strOf(b)));
      },
      'toString()Ljava/lang/String;': (vm, thr, o) => o,
      'getBytes()[B': (vm, thr, o) => {
        const bytes = utf8Encode(o.js);
        const arr = vm.newArray('B', bytes.length, thr);
        arr.a = bytes;
        return arr;
      },
      'getBytes(Ljava/lang/String;)[B': (vm, thr, o, [enc]) => {
        const bytes = utf8Encode(o.js);
        const arr = vm.newArray('B', bytes.length, thr);
        arr.a = bytes;
        return arr;
      },
      'toCharArray()[C': (vm, thr, o) => {
        const arr = vm.newArray('C', o.js.length, thr);
        for (let i = 0; i < o.js.length; i++) arr.a[i] = o.js.charCodeAt(i);
        return arr;
      },
      'intern()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.js),
      'compareToIgnoreCase(Ljava/lang/String;)I': (vm, thr, o, [x]) => {
        const a = o.js.toLowerCase(), b = x.js.toLowerCase();
        return a < b ? -1 : a > b ? 1 : 0;
      },
      // statics
      'valueOf(I)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(String(x | 0)),
      'valueOf(J)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(x.toString()),
      'valueOf(C)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(String.fromCharCode(x)),
      'valueOf(Z)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(x ? 'true' : 'false'),
      'valueOf(F)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(javaDoubleToString(x)),
      'valueOf(D)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(javaDoubleToString(x)),
      'valueOf(Ljava/lang/Object;)Ljava/lang/String;': (vm, thr, o, [x]) => {
        if (x === null || x === 0) return vm.newString('null');
        if (x.js !== undefined) return x;
        return vm.call(thr, x, 'toString()Ljava/lang/String;');
      },
      'valueOf([C)Ljava/lang/String;': (vm, thr, o, [c]) => vm.newString(c ? String.fromCharCode.apply(null, c.a) : 'null'),
      'format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;': (vm, thr, o, [fmt, args]) => {
        return vm.newString(javaFormat(vm, thr, fmt.js, args));
      },
      'split(Ljava/lang/String;)[Ljava/lang/String;': (vm, thr, o, [reObj]) => {
        const re = new RegExp(reObj.js);
        const parts = o.js.split(re);
        // Java split removes trailing empty strings
        while (parts.length && parts[parts.length - 1] === '') parts.pop();
        const arr = vm.newArray('Ljava/lang/String;', parts.length, thr);
        for (let i = 0; i < parts.length; i++) arr.a[i] = vm.newString(parts[i]);
        return arr;
      },
    },
    staticSigs: new Set([
      'valueOf(I)Ljava/lang/String;', 'valueOf(J)Ljava/lang/String;', 'valueOf(C)Ljava/lang/String;',
      'valueOf(Z)Ljava/lang/String;', 'valueOf(F)Ljava/lang/String;', 'valueOf(D)Ljava/lang/String;',
      'valueOf(Ljava/lang/Object;)Ljava/lang/String;', 'valueOf([C)Ljava/lang/String;',
      'format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;',
    ]),
  });

  function strOf(x) {
    if (x === null || x === 0) return 'null';
    return x.js !== undefined ? x.js : String(x);
  }
  vm._strOf = strOf;

  /* ---------------- java.lang.CharSequence ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/CharSequence;',
    accessFlags: 0x0601,
    methods: {},
  });

  /* ---------------- java.lang.StringBuilder ---------------- */
  const sbProto = {};
  const appendImpl = (conv) => (vm, thr, o, [x]) => { o.js += conv(x); return o; };
  vm.registerNative({
    desc: 'Ljava/lang/StringBuilder;',
    interfaces: ['Ljava/lang/CharSequence;'],
    methods: {
      '<init>()V': (vm, thr, o) => { o.js = ''; },
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [s]) => { o.js = s ? s.js : 'null'; },
      '<init>(I)V': (vm, thr, o) => { o.js = ''; },
      'append(Ljava/lang/String;)Ljava/lang/StringBuilder;': appendImpl((x) => x === null || x === 0 ? 'null' : x.js),
      'append(Ljava/lang/Object;)Ljava/lang/StringBuilder;': appendImpl((x) => x === null || x === 0 ? 'null' : (x.js !== undefined ? x.js : x)),
      'append(I)Ljava/lang/StringBuilder;': appendImpl((x) => String(x | 0)),
      'append(J)Ljava/lang/StringBuilder;': appendImpl((x) => x.toString()),
      'append(C)Ljava/lang/StringBuilder;': appendImpl((x) => String.fromCharCode(x)),
      'append(Z)Ljava/lang/StringBuilder;': appendImpl((x) => x ? 'true' : 'false'),
      'append(F)Ljava/lang/StringBuilder;': appendImpl((x) => javaDoubleToString(x)),
      'append(D)Ljava/lang/StringBuilder;': appendImpl((x) => javaDoubleToString(x)),
      'append([C)Ljava/lang/StringBuilder;': appendImpl((x) => x === null || x === 0 ? 'null' : String.fromCharCode.apply(null, x.a)),
      'append(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;': appendImpl((x) => x === null || x === 0 ? 'null' : strOf(x)),
      'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.js),
      'length()I': (vm, thr, o) => o.js.length,
      'charAt(I)C': (vm, thr, o, [i]) => o.js.charCodeAt(i),
      'delete(II)Ljava/lang/StringBuilder;': (vm, thr, o, [a, b]) => { o.js = o.js.slice(0, a) + o.js.slice(b); return o; },
      'insert(ILjava/lang/String;)Ljava/lang/StringBuilder;': (vm, thr, o, [i, s]) => { o.js = o.js.slice(0, i) + (s ? s.js : 'null') + o.js.slice(i); return o; },
      'substring(II)Ljava/lang/String;': (vm, thr, o, [a, b]) => vm.newString(o.js.substring(a, b)),
      'indexOf(Ljava/lang/String;)I': (vm, thr, o, [s]) => o.js.indexOf(s.js),
    },
  });

  /* ---------------- java.lang.Thread ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/Thread;',
    interfaces: ['Ljava/lang/Runnable;'],
    methods: {
      '<init>()V': (vm, thr, o) => { o._target = null; o._name = 'Thread-' + (o.id); o._vthread = null; },
      '<init>(Ljava/lang/Runnable;)V': (vm, thr, o, [r]) => { o._target = r; o._name = 'Thread-' + (o.id); o._vthread = null; },
      '<init>(Ljava/lang/Runnable;Ljava/lang/String;)V': (vm, thr, o, [r, n]) => { o._target = r; o._name = n ? n.js : 'thread'; o._vthread = null; },
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [n]) => { o._target = null; o._name = n ? n.js : 'thread'; o._vthread = null; },
      'start()V': (vm, thr, o) => {
        const t = vm.createThread(o._name, (vm2, thr2) => {
          const target = o._target && o._target !== 0 ? o._target : o;
          vm2.call(thr2, target, 'run()V');
        });
        o._vthread = t;
        o._started = true;
      },
      'join()V': (vm, thr, o) => {
        // cooperative: block current thread until target dead
        const t = o._vthread;
        if (t && !t.dead) {
          thr.blockedUntil = vm.now() + 10000000; // woken by scheduler reaping; simplified
        }
      },
      'sleep(J)V': (vm, thr, o, [ms]) => {
        if (thr.interrupted) { thr.interrupted = false; vm.throwNew(thr, 'Ljava/lang/InterruptedException;', ''); }
        const n = typeof ms === 'bigint' ? Number(ms) : ms;
        thr.blockedUntil = vm.now() + Math.max(0, n);
      },
      'yield()V': (vm, thr) => { thr.blockedUntil = vm.now() + 1; },
      'interrupt()V': (vm, thr, o) => {
        const t = o._vthread;
        if (t) { t.interrupted = true; t.blockedUntil = 0; }
      },
      'isAlive()Z': (vm, thr, o) => (o._vthread && !o._vthread.dead) ? 1 : 0,
      'run()V': (vm, thr, o) => {
        if (o._target && o._target !== 0) vm.call(thr, o._target, 'run()V');
      },
      'currentThread()Ljava/lang/Thread;': (vm, thr) => {
        thr._javaThreadObj = thr._javaThreadObj || (() => {
          const o2 = vm.newObject(vm.requireClass('Ljava/lang/Thread;'));
          o2._vthread = thr; o2._name = thr.name;
          return o2;
        })();
        return thr._javaThreadObj;
      },
      'activeCount()I': () => vm.threads.length,
      'getName()Ljava/lang/String;': (vm, thr, o) => vm.newString(o._name || 'thread'),
      'setName(Ljava/lang/String;)V': (vm, thr, o, [n]) => { o._name = n ? n.js : ''; },
    },
    staticSigs: new Set(['sleep(J)V', 'yield()V', 'currentThread()Ljava/lang/Thread;', 'activeCount()I']),
  });
  vm.registerNative({ desc: 'Ljava/lang/Runnable;', accessFlags: 0x0601, methods: {} });

  /* ---------------- java.lang.System ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/System;',
    methods: {
      'arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V': (vm, thr, o, [src, sp, dst, dp, len]) => {
        if (src === null || src === 0 || dst === null || dst === 0) vm.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
        if (sp < 0 || dp < 0 || len < 0 || sp + len > src.n || dp + len > dst.n) {
          vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', 'src.length=' + src.n + '; dest.length=' + dst.n + '; len=' + len);
        }
        const sw = ncIsWideDesc(src.et), dw = ncIsWideDesc(dst.et);
        // overlap-safe copy (use temp when same array & ranges overlap)
        const tmp = new Array(len);
        if (sw) {
          for (let i = 0; i < len; i++) tmp[i] = src.a[(sp + i) * 2];
          for (let i = 0; i < len; i++) { dst.a[(dp + i) * 2] = tmp[i]; dst.a[(dp + i) * 2 + 1] = NC_WIDE2; }
        } else {
          for (let i = 0; i < len; i++) tmp[i] = src.a[sp + i];
          for (let i = 0; i < len; i++) dst.a[dp + i] = tmp[i];
        }
      },
      'currentTimeMillis()J': (vm) => BigInt(Math.floor(vm.now())),
      'nanoTime()J': (vm) => BigInt(Math.floor(vm.now() * 1e6)),
      'exit(I)V': (vm, thr, o, [code]) => { vm.onLog('[vm] System.exit(' + code + ')'); if (vm.host) vm.host.onExit && vm.host.onExit(code); },
      'gc()V': () => { },
      'runFinalization()V': () => { },
      'getProperty(Ljava/lang/String;)Ljava/lang/String;': (vm, thr, o, [k]) => {
        const props = {
          'line.separator': '\n', 'file.separator': '/', 'path.separator': ':',
          'os.name': 'Linux', 'os.version': '2.6.32', 'java.version': '1.6.0',
          'java.vendor': 'The Android Project', 'user.home': '', 'user.name': '',
          'java.io.tmpdir': '/tmp', 'user.timezone': 'GMT',
          'microedition.configuration': 'CLDC-1.1', 'microedition.profiles': 'MIDP-2.0',
        };
        const v = props[vm.jstr(k)];
        return v === undefined ? null : vm.newString(v);
      },
      'getenv(Ljava/lang/String;)Ljava/lang/String;': () => null,
      'identityHashCode(Ljava/lang/Object;)I': (vm, thr, o, [x]) => x ? x.id | 0 : 0,
    },
    staticSigs: new Set(['arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V', 'currentTimeMillis()J', 'nanoTime()J', 'exit(I)V', 'gc()V', 'runFinalization()V', 'getProperty(Ljava/lang/String;)Ljava/lang/String;', 'getenv(Ljava/lang/String;)Ljava/lang/String;', 'identityHashCode(Ljava/lang/Object;)I']),
  });

  /* ---------------- java.lang.Math ---------------- */
  const mth = (f) => f;
  vm.registerNative({
    desc: 'Ljava/lang/Math;',
    methods: {
      'abs(I)I': (vm, thr, o, [x]) => Math.abs(x | 0),
      'abs(J)J': (vm, thr, o, [x]) => x < 0n ? -x : x,
      'abs(F)F': (vm, thr, o, [x]) => Math.fround(Math.abs(x)),
      'abs(D)D': (vm, thr, o, [x]) => Math.abs(x),
      'max(II)I': (vm, thr, o, [a, b]) => Math.max(a, b),
      'max(JJ)J': (vm, thr, o, [a, b]) => a > b ? a : b,
      'max(FF)F': (vm, thr, o, [a, b]) => Math.max(a, b),
      'max(DD)D': (vm, thr, o, [a, b]) => Math.max(a, b),
      'min(II)I': (vm, thr, o, [a, b]) => Math.min(a, b),
      'min(JJ)J': (vm, thr, o, [a, b]) => a < b ? a : b,
      'min(FF)F': (vm, thr, o, [a, b]) => Math.min(a, b),
      'min(DD)D': (vm, thr, o, [a, b]) => Math.min(a, b),
      'sin(D)D': (vm, thr, o, [x]) => Math.sin(x),
      'cos(D)D': (vm, thr, o, [x]) => Math.cos(x),
      'tan(D)D': (vm, thr, o, [x]) => Math.tan(x),
      'sqrt(D)D': (vm, thr, o, [x]) => Math.sqrt(x),
      'pow(DD)D': (vm, thr, o, [a, b]) => Math.pow(a, b),
      'exp(D)D': (vm, thr, o, [x]) => Math.exp(x),
      'log(D)D': (vm, thr, o, [x]) => Math.log(x),
      'floor(D)D': (vm, thr, o, [x]) => Math.floor(x),
      'ceil(D)D': (vm, thr, o, [x]) => Math.ceil(x),
      'rint(D)D': (vm, thr, o, [x]) => Math.round(x),
      'round(F)I': (vm, thr, o, [x]) => Math.floor(x + 0.5),
      'round(D)J': (vm, thr, o, [x]) => BigInt(Math.floor(x + 0.5)),
      'random()D': () => Math.random(),
      'toRadians(D)D': (vm, thr, o, [x]) => x * (Math.PI / 180),
      'toDegrees(D)D': (vm, thr, o, [x]) => x * (180 / Math.PI),
      'atan2(DD)D': (vm, thr, o, [y, x]) => Math.atan2(y, x),
      'hypot(DD)D': (vm, thr, o, [x, y]) => Math.sqrt(x * x + y * y),
    },
    staticSigs: new Set(['abs(I)I', 'abs(J)J', 'abs(F)F', 'abs(D)D', 'max(II)I', 'max(JJ)J', 'max(FF)F', 'max(DD)D', 'min(II)I', 'min(JJ)J', 'min(FF)F', 'min(DD)D', 'sin(D)D', 'cos(D)D', 'tan(D)D', 'sqrt(D)D', 'pow(DD)D', 'exp(D)D', 'log(D)D', 'floor(D)D', 'ceil(D)D', 'rint(D)D', 'round(F)I', 'round(D)J', 'random()D', 'toRadians(D)D', 'toDegrees(D)D', 'atan2(DD)D', 'hypot(DD)D']),
  });

  /* ---------------- boxed types ---------------- */
  const mkBox = (desc, primDesc, methods, statics) => {
    const def = { desc, methods, staticSigs: new Set(statics || []), sfields: [{ name: 'TYPE', desc: 'Ljava/lang/Class;' }] };
    def.clinit = (vm2, cls) => {
      const f = cls.sfields.find((x) => x.name === 'TYPE');
      if (f) cls.statics[f.slot] = vm2.classObject(primDesc);
    };
    vm.registerNative(def);
  };

  mkBox('Ljava/lang/Integer;', 'I', {
    '<init>(I)V': (vm, thr, o, [x]) => { o.v = x | 0; },
    'intValue()I': (vm, thr, o) => o.v | 0,
    'valueOf(I)Ljava/lang/Integer;': (vm, thr, o, [x]) => { const b = vm.newObject(vm.requireClass('Ljava/lang/Integer;')); b.v = x | 0; return b; },
    'parseInt(Ljava/lang/String;)I': (vm, thr, o, [s]) => parseIntJava(vm, thr, s.js, 10),
    'parseInt(Ljava/lang/String;I)I': (vm, thr, o, [s, r]) => parseIntJava(vm, thr, s.js, r),
    'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(String(o.v | 0)),
    'toString(I)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(String(x | 0)),
    'toString(II)Ljava/lang/String;': (vm, thr, o, [x, r]) => vm.newString((x | 0).toString(r)),
    'hashCode()I': (vm, thr, o) => o.v | 0,
    'equals(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => (x && x.c.desc === 'Ljava/lang/Integer;' && x.v === o.v) ? 1 : 0,
  }, ['valueOf(I)Ljava/lang/Integer;', 'parseInt(Ljava/lang/String;)I', 'parseInt(Ljava/lang/String;I)I', 'toString(I)Ljava/lang/String;', 'toString(II)Ljava/lang/String;']);

  mkBox('Ljava/lang/Long;', 'J', {
    '<init>(J)V': (vm, thr, o, [x]) => { o.v = BigInt.asIntN(64, x); },
    'longValue()J': (vm, thr, o) => o.v,
    'valueOf(J)Ljava/lang/Long;': (vm, thr, o, [x]) => { const b = vm.newObject(vm.requireClass('Ljava/lang/Long;')); b.v = BigInt.asIntN(64, x); return b; },
    'parseLong(Ljava/lang/String;)J': (vm, thr, o, [s]) => parseLongJava(vm, thr, s.js),
    'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.v.toString()),
    'toString(J)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(x.toString()),
  }, ['valueOf(J)Ljava/lang/Long;', 'parseLong(Ljava/lang/String;)J', 'toString(J)Ljava/lang/String;']);

  mkBox('Ljava/lang/Short;', 'S', {
    '<init>(S)V': (vm, thr, o, [x]) => { o.v = (x << 16) >> 16; },
    'shortValue()S': (vm, thr, o) => o.v,
    'valueOf(S)Ljava/lang/Short;': (vm, thr, o, [x]) => { const b = vm.newObject(vm.requireClass('Ljava/lang/Short;')); b.v = (x << 16) >> 16; return b; },
    'parseShort(Ljava/lang/String;)S': (vm, thr, o, [s]) => { const v = parseIntJava(vm, thr, s.js, 10); return (v << 16) >> 16; },
  }, ['valueOf(S)Ljava/lang/Short;', 'parseShort(Ljava/lang/String;)S']);

  mkBox('Ljava/lang/Float;', 'F', {
    '<init>(F)V': (vm, thr, o, [x]) => { o.v = Math.fround(x); },
    'floatValue()F': (vm, thr, o) => o.v,
    'valueOf(F)Ljava/lang/Float;': (vm, thr, o, [x]) => { const b = vm.newObject(vm.requireClass('Ljava/lang/Float;')); b.v = Math.fround(x); return b; },
    'parseFloat(Ljava/lang/String;)F': (vm, thr, o, [s]) => parseFloatJava(vm, thr, s.js),
    'floatToIntBits(F)I': (vm, thr, o, [x]) => { const f = new Float32Array(1); f[0] = x; return new Int32Array(f.buffer)[0]; },
    'intBitsToFloat(I)F': (vm, thr, o, [x]) => { const i = new Int32Array(1); i[0] = x; return new Float32Array(i.buffer)[0]; },
    'toString(F)Ljava/lang/String;': (vm, thr, o, [x]) => vm.newString(javaDoubleToString(x)),
  }, ['valueOf(F)Ljava/lang/Float;', 'parseFloat(Ljava/lang/String;)F', 'floatToIntBits(F)I', 'intBitsToFloat(I)F', 'toString(F)Ljava/lang/String;']);

  mkBox('Ljava/lang/Double;', 'D', {
    '<init>(D)V': (vm, thr, o, [x]) => { o.v = x; },
    'doubleValue()D': (vm, thr, o) => o.v,
    'valueOf(D)Ljava/lang/Double;': (vm, thr, o, [x]) => { const b = vm.newObject(vm.requireClass('Ljava/lang/Double;')); b.v = x; return b; },
    'parseDouble(Ljava/lang/String;)D': (vm, thr, o, [s]) => {
      const v = parseFloat(s.js.trim());
      if (isNaN(v) && !/^nan$/i.test(s.js.trim())) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s.js);
      return v;
    },
  }, ['valueOf(D)Ljava/lang/Double;', 'parseDouble(Ljava/lang/String;)D']);

  mkBox('Ljava/lang/Boolean;', 'Z', {
    '<init>(Z)V': (vm, thr, o, [x]) => { o.v = x ? 1 : 0; },
    'booleanValue()Z': (vm, thr, o) => o.v ? 1 : 0,
    'valueOf(Z)Ljava/lang/Boolean;': (vm, thr, o, [x]) => { const b = vm.newObject(vm.requireClass('Ljava/lang/Boolean;')); b.v = x ? 1 : 0; return b; },
    'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.v ? 'true' : 'false'),
  }, ['valueOf(Z)Ljava/lang/Boolean;']);

  mkBox('Ljava/lang/Byte;', 'B', {
    '<init>(B)V': (vm, thr, o, [x]) => { o.v = (x << 24) >> 24; },
    'byteValue()B': (vm, thr, o) => o.v,
  });

  vm.registerNative({
    desc: 'Ljava/lang/Character;',
    methods: {
      'isDigit(C)Z': (vm, thr, o, [c]) => (c >= 0x30 && c <= 0x39) ? 1 : 0,
      'isLetter(C)Z': (vm, thr, o, [c]) => {
        const ch = String.fromCharCode(c);
        return /[a-zA-Z]/.test(ch) ? 1 : 0;
      },
      'isLetterOrDigit(C)Z': (vm, thr, o, [c]) => {
        const ch = String.fromCharCode(c);
        return /[a-zA-Z0-9]/.test(ch) ? 1 : 0;
      },
      'toLowerCase(C)C': (vm, thr, o, [c]) => String.fromCharCode(c).toLowerCase().charCodeAt(0),
      'toUpperCase(C)C': (vm, thr, o, [c]) => String.fromCharCode(c).toUpperCase().charCodeAt(0),
      'isWhitespace(C)Z': (vm, thr, o, [c]) => /\s/.test(String.fromCharCode(c)) ? 1 : 0,
    },
    staticSigs: new Set(['isDigit(C)Z', 'isLetter(C)Z', 'isLetterOrDigit(C)Z', 'toLowerCase(C)C', 'toUpperCase(C)C', 'isWhitespace(C)Z']),
  });

  vm.registerNative({ desc: 'Ljava/lang/Comparable;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Ljava/io/Serializable;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Ljava/lang/Cloneable;', accessFlags: 0x0601, methods: {} });

  /* ---------------- Throwable hierarchy ---------------- */
  const throwCtor = (vm, thr, o) => { if (o.vmMsg === undefined) o.vmMsg = null; };
  const throwCtorMsg = (msgArg) => (vm, thr, o, args) => {
    const m = args && args.length ? args[0] : null;
    o.vmMsg = m === null || m === 0 ? null : (m.js !== undefined ? m.js : vm._strOf(m));
    if (!o.vmTrace) o.vmTrace = thr.frames.map((f) => '        at ' + f.m.fullName()).reverse();
  };
  const throwableToString = (vm, thr, o) => {
    return vm.newString(o.c.simpleName + (o.vmMsg ? ': ' + o.vmMsg : ''));
  };
  const printStackTraceImpl = (vm, thr, o) => {
    const lines = [o.c.simpleName + (o.vmMsg ? ': ' + o.vmMsg : '')].concat(o.vmTrace || []);
    vm.onLog('[stacktrace] ' + lines.join('\n'));
  };
  const mkThrowable = (desc, superDesc) => {
    vm.registerNative({
      desc,
      superDesc,
      methods: {
        '<init>()V': throwCtor,
        '<init>(Ljava/lang/String;)V': throwCtorMsg(0),
        '<init>(Ljava/lang/String;Ljava/lang/Throwable;)V': throwCtorMsg(0),
        '<init>(Ljava/lang/Throwable;)V': (vm, thr, o, [t]) => { o.vmMsg = t && t.vmMsg !== undefined ? t.vmMsg : (t ? t.c.simpleName : null); },
        'toString()Ljava/lang/String;': throwableToString,
        'printStackTrace()V': printStackTraceImpl,
        'getMessage()Ljava/lang/String;': (vm, thr, o) => o.vmMsg ? vm.newString(o.vmMsg) : null,
      },
    });
  };
  mkThrowable('Ljava/lang/Throwable;', 'Ljava/lang/Object;', null);
  mkThrowable('Ljava/lang/Error;', 'Ljava/lang/Throwable;');
  mkThrowable('Ljava/lang/Exception;', 'Ljava/lang/Throwable;');
  mkThrowable('Ljava/lang/RuntimeException;', 'Ljava/lang/Exception;');
  mkThrowable('Ljava/lang/ArithmeticException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/NullPointerException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/ClassCastException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/IndexOutOfBoundsException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/ArrayIndexOutOfBoundsException;', 'Ljava/lang/IndexOutOfBoundsException;');
  mkThrowable('Ljava/lang/StringIndexOutOfBoundsException;', 'Ljava/lang/IndexOutOfBoundsException;');
  mkThrowable('Ljava/lang/NegativeArraySizeException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/ArrayStoreException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/IllegalArgumentException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/NumberFormatException;', 'Ljava/lang/IllegalArgumentException;');
  mkThrowable('Ljava/lang/IllegalStateException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/UnsupportedOperationException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/InterruptedException;', 'Ljava/lang/Exception;');
  mkThrowable('Ljava/lang/IllegalMonitorStateException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/SecurityException;', 'Ljava/lang/RuntimeException;');
  mkThrowable('Ljava/lang/ClassNotFoundException;', 'Ljava/lang/Exception;');
  mkThrowable('Ljava/lang/NoClassDefFoundError;', 'Ljava/lang/Error;');
  mkThrowable('Ljava/lang/AssertionError;', 'Ljava/lang/Error;');
  mkThrowable('Ljava/lang/OutOfMemoryError;', 'Ljava/lang/Error;');
  mkThrowable('Ljava/lang/VerifyError;', 'Ljava/lang/Error;');

  /* ---------------- java.lang.Enum ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/Enum;',
    methods: {
      '<init>(Ljava/lang/String;I)V': (vm, thr, o, [name, ord]) => { o._enumName = name ? name.js : ''; o._enumOrdinal = ord; },
      'name()Ljava/lang/String;': (vm, thr, o) => vm.newString(o._enumName || ''),
      'ordinal()I': (vm, thr, o) => o._enumOrdinal | 0,
      'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(o._enumName || ''),
      'equals(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => o === x ? 1 : 0,
      'hashCode()I': (vm, thr, o) => o.id | 0,
      'valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;': (vm, thr, o, [clsObj, nameObj]) => {
        const t = vm.vmTypeOf(clsObj);
        if (t && t._enumTable) {
          const v = t._enumTable[vm.jstr(nameObj)];
          if (v) return v;
        }
        vm.throwNew(thr, 'Ljava/lang/IllegalArgumentException;', vm.jstr(nameObj));
      },
    },
    staticSigs: new Set(['valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;']),
  });
  vm.registerNative({ desc: 'Ljava/lang/reflect/GenericDeclaration;', accessFlags: 0x0601, methods: {} });

  /* ---------------- java.lang.reflect.Array ---------------- */
  vm.registerNative({
    desc: 'Ljava/lang/reflect/Array;',
    methods: {
      'newInstance(Ljava/lang/Class;I)Ljava/lang/Object;': (vm, thr, o, [clsObj, len]) => {
        const td = clsObj.typeDesc;
        return vm.newArray(td, len, thr);
      },
      'newInstance(Ljava/lang/Class;[I)Ljava/lang/Object;': (vm, thr, o, [clsObj, dims]) => {
        const build = (typeDesc, di) => {
          const len = dims.a[di];
          const arr = vm.newArray(typeDesc, len, thr);
          if (di < dims.n - 1) {
            const subDesc = typeDesc[0] === '[' ? typeDesc.slice(1) : typeDesc;
            for (let i = 0; i < len; i++) arr.a[i] = build(subDesc, di + 1);
          }
          return arr;
        };
        let td = clsObj.typeDesc;
        for (let i = 0; i < dims.n - 1; i++) td = '[' + td;
        return build(td, 0);
      },
      'getLength(Ljava/lang/Object;)I': (vm, thr, o, [arr]) => arr ? arr.n : 0,
    },
    staticSigs: new Set(['newInstance(Ljava/lang/Class;I)Ljava/lang/Object;', 'newInstance(Ljava/lang/Class;[I)Ljava/lang/Object;', 'getLength(Ljava/lang/Object;)I']),
  });

  /* ---------------- java.util.* ---------------- */
  vm.registerNative({
    desc: 'Ljava/util/Iterator;',
    accessFlags: 0x0601,
    methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/util/Enumeration;',
    accessFlags: 0x0601,
    methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/util/ListIterator;',
    accessFlags: 0x0601,
    interfaces: ['Ljava/util/Iterator;'],
    methods: {},
  });
  const collectionMethods = {
    'size()I': (vm, thr, o) => o.items.length,
    'isEmpty()Z': (vm, thr, o) => o.items.length === 0 ? 1 : 0,
    'clear()V': (vm, thr, o) => { o.items.length = 0; },
    'add(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => { o.items.push(x); return 1; },
    'addElement(Ljava/lang/Object;)V': (vm, thr, o, [x]) => { o.items.push(x); },
    'elementAt(I)Ljava/lang/Object;': (vm, thr, o, [i]) => {
      if (i < 0 || i >= o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', i + ' >= ' + o.items.length);
      return o.items[i];
    },
    'get(I)Ljava/lang/Object;': (vm, thr, o, [i]) => {
      if (i < 0 || i >= o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', i + ' >= ' + o.items.length);
      return o.items[i];
    },
    'firstElement()Ljava/lang/Object;': (vm, thr, o) => {
      if (!o.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
      return o.items[0];
    },
    'lastElement()Ljava/lang/Object;': (vm, thr, o) => {
      if (!o.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
      return o.items[o.items.length - 1];
    },
    'indexOf(Ljava/lang/Object;)I': (vm, thr, o, [x]) => {
      for (let i = 0; i < o.items.length; i++) {
        const it = o.items[i];
        const eq = (it === x) || (it && x && it.js !== undefined && it.js === x.js);
        if (eq) return i;
      }
      return -1;
    },
    'insertElementAt(Ljava/lang/Object;I)V': (vm, thr, o, [x, i]) => {
      if (i < 0 || i > o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', '');
      o.items.splice(i, 0, x);
    },
    'removeElement(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => {
      const i = o.items.indexOf(x);
      if (i < 0) {
        // try string-equality fallback
        for (let k = 0; k < o.items.length; k++) {
          const it = o.items[k];
          if (it && x && it.js !== undefined && x.js !== undefined && it.js === x.js) {
            o.items.splice(k, 1); return 1;
          }
        }
        return 0;
      }
      o.items.splice(i, 1);
      return 1;
    },
    'remove(Ljava/lang/Object;)Z': null, // filled below
    'removeElementAt(I)V': (vm, thr, o, [i]) => {
      if (i < 0 || i >= o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', '');
      o.items.splice(i, 1);
    },
    'remove(I)Ljava/lang/Object;': (vm, thr, o, [i]) => {
      if (i < 0 || i >= o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', '');
      return o.items.splice(i, 1)[0];
    },
    'removeAllElements()V': (vm, thr, o) => { o.items.length = 0; },
    'setElementAt(Ljava/lang/Object;I)V': (vm, thr, o, [x, i]) => {
      if (i < 0 || i >= o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', '');
      o.items[i] = x;
    },
    'set(ILjava/lang/Object;)Ljava/lang/Object;': (vm, thr, o, [i, x]) => {
      if (i < 0 || i >= o.items.length) vm.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', '');
      const old = o.items[i]; o.items[i] = x; return old;
    },
    'contains(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => {
      for (const it of o.items) {
        if (it === x) return 1;
        if (it && x && it.js !== undefined && x.js !== undefined && it.js === x.js) return 1;
      }
      return 0;
    },
    'toArray()[Ljava/lang/Object;': (vm, thr, o) => {
      const arr = vm.newArray('Ljava/lang/Object;', o.items.length, thr);
      for (let i = 0; i < o.items.length; i++) arr.a[i] = o.items[i];
      return arr;
    },
    'iterator()Ljava/util/Iterator;': (vm, thr, o) => {
      const it = vm.newObject(vm.requireClass('Ljava/util/Vector$Itr;'));
      it._vec = o; it._i = 0;
      return it;
    },
    'elements()Ljava/util/Enumeration;': (vm, thr, o) => {
      const it = vm.newObject(vm.requireClass('Ljava/util/Vector$Itr;'));
      it._vec = o; it._i = 0;
      return it;
    },
  };
  collectionMethods['remove(Ljava/lang/Object;)Z'] = collectionMethods['removeElement(Ljava/lang/Object;)Z'];

  const mkItr = (vm) => {
    vm.registerNative({
      desc: 'Ljava/util/Vector$Itr;',
      interfaces: ['Ljava/util/Iterator;', 'Ljava/util/Enumeration;'],
      methods: {
        '<init>()V': () => { },
        'hasNext()Z': (vm, thr, o) => (o._vec && o._i < o._vec.items.length) ? 1 : 0,
        'hasMoreElements()Z': (vm, thr, o) => (o._vec && o._i < o._vec.items.length) ? 1 : 0,
        'next()Ljava/lang/Object;': (vm, thr, o) => {
          if (!o._vec || o._i >= o._vec.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
          return o._vec.items[o._i++];
        },
        'nextElement()Ljava/lang/Object;': (vm, thr, o) => {
          if (!o._vec || o._i >= o._vec.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
          return o._vec.items[o._i++];
        },
        'remove()V': (vm, thr, o) => {
          if (!o._vec || o._i <= 0) vm.throwNew(thr, 'Ljava/lang/IllegalStateException;', '');
          o._vec.items.splice(--o._i, 1);
        },
      },
    });
  };
  mkItr(vm);

  vm.registerNative({
    desc: 'Ljava/util/Collection;',
    accessFlags: 0x0601, methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/util/List;',
    accessFlags: 0x0601, interfaces: ['Ljava/util/Collection;'], methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/util/Set;',
    accessFlags: 0x0601, interfaces: ['Ljava/util/Collection;'], methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/util/Queue;',
    accessFlags: 0x0601, interfaces: ['Ljava/util/Collection;'], methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/util/AbstractCollection;',
    interfaces: ['Ljava/util/Collection;'],
    methods: {
      '<init>()V': (vm, thr, o) => { o.items = o.items || []; },
    },
  });
  vm.registerNative({
    desc: 'Ljava/util/AbstractList;',
    superDesc: 'Ljava/util/AbstractCollection;',
    interfaces: ['Ljava/util/List;'],
    methods: { '<init>()V': (vm, thr, o) => { o.items = o.items || []; } },
  });

  const vectorDef = Object.assign({}, collectionMethods, {
    '<init>()V': (vm, thr, o) => { o.items = []; },
    '<init>(I)V': (vm, thr, o) => { o.items = []; },
  });

  vm.registerNative({
    desc: 'Ljava/util/Vector;',
    superDesc: 'Ljava/util/AbstractList;',
    methods: vectorDef,
  });

  mkThrowable('Ljava/util/NoSuchElementException;', 'Ljava/lang/RuntimeException;');

  vm.registerNative({
    desc: 'Ljava/util/HashSet;',
    interfaces: ['Ljava/util/Set;'],
    methods: {
      '<init>()V': (vm, thr, o) => { o.items = []; },
      '<init>(I)V': (vm, thr, o) => { o.items = []; },
      'add(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => {
        for (const it of o.items) {
          if (it === x) return 0;
          if (it && x && it.js !== undefined && x.js !== undefined && it.js === x.js) return 0;
        }
        o.items.push(x); return 1;
      },
      'contains(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => {
        for (const it of o.items) {
          if (it === x) return 1;
          if (it && x && it.js !== undefined && x.js !== undefined && it.js === x.js) return 1;
        }
        return 0;
      },
      'isEmpty()Z': (vm, thr, o) => o.items.length === 0 ? 1 : 0,
      'size()I': (vm, thr, o) => o.items.length,
      'remove(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => {
        const i = o.items.indexOf(x);
        if (i >= 0) { o.items.splice(i, 1); return 1; }
        for (let k = 0; k < o.items.length; k++) {
          const it = o.items[k];
          if (it && x && it.js !== undefined && x.js !== undefined && it.js === x.js) { o.items.splice(k, 1); return 1; }
        }
        return 0;
      },
      'clear()V': (vm, thr, o) => { o.items.length = 0; },
      'iterator()Ljava/util/Iterator;': (vm, thr, o) => {
        const it = vm.newObject(vm.requireClass('Ljava/util/Vector$Itr;'));
        it._vec = o; it._i = 0;
        return it;
      },
    },
  });

  vm.registerNative({
    desc: 'Ljava/util/LinkedList;',
    interfaces: ['Ljava/util/Queue;', 'Ljava/util/Collection;', 'Ljava/util/List;'],
    methods: {
      '<init>()V': (vm, thr, o) => { o.items = []; },
      'offer(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => { o.items.push(x); return 1; },
      'poll()Ljava/lang/Object;': (vm, thr, o) => o.items.length ? o.items.shift() : null,
      'peek()Ljava/lang/Object;': (vm, thr, o) => o.items.length ? o.items[0] : null,
      'add(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => { o.items.push(x); return 1; },
      'addFirst(Ljava/lang/Object;)V': (vm, thr, o, [x]) => { o.items.unshift(x); },
      'addLast(Ljava/lang/Object;)V': (vm, thr, o, [x]) => { o.items.push(x); },
      'removeFirst()Ljava/lang/Object;': (vm, thr, o) => {
        if (!o.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
        return o.items.shift();
      },
      'removeLast()Ljava/lang/Object;': (vm, thr, o) => {
        if (!o.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
        return o.items.pop();
      },
      'getFirst()Ljava/lang/Object;': (vm, thr, o) => {
        if (!o.items.length) vm.throwNew(thr, 'Ljava/util/NoSuchElementException;', '');
        return o.items[0];
      },
      'isEmpty()Z': (vm, thr, o) => o.items.length === 0 ? 1 : 0,
      'size()I': (vm, thr, o) => o.items.length,
      'clear()V': (vm, thr, o) => { o.items.length = 0; },
      'iterator()Ljava/util/Iterator;': (vm, thr, o) => {
        const it = vm.newObject(vm.requireClass('Ljava/util/Vector$Itr;'));
        it._vec = o; it._i = 0;
        return it;
      },
    },
  });

  /* java.util.Random — the EXACT JDK algorithm (48-bit LCG). The game saves     */
  /* and replays randomness-sensitive state, so bit-exactness matters.           */
  vm.registerNative({
    desc: 'Ljava/util/Random;',
    methods: {
      '<init>()V': (vm, thr, o) => {
        const seedUniq = BigInt(Math.floor(vm.now() * 1000) + (o.id * 8682522807148012 & 0xffffff));
        o._seed = (seedUniq ^ 0x5DEECE66Dn) & 0xffffffffffffn;
      },
      '<init>(J)V': (vm, thr, o, [seed]) => {
        o._seed = (seed ^ 0x5DEECE66Dn) & 0xffffffffffffn;
      },
      'setSeed(J)V': (vm, thr, o, [seed]) => {
        o._seed = (seed ^ 0x5DEECE66Dn) & 0xffffffffffffn;
      },
      'nextInt()I': (vm, thr, o) => javaRandomNext(vm, o, 32),
      'nextInt(I)I': (vm, thr, o, [bound]) => {
        if (bound <= 0) vm.throwNew(thr, 'Ljava/lang/IllegalArgumentException;', 'bound must be positive');
        if ((bound & -bound) === bound) {
          return ((bound * javaRandomNext(vm, o, 31)) >> 31) | 0;
        }
        let bits, val;
        do {
          bits = javaRandomNext(vm, o, 31);
          val = bits % bound;
        } while (((bits - val + (bound - 1)) | 0) < 0);
        return val | 0;
      },
      'nextBoolean()Z': (vm, thr, o) => javaRandomNext(vm, o, 1) !== 0 ? 1 : 0,
      'nextLong()J': (vm, thr, o) => {
        const hi = BigInt(javaRandomNext(vm, o, 32)) << 32n;
        const lo = BigInt(javaRandomNext(vm, o, 32)) & 0xffffffffn;
        return BigInt.asIntN(64, hi | lo);
      },
      'nextDouble()D': (vm, thr, o) => {
        const v = (BigInt(javaRandomNext(vm, o, 26)) << 27n) + BigInt(javaRandomNext(vm, o, 27));
        return Number(v) / 0x80000000000000;
      },
      'nextFloat()F': (vm, thr, o) => javaRandomNext(vm, o, 24) / 0x1000000,
      'nextBytes([B)V': (vm, thr, o, [b]) => {
        for (let i = 0; i < b.n; i++) {
          let r = javaRandomNext(vm, o, 32);
          b.a[i] = (r << 24) >> 24;
        }
      },
      'nextGaussian()D': (vm, thr, o) => {
        let v1, v2, s;
        do {
          v1 = 2 * (javaRandomNext(vm, o, 26) / 0x4000000) - 1;
          v2 = 2 * (javaRandomNext(vm, o, 26) / 0x4000000) - 1;
          s = v1 * v1 + v2 * v2;
        } while (s >= 1 || s === 0);
        const mult = Math.sqrt(-2 * Math.log(s) / s);
        o._nextGaussian = v2 * mult;
        return v1 * mult;
      },
    },
  });
  function javaRandomNext(vm, o, bits) {
    o._seed = (o._seed * 25214903917n + 11n) & 0xffffffffffffn;
    const shifted = o._seed >> BigInt(48 - bits);
    return bits === 32 ? Number(BigInt.asIntN(32, shifted)) | 0 : Number(shifted);
  }
  vm._randomNext = javaRandomNext;

  vm.registerNative({
    desc: 'Ljava/security/SecureRandom;',
    superDesc: 'Ljava/util/Random;',
    methods: {
      '<init>()V': (vm, thr, o) => {
        o._seed = (BigInt(Math.floor(Math.random() * 0xffffffffffff)) ^ 0x5DEECE66Dn) & 0xffffffffffffn;
      },
      'nextInt()I': (vm, thr, o) => (Math.random() * 0x100000000) | 0,
    },
  });

  /* ---------------- java.util.Date / Calendar / Locale ---------------- */
  vm.registerNative({
    desc: 'Ljava/util/Date;',
    methods: {
      '<init>()V': (vm, thr, o) => { o._t = Math.floor(vm.now()); },
      '<init>(J)V': (vm, thr, o, [t]) => { o._t = Number(t); },
      'getTime()J': (vm, thr, o) => BigInt(Math.floor(o._t)),
      'getYear()I': (vm, thr, o) => new Date(o._t).getFullYear() - 1900,
      'getMonth()I': (vm, thr, o) => new Date(o._t).getMonth(),
      'getDate()I': (vm, thr, o) => new Date(o._t).getDate(),
      'getDay()I': (vm, thr, o) => new Date(o._t).getDay(),
      'getHours()I': (vm, thr, o) => new Date(o._t).getHours(),
      'getMinutes()I': (vm, thr, o) => new Date(o._t).getMinutes(),
      'getSeconds()I': (vm, thr, o) => new Date(o._t).getSeconds(),
      'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(new Date(o._t).toString()),
      'after(Ljava/util/Date;)Z': (vm, thr, o, [d]) => o._t > d._t ? 1 : 0,
      'before(Ljava/util/Date;)Z': (vm, thr, o, [d]) => o._t < d._t ? 1 : 0,
      'equals(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => (x && x.c.desc === 'Ljava/util/Date;' && x._t === o._t) ? 1 : 0,
    },
  });

  vm.registerNative({
    desc: 'Ljava/util/Calendar;',
    sfields: [
      { name: 'YEAR', desc: 'I', value: 1 }, { name: 'MONTH', desc: 'I', value: 2 },
      { name: 'WEEK_OF_YEAR', desc: 'I', value: 3 }, { name: 'WEEK_OF_MONTH', desc: 'I', value: 4 },
      { name: 'DATE', desc: 'I', value: 5 }, { name: 'DAY_OF_MONTH', desc: 'I', value: 5 },
      { name: 'DAY_OF_YEAR', desc: 'I', value: 6 }, { name: 'DAY_OF_WEEK', desc: 'I', value: 7 },
      { name: 'AM_PM', desc: 'I', value: 9 }, { name: 'HOUR', desc: 'I', value: 10 },
      { name: 'HOUR_OF_DAY', desc: 'I', value: 11 }, { name: 'MINUTE', desc: 'I', value: 12 },
      { name: 'SECOND', desc: 'I', value: 13 }, { name: 'MILLISECOND', desc: 'I', value: 14 },
    ],
    methods: {
      '<init>()V': (vm, thr, o) => { o._t = Math.floor(vm.now()); },
      'getInstance()Ljava/util/Calendar;': (vm, thr) => {
        const c = vm.newObject(vm.requireClass('Ljava/util/Calendar;'));
        c._t = Math.floor(vm.now());
        return c;
      },
      'setTime(Ljava/util/Date;)V': (vm, thr, o, [d]) => { o._t = d._t; },
      'setTimeInMillis(J)V': (vm, thr, o, [t]) => { o._t = Number(t); },
      'getTimeInMillis()J': (vm, thr, o) => BigInt(Math.floor(o._t)),
      'getTime()Ljava/util/Date;': (vm, thr, o) => {
        const d = vm.newObject(vm.requireClass('Ljava/util/Date;'));
        d._t = o._t;
        return d;
      },
      'get(I)I': (vm, thr, o, [f]) => {
        const d = new Date(o._t);
        switch (f) {
          case 1: return d.getFullYear();
          case 2: return d.getMonth();
          case 3: return weekOfYear(d);
          case 4: return Math.floor((d.getDate() - 1) / 7) + 1;
          case 5: return d.getDate();
          case 6: return dayOfYear(d);
          case 7: return d.getDay() + 1;
          case 9: return d.getHours() < 12 ? 0 : 1;
          case 10: return d.getHours() % 12;
          case 11: return d.getHours();
          case 12: return d.getMinutes();
          case 13: return d.getSeconds();
          case 14: return d.getMilliseconds();
        }
        return 0;
      },
      'set(II)V': (vm, thr, o, [f, v]) => {
        const d = new Date(o._t);
        switch (f) {
          case 1: d.setFullYear(v); break;
          case 2: d.setMonth(v); break;
          case 5: d.setDate(v); break;
          case 10: d.setHours(o._ampmPM ? v + 12 : v); break;
          case 11: d.setHours(v); break;
          case 12: d.setMinutes(v); break;
          case 13: d.setSeconds(v); break;
          case 14: d.setMilliseconds(v); break;
        }
        o._t = d.getTime();
      },
    },
    staticSigs: new Set(['getInstance()Ljava/util/Calendar;']),
  });
  function weekOfYear(d) {
    const onejan = new Date(d.getFullYear(), 0, 1);
    return Math.ceil((((d - onejan) / 86400000) + onejan.getDay() + 1) / 7);
  }
  function dayOfYear(d) {
    return Math.floor((d - new Date(d.getFullYear(), 0, 0)) / 86400000);
  }

  const mkLocale = (vm, lang, country) => {
    const o = vm.newObject(vm.requireClass('Ljava/util/Locale;'));
    o._lang = lang; o._country = country;
    return o;
  };
  vm.registerNative({
    desc: 'Ljava/util/Locale;',
    sfields: [
      { name: 'JAPAN', desc: 'Ljava/util/Locale;' },
      { name: 'JAPANESE', desc: 'Ljava/util/Locale;' },
      { name: 'ENGLISH', desc: 'Ljava/util/Locale;' },
      { name: 'US', desc: 'Ljava/util/Locale;' },
    ],
    methods: {
      '<init>(Ljava/lang/String;Ljava/lang/String;)V': (vm, thr, o, [l, c]) => { o._lang = l ? l.js : ''; o._country = c ? c.js : ''; },
      'getDefault()Ljava/util/Locale;': (vm, thr) => mkLocale(vm, 'en', 'US'),
      'getLanguage()Ljava/lang/String;': (vm, thr, o) => vm.newString(o._lang || ''),
      'getCountry()Ljava/lang/String;': (vm, thr, o) => vm.newString(o._country || ''),
      'equals(Ljava/lang/Object;)Z': (vm, thr, o, [x]) => (x && x.c && x.c.desc === 'Ljava/util/Locale;' && x._lang === o._lang && x._country === o._country) ? 1 : 0,
      'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString((o._lang || '') + (o._country ? '_' + o._country : '')),
    },
    staticSigs: new Set(['getDefault()Ljava/util/Locale;']),
    clinit: (vm2, cls) => {
      const set = (name, l) => {
        const f = cls.sfields.find((x) => x.name === name);
        if (f) cls.statics[f.slot] = l;
      };
      set('JAPAN', mkLocale(vm2, 'ja', 'JP'));
      set('JAPANESE', mkLocale(vm2, 'ja', ''));
      set('ENGLISH', mkLocale(vm2, 'en', ''));
      set('US', mkLocale(vm2, 'en', 'US'));
    },
  });

  /* ---------------- java.util.regex.Pattern ---------------- */
  vm.registerNative({
    desc: 'Ljava/util/regex/Pattern;',
    methods: {
      'quote(Ljava/lang/String;)Ljava/lang/String;': (vm, thr, o, [s]) => {
        return vm.newString('\\Q' + s.js.replace(/\\E/g, '\\E\\\\E\\Q') + '\\E');
      },
    },
    staticSigs: new Set(['quote(Ljava/lang/String;)Ljava/lang/String;']),
  });

  /* ---------------- java.io ---------------- */
  vm.registerNative({ desc: 'Ljava/io/Closeable;', accessFlags: 0x0601, methods: {} });

  vm.registerNative({
    desc: 'Ljava/io/InputStream;',
    superDesc: 'Ljava/lang/Object;',
    interfaces: ['Ljava/io/Closeable;'],
    methods: {
      '<init>()V': () => { },
      'read()I': (vm, thr, o) => {
        const one = [0];
        const n = vm.call(thr, o, 'read([BII)I', [bytesToArr(vm, thr, one), 0, 1]);
        return n < 0 ? -1 : one[0] & 0xff;
      },
      'read([B)I': (vm, thr, o, [b]) => vm.call(thr, o, 'read([BII)I', [b, 0, b.n]),
      'read([BII)I': (vm, thr, o, [b, off, len]) => {
        if (o.a === undefined) return -1;
        if (o.pos >= o.n) return -1;
        const n = Math.min(len, o.n - o.pos);
        for (let i = 0; i < n; i++) b.a[off + i] = o.a[o.pos + i];
        o.pos += n;
        return n;
      },
      'available()I': (vm, thr, o) => o.a !== undefined ? Math.max(0, o.n - o.pos) : 0,
      'skip(J)J': (vm, thr, o, [n]) => {
        if (o.a === undefined) return 0n;
        const s = Math.min(Number(n), o.n - o.pos);
        o.pos += Math.max(0, s);
        return BigInt(Math.max(0, s));
      },
      'close()V': () => { },
      'markSupported()Z': () => 0,
    },
  });

  vm.registerNative({
    desc: 'Ljava/io/ByteArrayInputStream;',
    superDesc: 'Ljava/io/InputStream;',
    methods: {
      '<init>([B)V': (vm, thr, o, [b]) => { o.a = b.a.slice(); o.pos = 0; o.n = o.a.length; },
      '<init>([BII)V': (vm, thr, o, [b, off, len]) => { o.a = b.a.slice(off, off + len); o.pos = 0; o.n = o.a.length; },
      'read()I': (vm, thr, o) => {
        if (o.pos >= o.n) return -1;
        return o.a[o.pos++] & 0xff;
      },
      'read([BII)I': (vm, thr, o, [b, off, len]) => {
        if (o.pos >= o.n) return -1;
        const n = Math.min(len, o.n - o.pos);
        for (let i = 0; i < n; i++) b.a[off + i] = o.a[o.pos + i];
        o.pos += n;
        return n;
      },
      'available()I': (vm, thr, o) => Math.max(0, o.n - o.pos),
      'skip(J)J': (vm, thr, o, [n]) => {
        const s = Math.min(Number(n), o.n - o.pos);
        o.pos += Math.max(0, s);
        return BigInt(Math.max(0, s));
      },
      'close()V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Ljava/io/DataInputStream;',
    superDesc: 'Ljava/io/InputStream;',
    methods: {
      '<init>(Ljava/io/InputStream;)V': (vm, thr, o, [src]) => { o._in = src; },
      'read()I': (vm, thr, o) => vm.call(thr, o._in, 'read()I'),
      'read([BII)I': (vm, thr, o, [b, off, len]) => vm.call(thr, o._in, 'read([BII)I', [b, off, len]),
      'available()I': (vm, thr, o) => vm.call(thr, o._in, 'available()I'),
      'close()V': (vm, thr, o) => vm.call(thr, o._in, 'close()V'),
      'readByte()B': (vm, thr, o) => {
        const v = vm.call(thr, o._in, 'read()I');
        if (v < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
        return ((v & 0xff) << 24) >> 24;
      },
      'readShort()S': (vm, thr, o) => {
        const a = vm.call(thr, o._in, 'read()I');
        const b = vm.call(thr, o._in, 'read()I');
        if (a < 0 || b < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
        return (((a & 0xff) << 8) | (b & 0xff)) << 16 >> 16;
      },
      'readInt()I': (vm, thr, o) => {
        let v = 0;
        for (let i = 0; i < 4; i++) {
          const x = vm.call(thr, o._in, 'read()I');
          if (x < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
          v = (v << 8) | (x & 0xff);
        }
        return v | 0;
      },
      'readLong()J': (vm, thr, o) => {
        let v = 0n;
        for (let i = 0; i < 8; i++) {
          const x = vm.call(thr, o._in, 'read()I');
          if (x < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
          v = (v << 8n) | BigInt(x & 0xff);
        }
        return BigInt.asIntN(64, v);
      },
      'readUnsignedByte()I': (vm, thr, o) => {
        const v = vm.call(thr, o._in, 'read()I');
        if (v < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
        return v & 0xff;
      },
      'readUnsignedShort()I': (vm, thr, o) => {
        const a = vm.call(thr, o._in, 'read()I');
        const b = vm.call(thr, o._in, 'read()I');
        if (a < 0 || b < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
        return ((a & 0xff) << 8) | (b & 0xff);
      },
      'readBoolean()Z': (vm, thr, o) => {
        const v = vm.call(thr, o._in, 'read()I');
        if (v < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
        return v !== 0 ? 1 : 0;
      },
      'readUTF()Ljava/lang/String;': (vm, thr, o) => {
        const len = vm.call(thr, o, 'readUnsignedShort()I');
        const b = [];
        for (let i = 0; i < len; i++) b.push(vm.call(thr, o._in, 'read()I') & 0xff);
        return vm.newString(utf8Decode(b));
      },
      'readFully([B)V': (vm, thr, o, [b]) => vm.call(thr, o, 'readFully([BII)V', [b, 0, b.n]),
      'readFully([BII)V': (vm, thr, o, [b, off, len]) => {
        let got = 0;
        while (got < len) {
          const n = vm.call(thr, o._in, 'read([BII)I', [b, off + got, len - got]);
          if (n < 0) vm.throwNew(thr, 'Ljava/io/EOFException;', '');
          got += n;
        }
      },
      'skipBytes(I)I': (vm, thr, o, [n]) => Number(vm.call(thr, o._in, 'skip(J)J', [BigInt(n)])),
    },
  });
  vm.registerNative({
    desc: 'Ljava/io/DataInput;',
    accessFlags: 0x0601, methods: {},
  });
  vm.registerNative({
    desc: 'Ljava/io/DataOutput;',
    accessFlags: 0x0601, methods: {},
  });

  mkThrowable('Ljava/io/IOException;', 'Ljava/lang/Exception;');
  mkThrowable('Ljava/io/EOFException;', 'Ljava/io/IOException;');
  mkThrowable('Ljava/io/FileNotFoundException;', 'Ljava/io/IOException;');

  vm.registerNative({
    desc: 'Ljava/io/OutputStream;',
    interfaces: ['Ljava/io/Closeable;'],
    methods: {
      '<init>()V': () => { },
      'write(I)V': (vm, thr, o, [b]) => {
        const arr = vm.newArray('B', 1, thr);
        arr.a[0] = (b << 24) >> 24;
        vm.call(thr, o, 'write([B)V', [arr]);
      },
      'write([B)V': (vm, thr, o, [b]) => vm.call(thr, o, 'write([BII)V', [b, 0, b.n]),
      'write([BII)V': () => { },
      'flush()V': () => { },
      'close()V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Ljava/io/ByteArrayOutputStream;',
    superDesc: 'Ljava/io/OutputStream;',
    methods: {
      '<init>()V': (vm, thr, o) => { o.buf = []; },
      '<init>(I)V': (vm, thr, o) => { o.buf = []; },
      'write(I)V': (vm, thr, o, [b]) => { o.buf.push(b & 0xff); },
      'write([BII)V': (vm, thr, o, [b, off, len]) => {
        for (let i = 0; i < len; i++) o.buf.push(b.a[off + i] & 0xff);
      },
      'toByteArray()[B': (vm, thr, o) => {
        const arr = vm.newArray('B', o.buf.length, thr);
        arr.a = o.buf.slice();
        return arr;
      },
      'size()I': (vm, thr, o) => o.buf.length,
      'close()V': () => { },
      'flush()V': () => { },
      'reset()V': (vm, thr, o) => { o.buf.length = 0; },
    },
  });

  vm.registerNative({
    desc: 'Ljava/io/DataOutputStream;',
    superDesc: 'Ljava/io/OutputStream;',
    interfaces: ['Ljava/io/DataOutput;'],
    methods: {
      '<init>(Ljava/io/OutputStream;)V': (vm, thr, o, [src]) => { o._out = src; },
      'write(I)V': (vm, thr, o, [b]) => vm.call(thr, o._out, 'write(I)V', [b]),
      'write([B)V': (vm, thr, o, [b]) => vm.call(thr, o._out, 'write([B)V', [b]),
      'write([BII)V': (vm, thr, o, [b, off, len]) => vm.call(thr, o._out, 'write([BII)V', [b, off, len]),
      'flush()V': (vm, thr, o) => vm.call(thr, o._out, 'flush()V'),
      'close()V': (vm, thr, o) => vm.call(thr, o._out, 'close()V'),
      'writeByte(I)V': (vm, thr, o, [v]) => vm.call(thr, o._out, 'write(I)V', [v & 0xff]),
      'writeShort(I)V': (vm, thr, o, [v]) => {
        vm.call(thr, o._out, 'write(I)V', [(v >>> 8) & 0xff]);
        vm.call(thr, o._out, 'write(I)V', [v & 0xff]);
      },
      'writeInt(I)V': (vm, thr, o, [v]) => {
        for (let i = 3; i >= 0; i--) vm.call(thr, o._out, 'write(I)V', [(v >>> (8 * i)) & 0xff]);
      },
      'writeLong(J)V': (vm, thr, o, [v]) => {
        for (let i = 7n; i >= 0n; i--) {
          vm.call(thr, o._out, 'write(I)V', [Number((v >> (i * 8n)) & 0xffn)]);
        }
      },
      'writeBoolean(Z)V': (vm, thr, o, [v]) => vm.call(thr, o._out, 'write(I)V', [v ? 1 : 0]),
      'writeUTF(Ljava/lang/String;)V': (vm, thr, o, [s]) => {
        const bytes = utf8Encode(s.js);
        vm.call(thr, o, 'writeShort(I)V', [bytes.length]);
        for (const b of bytes) vm.call(thr, o._out, 'write(I)V', [b & 0xff]);
      },
    },
  });

  /* ---------------- java.io.File + streams (virtual FS via host) -------------- */
  vm.registerNative({
    desc: 'Ljava/io/File;',
    methods: {
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [p]) => { o.path = p ? p.js : ''; },
      '<init>(Ljava/lang/String;Ljava/lang/String;)V': (vm, thr, o, [dir, p]) => {
        o.path = (dir ? dir.js : '') + '/' + (p ? p.js : '');
      },
      '<init>(Ljava/io/File;Ljava/lang/String;)V': (vm, thr, o, [dir, p]) => {
        o.path = (dir ? dir.path : '') + '/' + (p ? p.js : '');
      },
      'getPath()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.path),
      'getAbsolutePath()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.path.startsWith('/') ? o.path : '/' + o.path),
      'getName()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.path.split('/').pop()),
      'getParent()Ljava/lang/String;': (vm, thr, o) => {
        const i = o.path.lastIndexOf('/');
        return vm.newString(i <= 0 ? '' : o.path.slice(0, i));
      },
      'exists()Z': (vm, thr, o) => vm.host.fsExists(o.path) ? 1 : 0,
      'isDirectory()Z': (vm, thr, o) => vm.host.fsIsDir(o.path) ? 1 : 0,
      'isFile()Z': (vm, thr, o) => vm.host.fsExists(o.path) && !vm.host.fsIsDir(o.path) ? 1 : 0,
      'mkdirs()Z': (vm, thr, o) => { vm.host.fsMkdirs(o.path); return 1; },
      'mkdir()Z': (vm, thr, o) => { vm.host.fsMkdirs(o.path); return 1; },
      'length()J': (vm, thr, o) => BigInt(vm.host.fsSize(o.path)),
      'delete()Z': (vm, thr, o) => { vm.host.fsDelete(o.path); return 1; },
      'createNewFile()Z': (vm, thr, o) => { vm.host.fsWrite(o.path, new Uint8Array(0)); return 1; },
      'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.path),
      'list()[Ljava/lang/String;': (vm, thr, o) => {
        const names = vm.host.fsList(o.path);
        const arr = vm.newArray('Ljava/lang/String;', names.length, thr);
        for (let i = 0; i < names.length; i++) arr.a[i] = vm.newString(names[i]);
        return arr;
      },
    },
  });

  vm.registerNative({
    desc: 'Ljava/io/FileInputStream;',
    superDesc: 'Ljava/io/InputStream;',
    methods: {
      '<init>(Ljava/io/File;)V': (vm, thr, o, [f]) => {
        const bytes = vm.host.fsRead(f.path);
        if (!bytes) vm.throwNew(thr, 'Ljava/io/FileNotFoundException;', f.path);
        o.a = Array.from(bytes); o.pos = 0; o.n = o.a.length;
      },
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [p]) => {
        const bytes = vm.host.fsRead(p ? p.js : '');
        if (!bytes) vm.throwNew(thr, 'Ljava/io/FileNotFoundException;', p ? p.js : '');
        o.a = Array.from(bytes); o.pos = 0; o.n = o.a.length;
      },
      'read()I': (vm, thr, o) => o.pos >= o.n ? -1 : (o.a[o.pos++] & 0xff),
      'read([B)I': (vm, thr, o, [b]) => {
        if (o.pos >= o.n) return -1;
        const n = Math.min(b.n, o.n - o.pos);
        for (let i = 0; i < n; i++) b.a[i] = o.a[o.pos + i];
        o.pos += n;
        return n;
      },
      'read([BII)I': (vm, thr, o, [b, off, len]) => {
        if (o.pos >= o.n) return -1;
        const n = Math.min(len, o.n - o.pos);
        for (let i = 0; i < n; i++) b.a[off + i] = o.a[o.pos + i];
        o.pos += n;
        return n;
      },
      'available()I': (vm, thr, o) => Math.max(0, o.n - o.pos),
      'close()V': () => { },
    },
  });

  vm.registerNative({
    desc: 'Ljava/io/FileOutputStream;',
    superDesc: 'Ljava/io/OutputStream;',
    methods: {
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [p]) => { o.path = p ? p.js : ''; o.buf = []; },
      '<init>(Ljava/lang/String;Z)V': (vm, thr, o, [p, append]) => {
        o.path = p ? p.js : '';
        o.buf = append ? Array.from(vm.host.fsRead(o.path) || []) : [];
      },
      '<init>(Ljava/io/File;)V': (vm, thr, o, [f]) => { o.path = f.path; o.buf = []; },
      '<init>(Ljava/io/File;Z)V': (vm, thr, o, [f, append]) => {
        o.path = f.path;
        o.buf = append ? Array.from(vm.host.fsRead(o.path) || []) : [];
      },
      'write(I)V': (vm, thr, o, [b]) => { o.buf.push(b & 0xff); },
      'write([B)V': (vm, thr, o, [b]) => { for (let i = 0; i < b.n; i++) o.buf.push(b.a[i] & 0xff); },
      'write([BII)V': (vm, thr, o, [b, off, len]) => {
        for (let i = 0; i < len; i++) o.buf.push(b.a[off + i] & 0xff);
      },
      'flush()V': (vm, thr, o) => { vm.host.fsWrite(o.path, Uint8Array.from(o.buf)); },
      'close()V': (vm, thr, o) => { vm.host.fsWrite(o.path, Uint8Array.from(o.buf)); },
    },
  });

  /* misc referenced platform bits */
  vm.registerNative({
    desc: 'Ljava/net/URLConnection;',
    methods: { '<init>()V': () => { } },
  });
  vm.registerNative({
    desc: 'Ljava/net/URL;',
    methods: {
      '<init>(Ljava/lang/String;)V': (vm, thr, o, [u]) => { o.url = u ? u.js : ''; },
      'openConnection()Ljava/net/URLConnection;': (vm, thr, o) => {
        const c = vm.newObject(vm.requireClass('Ljava/net/HttpURLConnection;'));
        c.url = o.url;
        return c;
      },
      'getPath()Ljava/lang/String;': (vm, thr, o) => {
        const m = o.url.match(/^[a-z]+:\/\/[^/]+(\/.*)?$/i);
        return vm.newString(m && m[1] ? m[1] : '');
      },
      'toString()Ljava/lang/String;': (vm, thr, o) => vm.newString(o.url),
    },
  });
  vm.registerNative({
    desc: 'Ljava/net/HttpURLConnection;',
    superDesc: 'Ljava/net/URLConnection;',
    methods: {
      '<init>()V': () => { },
      'connect()V': (vm, thr, o) => { vm.throwNew(thr, 'Ljava/io/IOException;', 'network unavailable in web port'); },
      'disconnect()V': () => { },
      'getContentLength()I': () => -1,
      'getInputStream()Ljava/io/InputStream;': (vm, thr, o) => { vm.throwNew(thr, 'Ljava/io/IOException;', 'network unavailable in web port'); },
      'getOutputStream()Ljava/io/OutputStream;': (vm, thr, o) => { vm.throwNew(thr, 'Ljava/io/IOException;', 'network unavailable in web port'); },
      'getResponseCode()I': () => 404,
      'setDoInput(Z)V': () => { },
      'setDoOutput(Z)V': () => { },
      'setRequestMethod(Ljava/lang/String;)V': (vm, thr, o, [m]) => { vm.throwNew(thr, 'Ljava/net/ProtocolException;', 'network unavailable in web port'); },
      'setRequestProperty(Ljava/lang/String;Ljava/lang/String;)V': () => { },
    },
  });
  mkThrowable('Ljava/net/ProtocolException;', 'Ljava/io/IOException;');

  /* security bits used only by the (dead) licensing validator */
  vm.registerNative({
    desc: 'Ljava/security/KeyFactory;',
    methods: {
      'getInstance(Ljava/lang/String;)Ljava/security/KeyFactory;': (vm, thr) => vm.newObject(vm.requireClass('Ljava/security/KeyFactory;')),
      'generatePublic(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;': (vm, thr, o, [spec]) => vm.newObject(vm.requireClass('Ljava/security/PublicKey;')),
    },
    staticSigs: new Set(['getInstance(Ljava/lang/String;)Ljava/security/KeyFactory;']),
  });
  vm.registerNative({ desc: 'Ljava/security/PublicKey;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({
    desc: 'Ljava/security/Signature;',
    methods: {
      'getInstance(Ljava/lang/String;)Ljava/security/Signature;': (vm, thr) => vm.newObject(vm.requireClass('Ljava/security/Signature;')),
      'initVerify(Ljava/security/PublicKey;)V': () => { },
      'update([B)V': () => { },
      'verify([B)Z': () => 1,    /* the embedded Google LICENSING key exists in the dex;
                                     we satisfy LVL locally by construction (see docs) */
    },
    staticSigs: new Set(['getInstance(Ljava/lang/String;)Ljava/security/Signature;']),
  });
  vm.registerNative({
    desc: 'Ljava/security/spec/X509EncodedKeySpec;',
    methods: { '<init>([B)V': () => { } },
  });
  vm.registerNative({ desc: 'Ljava/security/Key;', accessFlags: 0x0601, methods: {} });
  vm.registerNative({ desc: 'Ljava/security/spec/KeySpec;', accessFlags: 0x0601, methods: {} });
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
function ncIsWideDesc(d) { return d === "J" || d === "D"; }
const NC_WIDE2 = { _w: 1 };

function utf8Decode(bytes) {
  let s = '';
  const n = bytes.length;
  for (let i = 0; i < n;) {
    const b = bytes[i] & 0xff;
    if (b < 0x80) { s += String.fromCharCode(b); i += 1; }
    else if ((b & 0xe0) === 0xc0) { s += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i + 1] & 0x3f)); i += 2; }
    else if ((b & 0xf0) === 0xe0) { s += String.fromCharCode(((b & 0xf) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f)); i += 3; }
    else if ((b & 0xf8) === 0xf0) {
      let cp = ((b & 7) << 18) | ((bytes[i + 1] & 0x3f) << 12) | ((bytes[i + 2] & 0x3f) << 6) | (bytes[i + 3] & 0x3f);
      cp -= 0x10000;
      s += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
      i += 4;
    } else { i += 1; }
  }
  return s;
}
function utf8Encode(js) {
  const out = [];
  for (let i = 0; i < js.length; i++) {
    let c = js.charCodeAt(i);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    else if (c >= 0xd800 && c <= 0xdbff && i + 1 < js.length) {
      const c2 = js.charCodeAt(i + 1);
      const cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
      out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
      i++;
    } else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
  }
  return out;
}

function bytesToArr(vm, thr, bytesRef) {
  const arr = vm.newArray('B', bytesRef.length, thr);
  for (let i = 0; i < bytesRef.length; i++) arr.a[i] = ((bytesRef[i] & 0xff) << 24) >> 24;
  return arr;
}

function parseIntJava(vm, thr, s, radix) {
  if (typeof s !== 'string') vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', 'null');
  s = s.trim();
  // Java parseInt: optional sign, then digits in radix
  let i = 0, neg = false;
  if (s.startsWith('-')) { neg = true; i = 1; }
  else if (s.startsWith('+')) i = 1;
  if (i >= s.length) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
  let v = 0;
  let any = false;
  const digits = '0123456789abcdefghijklmnopqrstuvwxyz';
  for (; i < s.length; i++) {
    const d = digits.indexOf(s[i].toLowerCase());
    if (d < 0 || d >= radix) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
    v = v * radix + d;
    any = true;
  }
  if (!any) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
  let r = neg ? -v : v;
  if (r > 2147483647 || r < -2147483648) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
  return r | 0;
}

function parseLongJava(vm, thr, s) {
  if (typeof s !== 'string') vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', 'null');
  s = s.trim();
  let neg = false, i = 0;
  if (s.startsWith('-')) { neg = true; i = 1; }
  else if (s.startsWith('+')) i = 1;
  if (i >= s.length) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
  let v = 0n, any = false;
  for (; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 48 || c > 57) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
    v = v * 10n + BigInt(c - 48);
    any = true;
  }
  v = neg ? -v : v;
  if (v > 9223372036854775807n || v < -9223372036854775808n) vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
  return v;
}

function parseFloatJava(vm, thr, s) {
  if (typeof s !== 'string') vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', 'null');
  const t = s.trim();
  if (t === 'NaN') return NaN;
  if (t === 'Infinity' || t === '+Infinity') return Infinity;
  if (t === '-Infinity') return -Infinity;
  if (!/^[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?[fFdD]?$/.test(t)) {
    vm.throwNew(thr, 'Ljava/lang/NumberFormatException;', s);
  }
  return Math.fround(parseFloat(t));
}

/** Java Double.toString-ish for valueOf/append: integral values print as "5.0" */
function javaDoubleToString(x) {
  if (isNaN(x)) return 'NaN';
  if (x === Infinity) return 'Infinity';
  if (x === -Infinity) return '-Infinity';
  if (Number.isInteger(x) && Math.abs(x) < 1e21) return String(Math.trunc(x)) + '.0';
  return String(x);
}

/** printf subset for java.lang.String.format() (Kairosoft uses "%4d%02d%02d...") */
function javaFormat(vm, thr, fmt, args) {
  const argv = args && args.a ? args.a : [];
  let argi = 0;
  let out = '';
  let i = 0;
  while (i < fmt.length) {
    const c = fmt[i];
    if (c !== '%') { out += c; i++; continue; }
    if (fmt[i + 1] === '%') { out += '%'; i += 2; continue; }
    if (fmt[i + 1] === 'n') { out += '\n'; i += 2; continue; }
    // parse: % [flags] [width] [.precision] conversion
    let j = i + 1;
    let flags = '';
    while ('-0#+ ,'.includes(fmt[j]) && j < fmt.length) flags += fmt[j++];
    let width = '';
    while (fmt[j] >= '0' && fmt[j] <= '9') width += fmt[j++];
    let prec = null;
    if (fmt[j] === '.') {
      j++;
      let p = '';
      while (fmt[j] >= '0' && fmt[j] <= '9') p += fmt[j++];
      prec = parseInt(p || '0', 10);
    }
    const conv = fmt[j];
    j++;
    if (['d', 'x', 'X', 'o', 'c', 's', 'f', 'e', 'g', 'b', 'h', 'H'].indexOf(conv) < 0) {
      out += '%' + conv;
      i = j;
      continue;
    }
    let v = argv[argi++];
    let txt;
    const unbox = (x) => (x && x.v !== undefined) ? x.v : (x && x.js !== undefined ? x.js : x);
    switch (conv) {
      case 'd': {
        v = unbox(v);
        let s = typeof v === 'bigint' ? (v < 0 ? '-' : '') + (v < 0 ? -v : v).toString() : Math.abs(Math.trunc(v === undefined || v === null ? 0 : v)).toString();
        const neg = typeof v === 'bigint' ? v < 0 : v < 0;
        txt = padNum(s, neg, flags, width);
        break;
      }
      case 'x': case 'X': {
        v = unbox(v) | 0;
        let s = (v >>> 0).toString(16);
        txt = padNum(s, false, flags, width);
        if (conv === 'X') txt = txt.toUpperCase();
        break;
      }
      case 'f': {
        v = Number(unbox(v));
        txt = (v < 0 ? '-' : '') + Math.abs(v).toFixed(prec === null ? 6 : prec);
        if (flags.includes('0') && v < 0) txt = '-' + txt.slice(1).padStart(parseInt(width || '0', 10) - 1, '0');
        else if (width) txt = txt.padStart(parseInt(width, 10), flags.includes('0') ? '0' : ' ');
        break;
      }
      case 'c': txt = String.fromCharCode(unbox(v) | 0); break;
      case 's': {
        if (v === null || v === 0) txt = 'null';
        else if (v.js !== undefined) txt = v.js;
        else txt = String(unbox(v));
        break;
      }
      case 'b': txt = (v === null || v === 0 || v === false) ? 'false' : 'true'; break;
      default: txt = String(unbox(v));
    }
    if (conv !== 'd' && conv !== 'x' && conv !== 'X' && conv !== 'f' && width) {
      txt = txt.padStart(parseInt(width, 10), flags.includes('0') ? '0' : ' ');
      if (flags.includes('-')) txt = txt.padEnd(parseInt(width, 10), ' ');
    }
    out += txt;
    i = j;
  }
  return out;
}
function padNum(digits, neg, flags, width) {
  const w = parseInt(width || '0', 10);
  let body = digits;
  if (flags.includes('0') && w) {
    while (body.length + (neg ? 1 : 0) < w) body = '0' + body;
  }
  let s = (neg ? '-' : '') + body;
  if (w && !flags.includes('0')) s = s.padStart(w, ' ');
  return s;
}

if (typeof module !== 'undefined') module.exports = { installCoreNatives, utf8Encode, utf8Decode };
