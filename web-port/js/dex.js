/* ============================================================================
 * dex.js — DEX (Dalvik Executable) binary parser.
 *
 * Parses the ORIGINAL, unmodified classes.dex from the APK into an in-memory
 * model (strings/types/protos/fields/methods/class defs/code items) consumed
 * by js/vm.js, which executes the bytecode directly. This is a byte-faithful
 * loader: no decompilation takes place — the game's original Dalvik machine
 * code is what gets executed in the browser.
 * ========================================================================== */
'use strict';

const DEX_NO_INDEX = 0xffffffff;

class DexReader {
  constructor(buf) {
    this.u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    this.dv = new DataView(this.u8.buffer, this.u8.byteOffset, this.u8.byteLength);
  }
  u1(o)      { return this.u8[o]; }
  s1(o)      { return (this.u8[o] << 24) >> 24; }
  u2(o)      { return this.dv.getUint16(o, true); }
  s2(o)      { return this.dv.getInt16(o, true); }
  u4(o)      { return this.dv.getUint32(o, true); }
  s4(o)      { return this.dv.getInt32(o, true); }
  uleb(o)    { let r = 0, s = 0, b; do { b = this.u8[o++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return [r >>> 0, o]; }
  sleb(o)    {
    let r = 0, s = 0, b;
    do { b = this.u8[o++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80);
    if (s < 32 && (b & 0x40)) r |= (~0 << s);
    return [r | 0, o];
  }
}

/* Decode Modified UTF-8 (DEX string_data_item) into a JS string. */
function decodeMUTF8(u8, off) {
  let out = [];
  const [utf16Len, start] = (function () { const r = new DexReader(u8); return r.uleb(off); })();
  let i = start, produced = 0;
  const max = u8.length;
  while (produced < utf16Len && i < max) {
    let a = u8[i++];
    if (a === 0) break;
    if (a < 0x80) { out.push(a); produced++; }
    else if ((a & 0xe0) === 0xc0) {
      const b = u8[i++];
      out.push(((a & 0x1f) << 6) | (b & 0x3f)); produced++;
    } else if ((a & 0xf0) === 0xe0) {
      const b = u8[i++], c = u8[i++];
      out.push(((a & 0x0f) << 12) | ((b & 0x3f) << 6) | (c & 0x3f)); produced++;
    } else {
      out.push(0xfffd); produced++;
    }
  }
  // Assemble, keeping surrogate pairs intact.
  let s = '';
  for (let k = 0; k < out.length; k += 8192) {
    s += String.fromCharCode.apply(null, out.slice(k, k + 8192));
  }
  return s;
}

class DexFile {
  /** @param {ArrayBuffer|Uint8Array} buf raw classes.dex */
  constructor(buf) {
    this.r = new DexReader(buf);
    this.u8 = this.r.u8;
    const r = this.r;
    const magic = String.fromCharCode.apply(null, this.u8.slice(0, 8));
    if (magic.slice(0, 4) !== 'dex\n') throw new Error('not a dex file: ' + magic);
    this.version = magic.slice(4, 7);

    this.stringIdsSize = r.u4(0x38); this.stringIdsOff = r.u4(0x3c);
    this.typeIdsSize   = r.u4(0x40); this.typeIdsOff   = r.u4(0x44);
    this.protoIdsSize  = r.u4(0x48); this.protoIdsOff  = r.u4(0x4c);
    this.fieldIdsSize  = r.u4(0x50); this.fieldIdsOff  = r.u4(0x54);
    this.methodIdsSize = r.u4(0x58); this.methodIdsOff = r.u4(0x5c);
    this.classDefsSize = r.u4(0x60); this.classDefsOff = r.u4(0x64);

    this._strCache = new Array(this.stringIdsSize);
    this._typeCache = new Array(this.typeIdsSize);
    this._protoCache = new Array(this.protoIdsSize);
    this._fieldCache = new Array(this.fieldIdsSize);
    this._methodCache = new Array(this.methodIdsSize);
    this.classDefs = [];
    for (let i = 0; i < this.classDefsSize; i++) {
      const o = this.classDefsOff + i * 32;
      this.classDefs.push({
        idx: i,
        classIdx: r.u4(o), accessFlags: r.u4(o + 4), superclassIdx: r.u4(o + 8),
        interfacesOff: r.u4(o + 12), sourceFileIdx: r.u4(o + 16),
        annotationsOff: r.u4(o + 20), classDataOff: r.u4(o + 24), staticValuesOff: r.u4(o + 28),
      });
    }
  }

  string(idx) {
    if (idx === DEX_NO_INDEX) return null;
    let v = this._strCache[idx];
    if (v !== undefined) return v;
    const off = this.r.u4(this.stringIdsOff + idx * 4);
    v = decodeMUTF8(this.u8, off);
    this._strCache[idx] = v;
    return v;
  }

  typeDesc(idx) {
    if (idx === DEX_NO_INDEX) return null;
    let v = this._typeCache[idx];
    if (v !== undefined) return v;
    v = this.string(this.r.u4(this.typeIdsOff + idx * 4));
    this._typeCache[idx] = v;
    return v;
  }

  proto(idx) {
    let v = this._protoCache[idx];
    if (v !== undefined) return v;
    const o = this.protoIdsOff + idx * 12;
    const r = this.r;
    const shortyIdx = r.u4(o), returnTypeIdx = r.u4(o + 4), paramsOff = r.u4(o + 8);
    const params = [];
    if (paramsOff !== 0) {
      const size = r.u4(paramsOff);
      for (let i = 0; i < size; i++) params.push(this.typeDesc(r.u2(paramsOff + 4 + i * 2)));
    }
    v = {
      shorty: this.string(shortyIdx),
      returnType: this.typeDesc(returnTypeIdx),
      params,
      desc: '(' + params.join('') + ')' + this.typeDesc(returnTypeIdx),
    };
    this._protoCache[idx] = v;
    return v;
  }

  fieldRef(idx) {
    let v = this._fieldCache[idx];
    if (v !== undefined) return v;
    const o = this.fieldIdsOff + idx * 8;
    v = {
      classIdx: this.r.u2(o), typeIdx: this.r.u2(o + 2), nameIdx: this.r.u4(o + 4),
    };
    v.classDesc = this.typeDesc(v.classIdx);
    v.typeDesc = this.typeDesc(v.typeIdx);
    v.name = this.string(v.nameIdx);
    v.key = v.classDesc + '->' + v.name + ' ' + v.typeDesc;
    this._fieldCache[idx] = v;
    return v;
  }

  methodRef(idx) {
    let v = this._methodCache[idx];
    if (v !== undefined) return v;
    const o = this.methodIdsOff + idx * 8;
    v = {
      classIdx: this.r.u2(o), protoIdx: this.r.u2(o + 2), nameIdx: this.r.u4(o + 4),
    };
    v.classDesc = this.typeDesc(v.classIdx);
    v.name = this.string(v.nameIdx);
    v.proto = this.proto(v.protoIdx);
    v.key = v.classDesc + '->' + v.name + v.proto.desc;
    this._methodCache[idx] = v;
    return v;
  }

  typeListAt(off) {
    const out = [];
    if (off === 0) return out;
    const size = this.r.u4(off);
    for (let i = 0; i < size; i++) out.push(this.typeDesc(this.r.u2(off + 4 + i * 2)));
    return out;
  }

  /** Parse class_data_item. Returns encoded field/method arrays. */
  classData(classDef) {
    const r = this.r;
    if (classDef.classDataOff === 0) {
      return { staticFields: [], instanceFields: [], directMethods: [], virtualMethods: [] };
    }
    let o = classDef.classDataOff;
    let read;
    [read, o] = r.uleb(o); const nStaticF = read;
    [read, o] = r.uleb(o); const nInstF = read;
    [read, o] = r.uleb(o); const nDirectM = read;
    [read, o] = r.uleb(o); const nVirtualM = read;

    const readFields = (n) => {
      const arr = [];
      let idx = 0;
      for (let i = 0; i < n; i++) {
        let diff; [diff, o] = r.uleb(o); idx += diff;
        let af;   [af, o] = r.uleb(o);
        const ref = this.fieldRef(idx);
        arr.push({ idx, accessFlags: af, name: ref.name, desc: ref.typeDesc, classDesc: ref.classDesc });
      }
      return arr;
    };
    const readMethods = (n) => {
      const arr = [];
      let idx = 0;
      for (let i = 0; i < n; i++) {
        let diff;  [diff, o] = r.uleb(o); idx += diff;
        let af;    [af, o] = r.uleb(o);
        let co;    [co, o] = r.uleb(o);
        const ref = this.methodRef(idx);
        arr.push({
          idx, accessFlags: af, codeOff: co,
          name: ref.name, proto: ref.proto, desc: ref.proto.desc,
          classDesc: ref.classDesc, sig: ref.name + ref.proto.desc,
        });
      }
      return arr;
    };

    const staticFields    = readFields(nStaticF);
    const instanceFields  = readFields(nInstF);
    const directMethods   = readMethods(nDirectM);
    const virtualMethods  = readMethods(nVirtualM);
    return { staticFields, instanceFields, directMethods, virtualMethods };
  }

  /** Parse code_item + tries/catch handlers for an encoded method. */
  codeItem(codeOff) {
    const r = this.r;
    const registersSize = r.u2(codeOff);
    const insSize       = r.u2(codeOff + 2);
    const outsSize      = r.u2(codeOff + 4);
    const triesSize     = r.u2(codeOff + 6);
    const debugInfoOff  = r.u4(codeOff + 8);
    const insnsSize     = r.u4(codeOff + 12);
    const insnsOff      = codeOff + 16;
    let tries = [];
    if (triesSize > 0) {
      const padded = insnsSize * 2;
      let to = insnsOff + padded + ((insnsSize & 1) ? 2 : 0);
      for (let i = 0; i < triesSize; i++) {
        tries.push({ startAddr: r.u4(to), insnCount: r.u2(to + 4), handlerOff: r.u2(to + 6) });
        to += 8;
      }
      const handlersBase = to;
      const [listSize] = r.uleb(handlersBase);
      const handlers = [];
      for (let i = 0; i < triesSize; i++) {
        let ho = handlersBase + tries[i].handlerOff;
        let size; [size, ho] = r.sleb(ho);
        const pairs = [];
        const nTypes = Math.abs(size);
        for (let j = 0; j < nTypes; j++) {
          let t, a;
          [t, ho] = r.uleb(ho);
          [a, ho] = r.uleb(ho);
          pairs.push({ typeIdx: t, addr: a, typeDesc: this.typeDesc(t) });
        }
        let catchAllAddr = -1;
        if (size <= 0) { [catchAllAddr, ho] = r.uleb(ho); }
        handlers.push({ pairs, catchAllAddr });
      }
      for (let i = 0; i < triesSize; i++) {
        const t = tries[i], h = handlers[i];
        t.endAddr = t.startAddr + t.insnCount;
        t.handlers = h;
      }
    }
    return { registersSize, insSize, outsSize, debugInfoOff, insnsSize, insnsOff, tries };
  }

  /** Parse static_values_off encoded_array. Returns array of raw values. */
  staticValues(classDef) {
    const r = this.r;
    const vals = [];
    let o = classDef.staticValuesOff;
    if (!o) return vals;
    let n; [n, o] = r.uleb(o);
    const _f32buf = new Float32Array(1);
    const _f64buf = new Float64Array(1);
    const _u16buf = new DataView(_f64buf.buffer);
    const readValue = (off) => {
      const b = r.u1(off++);
      const arg = b >> 5, type = b & 0x1f;
      const nbytes = arg + 1;
      // raw little-endian unsigned integer of nbytes
      let raw = 0n;
      for (let i = 0; i < nbytes; i++) raw |= BigInt(r.u1(off + i)) << BigInt(8 * i);
      off += nbytes;
      switch (type) {
        case 0x00: // VALUE_BYTE
        case 0x02: // VALUE_SHORT
        case 0x03: // VALUE_CHAR (unsigned)
        case 0x04: { // VALUE_INT
          const v = BigInt.asIntN(BigInt(8 * nbytes), raw);
          if (type === 0x03) return [Number(BigInt.asUintN(BigInt(8 * nbytes), raw)), off];
          return [Number(v) | 0, off];
        }
        case 0x06: { // VALUE_LONG
          return [BigInt.asIntN(64, BigInt.asIntN(BigInt(8 * nbytes), raw)), off];
        }
        case 0x10: { // VALUE_FLOAT (right zero-extended) — raw i32 bits
          const bits = Number(BigInt.asUintN(32, raw << BigInt(8 * (3 - arg)))) | 0;
          return [bits, off];
        }
        case 0x11: { // VALUE_DOUBLE (right zero-extended) — raw i64 bits
          const bits = BigInt.asIntN(64, raw << BigInt(8 * (7 - arg)));
          return [bits, off];
        }
        case 0x17: { // VALUE_STRING
          return [{ _str: Number(raw) }, off];
        }
        case 0x18: { // VALUE_TYPE
          return [{ _type: Number(raw) }, off];
        }
        case 0x19: { // VALUE_FIELD
          return [{ _fieldRef: Number(raw) }, off];
        }
        case 0x1a: { // VALUE_METHOD
          return [{ _methodRef: Number(raw) }, off];
        }
        case 0x1b: { // VALUE_ENUM
          return [{ _enum: Number(raw) }, off];
        }
        case 0x1e: return [null, off];                // VALUE_NULL
        case 0x1f: return [arg !== 0, off];           // VALUE_BOOLEAN
        default: throw new Error('encoded_value type 0x' + type.toString(16) + ' @' + off.toString(16));
      }
    };
    for (let i = 0; i < n; i++) {
      let v; [v, o] = readValue(o);
      vals.push(v);
    }
    return vals;
  }
}

/* Convenience: unsigned 32-bit wrap for index ops */
const U32 = (x) => x >>> 0;

if (typeof module !== 'undefined') module.exports = { DexFile };
