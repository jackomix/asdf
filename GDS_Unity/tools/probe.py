"""Break on arbitrary libil2cpp.so file offsets while the real engine runs.

The static disassembly tells us which branch of a shipped method ends in a
null-reference throw, but not which guard actually fires at run time.  This
boots the game exactly like `run_game.py` and installs code hooks on the
addresses we care about, dumping the registers and the pointed-at objects so
the failing field can be identified in the running engine rather than guessed.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from kairovm.game import Game                                     # noqa: E402
from unicorn import UC_HOOK_CODE, arm64_const as A64              # noqa: E402

LIB_BASE = 0x40000000
REGS = [getattr(A64, 'UC_ARM64_REG_X%d' % i) for i in range(29)]


def describe(m, p):
    if not p:
        return 'null'
    bits = ['%#x' % p]
    o = m.probe_object(p)
    if o:
        bits.append(o)
    s = m.guest_string(p)
    if s:
        bits.append(repr(s[:60]))
    return ' '.join(bits)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--frames', type=int, default=6)
    ap.add_argument('--platform', type=int, default=11)
    ap.add_argument('--regs', default='', help='comma list of x-regs to dump')
    ap.add_argument('--deref', default='',
                    help='comma list of REG+OFF chains, eg x19+0x18+0x10')
    ap.add_argument('--limit', type=int, default=4, help='hits per address')
    ap.add_argument('at', nargs='+', help='libil2cpp.so file offsets (hex)')
    args = ap.parse_args()

    g = Game(args.apk, 640, 480, verbose=1, platform=args.platform)
    m = g.m
    want = [int(a, 16) for a in args.at]
    regs = [int(r.lstrip('xX')) for r in args.regs.split(',') if r]
    chains = [c for c in args.deref.split(',') if c]
    hits = {}

    def cb(uc, address, size, ud):
        off = address - LIB_BASE
        n = hits.get(off, 0) + 1
        hits[off] = n
        if n > args.limit:
            return
        print('[probe] %#x hit %d  (frame %d)' % (off, n, g.h.frame))
        for i in regs:
            print('        x%-2d = %s' % (i, describe(m, uc.reg_read(REGS[i]))))
        for c in chains:
            parts = c.split('+')
            p = uc.reg_read(REGS[int(parts[0].lstrip('xX'))])
            path = parts[0]
            ok = True
            for step in parts[1:]:
                if not p:
                    ok = False
                    break
                try:
                    p = m.read64(p + int(step, 16))
                except Exception:
                    ok = False
                    break
                path += '+' + step
            print('        %-16s = %s' % (c, describe(m, p) if ok else 'null (broken chain)'))
        sys.stdout.flush()

    for off in want:
        a = LIB_BASE + off
        m.uc.hook_add(UC_HOOK_CODE, cb, begin=a, end=a + 3)
    print('[probe] armed at %s' % ', '.join('%#x' % a for a in want))

    g.create_app()
    t = time.time()
    g.awake()
    print('[probe] Awake %.0fs' % (time.time() - t))
    g.post_frame()
    g.start()
    g.post_frame()
    for i in range(args.frames):
        g.frame(gui=True)
        print('[probe] frame %d: %d batches' % (g.h.frame, len(g.h.gl)))
        sys.stdout.flush()
        if g.s.quit:
            break
    print('[probe] hits: %r' % {('%#x' % k): v for k, v in hits.items()})


if __name__ == '__main__':
    main()
