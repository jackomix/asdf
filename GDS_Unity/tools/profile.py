"""Where does the shipped engine spend its time?

Runs real frames and counts the managed methods the trace hook sees, so a
stall in the loader shows up as the loop it is actually stuck in rather than
as "the game is still on the loading screen".
"""
import argparse
import collections
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..'))

from kairovm.game import Game                                     # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--frames', type=int, default=6)
    ap.add_argument('--rounds', type=int, default=64)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--depth', type=int, default=400000)
    args = ap.parse_args()

    g = Game(args.apk, 640, 480, verbose=1, trace_depth=args.depth, trace=True)
    g.thread_rounds = args.rounds
    g.create_app()
    g.awake()
    g.post_frame()
    g.start()
    g.post_frame()
    for _ in range(args.frames):
        t = time.time()
        n0 = len(g.m.method_trace)
        g.frame()
        print('[prof] frame %d: %.1fs  +%d calls  %s'
              % (g.h.frame, time.time() - t, len(g.m.method_trace) - n0,
                 g.m.thread_report()))
        sys.stdout.flush()
    c = collections.Counter(g.m.method_trace)
    print('[prof] %d traced calls, %d distinct' % (sum(c.values()), len(c)))
    for name, n in c.most_common(args.top):
        print('  %8d  %s' % (n, name))


if __name__ == '__main__':
    main()
