#!/usr/bin/env python3
"""Harvest the REAL GDS boot splash from the APK (user directive: do NOT
draw a made-up one -- extract what the game actually shows at boot).

The Kairosoft splash is res/iF.png in the APK: 1024x2048 portrait, navy
background, white KAIROSOFT wordmark centered.  We recompose for the
R36S's 640x480 landscape panel: same navy fill, the wordmark band cropped
out (auto bbox of non-background pixels), scaled to ~62% panel width,
centered.  Output: 640x480 24-bit BMP at ports/gamedevstory/gamedevstory/
splash.bmp (loader blits it during the boot window).

Run from anywhere:  python3 harvest_splash.py
"""
import os, sys, zipfile, io
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # GDS_Unity/
REPO = os.path.dirname(ROOT)                      # repo root
APK  = os.path.join(REPO, "APKs", "Game Dev Story_2.6.9.apk")
OUT  = os.path.join(ROOT, "ports", "gamedevstory", "gamedevstory", "splash.bmp")

def main():
    with zipfile.ZipFile(APK) as z:
        cands = []
        for n in z.namelist():
            if not n.lower().endswith(".png"):
                continue
            with z.open(n) as f:
                data = f.read()
            im = Image.open(io.BytesIO(data))
            w, h = im.size
            if h >= 2 * w and min(w, h) >= 512:   # full-screen portrait splash
                cands.append((w * h, n, data, im))
        if not cands:
            sys.exit("no full-screen splash PNG found in APK")
        cands.sort(reverse=True)
        _, name, data, im = cands[0]
        print(f"[splash] harvested {name} {im.size[0]}x{im.size[1]} from {os.path.basename(APK)}")

    im = im.convert("RGB")
    W, H = im.size
    bg = im.getpixel((5, 5))
    # bbox of pixels meaningfully different from the background
    def notbg(p):
        return max(abs(p[i] - bg[i]) for i in range(3)) > 40
    px = im.load()
    x0, x1, y0, y1 = W, 0, H, 0
    for y in range(H):
        for x in range(W):
            if notbg(px[x, y]):
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    pad = 30
    x0 = max(0, x0 - pad); y0 = max(0, y0 - pad)
    x1 = min(W, x1 + pad); y1 = min(H, y1 + pad)
    logo = im.crop((x0, y0, x1, y1))
    print(f"[splash] wordmark bbox {(x0,y0,x1,y1)} -> {logo.size[0]}x{logo.size[1]}  bg={bg}")

    PW, PH = 640, 480
    panel = Image.new("RGB", (PW, PH), bg)
    tw = int(PW * 0.62)
    th = max(1, round(logo.size[1] * tw / logo.size[0]))
    logo = logo.resize((tw, th), Image.LANCZOS)
    panel.paste(logo, ((PW - tw) // 2, (PH - th) // 2))
    panel.save(OUT, "BMP")
    print(f"[splash] wrote {OUT} ({PW}x{PH} 24-bit BMP)")

if __name__ == "__main__":
    main()
