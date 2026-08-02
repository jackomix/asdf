"""
dexlib.py -- a self-contained parser + disassembler for the Dalvik Executable
(DEX) format, version 035.

Written from the public DEX specification.  No external dependencies.
Used by dex2js.py to translate the original game's Dalvik bytecode into
JavaScript.

Layout reference:
    header_item      : 112 bytes, little endian
    string_id_item   : u4 string_data_off
    type_id_item     : u4 descriptor_idx  -> string
    proto_id_item    : u4 shorty, u4 return_type, u4 parameters_off
    field_id_item    : u2 class_idx, u2 type_idx, u4 name_idx
    method_id_item   : u2 class_idx, u2 proto_idx, u4 name_idx
    class_def_item   : u4 class_idx, u4 access, u4 superclass_idx,
                       u4 interfaces_off, u4 source_file_idx,
                       u4 annotations_off, u4 class_data_off,
                       u4 static_values_off
"""

import struct
from collections import namedtuple

# ---------------------------------------------------------------------------
# primitive readers
# ---------------------------------------------------------------------------


class Reader(object):
    __slots__ = ("d", "p")

    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u1(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u2(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def u4(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def i4(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def uleb(self):
        result = 0
        shift = 0
        while True:
            b = self.d[self.p]
            self.p += 1
            result |= (b & 0x7F) << shift
            if not (b & 0x80):
                break
            shift += 7
        return result

    def uleb_p1(self):
        return self.uleb() - 1

    def sleb(self):
        result = 0
        shift = 0
        while True:
            b = self.d[self.p]
            self.p += 1
            result |= (b & 0x7F) << shift
            shift += 7
            if not (b & 0x80):
                break
        if shift < 64 and (b & 0x40):
            result -= 1 << shift
        return result

    def bytes(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v


def mutf8(raw):
    """Decode Modified UTF-8 (as used in DEX string_data_item)."""
    out = []
    i = 0
    n = len(raw)
    while i < n:
        a = raw[i]
        if a == 0:
            break
        if a < 0x80:
            out.append(a)
            i += 1
        elif (a & 0xE0) == 0xC0:
            b = raw[i + 1]
            out.append(((a & 0x1F) << 6) | (b & 0x3F))
            i += 2
        elif (a & 0xF0) == 0xE0:
            b = raw[i + 1]
            c = raw[i + 2]
            out.append(((a & 0x0F) << 12) | ((b & 0x3F) << 6) | (c & 0x3F))
            i += 3
        else:  # not valid MUTF-8, be forgiving
            out.append(a)
            i += 1
    # `out` holds UTF-16 code units (surrogates stay as-is -> python handles)
    return "".join(chr(c) for c in out)


# ---------------------------------------------------------------------------
# structures
# ---------------------------------------------------------------------------

Proto = namedtuple("Proto", "shorty ret params")
FieldId = namedtuple("FieldId", "cls type name")
MethodId = namedtuple("MethodId", "cls proto name")
EncField = namedtuple("EncField", "fid access")
EncMethod = namedtuple("EncMethod", "mid access code")
Try = namedtuple("Try", "start end handlers catch_all")

ACC = {
    "PUBLIC": 0x1, "PRIVATE": 0x2, "PROTECTED": 0x4, "STATIC": 0x8,
    "FINAL": 0x10, "SYNCHRONIZED": 0x20, "VOLATILE": 0x40, "BRIDGE": 0x40,
    "TRANSIENT": 0x80, "VARARGS": 0x80, "NATIVE": 0x100, "INTERFACE": 0x200,
    "ABSTRACT": 0x400, "STRICT": 0x800, "SYNTHETIC": 0x1000,
    "ANNOTATION": 0x2000, "ENUM": 0x4000, "CONSTRUCTOR": 0x10000,
}


class Code(object):
    __slots__ = ("registers", "ins", "outs", "insns", "tries", "insns_off")

    def __init__(self):
        self.tries = []


class ClassDef(object):
    def __init__(self):
        self.name = None
        self.access = 0
        self.super = None
        self.interfaces = []
        self.source = None
        self.static_fields = []
        self.instance_fields = []
        self.direct_methods = []
        self.virtual_methods = []
        self.static_values = []

    @property
    def methods(self):
        return self.direct_methods + self.virtual_methods


class Dex(object):
    def __init__(self, data):
        self.d = data
        if data[:4] != b"dex\n":
            raise ValueError("not a dex file")
        self.version = data[4:7].decode()
        h = struct.unpack_from("<20I", data, 32)
        (self.file_size, self.header_size, self.endian, self.link_size,
         self.link_off, self.map_off,
         self.string_ids_size, self.string_ids_off,
         self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off,
         self.field_ids_size, self.field_ids_off,
         self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off,
         self.data_size, self.data_off) = h
        self._strings()
        self._types()
        self._protos()
        self._fields()
        self._methods()
        self._classes()

    # -- pools ------------------------------------------------------------
    def _strings(self):
        self.strings = []
        offs = struct.unpack_from("<%dI" % self.string_ids_size, self.d,
                                  self.string_ids_off)
        for off in offs:
            r = Reader(self.d, off)
            n = r.uleb()  # utf16 length
            start = r.p
            end = self.d.index(b"\x00", start)
            self.strings.append(mutf8(self.d[start:end]))

    def _types(self):
        idx = struct.unpack_from("<%dI" % self.type_ids_size, self.d,
                                 self.type_ids_off)
        self.types = [self.strings[i] for i in idx]

    def _type_list(self, off):
        if off == 0:
            return []
        n = struct.unpack_from("<I", self.d, off)[0]
        idx = struct.unpack_from("<%dH" % n, self.d, off + 4)
        return [self.types[i] for i in idx]

    def _protos(self):
        self.protos = []
        for i in range(self.proto_ids_size):
            s, r, p = struct.unpack_from("<3I", self.d, self.proto_ids_off + 12 * i)
            self.protos.append(Proto(self.strings[s], self.types[r],
                                     self._type_list(p)))

    def _fields(self):
        self.fields = []
        for i in range(self.field_ids_size):
            c, t, n = struct.unpack_from("<HHI", self.d, self.field_ids_off + 8 * i)
            self.fields.append(FieldId(self.types[c], self.types[t],
                                       self.strings[n]))

    def _methods(self):
        self.methods = []
        for i in range(self.method_ids_size):
            c, p, n = struct.unpack_from("<HHI", self.d, self.method_ids_off + 8 * i)
            self.methods.append(MethodId(self.types[c], self.protos[p],
                                         self.strings[n]))

    # -- classes ----------------------------------------------------------
    def _classes(self):
        self.classes = []
        for i in range(self.class_defs_size):
            (ci, acc, si, ioff, soff, aoff, doff, voff) = struct.unpack_from(
                "<8I", self.d, self.class_defs_off + 32 * i)
            c = ClassDef()
            c.name = self.types[ci]
            c.access = acc
            c.super = self.types[si] if si != 0xFFFFFFFF else None
            c.interfaces = self._type_list(ioff)
            c.source = self.strings[soff] if soff != 0xFFFFFFFF else None
            if doff:
                self._class_data(c, doff)
            if voff:
                c.static_values = self._encoded_array(Reader(self.d, voff))
            self.classes.append(c)

    def _class_data(self, c, off):
        r = Reader(self.d, off)
        nsf, nif, ndm, nvm = r.uleb(), r.uleb(), r.uleb(), r.uleb()
        idx = 0
        for _ in range(nsf):
            idx += r.uleb()
            c.static_fields.append(EncField(self.fields[idx], r.uleb()))
        idx = 0
        for _ in range(nif):
            idx += r.uleb()
            c.instance_fields.append(EncField(self.fields[idx], r.uleb()))
        for lst, cnt in ((c.direct_methods, ndm), (c.virtual_methods, nvm)):
            idx = 0
            for _ in range(cnt):
                idx += r.uleb()
                acc = r.uleb()
                coff = r.uleb()
                lst.append(EncMethod(self.methods[idx], acc,
                                     self._code(coff) if coff else None))

    def _code(self, off):
        r = Reader(self.d, off)
        c = Code()
        c.registers = r.u2()
        c.ins = r.u2()
        c.outs = r.u2()
        ntries = r.u2()
        r.u4()  # debug_info_off
        n = r.u4()
        c.insns_off = r.p
        c.insns = struct.unpack_from("<%dH" % n, self.d, r.p)
        r.p += n * 2
        if ntries:
            if n & 1:
                r.p += 2  # padding
            raw = []
            for _ in range(ntries):
                sa = r.u4()
                cnt = r.u2()
                hoff = r.u2()
                raw.append((sa, cnt, hoff))
            hlist_off = r.p
            hr = Reader(self.d, hlist_off)
            nh = hr.uleb()
            handlers = {}
            for _ in range(nh):
                hpos = hr.p - hlist_off
                size = hr.sleb()
                pairs = []
                for _ in range(abs(size)):
                    ti = hr.uleb()
                    ad = hr.uleb()
                    pairs.append((self.types[ti], ad))
                ca = hr.uleb() if size <= 0 else None
                handlers[hpos] = (pairs, ca)
            for sa, cnt, hoff in raw:
                pairs, ca = handlers[hoff]
                c.tries.append(Try(sa, sa + cnt, pairs, ca))
        return c

    # -- encoded values ---------------------------------------------------
    def _encoded_array(self, r):
        n = r.uleb()
        return [self._encoded_value(r) for _ in range(n)]

    def _encoded_value(self, r):
        b = r.u1()
        vtype = b & 0x1F
        varg = b >> 5
        size = varg + 1

        def raw(n):
            return r.bytes(n)

        def sint(n):
            v = int.from_bytes(raw(n), "little", signed=True)
            return v

        def uint(n):
            return int.from_bytes(raw(n), "little", signed=False)

        if vtype == 0x00:
            return ("byte", sint(1))
        if vtype == 0x02:
            return ("short", sint(size))
        if vtype == 0x03:
            return ("char", uint(size))
        if vtype == 0x04:
            return ("int", sint(size))
        if vtype == 0x06:
            return ("long", sint(size))
        if vtype == 0x10:  # float: high order bytes
            v = int.from_bytes(raw(size), "little") << (8 * (4 - size))
            return ("float", struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0])
        if vtype == 0x11:  # double
            v = int.from_bytes(raw(size), "little") << (8 * (8 - size))
            return ("double", struct.unpack("<d", struct.pack("<Q", v))[0])
        if vtype == 0x17:
            return ("string", self.strings[uint(size)])
        if vtype == 0x18:
            return ("type", self.types[uint(size)])
        if vtype == 0x19:
            return ("field", self.fields[uint(size)])
        if vtype == 0x1A:
            return ("method", self.methods[uint(size)])
        if vtype == 0x1B:
            return ("enum", self.fields[uint(size)])
        if vtype == 0x1C:
            return ("array", self._encoded_array(r))
        if vtype == 0x1D:
            raise NotImplementedError("annotation encoded value")
        if vtype == 0x1E:
            return ("null", None)
        if vtype == 0x1F:
            return ("boolean", bool(varg))
        raise ValueError("bad encoded_value type 0x%02x" % vtype)


# ---------------------------------------------------------------------------
# instruction table.  (opcode -> name, format)
# ---------------------------------------------------------------------------

# fmt: off
OPCODES = {
0x00:("nop","10x"),0x01:("move","12x"),0x02:("move/from16","22x"),0x03:("move/16","32x"),
0x04:("move-wide","12x"),0x05:("move-wide/from16","22x"),0x06:("move-wide/16","32x"),
0x07:("move-object","12x"),0x08:("move-object/from16","22x"),0x09:("move-object/16","32x"),
0x0a:("move-result","11x"),0x0b:("move-result-wide","11x"),0x0c:("move-result-object","11x"),
0x0d:("move-exception","11x"),0x0e:("return-void","10x"),0x0f:("return","11x"),
0x10:("return-wide","11x"),0x11:("return-object","11x"),0x12:("const/4","11n"),
0x13:("const/16","21s"),0x14:("const","31i"),0x15:("const/high16","21h"),
0x16:("const-wide/16","21s"),0x17:("const-wide/32","31i"),0x18:("const-wide","51l"),
0x19:("const-wide/high16","21h"),0x1a:("const-string","21c"),0x1b:("const-string/jumbo","31c"),
0x1c:("const-class","21c"),0x1d:("monitor-enter","11x"),0x1e:("monitor-exit","11x"),
0x1f:("check-cast","21c"),0x20:("instance-of","22c"),0x21:("array-length","12x"),
0x22:("new-instance","21c"),0x23:("new-array","22c"),0x24:("filled-new-array","35c"),
0x25:("filled-new-array/range","3rc"),0x26:("fill-array-data","31t"),0x27:("throw","11x"),
0x28:("goto","10t"),0x29:("goto/16","20t"),0x2a:("goto/32","30t"),
0x2b:("packed-switch","31t"),0x2c:("sparse-switch","31t"),
0x2d:("cmpl-float","23x"),0x2e:("cmpg-float","23x"),0x2f:("cmpl-double","23x"),
0x30:("cmpg-double","23x"),0x31:("cmp-long","23x"),
0x32:("if-eq","22t"),0x33:("if-ne","22t"),0x34:("if-lt","22t"),0x35:("if-ge","22t"),
0x36:("if-gt","22t"),0x37:("if-le","22t"),
0x38:("if-eqz","21t"),0x39:("if-nez","21t"),0x3a:("if-ltz","21t"),0x3b:("if-gez","21t"),
0x3c:("if-gtz","21t"),0x3d:("if-lez","21t"),
0x44:("aget","23x"),0x45:("aget-wide","23x"),0x46:("aget-object","23x"),
0x47:("aget-boolean","23x"),0x48:("aget-byte","23x"),0x49:("aget-char","23x"),
0x4a:("aget-short","23x"),0x4b:("aput","23x"),0x4c:("aput-wide","23x"),
0x4d:("aput-object","23x"),0x4e:("aput-boolean","23x"),0x4f:("aput-byte","23x"),
0x50:("aput-char","23x"),0x51:("aput-short","23x"),
0x52:("iget","22c"),0x53:("iget-wide","22c"),0x54:("iget-object","22c"),
0x55:("iget-boolean","22c"),0x56:("iget-byte","22c"),0x57:("iget-char","22c"),
0x58:("iget-short","22c"),0x59:("iput","22c"),0x5a:("iput-wide","22c"),
0x5b:("iput-object","22c"),0x5c:("iput-boolean","22c"),0x5d:("iput-byte","22c"),
0x5e:("iput-char","22c"),0x5f:("iput-short","22c"),
0x60:("sget","21c"),0x61:("sget-wide","21c"),0x62:("sget-object","21c"),
0x63:("sget-boolean","21c"),0x64:("sget-byte","21c"),0x65:("sget-char","21c"),
0x66:("sget-short","21c"),0x67:("sput","21c"),0x68:("sput-wide","21c"),
0x69:("sput-object","21c"),0x6a:("sput-boolean","21c"),0x6b:("sput-byte","21c"),
0x6c:("sput-char","21c"),0x6d:("sput-short","21c"),
0x6e:("invoke-virtual","35c"),0x6f:("invoke-super","35c"),0x70:("invoke-direct","35c"),
0x71:("invoke-static","35c"),0x72:("invoke-interface","35c"),
0x74:("invoke-virtual/range","3rc"),0x75:("invoke-super/range","3rc"),
0x76:("invoke-direct/range","3rc"),0x77:("invoke-static/range","3rc"),
0x78:("invoke-interface/range","3rc"),
0x7b:("neg-int","12x"),0x7c:("not-int","12x"),0x7d:("neg-long","12x"),0x7e:("not-long","12x"),
0x7f:("neg-float","12x"),0x80:("neg-double","12x"),0x81:("int-to-long","12x"),
0x82:("int-to-float","12x"),0x83:("int-to-double","12x"),0x84:("long-to-int","12x"),
0x85:("long-to-float","12x"),0x86:("long-to-double","12x"),0x87:("float-to-int","12x"),
0x88:("float-to-long","12x"),0x89:("float-to-double","12x"),0x8a:("double-to-int","12x"),
0x8b:("double-to-long","12x"),0x8c:("double-to-float","12x"),0x8d:("int-to-byte","12x"),
0x8e:("int-to-char","12x"),0x8f:("int-to-short","12x"),
0x90:("add-int","23x"),0x91:("sub-int","23x"),0x92:("mul-int","23x"),0x93:("div-int","23x"),
0x94:("rem-int","23x"),0x95:("and-int","23x"),0x96:("or-int","23x"),0x97:("xor-int","23x"),
0x98:("shl-int","23x"),0x99:("shr-int","23x"),0x9a:("ushr-int","23x"),
0x9b:("add-long","23x"),0x9c:("sub-long","23x"),0x9d:("mul-long","23x"),0x9e:("div-long","23x"),
0x9f:("rem-long","23x"),0xa0:("and-long","23x"),0xa1:("or-long","23x"),0xa2:("xor-long","23x"),
0xa3:("shl-long","23x"),0xa4:("shr-long","23x"),0xa5:("ushr-long","23x"),
0xa6:("add-float","23x"),0xa7:("sub-float","23x"),0xa8:("mul-float","23x"),
0xa9:("div-float","23x"),0xaa:("rem-float","23x"),
0xab:("add-double","23x"),0xac:("sub-double","23x"),0xad:("mul-double","23x"),
0xae:("div-double","23x"),0xaf:("rem-double","23x"),
0xb0:("add-int/2addr","12x"),0xb1:("sub-int/2addr","12x"),0xb2:("mul-int/2addr","12x"),
0xb3:("div-int/2addr","12x"),0xb4:("rem-int/2addr","12x"),0xb5:("and-int/2addr","12x"),
0xb6:("or-int/2addr","12x"),0xb7:("xor-int/2addr","12x"),0xb8:("shl-int/2addr","12x"),
0xb9:("shr-int/2addr","12x"),0xba:("ushr-int/2addr","12x"),
0xbb:("add-long/2addr","12x"),0xbc:("sub-long/2addr","12x"),0xbd:("mul-long/2addr","12x"),
0xbe:("div-long/2addr","12x"),0xbf:("rem-long/2addr","12x"),0xc0:("and-long/2addr","12x"),
0xc1:("or-long/2addr","12x"),0xc2:("xor-long/2addr","12x"),0xc3:("shl-long/2addr","12x"),
0xc4:("shr-long/2addr","12x"),0xc5:("ushr-long/2addr","12x"),
0xc6:("add-float/2addr","12x"),0xc7:("sub-float/2addr","12x"),0xc8:("mul-float/2addr","12x"),
0xc9:("div-float/2addr","12x"),0xca:("rem-float/2addr","12x"),
0xcb:("add-double/2addr","12x"),0xcc:("sub-double/2addr","12x"),
0xcd:("mul-double/2addr","12x"),0xce:("div-double/2addr","12x"),0xcf:("rem-double/2addr","12x"),
0xd0:("add-int/lit16","22s"),0xd1:("rsub-int","22s"),0xd2:("mul-int/lit16","22s"),
0xd3:("div-int/lit16","22s"),0xd4:("rem-int/lit16","22s"),0xd5:("and-int/lit16","22s"),
0xd6:("or-int/lit16","22s"),0xd7:("xor-int/lit16","22s"),
0xd8:("add-int/lit8","22b"),0xd9:("rsub-int/lit8","22b"),0xda:("mul-int/lit8","22b"),
0xdb:("div-int/lit8","22b"),0xdc:("rem-int/lit8","22b"),0xdd:("and-int/lit8","22b"),
0xde:("or-int/lit8","22b"),0xdf:("xor-int/lit8","22b"),0xe0:("shl-int/lit8","22b"),
0xe1:("shr-int/lit8","22b"),0xe2:("ushr-int/lit8","22b"),
}
# fmt: on

# instruction size in 16-bit code units, by format
FMT_SIZE = {
    "10x": 1, "12x": 1, "11n": 1, "11x": 1, "10t": 1,
    "20t": 2, "22x": 2, "21t": 2, "21s": 2, "21h": 2, "21c": 2,
    "23x": 2, "22b": 2, "22t": 2, "22s": 2, "22c": 2,
    "32x": 3, "30t": 3, "31i": 3, "31t": 3, "31c": 3, "35c": 3, "3rc": 3,
    "51l": 5,
}


class Insn(object):
    __slots__ = ("off", "op", "name", "fmt", "a", "b", "c", "args", "size", "payload")

    def __init__(self, off, op, name, fmt):
        self.off = off
        self.op = op
        self.name = name
        self.fmt = fmt
        self.a = self.b = self.c = None
        self.args = None
        self.payload = None
        self.size = FMT_SIZE.get(fmt, 1)

    def __repr__(self):
        return "%04x: %s a=%s b=%s c=%s args=%s" % (
            self.off, self.name, self.a, self.b, self.c, self.args)


def s2(v):
    return v - 0x10000 if v & 0x8000 else v


def s1(v):
    return v - 0x100 if v & 0x80 else v


def s4(v):
    return v - 0x100000000 if v & 0x80000000 else v


def decode(insns):
    """Decode a code unit array into a list of Insn, plus payload tables."""
    out = []
    i = 0
    n = len(insns)
    payloads = {}
    while i < n:
        u = insns[i]
        op = u & 0xFF
        hi = u >> 8
        if op == 0x00 and hi != 0:
            # pseudo instruction payload
            off = i
            if hi == 0x01:  # packed-switch-payload
                size = insns[i + 1]
                first = insns[i + 2] | (insns[i + 3] << 16)
                first = s4(first)
                targets = []
                for k in range(size):
                    t = insns[i + 4 + 2 * k] | (insns[i + 5 + 2 * k] << 16)
                    targets.append(s4(t))
                payloads[off] = ("packed", first, targets)
                i += 4 + size * 2
                continue
            if hi == 0x02:  # sparse-switch-payload
                size = insns[i + 1]
                keys = []
                for k in range(size):
                    v = insns[i + 2 + 2 * k] | (insns[i + 3 + 2 * k] << 16)
                    keys.append(s4(v))
                base = i + 2 + size * 2
                targets = []
                for k in range(size):
                    v = insns[base + 2 * k] | (insns[base + 1 + 2 * k] << 16)
                    targets.append(s4(v))
                payloads[off] = ("sparse", keys, targets)
                i += 2 + size * 4
                continue
            if hi == 0x03:  # fill-array-data-payload
                width = insns[i + 1]
                size = insns[i + 2] | (insns[i + 3] << 16)
                nbytes = width * size
                raw = bytearray()
                p = i + 4
                for k in range((nbytes + 1) // 2):
                    raw += struct.pack("<H", insns[p + k])
                payloads[off] = ("array", width, size, bytes(raw[:nbytes]))
                i += 4 + (nbytes + 1) // 2
                continue
            raise ValueError("unknown payload %02x" % hi)
        if op not in OPCODES:
            raise ValueError("unknown opcode 0x%02x at %d" % (op, i))
        name, fmt = OPCODES[op]
        ins = Insn(i, op, name, fmt)
        if fmt == "10x":
            pass
        elif fmt == "12x":
            ins.a = hi & 0xF
            ins.b = hi >> 4
        elif fmt == "11n":
            ins.a = hi & 0xF
            b = hi >> 4
            ins.b = b - 16 if b & 8 else b
        elif fmt == "11x":
            ins.a = hi
        elif fmt == "10t":
            ins.a = s1(hi)
        elif fmt == "20t":
            ins.a = s2(insns[i + 1])
        elif fmt in ("22x",):
            ins.a = hi
            ins.b = insns[i + 1]
        elif fmt in ("21t", "21s"):
            ins.a = hi
            ins.b = s2(insns[i + 1])
        elif fmt == "21h":
            ins.a = hi
            ins.b = s2(insns[i + 1])
        elif fmt == "21c":
            ins.a = hi
            ins.b = insns[i + 1]
        elif fmt == "23x":
            ins.a = hi
            ins.b = insns[i + 1] & 0xFF
            ins.c = insns[i + 1] >> 8
        elif fmt == "22b":
            ins.a = hi
            ins.b = insns[i + 1] & 0xFF
            ins.c = s1(insns[i + 1] >> 8)
        elif fmt in ("22t", "22s"):
            ins.a = hi & 0xF
            ins.b = hi >> 4
            ins.c = s2(insns[i + 1])
        elif fmt == "22c":
            ins.a = hi & 0xF
            ins.b = hi >> 4
            ins.c = insns[i + 1]
        elif fmt == "32x":
            ins.a = insns[i + 1]
            ins.b = insns[i + 2]
        elif fmt == "30t":
            ins.a = s4(insns[i + 1] | (insns[i + 2] << 16))
        elif fmt in ("31i", "31t"):
            ins.a = hi
            ins.b = s4(insns[i + 1] | (insns[i + 2] << 16))
        elif fmt == "31c":
            ins.a = hi
            ins.b = insns[i + 1] | (insns[i + 2] << 16)
        elif fmt == "35c":
            cnt = hi >> 4
            g = hi & 0xF
            idx = insns[i + 1]
            regs = insns[i + 2]
            args = [regs & 0xF, (regs >> 4) & 0xF, (regs >> 8) & 0xF,
                    (regs >> 12) & 0xF, g]
            ins.args = args[:cnt]
            ins.b = idx
            ins.a = cnt
        elif fmt == "3rc":
            cnt = hi
            idx = insns[i + 1]
            first = insns[i + 2]
            ins.args = list(range(first, first + cnt))
            ins.b = idx
            ins.a = cnt
        elif fmt == "51l":
            ins.a = hi
            v = 0
            for k in range(4):
                v |= insns[i + 1 + k] << (16 * k)
            if v & (1 << 63):
                v -= 1 << 64
            ins.b = v
        else:
            raise ValueError("format %s" % fmt)
        out.append(ins)
        i += ins.size
    return out, payloads


def pretty_type(desc):
    """Turn a type descriptor into a java-ish name."""
    dims = 0
    while desc.startswith("["):
        dims += 1
        desc = desc[1:]
    base = {
        "V": "void", "Z": "boolean", "B": "byte", "S": "short", "C": "char",
        "I": "int", "J": "long", "F": "float", "D": "double",
    }.get(desc)
    if base is None:
        base = desc[1:-1].replace("/", ".")
    return base + "[]" * dims
