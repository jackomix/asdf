"""Show what the guest's threads are doing while the game runs.

The shipped engine loads its data on background threads (kairo.unity's
Offscreen/Image work and the boot loader both spawn one).  Those are green
threads inside the VM, so if the player loop never lets them run the game
sits on its loading screen forever - this makes that visible.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..'))

from kairovm.game import Game                                     # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--frames', type=int, default=10)
    ap.add_argument('--pump', action='store_true',
                    help='let ready threads run between frames')
    args = ap.parse_args()

    g = Game(args.apk, 640, 480, verbose=1)
    g.create_app()
    g.awake()
    g.post_frame()
    g.start()
    g.post_frame()
    print('[thr] after Start: %s' % g.m.thread_report())
    for i in range(args.frames):
        g.frame()
        if args.pump:
            n = g.m.pump_threads(8)
            print('[thr] frame %d: pumped %d  %s'
                  % (g.h.frame, n, g.m.thread_report()))
        else:
            print('[thr] frame %d: %s' % (g.h.frame, g.m.thread_report()))
    print('[thr] spurious wakes: %d' % getattr(g.m, '_spurious', 0))


if __name__ == '__main__':
    main()
