"""Boot the game, run real frames and turn the engine's draw calls into a PNG.

Every batch played back here was produced by the shipped ARM64 code of Game
Dev Story running inside the VM: the meshes come out of the engine's own
MeshManager, the textures out of its asset decoder, the transforms out of its
Canvas/Graphics stack.  This tool is only the display.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..'))

from kairovm.game import Game                                     # noqa: E402
from kairovm import raster                                        # noqa: E402


def bbox(points):
    if not points:
        return None
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return (min(xs), min(ys), max(xs), max(ys))


def dump(batches, out=sys.stdout):
    for i, b in enumerate(batches):
        mode = b.get('mode')
        if mode == -2:
            mesh = b.get('mesh')
            mat = b.get('matrix')
            tris, pos, uv, col = raster.mesh_triangles(mesh) if mesh else ([],
                                                                           [],
                                                                           [],
                                                                           [])
            world = [raster.transform(mat, p[0], p[1],
                                      p[2] if len(p) > 2 else 0.0)
                     for p in pos]
            tex = b.get('tex')
            print('%3d  mesh   v=%-5d tri=%-5d local=%s world=%s uv=%s '
                  'tex=%s %s' %
                  (i, len(pos), len(tris),
                   fmt(bbox(pos)), fmt(bbox(world)), fmt(bbox(uv)),
                   getattr(tex, 'name', None),
                   '%dx%d' % (tex.w, tex.h) if getattr(tex, 'w', 0) else ''),
                  file=out)
            if mat:
                print('       matrix %s' % ' '.join('%.3f' % v for v in mat),
                      file=out)
        elif mode == -3:
            tex = b.get('tex')
            print('%3d  blit   rect=%s tex=%s' %
                  (i, fmt(b.get('rect')), getattr(tex, 'name', None)),
                  file=out)
        elif mode == -4:
            print('%3d  text   rect=%s %r' % (i, fmt(b.get('rect')),
                                              b.get('text')), file=out)
        elif mode == -1:
            print('%3d  clear  %s' % (i, fmt(b.get('clear'))), file=out)
        elif mode == -5:
            print('%3d  clip+  %s' % (i, fmt(b.get('clip'))), file=out)
        elif mode == -6:
            print('%3d  clip-' % i, file=out)
        else:
            v = b.get('v') or []
            tex = b.get('tex')
            print('%3d  gl%-3d  v=%-4d pos=%s uv=%s col=%s tex=%s %s' %
                  (i, mode, len(v),
                   fmt(bbox([(p[0], p[1]) for p in v])),
                   fmt(bbox([(p[3], p[4]) for p in v])),
                   fmt(v[0][5]) if v else '-',
                   getattr(tex, 'name', None),
                   '%dx%d' % (tex.w, tex.h) if getattr(tex, 'w', 0) else ''),
                  file=out)


def fmt(t):
    if t is None:
        return '-'
    return '(' + ','.join('%.1f' % v for v in t) + ')'


def inventory(h):
    """What the engine actually built on the native side this run."""
    kinds = {}
    for o in h.objects.values():
        kinds[o.kind] = kinds.get(o.kind, 0) + 1
    print('[render] native objects: %s'
          % ', '.join('%s=%d' % kv for kv in sorted(kinds.items())))
    texs = [o for o in h.objects.values()
            if o.kind in ('Texture2D', 'Texture', 'RenderTexture')]
    for t in texs[:24]:
        px = getattr(t, 'pixels', None)
        print('   tex %-28s %sx%s %s'
              % (t.name, getattr(t, 'w', '?'), getattr(t, 'h', '?'),
                 'empty' if not px or not any(px) else '%d bytes' % len(px)))
    mats = [o for o in h.objects.values() if o.kind == 'Material']
    for mm in mats[:12]:
        print('   mat %-28s %s' % (mm.name, sorted(mm.data)))
    print('   shader props: %s' % sorted(h._props))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', default='out/apk')
    ap.add_argument('--frames', type=int, default=2)
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--height', type=int, default=480)
    ap.add_argument('--platform', type=int, default=11)
    ap.add_argument('--out', default='out/frame.png')
    ap.add_argument('--every', action='store_true',
                    help='write a PNG for every frame, not just the last')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--no-flip', action='store_true')
    ap.add_argument('--verts', type=int, default=0,
                    help='print the raw vertices of the first N batches')
    ap.add_argument('--ascii', action='store_true',
                    help='print a luminance thumbnail of each frame written')
    ap.add_argument('--ascii-size', default='80x30',
                    help='thumbnail size, COLSxROWS')
    args = ap.parse_args()

    t0 = time.time()
    g = Game(args.apk, args.width, args.height, verbose=1,
             platform=args.platform)
    g.create_app()
    g.awake()
    g.post_frame()
    g.start()
    g.post_frame()

    for n in range(args.frames):
        t = time.time()
        batches = g.frame()
        print('[render] frame %d: %d batches, %d draw calls, %.0fs'
              % (g.h.frame, len(batches), g.h.draw_calls, time.time() - t))
        last = (n == args.frames - 1)
        if args.dump and last:
            print('[render] pixel matrix %s  viewport %s  clears %d'
                  % (fmt(g.h.pixel_matrix), fmt(g.h.viewport), g.h.clears))
            inventory(g.h)
            dump(batches)
            for b in batches[:args.verts]:
                if b.get('mode', -9) >= 0:
                    for v in b['v']:
                        print('     v %s' % ' '.join('%9.3f' % c
                                                     for c in v[:5]))
        if args.every or last:
            st = {}
            t = time.time()
            f = raster.render(batches, args.width, args.height,
                              flip_y=not args.no_flip, stats=st,
                              fonts=(g.h.font, g.h.font_bold))
            path = (args.out if last and not args.every
                    else '%s.%03d.png' % (args.out[:-4], g.h.frame))
            raster.write_png(path, args.width, args.height, f.rgba())
            print('[render] %s  %d triangles, %d pixels, %.0fs'
                  % (path, st.get('tris', 0), st.get('pixels', 0),
                     time.time() - t))
            if args.ascii:
                cols, _, rows = args.ascii_size.partition('x')
                print(f.ascii(int(cols), int(rows or 30)))
            for text, rect in st.get('text', [])[:20]:
                print('   text %-40r at %s' % (text, fmt(rect)))
    print('[render] total %.0fs' % (time.time() - t0))


if __name__ == '__main__':
    main()
