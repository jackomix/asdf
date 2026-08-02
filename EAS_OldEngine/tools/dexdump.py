#!/usr/bin/env python3
"""
dexdump.py -- smali-like disassembly listing for a classes.dex.

Usage:
    python3 tools/dexdump.py <classes.dex> [class-name-substring] [method-name]

Produces a human readable listing used during the reverse engineering phase
(and kept in the repo so the port is auditable).
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dexlib  # noqa: E402


def fmt_ref(d, ins):
    op = ins.name
    if op in ("const-string", "const-string/jumbo"):
        return '"%s"' % d.strings[ins.b].replace("\n", "\\n")
    if op in ("const-class", "check-cast", "new-instance"):
        return d.types[ins.b]
    if op == "new-array":
        return d.types[ins.c]
    if op == "instance-of":
        return d.types[ins.c]
    if op.startswith("invoke"):
        m = d.methods[ins.b]
        return "%s->%s(%s)%s" % (m.cls, m.name, "".join(m.proto.params),
                                 m.proto.ret)
    if op.startswith("iget") or op.startswith("iput"):
        f = d.fields[ins.c]
        return "%s->%s:%s" % (f.cls, f.name, f.type)
    if op.startswith("sget") or op.startswith("sput"):
        f = d.fields[ins.b]
        return "%s->%s:%s" % (f.cls, f.name, f.type)
    if op in ("filled-new-array", "filled-new-array/range"):
        return d.types[ins.b]
    return None


def dump_method(d, c, m, out=sys.stdout):
    acc = m.access
    kind = []
    for k, v in dexlib.ACC.items():
        if acc & v and k in ("PUBLIC", "PRIVATE", "PROTECTED", "STATIC",
                             "FINAL", "ABSTRACT", "NATIVE", "SYNCHRONIZED"):
            kind.append(k.lower())
    print(".method %s %s(%s)%s" % (" ".join(kind), m.mid.name,
                                   "".join(m.mid.proto.params),
                                   m.mid.proto.ret), file=out)
    if not m.code:
        print(".end method\n", file=out)
        return
    code = m.code
    print("    .registers %d  .ins %d  .outs %d" %
          (code.registers, code.ins, code.outs), file=out)
    ins_list, payloads = dexlib.decode(code.insns)
    for t in code.tries:
        print("    .try %04x-%04x -> %s catch_all=%s" %
              (t.start, t.end, t.handlers, t.catch_all), file=out)
    for ins in ins_list:
        ref = fmt_ref(d, ins)
        args = []
        f = ins.fmt
        if ins.args is not None:
            args.append("{%s}" % ", ".join("v%d" % r for r in ins.args))
        else:
            if ins.a is not None:
                args.append(("v%d" if f in ("12x", "11x", "22x", "23x", "32x",
                                            "21c", "21s", "21h", "21t", "22b",
                                            "22t", "22s", "22c", "31i", "31t",
                                            "31c", "11n", "51l") else "%d") % ins.a)
            if ins.b is not None and ref is None:
                if f in ("12x", "22x", "23x", "32x", "22b", "22c", "22t", "22s"):
                    args.append("v%d" % ins.b)
                else:
                    args.append("%d" % ins.b)
            if ins.c is not None and f in ("23x",):
                args.append("v%d" % ins.c)
            elif ins.c is not None and ref is None:
                args.append("%d" % ins.c)
        if ref:
            args.append(ref)
        tgt = ""
        if f in ("10t", "20t", "30t"):
            tgt = " -> %04x" % (ins.off + ins.a)
        elif f in ("21t",):
            tgt = " -> %04x" % (ins.off + ins.b)
        elif f in ("22t",):
            tgt = " -> %04x" % (ins.off + ins.c)
        elif ins.name in ("packed-switch", "sparse-switch", "fill-array-data"):
            tgt = " -> payload@%04x" % (ins.off + ins.b)
        print("    %04x: %-24s %s%s" % (ins.off, ins.name, ", ".join(args), tgt),
              file=out)
    for off, p in sorted(payloads.items()):
        if p[0] == "packed":
            print("    payload@%04x packed first=%d targets=%s" %
                  (off, p[1], [("%04x" % (off + t)) for t in p[2]]), file=out)
        elif p[0] == "sparse":
            print("    payload@%04x sparse keys=%s targets=%s" %
                  (off, p[1], [("%04x" % (off + t)) for t in p[2]]), file=out)
        else:
            print("    payload@%04x array width=%d size=%d" % (off, p[1], p[2]),
                  file=out)
    print(".end method\n", file=out)


def main():
    path = sys.argv[1]
    cfilter = sys.argv[2] if len(sys.argv) > 2 else ""
    mfilter = sys.argv[3] if len(sys.argv) > 3 else ""
    d = dexlib.Dex(open(path, "rb").read())
    for c in d.classes:
        if cfilter and cfilter not in c.name:
            continue
        print(".class %s" % c.name)
        print(".super %s" % c.super)
        for i in c.interfaces:
            print(".implements %s" % i)
        for f in c.static_fields:
            print(".field static %s:%s" % (f.fid.name, f.fid.type))
        for f in c.instance_fields:
            print(".field %s:%s" % (f.fid.name, f.fid.type))
        print()
        for m in c.methods:
            if mfilter and m.mid.name != mfilter:
                continue
            dump_method(d, c, m)


if __name__ == "__main__":
    main()
