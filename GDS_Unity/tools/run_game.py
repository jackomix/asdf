"""Boot the game and drive the recovered engine through real frames."""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from kairovm.game import Game                                     # noqa: E402
from kairovm.machine import GuestError                            # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--frames', type=int, default=1)
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--height', type=int, default=480)
    ap.add_argument('--no-awake', action='store_true')
    ap.add_argument('--no-gui', action='store_true')
    ap.add_argument('--log-all', action='store_true')
    ap.add_argument('--platform', type=int, default=11,
                    help='RuntimePlatform: 2=Windows 11=Android 13=Linux')
    ap.add_argument('-v', '--verbose', action='count', default=1)
    args = ap.parse_args()

    t0 = time.time()
    g = Game(args.apk, args.width, args.height, verbose=args.verbose,
             log_all=args.log_all, platform=args.platform)
    g.create_app()
    print('[run] type chain: %s' % g.hierarchy())

    if not args.no_awake:
        print('[run] --- Awake ---')
        t = time.time()
        g.awake()
        print('[run] Awake done in %.0fs (%d icalls)'
              % (time.time() - t, sum(g.h.counts.values())))
        g.post_frame()
        print('[run] --- Start ---')
        t = time.time()
        g.start()
        print('[run] Start done in %.0fs' % (time.time() - t))
        g.post_frame()

    for i in range(args.frames):
        t = time.time()
        g.frame(gui=not args.no_gui)
        print('[run] frame %d: %d batches, %d draw calls, %.0fs'
              % (g.h.frame, len(g.h.gl), g.h.draw_calls, time.time() - t))
        if g.s.quit:
            break

    print('[run] total %.0fs' % (time.time() - t0))
    g.report()
    if g.h.trace:
        print('[run] first unimplemented calls, in order:')
        seen = []
        for n in g.h.trace:
            if n not in seen:
                seen.append(n)
        for n in seen[:60]:
            print('    %s' % n)
    if g.h.resource_requests:
        print('[run] Resources.Load requests: %r' % g.h.resource_requests[:40])


if __name__ == '__main__':
    main()
