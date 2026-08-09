#!/usr/bin/env python3
"""Rasterize the primitives recorded by tools/render_osk.c with the real
atlas -> pixel-true preview of the actual C draw path.
Run after: gcc -O1 -o /tmp/render_osk tools/render_osk.c -lm &&
           (cd /tmp && /tmp/render_osk)"""
from PIL import Image
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AW = AH = 320
ATLAS = os.path.join(ROOT, 'ports/gamedevstory/gamedevstory/osk_font.rgba')
atlas = Image.frombytes('RGBA', (AW, AH), open(ATLAS, 'rb').read())

def render(prim_path, out_name):
    im = Image.new('RGBA', (640, 480), (90, 60, 130, 255))   # game-ish purple bg
    for line in open(prim_path):
        p = line.split()
        if not p or p[0] == 'END':
            continue
        if p[0] == 'RECT':
            x, y, w, h = (float(v) for v in p[1:5])
            r, g, b = (float(v) for v in p[5:8])
            tile = Image.new('RGBA', (max(1, round(w)), max(1, round(h))),
                             (int(r*255), int(g*255), int(b*255), 255))
            im.alpha_composite(tile, (round(x), round(y)))
        elif p[0] == 'RECTA':
            x, y, w, h = (float(v) for v in p[1:5])
            r, g, b, a = (float(v) for v in p[5:9])
            tile = Image.new('RGBA', (max(1, round(w)), max(1, round(h))),
                             (int(r*255), int(g*255), int(b*255), int(a*255)))
            im.alpha_composite(tile, (round(x), round(y)))
        elif p[0] == 'QUAD':
            x, y, w, h = (float(v) for v in p[1:5])
            u0, v0, u1, v1 = (float(v) for v in p[5:9])
            r, g, b = (float(v) for v in p[9:12])
            cell = atlas.crop((round(u0*AW), round(v0*AH),
                               round(u1*AW), round(v1*AH)))
            cell = cell.resize((max(1, round(w)), max(1, round(h))),
                               Image.BILINEAR)
            tint = Image.new('RGBA', cell.size,
                             (int(r*255), int(g*255), int(b*255), 255))
            alpha = cell.getchannel('A')
            tint.putalpha(alpha)
            im.alpha_composite(tint, (round(x), round(y)))
    out_path = os.path.join(ROOT, out_name)
    im.convert('RGB').save(out_path)
    print('wrote', out_path)

render('/tmp/osk2_scene1.prim', 'preview_osk_letters.png')
render('/tmp/osk2_scene2.prim', 'preview_osk_symbols.png')
render('/tmp/osk2_scene3.prim', 'preview_osk_caps.png')
render('/tmp/osk2_scene4.prim', 'preview_osk_shift.png')
