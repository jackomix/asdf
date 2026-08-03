"""Dump the live static fields of managed types after the engine has booted.

The shipped game configures itself from `kairo.common.cfg.Config`, and the
storage backend it picks (SharedPreferences vs files) falls out of those
values, so being able to read them out of the running runtime - rather than
guessing from metadata - is what tells us how the real app behaves.
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
}


def resolve(g, spec):
    ns, _, name = spec.rpartition('.')
    try:
        return g.s.find_class(ns, name)
    except KeyError:
        print('[statics] %s: not found' % spec)
        return 0


def dump(g, spec, k):
    rt, m = g.rt, g.m
    if not k:
        return
    rt.runtime_class_init(k)
    sf = m.read64(k + 0xb8)
    print('[statics] %s  klass=%#x static_fields=%#x' % (spec, k, sf))
    for f in rt.fields(k):
        off = rt.field_offset(f)
        nm = rt.field_name(f)
        ftype = rt.call('il2cpp_field_get_type', f)
        try:
            tname = rt._s(rt.call('il2cpp_type_get_name', ftype)) or '?' \
                if ftype else '?'
        except Exception:
            tname = '?'
        # static fields are indexed off klass->static_fields
        if not sf or off > 0x10000:
            continue
        try:
            raw = m.read(sf + off, 8)
        except Exception:
            continue
        val = None
        if tname in PRIMS:
            fmt, n = PRIMS[tname]
            val = struct.unpack('<' + fmt, raw[:n])[0]
        elif tname == 'System.String':
            p = struct.unpack('<Q', raw)[0]
            val = repr(m.guest_string(p)) if p else 'null'
        else:
            p = struct.unpack('<Q', raw)[0]
            val = ('%#x %s' % (p, m.probe_object(p) or '')) if p else 'null'
        print('    %#06x  %-28s %-22s %s' % (off, nm, tname.split('.')[-1], val))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--platform', type=int, default=11)
    ap.add_argument('--awake', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('types', nargs='+')
    args = ap.parse_args()

    g = Game(args.apk, 640, 480, verbose=1, platform=args.platform)
    g.create_app()
    # resolve up front: name lookups are not safe to make late in a run
    klasses = [(t, resolve(g, t)) for t in args.types]
    if args.awake or args.frames:
        g.awake()
        g.post_frame()
        g.start()
        g.post_frame()
    for _ in range(args.frames):
        g.frame()
    for t, k in klasses:
        dump(g, t, k)


if __name__ == '__main__':
    main()
