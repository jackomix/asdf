/* ============================================================================
 * vm.js — In-browser Dalvik virtual machine.
 *
 * Executes the ORIGINAL Dalvik bytecode from classes.dex (Epic Astro Story,
 * net.kairosoft.android.frontier_en) directly in the browser. No game code is
 * re-implemented: every executed instruction is the game's own dx-emitted
 * machine code, loaded verbatim from the APK by js/dex.js.
 *
 *  - complete DEX opcode interpreter (all ~200 opcodes emitted by javac+dx
 *    for a targetSdk=4 build, incl. wide / switch / array-data payloads)
 *  - faithful JVM semantics: 32-bit int wrap, 64-bit long via BigInt,
 *    float32 rounding after every fp op, exact java.lang arithmetic rules,
 *    checked exceptions with proper handler unwind
 *  - unified class model: "platform" classes (java.* / android.* shims that a
 *    real device supplies from the framework /system framework jars) mix
 *    freely with game classes, including cross-boundary subclassing,
 *    overriding and interface dispatch
 *  - green threads: java.lang.Thread and android.os.Handler/Looper are
 *    emulated on a cooperative scheduler so the game's render thread, its
 *    sound-completions and UI events interleave deterministically
 * ========================================================================== */
'use strict';

/* ---------------------------------------------------------------- */
/* value representation                                             */
/* ---------------------------------------------------------------- */
const WIDE2 = { _w: 1 };                 // upper-half sentinel of wide values
/* IEEE-754 bit-punning helpers. Dalvik const ops load RAW bit patterns, so
 * float slots hold i32 bits and double slots hold i64 (BigInt) bits; the
 * arithmetic/conversion ops reinterpret them (float32/float64) at use time. */
const _bb = new ArrayBuffer(8);
const _f32v = new Float32Array(_bb);
const _f64v = new Float64Array(_bb);
const _i32v = new Int32Array(_bb);
const _b64v = new BigInt64Array(_bb);
const f2i = (f) => { _f32v[0] = f; return _i32v[0]; };                                    // float32 -> raw i32
const i2f = (i) => { _i32v[0] = i | 0; return _f32v[0]; };                                // raw i32 -> float32
const d2b = (d) => { _f64v[0] = d; return _b64v[0]; };                                    // float64 -> raw i64
const b2d = (b) => { if (typeof b === 'number') return b; _b64v[0] = b; return _f64v[0]; }; // raw i64 -> float64 (number passthrough for safety)
const isWideDesc = (d) => d === 'J' || d === 'D';
const ACC_STATIC = 0x0008, ACC_INTERFACE = 0x0200;

let VMCLASS_SEQ = 1;
let OBJ_SEQ = 1;

class VMClass {
  constructor(vm, desc, opts) {
    opts = opts || {};
    this.vm = vm;
    this.id = VMCLASS_SEQ++;
    this.desc = desc;
    this.simpleName = desc.length > 1 && desc[0] === 'L'
      ? desc.slice(1, -1).replace(/\//g, '.')
      : desc;
    this.superClass = null;
    this.superDesc = opts.superDesc || null;
    this.interfaces = [];
    this.accessFlags = opts.accessFlags || 1;
    this.isNative = !!opts.isNative;
    this.isInterface = (this.accessFlags & ACC_INTERFACE) !== 0;
    this.isArray = !!opts.isArray;
    this.elemDesc = opts.elemDesc || null;
    this.ifields = [];                       // flattened with inherited
    this.nInstanceSlots = 0;
    this.sfields = [];                       // own statics
    this.statics = [];                       // storage
    this.methods = [];                       // own MethodEntry[]
    this.sigMap = new Map();                 // copy-down vtable: sig -> entry
    this.clinit = null;
    this.initialized = false;
    this.initializing = false;
    this.nativeDef = opts.nativeDef || null;
  }
  toString() { return this.desc; }
}

class MethodEntry {
  constructor(vmClass, info) {
    this.vmClass = vmClass;
    this.name = info.name;
    this.proto = info.proto || null;
    this.desc = info.proto ? info.proto.desc : info.desc;
    this.sig = this.name + this.desc;
    this.accessFlags = info.accessFlags || 0;
    this.codeOff = info.codeOff || 0;
    this.native = info.native || null;
    this.code = null;
    this.insns = null;
    this.isStatic = (this.accessFlags & ACC_STATIC) !== 0;
    this.isCtor = this.name === '<init>';
    this.isClinit = this.name === '<clinit>';
    this.paramDescs = this.proto ? this.proto.params : (info.paramDescs || []);
    this.returnType = this.proto ? this.proto.returnType : (info.returnType || 'V');
  }
  fullName() { return this.vmClass.simpleName + '.' + this.sig; }
}

class VMThrow { constructor(exc) { this.exc = exc; } }
const throwVM = (o) => { throw new VMThrow(o); };

/* ---------------------------------------------------------------- */
/* Helpers for short/int16 reads from the instruction stream        */
/* ---------------------------------------------------------------- */
const _s16b = new Int16Array(1);
const toS16 = (u) => { _s16b[0] = u; return _s16b[0]; };

/* ================================================================== */
class VM {
  constructor(opts) {
    opts = opts || {};
    this.dex = null;
    this.classesByName = new Map();          // desc -> VMClass | {pending, dexDef}
    this.fieldCache = new Map();
    this.methodExactCache = new Map();
    this.interned = new Map();
    this.classObjects = new Map();
    this._dexStrCache = new Map();
    this.timeOffset = 0;
    this.stats = { insns: 0, invokes: 0, nativeInvokes: 0, objects: 0 };
    this.debug = !!opts.debug;
    this.threads = [];
    this.uiQueue = [];
    this.schedCursor = 0;
    this.virtualTime = null;                     // when set, clock is simulation ms
    this.now = () => this.virtualTime !== null ? this.virtualTime : (Date.now() + this.timeOffset);
    this.onLog = opts.onLog || ((...a) => console.log(...a));
    this.onError = opts.onError || ((...a) => console.error(...a));
    this.onUncaught = opts.onUncaught || null;
    this.mainThread = this._newVThread('main');
  }

  /* ---------------- dex loading & linkage ---------------- */
  loadDex(dexFile) {
    this.dex = dexFile;
    for (const cd of dexFile.classDefs) {
      const desc = dexFile.typeDesc(cd.classIdx);
      this.classesByName.set(desc, { pending: true, dexDef: cd, desc });
    }
    this.onLog('[vm] dex loaded: ' + dexFile.classDefsSize + ' classes (DEX v' + dexFile.version + ')');
  }

  findClass(desc) {
    if (!desc) return null;
    let c = this.classesByName.get(desc);
    if (c) return c.pending ? this._linkDexClass(c) : c;
    if (desc[0] === '[') {
      c = new VMClass(this, desc, { isArray: true, elemDesc: desc.slice(1), superDesc: 'Ljava/lang/Object;' });
      c.superClass = this.requireClass('Ljava/lang/Object;');
      for (const [k, v] of c.superClass.sigMap) c.sigMap.set(k, v);   // arrays look like Object (clone, toString, ...)
      c.initialized = true;
      this.classesByName.set(desc, c);
      return c;
    }
    return null;
  }
  requireClass(desc) {
    const c = this.findClass(desc);
    if (!c) throw new Error('[vm] unresolved class ' + desc);
    return c;
  }

  _linkDexClass(entry) {
    const cd = entry.dexDef;
    const dex = this.dex;
    const desc = entry.desc;
    const superDesc = cd.superclassIdx === 0xffffffff ? null : dex.typeDesc(cd.superclassIdx);
    const ifDescs = dex.typeListAt(cd.interfacesOff);
    const cls = new VMClass(this, desc, { accessFlags: cd.accessFlags, superDesc });
    this.classesByName.set(desc, cls);
    cls.superClass = superDesc ? this.requireClass(superDesc) : null;
    if (cls.superClass && cls.superClass._isStub) (cls.superClass._stubChildren ||= new Set()).add(cls);
    cls.interfaces = ifDescs.map((d) => this.requireClass(d));

    const data = dex.classData(cd);
    let slot = 0;
    if (cls.superClass) {
      for (const f of cls.superClass.ifields) cls.ifields.push(f);
      slot = cls.superClass.nInstanceSlots;
    }
    for (const f of data.instanceFields) {
      const wide = isWideDesc(f.desc);
      cls.ifields.push({ name: f.name, desc: f.desc, slot, holder: cls, access: f.accessFlags, isStatic: false });
      slot += wide ? 2 : 1;
    }
    cls.nInstanceSlots = slot;

    const svals = dex.staticValues(cd);
    data.staticFields.forEach((f, i) => {
      const wide = isWideDesc(f.desc);
      const fld = { name: f.name, desc: f.desc, slot: cls.statics.length, holder: cls, access: f.accessFlags, isStatic: true, wide };
      let v = (f.desc[0] === 'L' || f.desc[0] === '[') ? null : ((f.desc === 'J' || f.desc === 'D') ? 0n : 0);
      if (i < svals.length) v = this._matStatic(svals[i], f.desc);
      cls.sfields.push(fld);
      cls.statics.push(v);
      if (wide) cls.statics.push(WIDE2);
    });

    for (const m of [...data.directMethods, ...data.virtualMethods]) {
      const e = new MethodEntry(cls, m);
      cls.methods.push(e);
      if (e.isClinit) cls.clinit = e;
    }
    if (cls.superClass) for (const [k, v] of cls.superClass.sigMap) cls.sigMap.set(k, v);
    for (const m of cls.methods) cls.sigMap.set(m.sig, m);
    return cls;
  }

  _matStatic(v, desc) {
    if (v === null || v === undefined) return (desc && (desc[0] === 'L' || desc[0] === '[')) ? null : v;
    if (typeof v !== 'object') return v;
    if (v._str !== undefined) return this.newString(this.dex.string(v._str));
    if (v._type !== undefined) return this.classObject(this.dex.typeDesc(v._type));
    return null;
  }

  /* Auto-create a minimal stub native class so registerNative order doesn't
   * matter for super/interface references. If the real def is registered
   * later, registerNative upgrades the stub in place (same object identity). */
  _ensureNativeStub(desc) {
    let c = this.classesByName.get(desc);
    if (c && !c.pending) return c;
    if (c && c.pending) return this._linkDexClass(c);
    c = new VMClass(this, desc, { isNative: true, superDesc: desc === 'Ljava/lang/Object;' ? null : 'Ljava/lang/Object;' });
    c._isStub = true;
    this.classesByName.set(desc, c);
    if (c.superDesc === null) { c.superClass = null; c.ifields = []; c.nInstanceSlots = 0; }
    else {
      c.superClass = this._ensureNativeStub('Ljava/lang/Object;');
      c.ifields = [...c.superClass.ifields]; c.nInstanceSlots = c.superClass.nInstanceSlots;
      if (c.superClass._isStub) (c.superClass._stubChildren ||= new Set()).add(c);
    }
    for (const [k, v] of (c.superClass ? c.superClass.sigMap : [])) c.sigMap.set(k, v);
    c.initialized = true;
    return c;
  }

  /* Rebuild field layout + sig map of a class that was linked while its
   * superclass was still an auto-stub (registration-order artifact). */
  _repairChild(child) {
    const parent = child.superClass;
    const ownFields = child.ifields.filter((f) => f.holder === child);
    child.ifields = parent ? [...parent.ifields] : [];
    let slot = parent ? parent.nInstanceSlots : 0;
    for (const f of ownFields) {
      f.slot = slot;
      child.ifields.push(f);
      slot += isWideDesc(f.desc) ? 2 : 1;
    }
    child.nInstanceSlots = slot;
    const ownSigs = new Map();
    for (const m of child.methods) ownSigs.set(m.sig, m);
    child.sigMap = new Map();
    if (parent) for (const [k, v] of parent.sigMap) child.sigMap.set(k, v);
    for (const [k, v] of ownSigs) child.sigMap.set(k, v);
    if (child._stubChildren) for (const gc of [...child._stubChildren]) this._repairChild(gc);
  }

  registerNative(def) {
    let cls = this.classesByName.get(def.desc);
    if (cls && !cls.pending && cls._isStub) {
      // upgrade in place: reset all linkage state
      cls.accessFlags = def.accessFlags !== undefined ? def.accessFlags : 1;
      cls.nativeDef = def; cls.ifields = []; cls.sfields = []; cls.statics = [];
      cls.methods = []; cls.sigMap = new Map(); cls.clinit = null; cls._isStub = false;
    } else if (cls && !cls.pending && !cls._isStub) {
      // re-registration of a real class: keep identity, replace def
      cls.accessFlags = def.accessFlags !== undefined ? def.accessFlags : 1;
      cls.nativeDef = def; cls.ifields = []; cls.sfields = []; cls.statics = [];
      cls.methods = []; cls.sigMap = new Map(); cls.clinit = null;
    } else {
      cls = new VMClass(this, def.desc, {
        accessFlags: def.accessFlags !== undefined ? def.accessFlags : 1,
        isNative: true, nativeDef: def, superDesc: def.superDesc || null,
      });
      this.classesByName.set(def.desc, cls);
    }
    cls.isNative = true;
    const resolveRef = (d) => {
      const e = this.classesByName.get(d);
      if (e) return e.pending ? this._linkDexClass(e) : e;
      return this._ensureNativeStub(d);
    };
    cls.superClass = def.superDesc !== undefined
      ? (def.superDesc ? resolveRef(def.superDesc) : null)
      : (def.desc === 'Ljava/lang/Object;' ? null : resolveRef('Ljava/lang/Object;'));
    if (cls.superClass && cls.superClass._isStub) (cls.superClass._stubChildren ||= new Set()).add(cls);
    cls.interfaces = (def.interfaces || []).map((d) => resolveRef(d));

    let slot = 0;
    if (cls.superClass) {
      for (const f of cls.superClass.ifields) cls.ifields.push(f);
      slot = cls.superClass.nInstanceSlots;
    }
    for (const f of def.ifields || []) {
      const wide = isWideDesc(f.desc);
      cls.ifields.push({ name: f.name, desc: f.desc, slot, holder: cls, access: 0, isStatic: false });
      slot += wide ? 2 : 1;
    }
    cls.nInstanceSlots = slot;
    for (const f of def.sfields || []) {
      const wide = isWideDesc(f.desc);
      const fld = { name: f.name, desc: f.desc, slot: cls.statics.length, holder: cls, access: ACC_STATIC, isStatic: true, wide };
      let v = f.value;
      if (v === undefined) v = (f.desc[0] === 'L' || f.desc[0] === '[') ? null : ((f.desc === 'J' || f.desc === 'D') ? 0n : 0);
      cls.sfields.push(fld);
      cls.statics.push(v);
      if (wide) cls.statics.push(WIDE2);
    }
    const staticSigs = def.staticSigs || new Set();
    for (const sig in def.methods || {}) {
      const paren = sig.indexOf('(');
      const desc = sig.slice(paren);
      const e = new MethodEntry(cls, {
        name: sig.slice(0, paren),
        desc,
        paramDescs: VM.parseParams(desc),
        returnType: desc.slice(desc.lastIndexOf(')') + 1),
        accessFlags: staticSigs.has(sig) ? ACC_STATIC : (sig.startsWith('<') ? 0x10000 : 0),
        native: def.methods[sig],
      });
      cls.methods.push(e);
      if (e.isClinit) cls.clinit = e;
    }
    if (cls.superClass) for (const [k, v] of cls.superClass.sigMap) cls.sigMap.set(k, v);
    for (const m of cls.methods) cls.sigMap.set(m.sig, m);
    cls.initialized = true;
    if (def.clinit) def.clinit(this, cls);
    if (cls._stubChildren) for (const ch of [...cls._stubChildren]) this._repairChild(ch);
    return cls;
  }

  /* ---------------- init ---------------- */
  ensureInit(thr, cls) {
    if (cls.initialized || cls.initializing || cls.isArray) return;
    if (cls.superClass) this.ensureInit(thr, cls.superClass);
    if (cls.initialized || cls.initializing) return;
    cls.initializing = true;
    try {
      if (cls.clinit) this._execMethod(thr, cls.clinit, null, []);
    } finally {
      cls.initializing = false;
      cls.initialized = true;
    }
  }

  /* ---------------- objects/arrays/strings ---------------- */
  newObject(cls) {
    const n = cls.nInstanceSlots;
    const f = new Array(n);
    const ifs = cls.ifields;
    for (let i = 0; i < ifs.length; i++) {
      const d = ifs[i].desc, sl = ifs[i].slot;
      f[sl] = (d === 'J' || d === 'D') ? 0n : ((d[0] === 'L' || d[0] === '[') ? null : 0);
      if (isWideDesc(d)) f[sl + 1] = WIDE2;
    }
    this.stats.objects++;
    return { c: cls, f, id: OBJ_SEQ++ };
  }

  newArray(elemDesc, len, thr) {
    if (len < 0 || len === null || len === undefined) {
      this.throwNew(thr, 'Ljava/lang/NegativeArraySizeException;', String(len));
    }
    const cls = this.findClass('[' + elemDesc);
    const wide = isWideDesc(elemDesc);
    let a;
    if (wide) {
      a = new Array(len * 2);
      for (let i = 0; i < len; i++) { a[i * 2] = (elemDesc === 'J' || elemDesc === 'D') ? 0n : 0; a[i * 2 + 1] = WIDE2; }
    } else {
      a = new Array(len);
      if (elemDesc[0] === 'L' || elemDesc[0] === '[') a.fill(null); else a.fill(0);
    }
    return { c: cls, a, n: len, et: elemDesc, id: OBJ_SEQ++ };
  }

  newString(js) {
    if (js === null || js === undefined) return null;
    let s = this.interned.get(js);
    if (s) return s;
    s = this.newObject(this.requireClass('Ljava/lang/String;'));
    s.js = js;
    s.hash = 0;
    this.interned.set(js, s);
    return s;
  }
  jstr(o) { return o === null ? null : o.js; }
  describe(o) {
    if (o === null) return 'null';
    if (o.js !== undefined) return '"' + o.js + '"';
    return (o.c ? o.c.desc : '?') + '@' + o.id.toString(16);
  }

  classObject(typeDesc) {
    let o = this.classObjects.get(typeDesc);
    if (o) return o;
    o = this.newObject(this.requireClass('Ljava/lang/Class;'));
    o.typeDesc = typeDesc;
    o.vmType = null;
    this.classObjects.set(typeDesc, o);
    return o;
  }
  vmTypeOf(clsObj) {
    if (!clsObj.vmType) clsObj.vmType = this.findClass(clsObj.typeDesc) || null;
    return clsObj.vmType;
  }

  /* ---------------- exceptions ---------------- */
  newException(thr, desc, msg) {
    const cls = this.requireClass(desc);
    this.ensureInit(thr, cls);
    const o = this.newObject(cls);
    o.vmMsg = msg || null;
    o.vmTrace = thr ? thr.frames.map((f) => '        at ' + f.m.fullName() + (f.m && f.m.native ? '' : ' (off=0x' + (f.pc | 0).toString(16) + ')')).reverse() : [];
    o.vmTraceTruncated = false;
    const ctor = cls.sigMap.get(msg !== null && msg !== undefined ? '<init>(Ljava/lang/String;)V' : '<init>()V');
    if (ctor) {
      this._execMethod(thr, ctor, o, msg !== null && msg !== undefined ? [this.newString(msg)] : []);
    }
    return o;
  }
  throwNew(thr, desc, msg) { throwVM(this.newException(thr, desc, msg)); }

  /* ---------------- assignability ---------------- */
  isAssignable(from, to) {
    if (from === to) return true;
    if (!from || !to) return false;
    if (from.isArray) {
      const objCls = this.findClass('Ljava/lang/Object;');
      if (to === objCls) return true;
      if (to.isArray) {
        if (from.elemDesc === to.elemDesc) return true;
        const fe = from.elemDesc[0], te = to.elemDesc[0];
        if ((fe === 'L' || fe === '[') && (te === 'L' || te === '[')) {
          return this.isAssignable(this.requireClass(from.elemDesc), this.requireClass(to.elemDesc));
        }
        return false;
      }
      return to.desc === 'Ljava/lang/Cloneable;' || to.desc === 'Ljava/io/Serializable;';
    }
    if (to.isInterface) {
      const seen = new Set();
      const stack = [from];
      while (stack.length) {
        const x = stack.pop();
        if (!x || seen.has(x.id)) continue;
        seen.add(x.id);
        if (x === to) return true;
        for (const i of x.interfaces) if (!seen.has(i.id)) stack.push(i);
        if (x.superClass) stack.push(x.superClass);
      }
      return false;
    }
    let c = from;
    while (c) { if (c === to) return true; c = c.superClass; }
    return false;
  }

  /* ---------------- resolution ---------------- */
  _resolveField(refIdx) {
    let r = this.fieldCache.get(refIdx);
    if (r) return r;
    const ref = this.dex.fieldRef(refIdx);
    const holder = this.requireClass(ref.classDesc);
    let c = holder;
    while (c) {
      for (const f of c.sfields) {
        if (f.name === ref.name && f.desc === ref.typeDesc) {
          r = { holder: c, fld: f, isStatic: true, slot: f.slot };
          this.fieldCache.set(refIdx, r);
          return r;
        }
      }
      // own ifields of this c: those with holder === c
      for (const f of c.ifields) {
        if (f.holder === c && f.name === ref.name && f.desc === ref.typeDesc) {
          r = { holder: c, fld: f, isStatic: false, slot: f.slot };
          this.fieldCache.set(refIdx, r);
          return r;
        }
      }
      c = c.superClass;
    }
    throw new Error('[vm] NoSuchFieldError: ' + ref.key);
  }

  _resolveInvoke(thr, op, refIdx, callerMethod, receiver) {
    const ref = this.dex.methodRef(refIdx);
    const sig = ref.name + ref.proto.desc;
    const cls = this.requireClass(ref.classDesc);
    switch (op) {
      case 0x71: case 0x77: { // static
        this.ensureInit(thr, cls);
        let c = cls;
        while (c) {
          for (const m2 of c.methods) if (m2.sig === sig) return { m: m2, isStatic: true };
          c = c.superClass;
        }
        if (cls.nativeDef && cls.nativeDef.missing) return { m: cls.nativeDef.missing(this, sig, cls), isStatic: true };
        throw new Error('[vm] NoSuchMethodError (static): ' + ref.key);
      }
      case 0x70: case 0x76: { // direct: no vtable
        for (const m2 of cls.methods) if (m2.sig === sig) return { m: m2, isStatic: false };
        if (cls.nativeDef && cls.nativeDef.missing) return { m: cls.nativeDef.missing(this, sig, cls), isStatic: false };
        // walk supers for private-in-parent calls (rare, javac emits exact class)
        let c = cls.superClass;
        while (c) { for (const m2 of c.methods) if (m2.sig === sig) return { m: m2, isStatic: false }; c = c.superClass; }
        throw new Error('[vm] NoSuchMethodError (direct): ' + ref.key);
      }
      case 0x6e: case 0x74: { // virtual
        const m2 = this._vtableOf(receiver.c, sig, cls);
        if (m2) return { m: m2, isStatic: false };
        throw new Error('[vm] AbstractMethodError/NoSuchMethod (virtual): ' + ref.key + ' on ' + receiver.c.desc);
      }
      case 0x6f: case 0x75: { // super: direct dispatch starting at caller's super
        const callerSuper = callerMethod.vmClass.superClass;
        let c = callerSuper;
        while (c) {
          for (const m2 of c.methods) if (m2.sig === sig) return { m: m2, isStatic: false };
          c = c.superClass;
        }
        throw new Error('[vm] NoSuchMethodError (super): ' + ref.key);
      }
      case 0x72: case 0x78: { // interface
        const m2 = this._vtableOf(receiver.c, sig, cls);
        if (m2) return { m: m2, isStatic: false };
        throw new Error('[vm] NoSuchMethodError (interface): ' + ref.key + ' on ' + receiver.c.desc);
      }
    }
    throw new Error('[vm] bad invoke op 0x' + op.toString(16));
  }

  _vtableOf(runtimeCls, sig, declCls) {
    let m = runtimeCls.sigMap.get(sig);
    if (m && m.native !== undefined) return m;
    // search interfaces (default lookups up the interface hierarchy)
    if (!m) {
      const seen = new Set(); const st = [declCls];
      // breadth: class first already failed; then interfaces of runtime class chain
      let c = runtimeCls;
      while (c) { st.push(...c.interfaces); c = c.superClass; }
      while (st.length) {
        const i = st.shift();
        if (!i || seen.has(i.id)) continue;
        seen.add(i.id);
        m = i.sigMap.get(sig);
        if (m) return m;
        st.push(...i.interfaces);
      }
    }
    return m || null;
  }

  /* ---------------- public invoke helpers ---------------- */
  /* convert a semantic (JS-side) argument into raw slot form for a dex callee */
  _argToRaw(a, pd) {
    if (pd === 'F' && typeof a === 'number') return f2i(a);
    if (pd === 'D' && typeof a === 'number') return d2b(a);
    if (pd === 'J' && typeof a === 'number') return BigInt(Math.trunc(a));
    return a;
  }
  /* convert a raw slot result into its semantic (JS-side) form */
  _rawToRes(v, rt) {
    if (rt === 'F') return i2f(v);
    if (rt === 'D') return b2d(v);
    return v;
  }

  _execMethod(thr, m, obj, args) {
    if (m.native) {
      this.stats.nativeInvokes++;
      const rv = m.native(this, thr, obj, args);
      thr.res0 = rv === undefined ? 0 : rv;
      thr.res1 = isWideDesc(m.returnType) ? WIDE2 : undefined;
      return;
    }
    const code = this._methodCode(m);
    const regs = new Array(code.registersSize);
    let dst = code.registersSize - code.insSize;
    if (obj !== null && obj !== undefined) regs[dst++] = obj;
    if (args) {
      for (let i = 0; i < m.paramDescs.length; i++) {
        regs[dst] = this._argToRaw(args[i], m.paramDescs[i]);
        if (isWideDesc(m.paramDescs[i])) { regs[dst + 1] = WIDE2; dst += 2; }
        else dst++;
      }
    }
    const depth = thr.frames.length;
    thr.frames.push({ m, regs, pc: 0 });
    this._execLoop(thr, depth, 0);
  }

  invokeSync(thr, m, obj, args) {
    const s0 = thr.res0, s1 = thr.res1;
    this._execMethod(thr, m, obj, args || []);
    let r = thr.res0;
    if (m.returnType === 'F') r = i2f(r);
    else if (m.returnType === 'D') r = b2d(r);
    thr.res0 = s0; thr.res1 = s1;
    return r;
  }

  method(classDesc, sig) {
    const cls = this.requireClass(classDesc);
    const m = cls.sigMap.get(sig);
    if (m) return m;
    let c = cls;
    while (c) {
      for (const mm of c.methods) if (mm.sig === sig) return mm;
      c = c.superClass;
    }
    throw new Error('[vm] method not found: ' + classDesc + '->' + sig);
  }

  call(thr, obj, sig, args) {          // virtual call by sig — for natives/boot
    let m = obj.c.sigMap.get(sig);
    if (!m) {
      const stack = []; let c = obj.c;
      while (c) { stack.push(...c.interfaces); c = c.superClass; }
      while (stack.length) {
        const i = stack.pop();
        m = i.sigMap.get(sig);
        if (m) break;
        stack.push(...i.interfaces);
      }
    }
    if (!m) throw new Error('[vm] no virtual method ' + sig + ' on ' + obj.c.desc);
    return this.invokeSync(thr, m, obj, args || []);
  }

  /* ---------------- threads ---------------- */
  _newVThread(name) {
    const thr = {
      name, frames: [], res0: 0, res1: undefined,
      blockedUntil: 0, dead: false, exc: null, excInfo: null,
      entry: null, entryStarted: false,
    };
    this.threads.push(thr);
    return thr;
  }
  createThread(name, entry) { const t = this._newVThread(name); t.entry = entry; return t; }
  runOnUi(fn) { this.uiQueue.push(fn); }

  pump(maxInsns) {
    let remaining = maxInsns || 800000;
    const nowMs = this.now();
    // UI tasks on the main thread
    let guard = 0;
    while (this.uiQueue.length && guard++ < 100000) {
      const t = this.uiQueue.shift();
      try { if (typeof t === 'function') t(this.mainThread); else t.fn(this.mainThread); }
      catch (e) { this._reportUncaught(this.mainThread, e); }
    }
    // round-robin threads
    const n = this.threads.length;
    for (let i = 0; i < n && remaining > 0; i++) {
      this.schedCursor = (this.schedCursor + 1) % this.threads.length;
      const thr = this.threads[this.schedCursor];
      if (thr.dead) continue;
      /* The main thread is normally driven synchronously (uiQueue tasks /
       * vm.call deliveries) and has no business here — EXCEPT when a callee
       * on it blocked (Thread.yield/sleep) and _execLoop suspended with the
       * frame still on the stack. Only this round-robin can resume such a
       * frame once the block expires. A main thread with empty frames is
       * skipped as before. */
      if (thr === this.mainThread && !thr.frames.length) continue;
      if (thr.blockedUntil > nowMs) continue;
      const before = this.stats.insns;
      try {
        if (thr.entry && !thr.entryStarted) { thr.entryStarted = true; thr.entry(this, thr); }
        if (thr.frames.length) this._execLoop(thr, 0, remaining);
      } catch (e) {
        this._reportUncaught(thr, e);
      }
      remaining -= Math.max(1, this.stats.insns - before);
    }
    // reap
    for (let i = this.threads.length - 1; i >= 1; i--) {
      if (this.threads[i].dead) this.threads.splice(i, 1);
    }
  }

  _reportUncaught(thr, e) {
    thr.dead = true;
    if (e instanceof VMThrow) {
      const x = e.exc;
      const tr = (x.vmTrace || []).slice(0, 16).join('\n');
      this.onError('[vm] uncaught ' + x.c.desc + (x.vmMsg ? ': ' + x.vmMsg : '') + ' (thread ' + thr.name + ')\n' + tr);
      if (this.onUncaught) this.onUncaught(thr, x);
    } else {
      this.onError('[vm] interpreter error (thread ' + thr.name + '):', e && e.stack || e);
      throw e;
    }
  }

  _methodCode(m) {
    if (m.code) return m.code;
    if (!m.codeOff) throw new Error('[vm] method has no code: ' + m.fullName());
    const code = this.dex.codeItem(m.codeOff);
    m.code = code;
    m.insns = new Uint16Array(this.dex.u8.buffer, this.dex.u8.byteOffset + code.insnsOff, code.insnsSize);
    return code;
  }

  _findHandler(m, pcUnits, excCls) {
    const code = this._methodCode(m);
    for (let ti = 0; ti < code.tries.length; ti++) {
      const t = code.tries[ti];
      if (pcUnits < t.startAddr || pcUnits >= t.endAddr) continue;
      const h = t.handlers;
      for (const p of h.pairs) {
        const cls = this.requireClass(p.typeDesc);
        if (this.isAssignable(excCls, cls)) return p.addr;
      }
      if (h.catchAllAddr >= 0) return h.catchAllAddr;
    }
    return -1;
  }

  /* ==================== INTERPRETER CORE ==================== */
  _execLoop(thr, stopDepth, budget) {
    const frames = thr.frames;
    if (!frames.length) return;
    let fr = frames[frames.length - 1];
    let m = fr.m;
    if (!m.code) this._methodCode(m);
    let ins = m.insns;
    let regs = fr.regs;
    let pc = fr.pc;
    let tick = 0;

    const reload = () => { fr = frames[frames.length - 1]; m = fr.m; ins = m.insns; regs = fr.regs; };

    for (;;) {
      this.stats.insns++;
      if (++tick >= 2048) {
        tick = 0;
        if (thr.blockedUntil > this.now()) { fr.pc = pc; return; }
        if (budget) { budget -= 2048; if (budget <= 0) { fr.pc = pc; return; } }
      } else if (thr.blockedUntil > 0 && thr.blockedUntil <= this.now()) {
        thr.blockedUntil = 0;
      }

      let u0, op;
      try {
        u0 = ins[pc]; op = u0 & 0xff;
        const aa = u0 >>> 8;
        switch (op) {

          /* ---------------- move ---------------- */
          case 0x01: regs[aa & 0xf] = regs[aa >>> 4]; pc += 1; break;                                   // move
          case 0x02: regs[aa] = regs[ins[pc + 1]]; pc += 2; break;                                      // move/from16
          case 0x03: regs[ins[pc + 1]] = regs[ins[pc + 2]]; pc += 3; break;                             // move/16
          case 0x04: { const b = aa >>> 4; regs[aa & 0xf] = regs[b]; regs[(aa & 0xf) + 1] = regs[b + 1]; pc += 1; } break; // move-wide
          case 0x05: { const b = ins[pc + 1]; regs[aa] = regs[b]; regs[aa + 1] = regs[b + 1]; pc += 2; } break;
          case 0x06: { const a2 = ins[pc + 1], b = ins[pc + 2]; regs[a2] = regs[b]; regs[a2 + 1] = regs[b + 1]; pc += 3; } break;
          case 0x07: regs[aa & 0xf] = regs[aa >>> 4]; pc += 1; break;                                   // move-object
          case 0x08: regs[aa] = regs[ins[pc + 1]]; pc += 2; break;
          case 0x09: regs[ins[pc + 1]] = regs[ins[pc + 2]]; pc += 3; break;
          case 0x0a: regs[aa] = thr.res0; pc += 1; break;                                               // move-result
          case 0x0b: regs[aa] = thr.res0; regs[aa + 1] = WIDE2; pc += 1; break;                         // move-result-wide
          case 0x0c: regs[aa] = thr.res0; pc += 1; break;                                               // move-result-object
          case 0x0d: regs[aa] = thr.exc; thr.exc = null; pc += 1; break;                                // move-exception

          /* ---------------- return ---------------- */
          case 0x0e: case 0x0f: case 0x10: case 0x11: {
            if (op === 0x0e) { thr.res0 = 0; thr.res1 = undefined; }
            else if (op === 0x0f) { thr.res0 = regs[aa]; thr.res1 = undefined; }
            else if (op === 0x10) { thr.res0 = regs[aa]; thr.res1 = regs[aa + 1]; }
            else { thr.res0 = regs[aa]; thr.res1 = undefined; }
            frames.pop();
            if (frames.length <= stopDepth) {
              if (!frames.length && thr !== this.mainThread) thr.dead = true;
              return;
            }
            reload(); pc = fr.pc;
          } continue;

          /* ---------------- const ---------------- */
          case 0x12: regs[aa & 0xf] = ((aa & 0xf0) << 24) >> 28; pc += 1; break;                        // const/4
          case 0x13: regs[aa] = toS16(ins[pc + 1]); pc += 2; break;                                     // const/16
          case 0x14: regs[aa] = (ins[pc + 1] | (ins[pc + 2] << 16)); pc += 3; break;                    // const
          case 0x15: regs[aa] = toS16(ins[pc + 1]) << 16; pc += 2; break;                               // const/high16
          case 0x16: regs[aa] = BigInt(toS16(ins[pc + 1])); regs[aa + 1] = WIDE2; pc += 2; break;       // const-wide/16
          case 0x17: regs[aa] = BigInt.asIntN(64, BigInt(ins[pc + 1] | (ins[pc + 2] << 16))); regs[aa + 1] = WIDE2; pc += 3; break;
          case 0x18: {                                                                                  // const-wide
            let v = 0n;
            for (let i = 0; i < 4; i++) v |= BigInt(ins[pc + 1 + i]) << BigInt(16 * i);
            regs[aa] = BigInt.asIntN(64, v); regs[aa + 1] = WIDE2; pc += 5;
          } break;
          case 0x19: regs[aa] = BigInt.asIntN(64, BigInt(toS16(ins[pc + 1])) << 48n); regs[aa + 1] = WIDE2; pc += 2; break;
          case 0x1a: regs[aa] = this._constString(ins[pc + 1]); pc += 2; break;                         // const-string
          case 0x1b: regs[aa] = this._constString((ins[pc + 1] | (ins[pc + 2] << 16)) >>> 0); pc += 3; break;
          case 0x1c: regs[aa] = this.classObject(this.dex.typeDesc(ins[pc + 1])); pc += 2; break;       // const-class

          /* ---------------- monitors / casts ---------------- */
          case 0x1d: pc += 1; break;                                                                    // monitor-enter
          case 0x1e: pc += 1; break;                                                                    // monitor-exit
          case 0x1f: {                                                                                  // check-cast
            const o = regs[aa];
            if (o !== null && o !== 0) {
              const t = this.requireClass(this.dex.typeDesc(ins[pc + 1]));
              if (!this.isAssignable(o.c, t)) {
                this.throwNew(thr, 'Ljava/lang/ClassCastException;', o.c.simpleName + ' cannot be cast to ' + t.simpleName);
              }
            }
            pc += 2;
          } break;
          case 0x20: {                                                                                  // instance-of
            const o = regs[(aa >>> 4) & 0xf];
            if (o === null || o === 0) regs[aa & 0xf] = 0;
            else {
              const t = this.requireClass(this.dex.typeDesc(ins[pc + 1]));
              regs[aa & 0xf] = this.isAssignable(o.c, t) ? 1 : 0;
            }
            pc += 2;
          } break;
          case 0x21: {                                                                                  // array-length
            const o = regs[(aa >>> 4) & 0xf];
            if (o === null || o === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            regs[aa & 0xf] = o.n;
            pc += 1;
          } break;
          case 0x22: {                                                                                  // new-instance
            const t = this.requireClass(this.dex.typeDesc(ins[pc + 1]));
            this.ensureInit(thr, t);
            regs[aa] = this.newObject(t);
            reload(); pc += 2;
          } break;
          case 0x23: {                                                                                  // new-array
            const ed = this.dex.typeDesc(ins[pc + 1]).slice(1);
            const len = regs[(aa >>> 4) & 0xf];
            regs[aa & 0xf] = this.newArray(ed, len, thr);
            pc += 2;
          } break;
          case 0x24: {                                                                                  // filled-new-array
            const cnt = aa >>> 4;
            const g = aa & 0xf;
            const u2 = ins[pc + 2];
            const ids = [u2 & 0xf, (u2 >>> 4) & 0xf, (u2 >>> 8) & 0xf, (u2 >>> 12) & 0xf, g];
            const et = this.dex.typeDesc(ins[pc + 1]).slice(1);
            const arr = this.newArray(et, cnt, thr);
            if (et[0] === 'L' || et[0] === '[') for (let i = 0; i < cnt; i++) { const v = regs[ids[i]]; arr.a[i] = (v === 0) ? null : v; }
            else for (let i = 0; i < cnt; i++) arr.a[i] = regs[ids[i]];
            thr.res0 = arr; thr.res1 = undefined;
            pc += 3;
          } break;
          case 0x25: {                                                                                  // filled-new-array/range
            const cnt = aa, start = ins[pc + 2];
            const et = this.dex.typeDesc(ins[pc + 1]).slice(1);
            const arr = this.newArray(et, cnt, thr);
            if (et[0] === 'L' || et[0] === '[') for (let i = 0; i < cnt; i++) { const v = regs[start + i]; arr.a[i] = (v === 0) ? null : v; }
            else for (let i = 0; i < cnt; i++) arr.a[i] = regs[start + i];
            thr.res0 = arr; thr.res1 = undefined;
            pc += 3;
          } break;
          case 0x26: {                                                                                  // fill-array-data
            const off = (ins[pc + 1] | (ins[pc + 2] << 16));
            const arr = regs[aa];
            if (arr === null) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            const po = pc + off;
            const width = ins[po + 1];
            const size = (ins[po + 2] | (ins[po + 3] << 16)) >>> 0;
            if (size > arr.n) this.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', 'length=' + arr.n + '; needed=' + size);
            const dv = this.dex.r.dv;
            const byteBase = this.dex.u8.byteOffset + fr.m.code.insnsOff + (po + 4) * 2;
            if (width === 1) for (let i = 0; i < size; i++) arr.a[i] = dv.getInt8(byteBase + i);
            else if (width === 2) for (let i = 0; i < size; i++) arr.a[i] = dv.getInt16(byteBase + i * 2, true);
            else if (width === 4) for (let i = 0; i < size; i++) arr.a[i] = dv.getInt32(byteBase + i * 4, true);
            else for (let i = 0; i < size; i++) {
              const lo = BigInt(dv.getUint32(byteBase + i * 8, true));
              const hi = BigInt(dv.getInt32(byteBase + i * 8 + 4, true));
              arr.a[i * 2] = BigInt.asIntN(64, (hi << 32n) | lo);
              arr.a[i * 2 + 1] = WIDE2;
            }
            pc += 3;
          } break;
          case 0x27: {                                                                                  // throw
            const o = regs[aa];
            if (o === null || o === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            throwVM(o);
          }
          case 0x28: pc += ((aa << 24) >> 24); break;                                                   // goto
          case 0x29: pc += toS16(ins[pc + 1]); break;                                                   // goto/16
          case 0x2a: pc += (ins[pc + 1] | (ins[pc + 2] << 16)); break;                                  // goto/32
          case 0x2b: {                                                                                  // packed-switch
            const off = ins[pc + 1] | (ins[pc + 2] << 16);
            const po = pc + off;
            const size = ins[po + 1];
            const firstKey = ins[po + 2] | (ins[po + 3] << 16);
            const idx = regs[aa] - firstKey;
            pc += (idx >= 0 && idx < size)
              ? (ins[po + 4 + idx * 2] | (ins[po + 5 + idx * 2] << 16))
              : 3;
          } break;
          case 0x2c: {                                                                                  // sparse-switch
            const off = ins[pc + 1] | (ins[pc + 2] << 16);
            const po = pc + off;
            const size = ins[po + 1];
            const key = regs[aa];
            let hit = -1;
            for (let i = 0; i < size; i++) {
              const k = ins[po + 2 + i * 2] | (ins[po + 3 + i * 2] << 16);
              if (k === key) { hit = i; break; }
              /* keys are signed & sorted ascending — stop once past the target */
              if (key >= 0 && k > key) break;
            }
            pc += hit >= 0
              ? (ins[po + 2 + size * 2 + hit * 2] | (ins[po + 3 + size * 2 + hit * 2] << 16))
              : 3;
          } break;

          /* ---------------- compare (23x, len 2) ---------------- */
          case 0x2d: {                                                                                  // cmpl-float
            const u1 = ins[pc + 1], v1 = i2f(regs[u1 & 0xff]), v2 = i2f(regs[u1 >>> 8]);
            regs[aa] = (isNaN(v1) || isNaN(v2)) ? -1 : v1 < v2 ? -1 : v1 > v2 ? 1 : (v1 === v2 ? 0 : -1);
            pc += 2;
          } break;
          case 0x2e: {                                                                                  // cmpg-float
            const u1 = ins[pc + 1], v1 = i2f(regs[u1 & 0xff]), v2 = i2f(regs[u1 >>> 8]);
            regs[aa] = (isNaN(v1) || isNaN(v2)) ? 1 : v1 < v2 ? -1 : v1 > v2 ? 1 : (v1 === v2 ? 0 : 1);
            pc += 2;
          } break;
          case 0x2f: {                                                                                  // cmpl-double
            const u1 = ins[pc + 1], v1 = b2d(regs[u1 & 0xff]), v2 = b2d(regs[u1 >>> 8]);
            regs[aa] = (isNaN(v1) || isNaN(v2)) ? -1 : v1 < v2 ? -1 : v1 > v2 ? 1 : (v1 === v2 ? 0 : -1);
            pc += 2;
          } break;
          case 0x30: {                                                                                  // cmpg-double
            const u1 = ins[pc + 1], v1 = b2d(regs[u1 & 0xff]), v2 = b2d(regs[u1 >>> 8]);
            regs[aa] = (isNaN(v1) || isNaN(v2)) ? 1 : v1 < v2 ? -1 : v1 > v2 ? 1 : (v1 === v2 ? 0 : 1);
            pc += 2;
          } break;
          case 0x31: {                                                                                  // cmp-long
            const u1 = ins[pc + 1], v1 = regs[u1 & 0xff], v2 = regs[u1 >>> 8];
            regs[aa] = v1 < v2 ? -1 : v1 > v2 ? 1 : 0;
            pc += 2;
          } break;

          /* ---------------- if ---------------- */
          case 0x32: pc += (regs[aa & 0xf] === regs[aa >>> 4]) ? toS16(ins[pc + 1]) : 2; break;         // if-eq
          case 0x33: pc += (regs[aa & 0xf] !== regs[aa >>> 4]) ? toS16(ins[pc + 1]) : 2; break;         // if-ne
          case 0x34: pc += (regs[aa & 0xf] < regs[aa >>> 4]) ? toS16(ins[pc + 1]) : 2; break;           // if-lt
          case 0x35: pc += (regs[aa & 0xf] >= regs[aa >>> 4]) ? toS16(ins[pc + 1]) : 2; break;          // if-ge
          case 0x36: pc += (regs[aa & 0xf] > regs[aa >>> 4]) ? toS16(ins[pc + 1]) : 2; break;           // if-gt
          case 0x37: pc += (regs[aa & 0xf] <= regs[aa >>> 4]) ? toS16(ins[pc + 1]) : 2; break;          // if-le
          case 0x38: pc += (regs[aa] === 0 || regs[aa] === null) ? toS16(ins[pc + 1]) : 2; break;       // if-eqz
          case 0x39: pc += (regs[aa] !== 0 && regs[aa] !== null) ? toS16(ins[pc + 1]) : 2; break;       // if-nez
          case 0x3a: pc += (regs[aa] < 0) ? toS16(ins[pc + 1]) : 2; break;                              // if-ltz
          case 0x3b: pc += (regs[aa] >= 0) ? toS16(ins[pc + 1]) : 2; break;                             // if-gez
          case 0x3c: pc += (regs[aa] > 0) ? toS16(ins[pc + 1]) : 2; break;                              // if-gtz
          case 0x3d: pc += (regs[aa] <= 0) ? toS16(ins[pc + 1]) : 2; break;                             // if-lez

          /* ---------------- aget 23x: vAA, [vBB=C, idx] ---------------- */
          case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: {
            const u1 = ins[pc + 1], o = regs[u1 & 0xff], i = regs[u1 >>> 8];
            if (o === null || o === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            if (i < 0 || i >= o.n) this.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', 'length=' + o.n + '; index=' + i);
            if (op === 0x45) { regs[aa] = o.a[i * 2]; regs[aa + 1] = WIDE2; }                            // wide
            else if (op === 0x48) regs[aa] = (o.a[i] << 24) >> 24;                                       // byte
            else if (op === 0x49) regs[aa] = o.a[i] & 0xffff;                                            // char
            else if (op === 0x4a) regs[aa] = (o.a[i] << 16) >> 16;                                       // short
            else regs[aa] = o.a[i];
            pc += 2;
          } break;
          /* ---------------- aput ---------------- */
          case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {
            const u1 = ins[pc + 1], o = regs[u1 & 0xff], i = regs[u1 >>> 8];
            if (o === null || o === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            if (i < 0 || i >= o.n) this.throwNew(thr, 'Ljava/lang/ArrayIndexOutOfBoundsException;', 'length=' + o.n + '; index=' + i);
            const v = regs[aa];
            if (op === 0x4c) { o.a[i * 2] = v; o.a[i * 2 + 1] = WIDE2; }                                 // wide
            else if (op === 0x4d) {                                                                      // object
              const vv = (v === 0) ? null : v;
              o.a[i] = vv;
              if (vv !== null && o.et !== 'Ljava/lang/Object;') {
                const et = o.et;
                if (et[0] === 'L' || et[0] === '[') {
                  if (!this.isAssignable(vv.c, this.requireClass(et))) {
                    this.throwNew(thr, 'Ljava/lang/ArrayStoreException;', vv.c.desc);
                  }
                }
              }
            }
            else if (op === 0x4f) o.a[i] = (v << 24) >> 24;                                              // byte(+boolean→byte path)
            else if (op === 0x50) o.a[i] = v & 0xffff;                                                   // char
            else if (op === 0x51) o.a[i] = (v << 16) >> 16;                                              // short
            else o.a[i] = v | 0;                                                                         // int / boolean
            pc += 2;
          } break;

          /* ---------------- iget / iput (22c) ---------------- */
          case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: {
            const r = this._resolveField(ins[pc + 1]);
            const o = regs[(aa >>> 4) & 0xf];
            if (o === null || o === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            regs[aa & 0xf] = o.f[r.slot];
            if (op === 0x53) regs[(aa & 0xf) + 1] = WIDE2;                                               // iget-wide
            pc += 2;
          } break;
          case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
            const r = this._resolveField(ins[pc + 1]);
            const o = regs[(aa >>> 4) & 0xf];
            if (o === null || o === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            let _v = regs[aa & 0xf];
            if (op === 0x5b && _v === 0) _v = null;                                                      // iput-object null normalize
            o.f[r.slot] = _v;
            if (op === 0x5a) o.f[r.slot + 1] = WIDE2;                                                    // iput-wide
            pc += 2;
          } break;
          /* ---------------- sget / sput (21c) ---------------- */
          case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: {
            const r = this._resolveField(ins[pc + 1]);
            this.ensureInit(thr, r.holder);
            reload();
            regs[aa] = r.holder.statics[r.slot];
            if (op === 0x61) regs[aa + 1] = WIDE2;                                                       // sget-wide
            pc += 2;
          } break;
          case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: {
            const r = this._resolveField(ins[pc + 1]);
            this.ensureInit(thr, r.holder);
            reload();
            let _v = regs[aa];
            if (op === 0x69 && _v === 0) _v = null;                                                      // sput-object null normalize
            r.holder.statics[r.slot] = _v;
            if (op === 0x68) r.holder.statics[r.slot + 1] = WIDE2;                                       // sput-wide
            pc += 2;
          } break;

          /* ---------------- invoke ---------------- */
          case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
          case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
            const range = op >= 0x74;
            const refIdx = ins[pc + 1];
            let argRegs, receiver = null;
            let count;
            if (range) {
              count = aa;
              const start = ins[pc + 2];
              argRegs = new Array(count);
              for (let i = 0; i < count; i++) argRegs[i] = regs[start + i];
            } else {
              count = aa >>> 4;
              const g = aa & 0xf, u2 = ins[pc + 2];
              const ids = [u2 & 0xf, (u2 >>> 4) & 0xf, (u2 >>> 8) & 0xf, (u2 >>> 12) & 0xf, g];
              argRegs = new Array(count);
              for (let i = 0; i < count; i++) argRegs[i] = regs[ids[i]];
            }
            if (op !== 0x71 && op !== 0x77) {
              receiver = argRegs[0];
              if (receiver === null || receiver === 0) this.throwNew(thr, 'Ljava/lang/NullPointerException;', '');
            }
            const rs = this._resolveInvoke(thr, op, refIdx, fr.m, receiver);
            const target = rs.m;
            const firstArg = rs.isStatic ? 0 : 1;

            if (target.native) {
              // natives receive one JS value per declared parameter;
              // F/D params are reinterpreted from raw slot bits to JS numbers
              const nArgs = [];
              let ai2 = firstArg;
              for (const pd of target.paramDescs) {
                let v = argRegs[ai2];
                if (pd === 'F') v = i2f(v);
                else if (pd === 'D') v = b2d(v);
                nArgs.push(v);
                ai2 += isWideDesc(pd) ? 2 : 1;
              }
              this.stats.nativeInvokes++;
              fr.pc = pc + 3;                                   // commit before native runs
              let rv = target.native(this, thr, receiver, nArgs);
              reload();
              // native results come as JS values; store raw slot form
              if (target.returnType === 'F') rv = f2i(rv === undefined ? 0 : rv);
              else if (target.returnType === 'D') rv = d2b(rv === undefined ? 0 : rv);
              else if (target.returnType === 'J') rv = rv === undefined ? 0n : (typeof rv === 'bigint' ? rv : BigInt(Math.trunc(rv)));
              else if (rv !== null && typeof rv === 'number' && 'IBSCZ'.indexOf(target.returnType) >= 0) rv = rv | 0;
              thr.res0 = rv === undefined ? 0 : rv;
              thr.res1 = isWideDesc(target.returnType) ? WIDE2 : undefined;
              if (thr.blockedUntil > this.now()) { fr.pc = pc + 3; return; }
              pc += 3;
            } else {
              this.stats.invokes++;
              const code2 = this._methodCode(target);
              fr.pc = pc + 3;
              const regs2 = new Array(code2.registersSize);
              let dst = code2.registersSize - code2.insSize;
              let ai2 = firstArg;
              if (!rs.isStatic) regs2[dst++] = receiver;
              for (const pd of target.paramDescs) {
                regs2[dst] = argRegs[ai2++];                    // low word carries the value
                if (isWideDesc(pd)) { ai2++; regs2[dst + 1] = WIDE2; dst += 2; }
                else dst++;
              }
              frames.push({ m: target, regs: regs2, pc: 0 });
              reload(); pc = 0;
            }
          } continue;

          /* ---------------- unary ops (12x, A|B) ---------------- */
          case 0x7b: regs[aa & 0xf] = (-regs[aa >>> 4]) | 0; pc += 1; break;                            // neg-int
          case 0x7c: regs[aa & 0xf] = ~regs[aa >>> 4]; pc += 1; break;                                  // not-int
          case 0x7d: regs[aa & 0xf] = -regs[aa >>> 4]; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;    // neg-long
          case 0x7e: regs[aa & 0xf] = ~regs[aa >>> 4]; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;    // not-long
          case 0x7f: regs[aa & 0xf] = f2i(-i2f(regs[aa >>> 4])); pc += 1; break;                        // neg-float
          case 0x80: regs[aa & 0xf] = d2b(-b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;    // neg-double
          case 0x81: regs[aa & 0xf] = BigInt(regs[aa >>> 4] | 0); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break; // int-to-long
          case 0x82: regs[aa & 0xf] = f2i(regs[aa >>> 4] | 0); pc += 1; break;                          // int-to-float
          case 0x83: regs[aa & 0xf] = d2b(regs[aa >>> 4] | 0); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;     // int-to-double
          case 0x84: regs[aa & 0xf] = Number(BigInt.asIntN(32, regs[aa >>> 4])) | 0; pc += 1; break;    // long-to-int
          case 0x85: regs[aa & 0xf] = f2i(Number(regs[aa >>> 4])); pc += 1; break;                      // long-to-float
          case 0x86: regs[aa & 0xf] = d2b(Number(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;   // long-to-double
          case 0x87: regs[aa & 0xf] = VM._d2i(i2f(regs[aa >>> 4])); pc += 1; break;                     // float-to-int
          case 0x88: regs[aa & 0xf] = VM._d2l(i2f(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;  // float-to-long
          case 0x89: regs[aa & 0xf] = d2b(i2f(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;     // float-to-double
          case 0x8a: regs[aa & 0xf] = VM._d2i(b2d(regs[aa >>> 4])); pc += 1; break;                     // double-to-int
          case 0x8b: regs[aa & 0xf] = VM._d2l(b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;  // double-to-long
          case 0x8c: regs[aa & 0xf] = f2i(b2d(regs[aa >>> 4])); pc += 1; break;                         // double-to-float
          case 0x8d: regs[aa & 0xf] = (regs[aa >>> 4] << 24) >> 24; pc += 1; break;                     // int-to-byte
          case 0x8e: regs[aa & 0xf] = regs[aa >>> 4] & 0xffff; pc += 1; break;                          // int-to-char
          case 0x8f: regs[aa & 0xf] = (regs[aa >>> 4] << 16) >> 16; pc += 1; break;                     // int-to-short

          /* ---------------- binary int (23x: AA = BB op CC) ---------------- */
          case 0x90: this._binI(ins, pc, regs, aa, (a, b) => (a + b) | 0); pc += 2; break;              // add-int
          case 0x91: this._binI(ins, pc, regs, aa, (a, b) => (a - b) | 0); pc += 2; break;              // sub-int
          case 0x92: this._binI(ins, pc, regs, aa, (a, b) => Math.imul(a, b)); pc += 2; break;          // mul-int
          case 0x93: this._binI(ins, pc, regs, aa, (a, b) => { if (b === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); return (a / b) | 0; }); pc += 2; break;
          case 0x94: this._binI(ins, pc, regs, aa, (a, b) => { if (b === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); return (a % b) | 0; }); pc += 2; break;
          case 0x95: this._binI(ins, pc, regs, aa, (a, b) => a & b); pc += 2; break;                    // and-int
          case 0x96: this._binI(ins, pc, regs, aa, (a, b) => a | b); pc += 2; break;                    // or-int
          case 0x97: this._binI(ins, pc, regs, aa, (a, b) => a ^ b); pc += 2; break;                    // xor-int
          case 0x98: this._binI(ins, pc, regs, aa, (a, b) => a << (b & 31)); pc += 2; break;            // shl-int
          case 0x99: this._binI(ins, pc, regs, aa, (a, b) => a >> (b & 31)); pc += 2; break;            // shr-int
          case 0x9a: this._binI(ins, pc, regs, aa, (a, b) => (a >>> (b & 31)) | 0); pc += 2; break;     // ushr-int
          /* ---------------- binary long ---------------- */
          case 0x9b: this._binL(ins, pc, regs, aa, (a, b) => BigInt.asIntN(64, a + b)); pc += 2; break;
          case 0x9c: this._binL(ins, pc, regs, aa, (a, b) => BigInt.asIntN(64, a - b)); pc += 2; break;
          case 0x9d: this._binL(ins, pc, regs, aa, (a, b) => BigInt.asIntN(64, a * b)); pc += 2; break;
          case 0x9e: this._binL(ins, pc, regs, aa, (a, b) => { if (b === 0n) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); return a / b; }); pc += 2; break;
          case 0x9f: this._binL(ins, pc, regs, aa, (a, b) => { if (b === 0n) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); return a % b; }); pc += 2; break;
          case 0xa0: this._binL(ins, pc, regs, aa, (a, b) => a & b); pc += 2; break;
          case 0xa1: this._binL(ins, pc, regs, aa, (a, b) => a | b); pc += 2; break;
          case 0xa2: this._binL(ins, pc, regs, aa, (a, b) => a ^ b); pc += 2; break;
          case 0xa3: this._binL(ins, pc, regs, aa, (a, b) => BigInt.asIntN(64, a << BigInt(b & 63))); pc += 2; break;
          case 0xa4: this._binL(ins, pc, regs, aa, (a, b) => a >> BigInt(b & 63)); pc += 2; break;
          case 0xa5: this._binL(ins, pc, regs, aa, (a, b) => BigInt.asUintN(64, a) >> BigInt(b & 63)); pc += 2; break;
          /* ---------------- binary float (fround every op) ---------------- */
          case 0xa6: this._binF(ins, pc, regs, aa, (a, b) => a + b); pc += 2; break;
          case 0xa7: this._binF(ins, pc, regs, aa, (a, b) => a - b); pc += 2; break;
          case 0xa8: this._binF(ins, pc, regs, aa, (a, b) => a * b); pc += 2; break;
          case 0xa9: this._binF(ins, pc, regs, aa, (a, b) => a / b); pc += 2; break;
          case 0xaa: this._binF(ins, pc, regs, aa, (a, b) => a % b); pc += 2; break;
          /* ---------------- binary double ---------------- */
          case 0xab: this._binD(ins, pc, regs, aa, (a, b) => a + b); pc += 2; break;
          case 0xac: this._binD(ins, pc, regs, aa, (a, b) => a - b); pc += 2; break;
          case 0xad: this._binD(ins, pc, regs, aa, (a, b) => a * b); pc += 2; break;
          case 0xae: this._binD(ins, pc, regs, aa, (a, b) => a / b); pc += 2; break;
          case 0xaf: this._binD(ins, pc, regs, aa, (a, b) => a % b); pc += 2; break;


          /* ---------------- /2addr (12x: A op= B) ---------------- */
          case 0xb0: regs[aa & 0xf] = (regs[aa & 0xf] + regs[aa >>> 4]) | 0; pc += 1; break;
          case 0xb1: regs[aa & 0xf] = (regs[aa & 0xf] - regs[aa >>> 4]) | 0; pc += 1; break;
          case 0xb2: regs[aa & 0xf] = Math.imul(regs[aa & 0xf], regs[aa >>> 4]); pc += 1; break;
          case 0xb3: { const b = regs[aa >>> 4]; if (b === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); regs[aa & 0xf] = (regs[aa & 0xf] / b) | 0; pc += 1; } break;
          case 0xb4: { const b = regs[aa >>> 4]; if (b === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); regs[aa & 0xf] = (regs[aa & 0xf] % b) | 0; pc += 1; } break;
          case 0xb5: regs[aa & 0xf] = regs[aa & 0xf] & regs[aa >>> 4]; pc += 1; break;
          case 0xb6: regs[aa & 0xf] = regs[aa & 0xf] | regs[aa >>> 4]; pc += 1; break;
          case 0xb7: regs[aa & 0xf] = regs[aa & 0xf] ^ regs[aa >>> 4]; pc += 1; break;
          case 0xb8: regs[aa & 0xf] = regs[aa & 0xf] << (regs[aa >>> 4] & 31); pc += 1; break;
          case 0xb9: regs[aa & 0xf] = regs[aa & 0xf] >> (regs[aa >>> 4] & 31); pc += 1; break;
          case 0xba: regs[aa & 0xf] = (regs[aa & 0xf] >>> (regs[aa >>> 4] & 31)) | 0; pc += 1; break;
          case 0xbb: regs[aa & 0xf] = BigInt.asIntN(64, regs[aa & 0xf] + regs[aa >>> 4]); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xbc: regs[aa & 0xf] = BigInt.asIntN(64, regs[aa & 0xf] - regs[aa >>> 4]); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xbd: regs[aa & 0xf] = BigInt.asIntN(64, regs[aa & 0xf] * regs[aa >>> 4]); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xbe: { const b = regs[aa >>> 4]; if (b === 0n) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); regs[aa & 0xf] = regs[aa & 0xf] / b; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; } break;
          case 0xbf: { const b = regs[aa >>> 4]; if (b === 0n) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); regs[aa & 0xf] = regs[aa & 0xf] % b; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; } break;
          case 0xc0: regs[aa & 0xf] = regs[aa & 0xf] & regs[aa >>> 4]; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xc1: regs[aa & 0xf] = regs[aa & 0xf] | regs[aa >>> 4]; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xc2: regs[aa & 0xf] = regs[aa & 0xf] ^ regs[aa >>> 4]; regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xc3: regs[aa & 0xf] = BigInt.asIntN(64, regs[aa & 0xf] << BigInt(regs[aa >>> 4] & 63)); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xc4: regs[aa & 0xf] = BigInt.asIntN(64, regs[aa & 0xf] >> BigInt(regs[aa >>> 4] & 63)); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xc5: regs[aa & 0xf] = BigInt.asUintN(64, regs[aa & 0xf]) >> BigInt(regs[aa >>> 4] & 63); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xc6: regs[aa & 0xf] = f2i(i2f(regs[aa & 0xf]) + i2f(regs[aa >>> 4])); pc += 1; break;
          case 0xc7: regs[aa & 0xf] = f2i(i2f(regs[aa & 0xf]) - i2f(regs[aa >>> 4])); pc += 1; break;
          case 0xc8: regs[aa & 0xf] = f2i(i2f(regs[aa & 0xf]) * i2f(regs[aa >>> 4])); pc += 1; break;
          case 0xc9: regs[aa & 0xf] = f2i(i2f(regs[aa & 0xf]) / i2f(regs[aa >>> 4])); pc += 1; break;
          case 0xca: regs[aa & 0xf] = f2i(i2f(regs[aa & 0xf]) % i2f(regs[aa >>> 4])); pc += 1; break;
          case 0xcb: regs[aa & 0xf] = d2b(b2d(regs[aa & 0xf]) + b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xcc: regs[aa & 0xf] = d2b(b2d(regs[aa & 0xf]) - b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xcd: regs[aa & 0xf] = d2b(b2d(regs[aa & 0xf]) * b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xce: regs[aa & 0xf] = d2b(b2d(regs[aa & 0xf]) / b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;
          case 0xcf: regs[aa & 0xf] = d2b(b2d(regs[aa & 0xf]) % b2d(regs[aa >>> 4])); regs[(aa & 0xf) + 1] = WIDE2; pc += 1; break;

          /* ---------------- /lit16 (22s: A = B op lit16) ---------------- */
          case 0xd0: { const t = toS16(ins[pc + 1]); regs[aa & 0xf] = (regs[aa >>> 4] + t) | 0; pc += 2; } break;
          case 0xd1: { const t = toS16(ins[pc + 1]); regs[aa & 0xf] = (t - regs[aa >>> 4]) | 0; pc += 2; } break;              // rsub-int
          case 0xd2: { const t = toS16(ins[pc + 1]); regs[aa & 0xf] = Math.imul(regs[aa >>> 4], t); pc += 2; } break;
          case 0xd3: { const t = toS16(ins[pc + 1]); if (t === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); regs[aa & 0xf] = (regs[aa >>> 4] / t) | 0; pc += 2; } break;
          case 0xd4: { const t = toS16(ins[pc + 1]); if (t === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); regs[aa & 0xf] = (regs[aa >>> 4] % t) | 0; pc += 2; } break;
          case 0xd5: { const t = toS16(ins[pc + 1]); regs[aa & 0xf] = regs[aa >>> 4] & t; pc += 2; } break;
          case 0xd6: { const t = toS16(ins[pc + 1]); regs[aa & 0xf] = regs[aa >>> 4] | t; pc += 2; } break;
          case 0xd7: { const t = toS16(ins[pc + 1]); regs[aa & 0xf] = regs[aa >>> 4] ^ t; pc += 2; } break;

          /* ---------------- /lit8 (22b: regs[AA] = regs[BB] op lit8 @CC) ---------------- */
          case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
          case 0xe0: case 0xe1: case 0xe2: {
            const u1 = ins[pc + 1];
            const vb = u1 & 0xff;
            const L = ((u1 >>> 8) << 24) >> 24;
            const b = regs[vb];
            let rv;
            switch (op) {
              case 0xd8: rv = (b + L) | 0; break;
              case 0xd9: rv = (L - b) | 0; break;                  // rsub-int/lit8
              case 0xda: rv = Math.imul(b, L); break;
              case 0xdb: if (L === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); rv = (b / L) | 0; break;
              case 0xdc: if (L === 0) this.throwNew(thr, 'Ljava/lang/ArithmeticException;', 'divide by zero'); rv = (b % L) | 0; break;
              case 0xdd: rv = b & L; break;
              case 0xde: rv = b | L; break;
              case 0xdf: rv = b ^ L; break;
              case 0xe0: rv = b << (L & 31); break;
              case 0xe1: rv = b >> (L & 31); break;
              case 0xe2: rv = (b >>> (L & 31)) | 0; break;
            }
            regs[aa] = rv;
            pc += 2;
          } break;

          default:
            throw new Error('[vm] unimplemented opcode 0x' + op.toString(16) + ' at ' + m.fullName() + ' pc=' + pc);
        }
      } catch (e) {
        if (!(e instanceof VMThrow)) throw e;
        if (this.onThrow) { try { this.onThrow(e.exc, thr, m, pc); } catch (_) { } }
        // Dalvik exception handling: walk handlers from the faulted frame outward
        thr.exc = e.exc;
        if (e.exc._faultM === undefined) { e.exc._faultM = m; e.exc._faultPc = pc; }
        let handled = false;
        // current frame: the faulting insn is `pc`
        let searchPc = pc;
        while (frames.length > stopDepth) {
          const cur = frames[frames.length - 1];
          const haddr = this._findHandler(cur.m, searchPc, thr.exc.c);
          if (haddr >= 0) {
            cur.pc = haddr;
            if (cur !== fr) { fr = cur; m = fr.m; ins = m.insns; regs = fr.regs; }
            pc = haddr;
            handled = true;
            break;
          }
          frames.pop();
          if (frames.length <= stopDepth) break;
          fr = frames[frames.length - 1]; m = fr.m; ins = m.insns; regs = fr.regs;
          // the caller was suspended at an invoke; its handler-search address
          // is the invoke instruction itself (stored pc points past it)
          searchPc = Math.max(0, fr.pc - 3);
          pc = fr.pc;
        }
        if (!handled) {
          if (frames.length) { frames[frames.length - 1].pc = pc; }
          throw e;                        // propagate to _execMethod / pump
        }
      }
    }
  }

  /* small inline-decoded helpers */
  _binI(ins, pc, regs, aa, f) { const u1 = ins[pc + 1]; regs[aa] = f(regs[u1 & 0xff], regs[u1 >>> 8]) | 0; }
  _binL(ins, pc, regs, aa, f) { const u1 = ins[pc + 1]; regs[aa] = BigInt.asIntN(64, f(regs[u1 & 0xff], regs[u1 >>> 8])); regs[aa + 1] = WIDE2; }
  _binF(ins, pc, regs, aa, f) { const u1 = ins[pc + 1]; regs[aa] = f2i(f(i2f(regs[u1 & 0xff]), i2f(regs[u1 >>> 8]))); }
  _binD(ins, pc, regs, aa, f) { const u1 = ins[pc + 1]; regs[aa] = d2b(f(b2d(regs[u1 & 0xff]), b2d(regs[u1 >>> 8]))); regs[aa + 1] = WIDE2; }

  static _d2i(x) {
    if (isNaN(x)) return 0;
    if (x >= 2147483647) return 2147483647;
    if (x <= -2147483648) return -2147483648;
    return x | 0;
  }
  static _d2l(x) {
    if (isNaN(x)) return 0n;
    if (x >= 9223372036854775807) return 9223372036854775807n;
    if (x <= -9223372036854775808) return -9223372036854775808n;
    return BigInt(Math.trunc(x));
  }

  /** Parse a JVM method descriptor's parameter list into desc array. */
  static parseParams(desc) {
    const out = [];
    let i = desc.indexOf('(') + 1;
    while (desc[i] !== ')') {
      const start = i;
      while (desc[i] === '[') i++;
      if (desc[i] === 'L') i = desc.indexOf(';', i) + 1;
      else i++;
      out.push(desc.slice(start, i));
    }
    return out;
  }

  _constString(idx) {
    let s = this._dexStrCache.get(idx);
    if (s === undefined) {
      s = this.newString(this.dex.string(idx));
      this._dexStrCache.set(idx, s);
    }
    return s;
  }
}

if (typeof module !== 'undefined') module.exports = { VM, VMClass, MethodEntry, VMThrow, WIDE2, isWideDesc };
