#!/usr/bin/env python3
"""make_osk_font.py -- rasterize the OSK font atlas + generate the single
source of truth for the OSK (0.95.10 v3: monochromatic, console-neutral).

v3 user feedback (device-tested 0.95.9) drove this round:

  * Selection is back to a FOCUS RING around the key (the 0.95.9 inverted
    white face read as "odd"; white is now reserved for CAPS LOCK, and
    the ring fully encloses the key + its drop shadow, fixing "outline
    surrounds the sides of the shadow but not underneath").
  * Caps lock = the Shift key turns WHITE with dark text (user: "maybe
    for the caps button, it can turn white"); one-shot shift = lit gray.
  * Symbols page is FLAT: all 32 ASCII punctuation glyphs fill a 4x8
    grid (user: "barely anything there ... there's so much room").
    Shift PAIRS moved to the LETTERS page: digits get !@#$%^&*() and the
    bottom-row punctuation gets its physical-keyboard partners (user:
    "why not have the shift symbols on the actual letters page").
  * Panel moved down 8px -- it sat too high, now truly centred.
  * Scrim 0.68 -> 0.74 ("darken just a tad bit more").
  * Badges unified: pills (START/L1/R1/SEL) are light-face + dark text
    like the ABXY circles (were dark-face + light text); SEL pill moved
    from the lonely bottom band up to the title band next to n/max.
  * Max-length error: counter CUTS to red then fades back + a stronger
    shake (user: "not as noticeable ... cut to red then fade back") --
    the one sanctioned colour exception, requested by the user.
  * Title text and the n/max counter now share one vertical centre.

v2 (0.95.9) basics that still hold: pure grayscale family, grid-aligned
function row (2u/2u/3u/1u/2u, 8px gaps both axes), bottom-row punct,
title-case labels, "#+="/"ABC" page key like Nintendo's, DejaVu Sans
Book, badges instead of a footer legend, smooth panel gradient.

Outputs:
  ports/gamedevstory/gamedevstory/osk_font.rgba  (320x320 RGBA atlas, runtime-loaded)
  loader_ref/osk_font_data.h                     (advances + metrics, compiled in)
  loader_ref/osk_layout.h                        (palette + panel + key tables, compiled in)
  mock_osk.png                                   (rough preview; preview_osk_*.png from
                                                  the real C code is the pixel-true one)
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_RGBA = os.path.join(ROOT, 'ports/gamedevstory/gamedevstory/osk_font.rgba')
OUT_FONTH = os.path.join(ROOT, 'loader_ref/osk_font_data.h')
OUT_LAY = os.path.join(ROOT, 'loader_ref/osk_layout.h')
OUT_MOCK = os.path.join(ROOT, 'mock_osk.png')

TTF = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
PX = 24            # nominal glyph px in the atlas (24 keeps tall glyphs inside 32px cells)
CELL = 32          # atlas cell
COLS = 10          # 95 glyphs 0x20..0x7E -> 10 cols
AW = COLS * CELL
AH = 10 * CELL

# ---------------------------------------------------------------- layout (640x480)
ACT_CHAR, ACT_SHIFT, ACT_SYM, ACT_SPACE, ACT_BKSP, ACT_DONE = range(6)

L = {}
L['panel'] = (30, 42, 580, 396)                    # x, y, w, h (v-centred: 42/42 margins)
L['panel_top'] = (0x2e, 0x2e, 0x34)                # smooth gradient ends (C lerps per row)
L['panel_bot'] = (0x14, 0x14, 0x18)
L['panel_hi'] = (0x55, 0x55, 0x5f)                 # top highlight line
L['panel_lo'] = (0x0b, 0x0b, 0x0e)                 # bottom/side shadow line
L['title_c'] = (0xda, 0xda, 0xe0)
L['dim_c'] = (0xa4, 0xa4, 0xab)
L['box_border'] = (0x0a, 0x0a, 0x0c)
L['box_fill'] = (0x12, 0x12, 0x15)
L['text_c'] = (0xf4, 0xf4, 0xf6)
L['sel_edge'] = (0xff, 0xff, 0xff)                 # focus ring (fully encloses key)
L['key_face'] = (0x3c, 0x3c, 0x44)
L['key_edge'] = (0x57, 0x57, 0x5f)
L['key_lo'] = (0x15, 0x15, 0x1a)                   # bottom bevel shadow
L['fn_face'] = (0x33, 0x33, 0x3a)
L['done_face'] = (0x58, 0x58, 0x62)
L['shift_face'] = (0x7a, 0x7a, 0x84)               # one-shot shift = lit gray
L['caps_face'] = (0xe2, 0xe2, 0xe8)                # caps lock = WHITE key (user ask)
L['badge_face'] = (0xe2, 0xe2, 0xe8)               # face-button circles (light)
L['badge_text'] = (0x17, 0x17, 0x1b)
L['pill_face'] = (0xe2, 0xe2, 0xe8)                # pills now light like the circles
L['pill_edge'] = (0x6f, 0x6f, 0x78)                # hairline around the light pill
L['pill_text'] = (0xc9, 0xc9, 0xcf)                # light text for DARK badges (caps key)
L['caret'] = (0xff, 0xff, 0xff)
L['err'] = (0xe8, 0x2a, 0x20)                      # maxlen flash; sanctioned red exception
L['textbox'] = (56, 92, 528, 42)                   # x,y,w,h (border incl.)
L['title_pos'] = (58, 67)                          # x, BAND-CENTRE y (title & counter)

KX0, KW, KGAP, KH, VGAP = 44, 48, 8, 48, 8         # 8px gaps BOTH axes (user note)
ROWY = [150, 206, 262, 318]                        # +8 vs 0.95.9 (panel re-centre)

letters_rows = [           # bottom-right dead zone gets common punctuation
    "1234567890",
    "qwertyuiop",
    "asdfghjkl'",
    "zxcvbnm,.-",
]
# physical-keyboard shift pairs, on the LETTERS page (user v3 request):
# digits -> shifted symbols, bottom-row punct -> its shift partners.
# Letters pair with their own uppercase.  Caps lock uppercases letters
# ONLY (like a real Caps Lock); one-shot shift applies the whole layer.
pairs = {
    '1': '!', '2': '@', '3': '#', '4': '$', '5': '%',
    '6': '^', '7': '&', '8': '*', '9': '(', '0': ')',
    "'": '"', ',': '<', '.': '>', '-': '_',
}
sym_rows = [               # FLAT: every printable ASCII punct, one key each,
    "!@#$%^&*",            # 4x8 = 32 keys exactly fills the grid (v3)
    "()-_=+`~",
    "[]{}\\|;:",
    "'\",<.>/?",
]
SX0, SW, SGAP = 44, 62, 8                          # symbols grid: 8 x 62px = same span

FN_Y = 374
fn_keys = [   # label, x, y, w, action -- grid units: 2u+2u+3u+1u+2u, 8px gaps
    ("Shift", 44, FN_Y, 104, ACT_SHIFT),
    ("#+=",   156, FN_Y, 104, ACT_SYM),
    ("Space", 268, FN_Y, 160, ACT_SPACE),
    ("Del",   436, FN_Y, 48, ACT_BKSP),
    ("Done",  492, FN_Y, 104, ACT_DONE),
]

# ---------------------------------------------------------------- font atlas
def build_atlas():
    font = ImageFont.truetype(TTF, PX)
    asc, desc = font.getmetrics()
    top_pad = 3
    atlas = Image.new('RGBA', (AW, AH), (255, 255, 255, 0))
    dr = ImageDraw.Draw(atlas)
    adv = [0.0] * 96
    for ci, cp in enumerate(range(0x20, 0x7F)):
        c = chr(cp)
        col, row = ci % COLS, ci // COLS
        ox, oy = col * CELL, row * CELL
        if c != ' ':
            dr.text((ox + 1, oy + top_pad), c, font=font, fill=(255, 255, 255, 255))
        adv[ci] = font.getlength(c)
        bb = font.getbbox(c)
        if c != ' ' and (bb[3] + top_pad > CELL or bb[2] + 1 > CELL):
            print(f"WARN: glyph {c!r} bbox {bb} exceeds cell")
    rgba = atlas.tobytes()
    os.makedirs(os.path.dirname(OUT_RGBA), exist_ok=True)
    with open(OUT_RGBA, 'wb') as f:
        f.write(rgba)
    space_w = font.getlength(' ')   # natural space advance (user: too wide before)
    return asc, desc, top_pad, adv, space_w

# ---------------------------------------------------------------- header emit
def fl(c):
    return "%.5ff, %.5ff, %.5ff" % (c[0] / 255, c[1] / 255, c[2] / 255)

def emit_font_h(asc, top_pad, adv, space_w):
    with open(OUT_FONTH, 'w') as f:
        f.write("/* generated by tools/make_osk_font.py -- do not edit by hand */\n")
        f.write("#define OVK_AW %d\n#define OVK_AH %d\n#define OVK_CELL %d\n"
                "#define OVK_COLS %d\n#define OVK_ASC_CELL %.1ff   /* cell-origin to baseline */\n"
                "#define OVK_PX %d\n#define OVK_SPACE_W %.3ff\n" %
                (AW, AH, CELL, COLS, top_pad + asc, PX, space_w))
        f.write("static const float ovk_adv[96] = {\n")
        for i in range(0, 96, 8):
            f.write("    " + ", ".join("%.3ff" % a for a in adv[i:i + 8]) + ",\n")
        f.write("};\n")

def emit_key(f, label, x, y, w, h, lo, hi, act):
    # C-escape every field: lo/hi go in single quotes, label in double.
    lab = label.replace('\\', '\\\\').replace('"', '\\"')
    f.write('    { %d, %d, %d, %d, \'%s\', \'%s\', %s, "%s" },\n' %
            (x, y, w, h, lo.replace('\\', '\\\\').replace("'", "\\'"),
             hi.replace('\\', '\\\\').replace("'", "\\'"),
             "OVKA_" + {ACT_CHAR: 'CHAR', ACT_SHIFT: 'SHIFT', ACT_SYM: 'SYM',
                        ACT_SPACE: 'SPACE', ACT_BKSP: 'BKSP', ACT_DONE: 'DONE'}[act],
             lab))

def emit_layout_h():
    with open(OUT_LAY, 'w') as f:
        f.write("/* generated by tools/make_osk_font.py -- do not edit by hand */\n")
        f.write("enum { OVKA_CHAR, OVKA_SHIFT, OVKA_SYM, OVKA_SPACE, OVKA_BKSP, OVKA_DONE };\n")
        f.write("typedef struct { short x, y, w, h; char lo, hi; unsigned char act; const char *label; } ovk_key_t;\n")
        for name, c in L.items():
            if isinstance(c, tuple) and len(c) == 3:
                f.write("#define OVKC_%-10s %s\n" % (name.upper(), fl(c)))
        px, py, pw, ph = L['panel']
        f.write("#define OVK_PANEL %d, %d, %d, %d\n" % (px, py, pw, ph))
        bx, by, bw, bh = L['textbox']
        f.write("#define OVK_BOX %d, %d, %d, %d\n" % (bx, by, bw, bh))
        tx, ty = L['title_pos']
        f.write("#define OVK_TITLE_POS %d, %d   /* x, band-centre y */\n" % (tx, ty))
        f.write("static const ovk_key_t ovk_letters[] = {\n")
        for ri, row in enumerate(letters_rows):
            for ci, c in enumerate(row):
                hi = pairs.get(c, c.upper() if c.isalpha() else c)
                emit_key(f, c, KX0 + ci * (KW + KGAP), ROWY[ri], KW, KH,
                         c, hi, ACT_CHAR)
        f.write("};\n")
        f.write("static const ovk_key_t ovk_symbols[] = {\n")
        for ri, row in enumerate(sym_rows):
            for ci, c in enumerate(row):
                emit_key(f, c, SX0 + ci * (SW + SGAP), ROWY[ri], SW, KH,
                         c, c, ACT_CHAR)
        f.write("};\n")
        f.write("static const ovk_key_t ovk_func[] = {\n")
        for lab, x, y, w, act in fn_keys:
            emit_key(f, lab, x, y, w, KH, '0', '0', act)
        f.write("};\n")
        f.write("#define OVK_N_LETTERS %d\n#define OVK_N_SYMBOLS %d\n#define OVK_N_FUNC %d\n"
                % (sum(len(r) for r in letters_rows), sum(len(r) for r in sym_rows),
                   len(fn_keys)))

# ---------------------------------------------------------------- mock preview
def mock(asc, top_pad):
    font = ImageFont.truetype(TTF, PX)
    S = 2  # draw at 2x
    im = Image.new('RGB', (640 * S, 480 * S), (0x55, 0x33, 0x7a))  # game-ish bg
    dr = ImageDraw.Draw(im)
    def rr(x, y, w, h, c):
        dr.rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], fill=c)
    ov = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ImageDraw.Draw(ov).rectangle([0, 0, im.size[0] - 1, im.size[1] - 1],
                                 fill=(0, 0, 0, int(0.74 * 255)))
    im = Image.alpha_composite(im.convert('RGBA'), ov).convert('RGB')
    dr = ImageDraw.Draw(im)
    px, py, pw, ph = L['panel']
    for yy in range(ph):              # smooth gradient like the C side
        t = yy / (ph - 1)
        c = tuple(round(L['panel_top'][i] + (L['panel_bot'][i] - L['panel_top'][i]) * t)
                  for i in range(3))
        rr(px, py + yy, pw, 1, c)
    rr(px, py, pw, 2, L['panel_hi'])
    rr(px, py + ph - 2, pw, 2, L['panel_lo'])
    def text(x, y_baseline, s, c, scale=1.0):
        pen = x
        for ch in s:
            f2 = font if scale == 1.0 else ImageFont.truetype(TTF, max(6, int(PX * scale)))
            a = font.getlength(ch) * scale if scale == 1.0 else f2.getlength(ch)
            if ch != ' ':
                dr.text((pen * S, (y_baseline - (top_pad + asc) * scale) * S), ch,
                        font=f2, fill=c)
            pen += a
        return pen
    tx, ty = L['title_pos']                        # ty = band centre (v3)
    text(tx, ty + 9, "Company name", L['title_c'], 0.75)
    text(530, ty + 7, "13/14", L['dim_c'], 0.58)
    bx, by, bw, bh = L['textbox']
    rr(bx - 2, by - 2, bw + 4, bh + 4, L['box_border'])
    rr(bx, by, bw, bh, L['box_fill'])
    t = "Sunny Studios"
    endx = text(bx + 16, by + 30, t, L['text_c'], 0.846)
    rr(int(endx) + 2, by + 8, 3, bh - 16, L['caret'])
    sel = (1, 5)
    for ri, row in enumerate(letters_rows):
        for ci, c in enumerate(row):
            x, y = KX0 + ci * (KW + KGAP), ROWY[ri]
            is_sel = (ri, ci) == sel
            face = L['key_face']
            edge = L['key_edge']
            txt = L['text_c']
            if is_sel:                             # focus ring encloses shadow too
                rr(x - 2, y - 2, KW + 4, KH + 6, L['sel_edge'])
            rr(x + 1, y + 2, KW, KH, L['key_lo'])
            rr(x, y, KW, KH, face)
            rr(x, y, KW, 2, edge)
            rr(x, y + KH - 2, KW, 2, L['key_lo'])
            f3 = ImageFont.truetype(TTF, int(PX * 0.85))
            text(x + (KW - f3.getlength(c)) / 2, y + KH / 2 + 8, c, txt, 0.85)
    for lab, x, y, w, act in fn_keys:
        face = L['done_face'] if act == ACT_DONE else L['fn_face']
        edge = L['key_edge']
        rr(x + 1, y + 2, w, KH, L['key_lo'])
        rr(x, y, w, KH, face)
        rr(x, y, w, 2, edge)
        rr(x, y + KH - 2, w, 2, L['key_lo'])
        f2 = ImageFont.truetype(TTF, int(PX * 0.66))
        tw = f2.getlength(lab)
        text(x + (w - tw) / 2, y + KH / 2 + 7, lab, L['text_c'], 0.66)
    im = im.resize((640, 480))
    im.save(OUT_MOCK)
    print("mock:", OUT_MOCK)

def main():
    asc, desc, top_pad, adv, space_w = build_atlas()
    print("atlas:", OUT_RGBA, "asc=%d desc=%d" % (asc, desc))
    emit_font_h(asc, top_pad, adv, space_w)
    emit_layout_h()
    print("headers:", OUT_FONTH, "and", OUT_LAY)
    mock(asc, top_pad)

if __name__ == '__main__':
    main()
