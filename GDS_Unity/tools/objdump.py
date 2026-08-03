"""Walk live managed objects and print their real field values.

`statics.py` can only see class statics; most of the engine's state hangs off
an instance (kairo.unity.ui.Canvas.graphics_ is the whole 2D pipeline, for
example).  Give this tool a dotted path rooted at a type and it follows the
field chain through the running heap, then prints every field of whatever it
lands on - names and offsets from the shipped metadata, values from VM memory.

    python3 tools/objdump.py --frames 60 kairo.unity.ui.Canvas.graphics_
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from kairovm.game import Game                                     # noqa: E402

PRIMS = {
    'System.Boolean': ('?', 1), 'System.Byte': ('B', 1),
    'System.SByte': ('b', 1), 'System.Char': ('H', 2),
    'System.Int16': ('h', 2), 'System.UInt16': ('H', 2),
    'System.Int32': ('i', 4), 'System.UInt32': ('I', 4),
    'System.Int64': ('q', 8), 'System.UInt64': ('Q', 8),
    'System.Single': ('f', 4), 'System.Double': ('d', 8),
    'System.IntPtr': ('Q', 8), 'System.UIntPtr': ('Q', 8),
}


def type_name(rt, f):
    try:
        t = rt.call('il2cpp_field_get_type', f)
        return (rt._s(rt.call('il2cpp_type_get_name', t)) or '?') if t else '?'
    except Exception:
        return '?'


FIELD_STATIC = 0x0010
FIELD_LITERAL = 0x0040


def field_table(rt, klass):
    """[(name, offset, typename, flags)] for klass and every base class."""
    out = []
    k = klass
    seen = set()
    while k and k not in seen:
        seen.add(k)
        for f in rt.fields(k):
            try:
                flags = rt.call('il2cpp_field_get_flags', f)
            except Exception:
                flags = 0
            out.append((rt.field_name(f), rt.field_offset(f),
                        type_name(rt, f), flags))
        k = rt.call('il2cpp_class_get_parent', k)
    return out


def read_value(g, base, off, tname, static):
    m = g.m
    try:
        raw = m.read(base + off, 8)
    except Exception:
        return '<unmapped>'
    if tname in PRIMS:
        fmt, n = PRIMS[tname]
        return struct.unpack('<' + fmt, raw[:n])[0]
    p = struct.unpack('<Q', raw)[0]
    if not p:
        return 'null'
    if tname == 'System.String':
        try:
            return repr(m.guest_string(p))
        except Exception:
            return '%#x <bad string>' % p
    try:
        return '%#x %s' % (p, m.probe_object(p) or '')
    except Exception:
        return '%#x' % p


def resolve_root(g, spec):
    """Longest type-name prefix of `spec` that exists, plus the field trail."""
    parts = spec.split('.')
    for cut in range(len(parts) - 1, 0, -1):
        full = '.'.join(parts[:cut])
        ns, _, name = full.rpartition('.')
        try:
            k = g.s.find_class(ns, name)
        except KeyError:
            continue
        if k:
            return full, k, parts[cut:]
    raise SystemExit('no type found in %r' % spec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--platform', type=int, default=11)
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('paths', nargs='+')
    args = ap.parse_args()

    g = Game(args.apk, 640, 480, verbose=1, platform=args.platform)
    g.create_app()
    roots = [resolve_root(g, p) for p in args.paths]
    g.awake()
    g.post_frame()
    g.start()
    g.post_frame()
    for _ in range(args.frames):
        g.frame()

    rt = g.rt
    for (tname, klass, trail), spec in zip(roots, args.paths):
        rt.runtime_class_init(klass)
        base = g.m.read64(klass + 0xb8)                 # static_fields
        static = True
        cur_klass = klass
        ok = True
        for step in trail:
            tbl = field_table(rt, cur_klass)
            hit = [e for e in tbl
                   if e[0] == step and bool(e[3] & FIELD_STATIC) == static]
            if not hit:
                hit = [e for e in tbl if e[0] == step]
            if not hit or not base:
                print('[obj] %s: no field %r' % (spec, step))
                ok = False
                break
            _n, off, ftype, _fl = hit[0]
            ptr = g.m.read64(base + off)
            if not ptr:
                print('[obj] %s: %s is null' % (spec, step))
                ok = False
                break
            base = ptr
            static = False
            cur_klass = rt.call('il2cpp_object_get_class', ptr)
        if not ok:
            continue
        print('[obj] %s  @ %#x  class=%#x %s'
              % (spec, base, cur_klass,
                 rt._s(rt.call('il2cpp_class_get_name', cur_klass)) or ''))
        for name, off, ftype, flags in field_table(rt, cur_klass):
            if off > 0x10000 or (flags & (FIELD_STATIC | FIELD_LITERAL)
                                 and not static):
                continue
            print('    %#06x  %-30s %-24s %s'
                  % (off, name, ftype.split('.')[-1],
                     read_value(g, base, off, ftype, static)))


if __name__ == '__main__':
    main()
