#!/usr/bin/env python3
"""
dex2js.py -- ahead-of-time translator from Dalvik bytecode (classes.dex) to
JavaScript.

This is a *recompiler*, not a re-implementation:  every class, method and
instruction of the original application is translated 1:1.  Register machine
semantics are preserved exactly (32-bit int wrap-around, float rounding,
64-bit longs via BigInt, Dalvik exception tables, monitor ops, etc).

Output
------
    <out>/dex-classes.js   translated classes (registers into $C)
    <out>/dex-meta.js      method-signature -> mangled-name table used by
                           the hand written runtime to expose android/java
                           APIs under the same mangling scheme.

Execution model
---------------
Dalvik threads are cooperative coroutines.  Methods that can (transitively)
block -- Thread.sleep / join / Object.wait -- are compiled to generator
functions and their call sites use `yield*`; everything else compiles to a
plain JS function so the hot drawing/logic code keeps full JIT speed.
"""

import base64
import json
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dexlib  # noqa: E402
from dexlib import decode  # noqa: E402

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

PRIM = set("VZBSCIJFD")

ARRAY_CTOR = {
    "Z": "Uint8Array", "B": "Int8Array", "S": "Int16Array", "C": "Uint16Array",
    "I": "Int32Array", "J": "BigInt64Array", "F": "Float32Array",
    "D": "Float64Array",
}

DEFAULT = {"Z": "0", "B": "0", "S": "0", "C": "0", "I": "0", "F": "0",
           "D": "0", "J": "0n"}


def jsfloat(bits, width):
    """raw IEEE-754 bit pattern -> a JavaScript numeric literal"""
    import struct
    if width == 4:
        (v,) = struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))
    else:
        (v,) = struct.unpack("<d", struct.pack("<Q", bits & 0xFFFFFFFFFFFFFFFF))
    if v != v:
        return "NaN"
    if v == float("inf"):
        return "Infinity"
    if v == float("-inf"):
        return "-Infinity"
    if v == 0.0 and str(v)[0] == "-":
        return "-0"
    r = repr(v)
    return r


def cls_js(desc):
    """Lfoo/bar/Baz$Inner;  ->  foo_bar_Baz_S_Inner"""
    assert desc.startswith("L") and desc.endswith(";"), desc
    return desc[1:-1].replace("/", "_").replace("$", "_S_")


def cls_name(desc):
    """Lfoo/bar/Baz;  ->  foo/bar/Baz  (registry key)"""
    return desc[1:-1]


def jstr(s):
    """Emit a JS string literal, escaping everything non-ASCII."""
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif 0x20 <= o < 0x7F:
            out.append(ch)
        else:
            out.append("\\u%04x" % o)
    out.append('"')
    return "".join(out)


def sig_of(mid):
    return "%s(%s)" % (mid.name, "".join(mid.proto.params))


def full_sig(mid):
    return "%s(%s)%s" % (mid.name, "".join(mid.proto.params), mid.proto.ret)


# ---------------------------------------------------------------------------
# union find for virtual dispatch groups
# ---------------------------------------------------------------------------

class UF(object):
    def __init__(self):
        self.p = {}

    def find(self, x):
        p = self.p
        if x not in p:
            p[x] = x
            return x
        r = x
        while p[r] != r:
            r = p[r]
        while p[x] != r:
            p[x], x = r, p[x]
        return r

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[rb] = ra


# ---------------------------------------------------------------------------
# the translator
# ---------------------------------------------------------------------------

BLOCKING = {
    "java/lang/Thread.sleep(J)V",
    "java/lang/Thread.sleep(JI)V",
    "java/lang/Thread.yield()V",
    "java/lang/Thread.join()V",
    "java/lang/Object.wait()V",
    "java/lang/Object.wait(J)V",
}


class Translator(object):
    def __init__(self, dex, checked=False):
        self.d = dex
        self.checked = checked
        self.classes = {c.name: c for c in dex.classes}
        self.msig = {}          # "name(params)" -> mangled js name
        self._msig_ctr = defaultdict(int)
        self.fmap = {}          # (declClass, fieldName) -> js prop
        self.static_owner = {}
        self.warnings = []
        self.fw = {}
        self._ctx = {}
        self._prepare_methods()
        self._prepare_fields()
        self._prepare_async()

    # -- name mangling ---------------------------------------------------
    def mangle(self, mid):
        s = sig_of(mid)
        m = self.msig.get(s)
        if m is None:
            base = mid.name
            if base == "<init>":
                base = "$init"
            elif base == "<clinit>":
                self.msig[s] = "$clinit"
                return "$clinit"
            n = self._msig_ctr[base]
            self._msig_ctr[base] = n + 1
            m = "%s$%d" % (base, n)
            self.msig[s] = m
        return m

    def _prepare_methods(self):
        # deterministic order: assign mangled names over the whole method pool
        for mid in sorted(self.d.methods, key=lambda m: (m.name, sig_of(m))):
            self.mangle(mid)

    # -- fields ----------------------------------------------------------
    def supers(self, name):
        out = []
        while name in self.classes:
            name = self.classes[name].super
            out.append(name)
        return out

    def _prepare_fields(self):
        # instance fields: guarantee no shadowing collisions along a chain
        order = []
        seen = set()

        def visit(n):
            if n in seen or n not in self.classes:
                return
            seen.add(n)
            visit(self.classes[n].super)
            order.append(n)

        for n in self.classes:
            visit(n)
        inherited = defaultdict(dict)   # class -> {jsname: declClass}
        for n in order:
            c = self.classes[n]
            table = dict(inherited[c.super]) if c.super in inherited else {}
            for f in c.instance_fields:
                base = "f_" + f.fid.name
                js = base
                k = 1
                while js in table:
                    js = "%s$%d" % (base, k)
                    k += 1
                table[js] = n
                self.fmap[(n, f.fid.name)] = js
            for f in c.static_fields:
                self.fmap[(n, f.fid.name)] = "s_" + f.fid.name
            inherited[n] = table

    def resolve_field(self, fid, static):
        """Find the class that actually declares the field."""
        cn = fid.cls
        while cn in self.classes:
            c = self.classes[cn]
            lst = c.static_fields if static else c.instance_fields
            for f in lst:
                if f.fid.name == fid.name and f.fid.type == fid.type:
                    return cn, self.fmap[(cn, fid.name)]
            # a static may be declared in an interface
            if static:
                for i in c.interfaces:
                    if i in self.classes:
                        for f in self.classes[i].static_fields:
                            if f.fid.name == fid.name:
                                return i, self.fmap[(i, fid.name)]
            cn = c.super
        # framework field
        return cn, ("s_" if static else "f_") + fid.name

    # -- virtual dispatch groups & async analysis ------------------------
    def all_supertypes(self, name):
        out = []
        stack = [name]
        seen = set()
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            seen.add(n)
            c = self.classes.get(n)
            if not c:
                continue
            if c.super:
                out.append(c.super)
                stack.append(c.super)
            for i in c.interfaces:
                out.append(i)
                stack.append(i)
        return out

    def _prepare_async(self):
        uf = UF()
        declares = defaultdict(set)      # sig -> {class}
        self.mtable = {}                 # (class, sig) -> EncMethod
        for c in self.d.classes:
            for m in c.methods:
                s = sig_of(m.mid)
                self.mtable[(c.name, s)] = m
                if (m.access & (dexlib.ACC["STATIC"] | dexlib.ACC["PRIVATE"] |
                                dexlib.ACC["CONSTRUCTOR"])) == 0:
                    declares[s].add(c.name)
                uf.find((c.name, s))
        for s, owners in declares.items():
            for cn in owners:
                for sup in self.all_supertypes(cn):
                    if sup in owners:
                        uf.union((cn, s), (sup, s))
        self.uf = uf

        # seed: methods that call a blocking runtime primitive
        async_groups = set()
        callers = defaultdict(set)       # group -> {caller group}
        for c in self.d.classes:
            for m in c.methods:
                if not m.code:
                    continue
                g = uf.find((c.name, sig_of(m.mid)))
                ins, _ = decode(m.code.insns)
                for i in ins:
                    if not i.name.startswith("invoke"):
                        continue
                    tgt = self.d.methods[i.b]
                    key = "%s.%s" % (cls_name(tgt.cls) if tgt.cls.startswith("L")
                                     else tgt.cls, full_sig(tgt))
                    if key in BLOCKING:
                        async_groups.add(g)
                        continue
                    tg = self.target_group(tgt, i.name)
                    if tg is not None:
                        callers[tg].add(g)
        # propagate
        work = list(async_groups)
        while work:
            g = work.pop()
            for cg in callers.get(g, ()):
                if cg not in async_groups:
                    async_groups.add(cg)
                    work.append(cg)
        self.async_groups = async_groups

    def target_group(self, mid, opname):
        """Resolve an invoke to a dispatch group key, or None if framework."""
        cn = mid.cls
        s = sig_of(mid)
        if cn.startswith("["):
            return None
        seen = set()
        stack = [cn]
        while stack:
            n = stack.pop(0)
            if n in seen:
                continue
            seen.add(n)
            if (n, s) in self.mtable:
                return self.uf.find((n, s))
            c = self.classes.get(n)
            if not c:
                continue
            if c.super:
                stack.append(c.super)
            stack.extend(c.interfaces)
        return None

    def is_async_target(self, mid, opname):
        g = self.target_group(mid, opname)
        return g is not None and g in self.async_groups

    def is_async_method(self, cname, mid):
        return self.uf.find((cname, sig_of(mid))) in self.async_groups

    # ==================================================================
    # code generation
    # ==================================================================
    def emit(self):
        out = []
        out.append("// Generated by tools/dex2js.py -- do not edit.")
        out.append("// Translated from the original classes.dex of the APK.")
        out.append("'use strict';")
        out.append("(function(){")
        out.append("const $C = $rt.classes, $S = $rt.str, $A = $rt.arr;")
        # topological order of classes (superclass first)
        order = []
        seen = set()

        def visit(n):
            if n in seen or n not in self.classes:
                return
            seen.add(n)
            c = self.classes[n]
            visit(c.super)
            for i in c.interfaces:
                visit(i)
            order.append(n)

        for c in self.d.classes:
            visit(c.name)
        bodies = [self.emit_class(self.classes[n]) for n in order]
        for name in sorted(self.fw):
            out.append("const %s = $rt.need(%s);" % (self.fw[name], jstr(name)))
        out.extend(bodies)
        out.append("})();")
        return "\n".join(out)

    def emit_class(self, c):
        js = cls_js(c.name)
        L = []
        sup = c.super
        if sup in self.classes:
            base = "$C[%s]" % jstr(cls_name(sup))
        elif sup == "Ljava/lang/Object;" or sup is None:
            base = "$rt.JObject"
        else:
            base = "$rt.need(%s)" % jstr(cls_name(sup))
        L.append("/* ------- class %s ------- */" % c.name)
        L.append("const %s = class %s extends %s {" % (js, js, base))
        # constructor: default field values
        ctor = ["  constructor(){", "    super();"]
        for f in c.instance_fields:
            t = f.fid.type
            d = DEFAULT.get(t, "null")
            ctor.append("    this.%s = %s;" % (self.fmap[(c.name, f.fid.name)], d))
        ctor.append("  }")
        L.append("\n".join(ctor))
        for m in c.methods:
            L.append(self.emit_method(c, m))
        L.append("};")
        # class metadata
        L.append("$C[%s] = %s;" % (jstr(cls_name(c.name)), js))
        L.append("%s.$name = %s;" % (js, jstr(cls_name(c.name))))
        ifaces = []
        for i in self.all_supertypes(c.name):
            if i.startswith("L"):
                ifaces.append(cls_name(i))
        L.append("%s.$super = %s;" % (js, jstr(cls_name(sup)) if sup else "null"))
        L.append("%s.$types = %s;" % (js, json.dumps([cls_name(c.name)] + ifaces)))
        # static fields
        for f in c.static_fields:
            L.append("%s.%s = %s;" % (js, self.fmap[(c.name, f.fid.name)],
                                      DEFAULT.get(f.fid.type, "null")))
        # constant static values
        if c.static_values:
            for f, v in zip(c.static_fields, c.static_values):
                L.append("%s.%s = %s;" % (js, self.fmap[(c.name, f.fid.name)],
                                          self.encoded_value(v)))
        has_clinit = any(m.mid.name == "<clinit>" for m in c.methods)
        if has_clinit:
            L.append("%s.$ci = 0;" % js)
        L.append("$rt.link(%s);" % js)
        return "\n".join(L)

    def encoded_value(self, v):
        k, val = v
        if k == "string":
            return jstr(val)
        if k == "null":
            return "null"
        if k == "boolean":
            return "1" if val else "0"
        if k == "long":
            return "%dn" % val
        if k in ("float", "double"):
            if val != val:
                return "NaN"
            if val in (float("inf"), float("-inf")):
                return "Infinity" if val > 0 else "-Infinity"
            return repr(val)
        if k == "type":
            return "$rt.classFor(%s)" % jstr(val)
        if k == "array":
            return "[%s]" % ",".join(self.encoded_value(x) for x in val)
        return str(val)

    # -- per method -----------------------------------------------------
    def emit_method(self, c, m):
        mid = m.mid
        name = self.mangle(mid)
        static = bool(m.access & dexlib.ACC["STATIC"])
        gen = self.is_async_method(c.name, mid)
        if mid.name == "<clinit>":
            gen = False
        nparams = len(mid.proto.params)
        args = ["a%d" % i for i in range(nparams)]
        head = "  %s%s%s(%s) { /* %s */" % (
            "static " if static else "", "*" if gen else "", name,
            ", ".join(args), full_sig(mid))
        if not m.code:
            return head + " $rt.abstract(); }"
        body = self.emit_body(c, m, static, gen)
        return head + "\n" + body + "\n  }"

    def emit_body(self, c, m, static, gen):
        d = self.d
        code = m.code
        ins_list, payloads = decode(code.insns)
        nregs = code.registers
        # parameter registers
        pstart = nregs - code.ins
        prologue = []
        reg = pstart
        pi = 0
        if not static:
            prologue.append("v%d = this;" % reg)
            reg += 1
        for t in m.mid.proto.params:
            prologue.append("v%d = a%d;" % (reg, pi))
            pi += 1
            reg += 2 if t in "JD" else 1

        # ---- basic blocks
        leaders = {0}
        for i, ins in enumerate(ins_list):
            nxt = ins.off + ins.size
            f = ins.fmt
            if ins.name in ("goto", "goto/16", "goto/32"):
                leaders.add(ins.off + ins.a)
                leaders.add(nxt)
            elif f in ("21t",):
                leaders.add(ins.off + ins.b)
                leaders.add(nxt)
            elif f in ("22t",):
                leaders.add(ins.off + ins.c)
                leaders.add(nxt)
            elif ins.name in ("packed-switch", "sparse-switch"):
                p = payloads[ins.off + ins.b]
                tg = p[2] if p[0] == "packed" else p[2]
                for t in tg:
                    leaders.add(ins.off + t)
                leaders.add(nxt)
            elif ins.name.startswith("return") or ins.name == "throw":
                leaders.add(nxt)
        for t in code.tries:
            leaders.add(t.start)
            leaders.add(t.end)
            for _, h in t.handlers:
                leaders.add(h)
            if t.catch_all is not None:
                leaders.add(t.catch_all)
        offs = [i.off for i in ins_list]
        offset_set = set(offs)
        leaders = sorted(x for x in leaders if x in offset_set)
        block_of = {}
        cur = None
        for ins in ins_list:
            if ins.off in leaders:
                cur = ins.off
            block_of[ins.off] = cur

        self._ctx = dict(cls=c, method=m, gen=gen, leaders=leaders,
                         payloads=payloads, nregs=nregs,
                         ctype={}, looseif=set())
        ct, li = self.const_types(c, m, ins_list, payloads, code)
        self._ctx["ctype"] = ct
        self._ctx["looseif"] = li

        has_try = bool(code.tries)
        multi = len(leaders) > 1 or has_try
        lines = []
        decl = ", ".join("v%d = 0" % i for i in range(nregs))
        if decl:
            lines.append("    let %s;" % decl)
        lines.append("    let $r, $e;")
        lines.extend("    " + p for p in prologue)

        body = []
        idx = 0
        for bi, start in enumerate(leaders):
            end = leaders[bi + 1] if bi + 1 < len(leaders) else None
            body.append("      case %d: {" % start)
            if has_try:
                body.append("       $p = %d;" % start)
            while idx < len(ins_list) and (end is None or ins_list[idx].off < end):
                ins = ins_list[idx]
                nxt = ins_list[idx + 1].off if idx + 1 < len(ins_list) else None
                for line in self.emit_insn(ins, nxt, block_of):
                    body.append("       " + line)
                idx += 1
            body.append("      }")
        if multi:
            lines.append("    let $p = 0;")
            lines.append("    $L: for(;;) {")
            if has_try:
                lines.append("     try {")
            lines.append("      switch($p) {")
            lines.extend(body)
            lines.append("      }")
            lines.append("      break;")
            if has_try:
                lines.append("     } catch($t) {")
                lines.append("      $t = $rt.jt($t);")
                lines.append(self.emit_catch(code, block_of, leaders))
                lines.append("     }")
            lines.append("    }")
        else:
            # single basic block, no branches: straight line code
            for b in body[1:-1]:
                lines.append(b)
        return "\n".join(lines)

    # ==================================================================
    # constant typing
    #
    # `const v0, 0x3f800000` is 1.0f *or* 1065353216 depending only on how
    # the register is later consumed -- Dalvik registers are untyped, the
    # verifier reconstructs the types.  JavaScript has a single numeric type
    # so we must know which one is meant before we can emit the literal.
    # The pass below rebuilds that information: reaching definitions +
    # backward propagation of the type each *use* demands.
    # ==================================================================

    @staticmethod
    def _t(desc):
        """descriptor -> lattice type: 'I' 'J' 'F' 'D' (primitives) or 'L'

        'L' means the slot holds a reference, which is what turns a
        `const/4 vN, 0` into a JavaScript `null` instead of the number 0."""
        if not desc:
            return None
        c = desc[0]
        if c in "ZBSCI":
            return "I"
        if c == "J":
            return "J"
        if c == "F":
            return "F"
        if c == "D":
            return "D"
        if c == "V":
            return None
        return "L"

    CONV_SRC = {
        "int-to-long": "I", "int-to-float": "I", "int-to-double": "I",
        "int-to-byte": "I", "int-to-char": "I", "int-to-short": "I",
        "long-to-int": "J", "long-to-float": "J", "long-to-double": "J",
        "float-to-int": "F", "float-to-long": "F", "float-to-double": "F",
        "double-to-int": "D", "double-to-long": "D", "double-to-float": "D",
    }
    UNARY_SRC = {"neg-int": "I", "not-int": "I", "neg-long": "J",
                 "not-long": "J", "neg-float": "F", "neg-double": "D"}
    TYPE_OF = {"int": "I", "long": "J", "float": "F", "double": "D"}
    NODEF = ("invoke", "aput", "iput", "sput", "if-", "goto", "packed-switch",
             "sparse-switch", "return", "throw", "nop", "monitor-",
             "check-cast", "fill-array-data", "filled-new-array")
    MOVES = ("move", "move/from16", "move/16", "move-object",
             "move-object/from16", "move-object/16", "move-wide",
             "move-wide/from16", "move-wide/16")

    def _uses(self, ins, m, arrty):
        """[(register, required type or None)] read by this instruction."""
        d = self.d
        n = ins.name
        if n in self.MOVES:
            return [(ins.b, None)]
        if n in self.CONV_SRC:
            return [(ins.b, self.CONV_SRC[n])]
        if n in self.UNARY_SRC:
            return [(ins.b, self.UNARY_SRC[n])]
        if n in ("cmpl-float", "cmpg-float"):
            return [(ins.b, "F"), (ins.c, "F")]
        if n in ("cmpl-double", "cmpg-double"):
            return [(ins.b, "D"), (ins.c, "D")]
        if n == "cmp-long":
            return [(ins.b, "J"), (ins.c, "J")]
        if n == "new-array":
            return [(ins.b, "I")]
        if n == "array-length":
            return [(ins.b, "L")]
        if n == "instance-of":
            return [(ins.b, "L")]
        if n.startswith("if-"):
            return ([(ins.a, None)] if n.endswith("z")
                    else [(ins.a, None), (ins.b, None)])
        if n.startswith("aget") or n.startswith("aput"):
            u = [(ins.b, "L"), (ins.c, "I")]
            if n.startswith("aput"):
                el = None
                if n.endswith(("-boolean", "-byte", "-char", "-short")):
                    el = "I"
                elif n == "aput-object":
                    el = "L"
                else:
                    at = arrty.get(ins.b)
                    if at and at.startswith("["):
                        el = self._t(at[1:])
                        if n == "aput-wide" and el not in ("J", "D"):
                            el = None
                        if n == "aput" and el not in ("I", "F"):
                            el = None
                u.append((ins.a, el))
            return u
        if n.startswith("iget"):
            return [(ins.b, "L")]
        if n.startswith("iput"):
            return [(ins.b, "L"), (ins.a, self._t(d.fields[ins.c].type))]
        if n.startswith("sget"):
            return []
        if n.startswith("sput"):
            return [(ins.a, self._t(d.fields[ins.b].type))]
        if n.startswith("invoke"):
            mid = d.methods[ins.b]
            regs = list(ins.args)
            i = 0
            u = []
            if not n.startswith("invoke-static"):
                u.append((regs[0], "L"))
                i = 1
            for t in mid.proto.params:
                if i < len(regs):
                    u.append((regs[i], self._t(t)))
                i += 2 if t in "JD" else 1
            return u
        if n in ("filled-new-array", "filled-new-array/range"):
            el = self._t(d.types[ins.b][1:])
            return [(r, el) for r in ins.args]
        if n in ("return", "return-wide", "return-object"):
            return [(ins.a, self._t(m.mid.proto.ret))]
        if n == "throw" or n in ("monitor-enter", "monitor-exit") or \
                n == "check-cast" or n == "fill-array-data":
            return [(ins.a, "L")]
        # arithmetic
        if "-" in n:
            base = n.split("/")[0]
            if "-" not in base:
                return []
            op, typ = base.rsplit("-", 1)
            t = self.TYPE_OF.get(typ)
            if t is None:
                return []
            shift = op in ("shl", "shr", "ushr") and t == "J"
            if n.endswith("/2addr"):
                return [(ins.a, t), (ins.b, "I" if shift else t)]
            if "/lit" in n or op == "rsub":
                if "/lit" in n:
                    return [(ins.b, t)]
            return [(ins.b, t), (ins.c, "I" if shift else t)]
        return []

    def const_types(self, c, m, ins_list, payloads, code):
        """offset -> 'F' | 'D' for every const whose register is a float."""
        consts = {i.off: i for i in ins_list
                  if i.name in ("const", "const/high16", "const-wide",
                                "const-wide/16", "const-wide/32",
                                "const-wide/high16", "const/4", "const/16")}
        if not consts:
            return {}, set()
        by_off = {i.off: i for i in ins_list}
        order = [i.off for i in ins_list]
        pos = {o: k for k, o in enumerate(order)}
        nxt = {o: (order[k + 1] if k + 1 < len(order) else None)
               for k, o in enumerate(order)}

        # -- control flow graph -----------------------------------------
        handlers = set()
        in_try = {}
        for t in code.tries:
            hs = [h for _, h in t.handlers]
            if t.catch_all is not None:
                hs.append(t.catch_all)
            handlers.update(hs)
            for o in order:
                if t.start <= o < t.end:
                    in_try.setdefault(o, []).extend(hs)

        def succs(o):
            ins = by_off[o]
            n = ins.name
            s = list(in_try.get(o, ()))
            if n in ("goto", "goto/16", "goto/32"):
                s.append(o + ins.a)
                return s
            if n.startswith("return") or n == "throw":
                return s
            if n in ("packed-switch", "sparse-switch"):
                p = payloads[o + ins.b]
                s.extend(o + t for t in p[2])
            elif ins.fmt == "21t":
                s.append(o + ins.b)
            elif ins.fmt == "22t":
                s.append(o + ins.c)
            if nxt[o] is not None:
                s.append(nxt[o])
            return s

        # -- forward: reaching definitions + array descriptors ----------
        nregs = code.registers
        entry = {}
        reg = nregs - code.ins
        if not (m.access & dexlib.ACC["STATIC"]):
            entry[reg] = frozenset([("p", reg)])
            reg += 1
        pty = {}
        for t in m.mid.proto.params:
            entry[reg] = frozenset([("p", reg)])
            if t.startswith("["):
                pty[reg] = t
            reg += 2 if t in "JD" else 1

        IN = {order[0]: entry}
        ATY = {order[0]: dict(pty)}
        work = [order[0]]
        seen = 0
        while work and seen < 200000:
            seen += 1
            o = work.pop()
            ins = by_off[o]
            st = dict(IN.get(o, {}))
            aty = dict(ATY.get(o, {}))
            n = ins.name
            if not n.startswith(self.NODEF):
                a = ins.a
                st[a] = frozenset([("i", o)])
                aty.pop(a, None)
                if n == "new-array":
                    aty[a] = self.d.types[ins.c]
                elif n in self.MOVES and ins.b in aty:
                    aty[a] = aty[ins.b]
                elif n.startswith("iget"):
                    ft = self.d.fields[ins.c].type
                    if ft.startswith("["):
                        aty[a] = ft
                elif n.startswith("sget"):
                    ft = self.d.fields[ins.b].type
                    if ft.startswith("["):
                        aty[a] = ft
                elif n == "check-cast":
                    pass
                if ins.name.endswith("-wide") or ins.name.startswith("const-wide"):
                    st[a + 1] = frozenset([("hi", o)])
            elif n.startswith("invoke"):
                st[-1] = frozenset([("r", o)])          # $r pseudo register
                rt = self.d.methods[ins.b].proto.ret
                aty[-1] = rt if rt.startswith("[") else None
            elif n in ("filled-new-array", "filled-new-array/range"):
                st[-1] = frozenset([("r", o)])
                aty[-1] = self.d.types[ins.b]
            if n.startswith("move-result"):
                st[ins.a] = IN.get(o, {}).get(-1, frozenset())
                t = ATY.get(o, {}).get(-1)
                if t:
                    aty[ins.a] = t
                else:
                    aty.pop(ins.a, None)
            for s in succs(o):
                if s not in by_off:
                    continue
                old = IN.get(s)
                merged = dict(st) if old is None else None
                if old is not None:
                    merged = dict(old)
                    ch = False
                    for k, vset in st.items():
                        cur = merged.get(k)
                        if cur is None:
                            merged[k] = vset
                            ch = True
                        elif not vset <= cur:
                            merged[k] = cur | vset
                            ch = True
                    if not ch and s in ATY:
                        oa = ATY[s]
                        if all(oa.get(k) == v for k, v in aty.items()
                               if k in oa) and all(k in oa for k in aty):
                            continue
                IN[s] = merged
                oa = ATY.get(s)
                if oa is None:
                    ATY[s] = dict(aty)
                else:
                    na = {}
                    for k, v in aty.items():
                        if oa.get(k) == v:
                            na[k] = v
                    ATY[s] = na
                work.append(s)

        # -- backward: what type does every use demand ------------------
        need = defaultdict(set)
        for o in order:
            st = IN.get(o)
            if st is None:
                continue
            for r, t in self._uses(by_off[o], m, ATY.get(o, {})):
                if t is None:
                    continue
                for k in st.get(r, ()):
                    need[k].add(t)
        # flow requirements backwards through moves until stable
        for _ in range(8):
            changed = False
            for o in order:
                ins = by_off[o]
                if ins.name not in self.MOVES:
                    continue
                want = need.get(("i", o))
                if not want:
                    continue
                for k in IN.get(o, {}).get(ins.b, ()):
                    if not want <= need[k]:
                        need[k] |= want
                        changed = True
            if not changed:
                break

        out = {}
        for o in consts:
            t = need.get(("i", o))
            if t == {"F"} or t == {"D"} or t == {"L"}:
                out[o] = next(iter(t))

        # `if-eq vA, vB` where one side may be a reference and the other a
        # zero constant: Dalvik compares "null == null", JavaScript would
        # compare `null === 0`.  Flag those sites for a null tolerant test.
        zeros = set()
        for o, i in consts.items():
            if not i.name.startswith("const-wide") and (i.b or 0) == 0:
                zeros.add(("i", o))
        loose = set()
        for o in order:
            i = by_off[o]
            if i.name not in ("if-eq", "if-ne"):
                continue
            st = IN.get(o)
            if st is None:
                continue
            if (st.get(i.a, frozenset()) & zeros) or \
               (st.get(i.b, frozenset()) & zeros):
                loose.add(o)
        return out, loose

    def emit_catch(self, code, block_of, leaders):
        # for every block start, the ordered handler list that covers it
        cover = {}
        for start in leaders:
            hs = []
            for t in code.tries:
                if t.start <= start < t.end:
                    for ty, h in t.handlers:
                        hs.append((ty, h))
                    if t.catch_all is not None:
                        hs.append((None, t.catch_all))
            if hs:
                cover[start] = tuple(hs)
        groups = defaultdict(list)
        for start, hs in cover.items():
            groups[hs].append(start)
        out = ["      switch($p) {"]
        for hs, starts in groups.items():
            for s in starts:
                out.append("      case %d:" % s)
            for ty, h in hs:
                if ty is None:
                    out.append("        { $e = $t; $p = %d; continue $L; }" % h)
                else:
                    out.append("        if ($rt.iof($t, %s)) { $e = $t; $p = %d;"
                               " continue $L; }" % (jstr(ty), h))
            out.append("        break;")
        out.append("      }")
        out.append("      throw $t;")
        return "\n".join(out)

    # -- instruction translation ----------------------------------------
    def emit_insn(self, ins, nxt, block_of):
        d = self.d
        n = ins.name
        E = []

        def jump(target):
            if target == nxt:
                return []
            return ["$p = %d; continue $L;" % target]

        def v(i):
            return "v%d" % i

        # ---- moves / constants
        if n == "nop":
            return []
        if n in ("move", "move/from16", "move/16", "move-object",
                 "move-object/from16", "move-object/16", "move-wide",
                 "move-wide/from16", "move-wide/16"):
            return ["v%d = v%d;" % (ins.a, ins.b)]
        if n in ("move-result", "move-result-wide", "move-result-object"):
            return ["v%d = $r;" % ins.a]
        if n == "move-exception":
            return ["v%d = $e;" % ins.a]
        if n == "return-void":
            return ["return;"]
        if n in ("return", "return-object", "return-wide"):
            return ["return v%d;" % ins.a]
        if n in ("const/4", "const/16", "const", "const/high16",
                 "const-wide/16", "const-wide/32", "const-wide",
                 "const-wide/high16"):
            wide = n.startswith("const-wide")
            if n == "const/high16":
                bits = (ins.b << 16) & 0xFFFFFFFF
            elif n == "const-wide/high16":
                bits = (ins.b << 48) & 0xFFFFFFFFFFFFFFFF
            else:
                bits = ins.b & (0xFFFFFFFFFFFFFFFF if wide else 0xFFFFFFFF)
            t = self._ctx["ctype"].get(ins.off)
            if t == "L" and not wide and bits == 0:
                return ["v%d = null;" % ins.a]
            if t == "F" and not wide:
                return ["v%d = %s;" % (ins.a, jsfloat(bits, 4))]
            if t == "D" and wide:
                return ["v%d = %s;" % (ins.a, jsfloat(bits, 8))]
            lim = 1 << (64 if wide else 32)
            val = bits - lim if bits >= lim >> 1 else bits
            return ["v%d = %d%s;" % (ins.a, val, "n" if wide else "")]
        if n in ("const-string", "const-string/jumbo"):
            return ["v%d = %s;" % (ins.a, jstr(d.strings[ins.b]))]
        if n == "const-class":
            return ["v%d = $rt.classFor(%s);" % (ins.a, jstr(d.types[ins.b]))]
        if n in ("monitor-enter", "monitor-exit"):
            return []
        if n == "check-cast":
            if self.checked:
                return ["$rt.cast(v%d, %s);" % (ins.a, jstr(d.types[ins.b]))]
            return []
        if n == "instance-of":
            return ["v%d = $rt.iof(v%d, %s) ? 1 : 0;" %
                    (ins.a, ins.b, jstr(d.types[ins.c]))]
        if n == "array-length":
            return ["v%d = v%d.length;" % (ins.a, ins.b)]
        if n == "new-instance":
            t = d.types[ins.b]
            return self.clinit_guard(t) + ["v%d = %s;" % (ins.a, self.alloc(t))]
        if n == "new-array":
            t = d.types[ins.c]
            return ["v%d = %s;" % (ins.a, self.new_array(t, "v%d" % ins.b))]
        if n in ("filled-new-array", "filled-new-array/range"):
            t = d.types[ins.b]
            el = t[1:]
            vals = ", ".join("v%d" % r for r in ins.args)
            if el in ARRAY_CTOR:
                return ["$r = %s.of(%s);" % (ARRAY_CTOR[el], vals)]
            return ["$r = $A.obj(%s, [%s]);" % (jstr(t), vals)]
        if n == "fill-array-data":
            p = self._ctx["payloads"][ins.off + ins.b]
            b64 = base64.b64encode(p[3]).decode()
            return ['$A.fill(v%d, "%s", %d);' % (ins.a, b64, p[1])]
        if n == "throw":
            return ["throw v%d;" % ins.a]
        if n in ("goto", "goto/16", "goto/32"):
            return jump(ins.off + ins.a)
        if n == "packed-switch" or n == "sparse-switch":
            p = self._ctx["payloads"][ins.off + ins.b]
            E.append("switch(v%d) {" % ins.a)
            if p[0] == "packed":
                first, targets = p[1], p[2]
                keys = range(first, first + len(targets))
            else:
                keys, targets = p[1], p[2]
            for k, t in zip(keys, targets):
                E.append("  case %d: $p = %d; continue $L;" % (k, ins.off + t))
            E.append("}")
            E.extend(jump(nxt) if nxt is not None else [])
            return E
        # ---- comparisons
        if n in ("cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double"):
            nanv = "-1" if n.startswith("cmpl") else "1"
            return ["v%d = $rt.cmpf(v%d, v%d, %s);" % (ins.a, ins.b, ins.c, nanv)]
        if n == "cmp-long":
            return ["v%d = (v%d > v%d) ? 1 : ((v%d < v%d) ? -1 : 0);" %
                    (ins.a, ins.b, ins.c, ins.b, ins.c)]
        if n.startswith("if-"):
            op = {"eq": "===", "ne": "!==", "lt": "<", "ge": ">=", "gt": ">",
                  "le": "<="}[n[3:5]]
            if n.endswith("z"):
                target = ins.off + ins.b
                if op == "===":      # if-eqz: zero *or* null reference
                    cond = "(v%d === 0 || v%d == null)" % (ins.a, ins.a)
                elif op == "!==":
                    cond = "(v%d !== 0 && v%d != null)" % (ins.a, ins.a)
                else:
                    cond = "v%d %s 0" % (ins.a, op)
            else:
                target = ins.off + ins.c
                if ins.off in self._ctx["looseif"]:
                    cond = "%s$rt.same(v%d, v%d)" % (
                        "" if op == "===" else "!", ins.a, ins.b)
                else:
                    cond = "v%d %s v%d" % (ins.a, op, ins.b)
            if target == nxt:
                return []
            E.append("if (%s) { %s }" % (cond, " ".join(jump(target))))
            return E
        # ---- array access
        if n.startswith("aget") or n.startswith("aput"):
            arr, idx = "v%d" % ins.b, "v%d" % ins.c
            acc = "%s[%s]" % (arr, idx)
            if self.checked:
                acc = "%s[$A.ck(%s,%s)]" % (arr, idx, arr)
            if n.startswith("aget"):
                if n == "aget-boolean":
                    return ["v%d = %s;" % (ins.a, acc)]
                return ["v%d = %s;" % (ins.a, acc)]
            return ["%s = v%d;" % (acc, ins.a)]
        # ---- instance fields
        if n.startswith("iget") or n.startswith("iput"):
            fid = d.fields[ins.c]
            _, prop = self.resolve_field(fid, False)
            if n.startswith("iget"):
                return ["v%d = v%d.%s;" % (ins.a, ins.b, prop)]
            return ["v%d.%s = v%d;" % (ins.b, prop, ins.a)]
        # ---- static fields
        if n.startswith("sget") or n.startswith("sput"):
            fid = d.fields[ins.b]
            owner, prop = self.resolve_field(fid, True)
            ref = self.class_ref(owner)
            init = self.clinit_guard(owner)
            if n.startswith("sget"):
                return init + ["v%d = %s.%s;" % (ins.a, ref, prop)]
            return init + ["%s.%s = v%d;" % (ref, prop, ins.a)]
        # ---- invokes
        if n.startswith("invoke"):
            return self.emit_invoke(ins)
        # ---- unary / binary arithmetic
        return self.emit_arith(ins)

    # -- helpers ---------------------------------------------------------
    def class_ref(self, desc):
        if desc in self.classes:
            return cls_js(desc)
        n = cls_name(desc)
        ident = "$F_" + n.replace("/", "_").replace("$", "_S_")
        self.fw[n] = ident
        return ident

    def clinit_guard(self, desc):
        if (self._ctx.get("method") is not None
                and self._ctx["method"].mid.name == "<clinit>"
                and self._ctx["cls"].name == desc):
            return []
        c = self.classes.get(desc)
        if c and any(m.mid.name == "<clinit>" for m in c.methods):
            r = cls_js(desc)
            return ["if (!%s.$ci) { %s.$ci = 1; %s.$clinit(); }" % (r, r, r)]
        return []

    def alloc(self, t):
        return "new %s()" % self.class_ref(t)

    def new_array(self, t, size):
        el = t[1:]
        if el in ARRAY_CTOR:
            return "new %s(%s)" % (ARRAY_CTOR[el], size)
        return "$A.newObj(%s, %s)" % (jstr(el), size)

    STRING_HELPERS = {"java/lang/String", "java/lang/CharSequence"}

    def emit_invoke(self, ins):
        d = self.d
        mid = d.methods[ins.b]
        cname = cls_name(mid.cls) if mid.cls.startswith("L") else mid.cls
        kind = ins.name.split("/")[0]
        # collect argument registers honouring wide params
        regs = list(ins.args)
        i = 0
        argv = []
        recv = None
        if kind in ("invoke-virtual", "invoke-super", "invoke-direct",
                    "invoke-interface"):
            recv = "v%d" % regs[0]
            i = 1
        for t in mid.proto.params:
            argv.append("v%d" % regs[i])
            i += 2 if t in "JD" else 1
        args = ", ".join(argv)
        mangled = self.mangle(mid)
        is_async = self.is_async_target(mid, ins.name)
        blocking_key = "%s.%s" % (cname, full_sig(mid))
        y = "yield* " if (is_async or blocking_key in BLOCKING) else ""

        # array pseudo-class methods (clone)
        if mid.cls.startswith("["):
            if mid.name == "clone":
                return ["$r = $A.clone(%s);" % recv]
            return ["$r = $rt.unsupported(%s);" % jstr(str(mid))]

        # java.lang.String receivers are JS primitives
        if cname == "java/lang/String" and mid.name == "<init>":
            # `new-instance String` + `<init>` produces a *primitive* string:
            # write it straight back into the receiver register.
            return ["%s = $S[%s](%s);" % (recv, jstr(full_sig(mid)), args)]
        if cname in self.STRING_HELPERS and kind != "invoke-static":
            return ["$r = $S[%s](%s);" % (jstr(full_sig(mid)),
                                          ", ".join([recv] + argv))]
        if cname == "java/lang/String" and kind == "invoke-static":
            return ["$r = $S[%s](%s);" % (jstr(full_sig(mid)), args)]
        # java.lang.Object virtuals may land on primitives (string) too
        if cname == "java/lang/Object" and kind == "invoke-virtual":
            return ["$r = $rt.obj[%s](%s);" % (jstr(full_sig(mid)),
                                               ", ".join([recv] + argv))]
        if kind == "invoke-static":
            owner = self.resolve_static_owner(mid)
            ref = self.class_ref(owner)
            guard = self.clinit_guard(owner)
            return guard + ["$r = %s%s.%s(%s);" % (y, ref, mangled, args)]
        if kind == "invoke-direct":
            ref = self.class_ref(mid.cls)
            return ["$r = %s%s.prototype.%s.call(%s);" %
                    (y, ref, mangled, ", ".join([recv] + argv))]
        if kind == "invoke-super":
            sup = self.classes[self._ctx["cls"].name].super
            ref = self.class_ref(sup)
            return ["$r = %s%s.prototype.%s.call(%s);" %
                    (y, ref, mangled, ", ".join([recv] + argv))]
        # virtual / interface
        return ["$r = %s%s.%s(%s);" % (y, recv, mangled, args)]

    def resolve_static_owner(self, mid):
        cn = mid.cls
        s = sig_of(mid)
        while cn in self.classes:
            if (cn, s) in self.mtable:
                return cn
            cn = self.classes[cn].super
        return mid.cls

    # -- arithmetic ------------------------------------------------------
    INT_BIN = {
        "add": "(%s + %s) | 0", "sub": "(%s - %s) | 0",
        "mul": "Math.imul(%s, %s)",
        "div": "$rt.idiv(%s, %s)", "rem": "$rt.irem(%s, %s)",
        "and": "%s & %s", "or": "%s | %s", "xor": "%s ^ %s",
        "shl": "%s << (%s & 31)", "shr": "%s >> (%s & 31)",
        "ushr": "(%s >>> (%s & 31)) | 0",
    }
    LONG_BIN = {
        "add": "BigInt.asIntN(64, %s + %s)", "sub": "BigInt.asIntN(64, %s - %s)",
        "mul": "BigInt.asIntN(64, %s * %s)",
        "div": "$rt.ldiv(%s, %s)", "rem": "$rt.lrem(%s, %s)",
        "and": "(%s & %s)", "or": "(%s | %s)", "xor": "(%s ^ %s)",
        "shl": "BigInt.asIntN(64, %s << (%s & 63n))",
        "shr": "BigInt.asIntN(64, %s >> (%s & 63n))",
        "ushr": "$rt.lushr(%s, %s)",
    }
    FLOAT_BIN = {
        "add": "Math.fround(%s + %s)", "sub": "Math.fround(%s - %s)",
        "mul": "Math.fround(%s * %s)", "div": "Math.fround(%s / %s)",
        "rem": "Math.fround(%s %% %s)",
    }
    DOUBLE_BIN = {
        "add": "(%s + %s)", "sub": "(%s - %s)", "mul": "(%s * %s)",
        "div": "(%s / %s)", "rem": "(%s %% %s)",
    }

    def emit_arith(self, ins):
        n = ins.name
        a = ins.a
        # unary
        UN = {
            "neg-int": "(-v%(b)s) | 0", "not-int": "~v%(b)s",
            "neg-long": "BigInt.asIntN(64, -v%(b)s)", "not-long": "(~v%(b)s)",
            "neg-float": "Math.fround(-v%(b)s)", "neg-double": "(-v%(b)s)",
            "int-to-long": "BigInt(v%(b)s)", "int-to-float": "Math.fround(v%(b)s)",
            "int-to-double": "v%(b)s",
            "long-to-int": "Number(BigInt.asIntN(32, v%(b)s))",
            "long-to-float": "Math.fround(Number(v%(b)s))",
            "long-to-double": "Number(v%(b)s)",
            "float-to-int": "$rt.f2i(v%(b)s)", "float-to-long": "$rt.f2l(v%(b)s)",
            "float-to-double": "v%(b)s", "double-to-int": "$rt.f2i(v%(b)s)",
            "double-to-long": "$rt.f2l(v%(b)s)",
            "double-to-float": "Math.fround(v%(b)s)",
            "int-to-byte": "(v%(b)s << 24) >> 24",
            "int-to-char": "v%(b)s & 0xffff",
            "int-to-short": "(v%(b)s << 16) >> 16",
        }
        if n in UN:
            return ["v%d = %s;" % (a, UN[n] % {"b": ins.b})]
        # binary
        if "/2addr" in n:
            base = n[:-len("/2addr")]
            op, typ = base.rsplit("-", 1)
            x, yv = "v%d" % ins.a, "v%d" % ins.b
            dst = ins.a
        elif "/lit8" in n or "/lit16" in n:
            base = n.split("/")[0]
            op, typ = base.rsplit("-", 1)
            if op == "rsub":
                return ["v%d = (%d - v%d) | 0;" % (ins.a, ins.c if "lit8" in n
                                                   else ins.c, ins.b)]
            x, yv = "v%d" % ins.b, str(ins.c)
            dst = ins.a
        else:
            op, typ = n.rsplit("-", 1)
            x, yv = "v%d" % ins.b, "v%d" % ins.c
            dst = ins.a
        if n == "rsub-int":  # 22s form
            return ["v%d = (%d - v%d) | 0;" % (ins.a, ins.c, ins.b)]
        table = {"int": self.INT_BIN, "long": self.LONG_BIN,
                 "float": self.FLOAT_BIN, "double": self.DOUBLE_BIN}[typ]
        if typ == "int" and op in ("div", "rem") and yv.lstrip("-").isdigit() \
                and int(yv) != 0:
            expr = ("((%s / %s) | 0)" if op == "div" else "(%s %% %s)") % (x, yv)
            return ["v%d = %s;" % (dst, expr)]
        if typ == "long" and op in ("shl", "shr", "ushr"):
            yv = "BigInt(%s)" % yv
        expr = table[op] % (x, yv)
        return ["v%d = %s;" % (dst, expr)]


# ---------------------------------------------------------------------------

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("dex")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--checked", action="store_true",
                    help="emit array bounds / cast checks (debug builds)")
    a = ap.parse_args()
    dex = dexlib.Dex(open(a.dex, "rb").read())
    t = Translator(dex, checked=a.checked)
    os.makedirs(a.out, exist_ok=True)
    js = t.emit()
    with open(os.path.join(a.out, "dex-classes.js"), "w") as f:
        f.write(js)
    meta = {
        "methodSignatures": t.msig,
        "classes": [cls_name(c.name) for c in dex.classes],
        "asyncMethods": sorted(set("%s.%s" % (cls_name(k[0]), k[1])
                                   for k in t.uf.p
                                   if t.uf.find(k) in t.async_groups)),
    }
    with open(os.path.join(a.out, "dex-meta.js"), "w") as f:
        f.write("// Generated by tools/dex2js.py -- signature mangling table.\n")
        f.write("var $DEXMETA = %s;\n" % json.dumps(meta, indent=1))
        f.write("if (typeof module !== 'undefined') module.exports = $DEXMETA;\n")
    print("classes: %d  methods: %d  async: %d  bytes: %d" %
          (len(dex.classes), len(dex.methods), len(t.async_groups), len(js)))


if __name__ == "__main__":
    main()
