"""Log the arguments and results of selected internal calls, live.

`run_game.py --log-all` is too coarse to debug a single engine path: it prints
every icall the shipped code makes.  This wraps only the handlers whose
signature matches a substring, so a specific interaction between the recovered
engine and the host - GetComponent on a freshly instantiated prefab, say - can
be watched without drowning in noise.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

import kairovm.unity as U                                         # noqa: E402
from kairovm.game import Game                                     # noqa: E402


def describe(h, p):
    if not isinstance(p, int) or not p:
        return repr(p)
    bits = ['%#x' % p]
    try:
        o = h.obj(p)
    except Exception:
        o = None
    if o is not None:
        bits.append(repr(o))
        if o.kind == 'GameObject':
            bits.append('components=%r transform=%r'
                        % ([c.kind for c in o.components], o.transform))
    else:
        try:
            bits.append(h.class_name(p))
        except Exception:
            pass
    return ' '.join(bits)


def wrap(name, fn, deref):
    def w(h, this, a):
        r = fn(h, this, a)
        print('[icall] %s' % name)
        print('        this = %s' % describe(h, this))
        for i, x in enumerate(a):
            print('        a%-3d = %s' % (i, describe(h, x)))
        print('        ->     %s' % describe(h, r if isinstance(r, int) else 0))
        for off in deref:
            for i, x in enumerate(a):
                if isinstance(x, int) and x:
                    try:
                        print('        [a%d+%#x] = %#x' % (i, off, h.m.read64(x + off)))
                    except Exception:
                        pass
        sys.stdout.flush()
        return r
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--frames', type=int, default=5)
    ap.add_argument('--platform', type=int, default=11)
    ap.add_argument('--out-offsets', default='0',
                    help='byte offsets to read back from pointer args after the call')
    ap.add_argument('match', nargs='+', help='icall signature substrings')
    args = ap.parse_args()

    deref = [int(x, 0) for x in args.out_offsets.split(',') if x]
    hit = []
    for table in (U.IMPL, U.SHORT):
        for k in list(table):
            if any(mm.lower() in k.lower() for mm in args.match):
                table[k] = wrap(k, table[k], deref)
                hit.append(k)
    print('[trace] wrapping %d handlers:' % len(hit))
    for k in hit:
        print('    %s' % k)

    g = Game(args.apk, 640, 480, verbose=1, platform=args.platform)
    g.create_app()
    t = time.time()
    g.awake()
    print('[trace] Awake %.0fs' % (time.time() - t))
    g.post_frame()
    g.start()
    g.post_frame()
    for _ in range(args.frames):
        g.frame(gui=True)
        print('[trace] frame %d: %d batches' % (g.h.frame, len(g.h.gl)))
        sys.stdout.flush()
        if g.s.quit:
            break


if __name__ == '__main__':
    main()
