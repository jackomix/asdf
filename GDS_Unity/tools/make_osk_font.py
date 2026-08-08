#!/usr/bin/env python3
"""make_osk_font.py -- rasterize the OSK font atlas + generate the single
source of truth for the OSK (0.95.9 v2: monochromatic, console-neutral).

v2 user feedback drove the redesign, cross-checked against real console
OSKs (Xbox gamepad layout 2024, Steam Deck Big Picture keyboard, Switch):

  * NO colour anywhere -- pure grayscale so it sits neutrally over every
    Kairosoft game (user: "it should be monochromatic ... very
    aesthetically agnostic").
  * Grid-aligned function row: Shift/#+= = 2u, Space = 3u, Del = 1u,
    Done = 2u, same 8px gaps as the letter grid (user: "space bar 3.5
    key width ... kind of weird").
  * Horizontal key gap = vertical gap (8px).
  * Common punctuation fills the bottom-right dead zone (',', '.', '-',
    apostrophe) -- user: "that's where the quotes and . can be".
  * Symbols page = physical-keyboard shift PAIRS (Deck/Xbox style: small
    glyph above, big glyph below), so shift MEANS something there
    (user: "shift doesn't do anything on the symbols page").
  * Title-case labels (Shift/Space/Del/Done, "#+=" like Nintendo's),
    no ALL-CAPS assault; title displayed as the game sends it.
  * DejaVu Sans (Book, not Bold) -- user: "text is a bit too bold".
  * No footer legend: controller button glyphs are drawn as badges on the
    keys themselves (user's suggestion; Xbox does exactly this).
  * Smooth per-pixel panel gradient (not 4 strips), 0.68 scrim.

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
L['panel'] = (30, 34, 580, 396)                    # x, y, w, h
L['panel_top'] = (0x2e, 0x2e, 0x34)                # smooth gradient ends (C lerps per row)
L['panel_bot'] = (0x14, 0x14, 0x18)
L['panel_hi'] = (0x55, 0x55, 0x5f)                 # top highlight line
L['panel_lo'] = (0x0b, 0x0b, 0x0e)                 # bottom/side shadow line
L['title_c'] = (0xda, 0xda, 0xe0)
L['dim_c'] = (0xa4, 0xa4, 0xab)
L['box_border'] = (0x0a, 0x0a, 0x0c)
L['box_fill'] = (0x12, 0x12, 0x15)
L['text_c'] = (0xf4, 0xf4, 0xf6)
L['sel_face'] = (0xe2, 0xe2, 0xe8)                 # inverted selection (high contrast, mono)
L['sel_edge'] = (0xff, 0xff, 0xff)
L['sel_text'] = (0x17, 0x17, 0x1b)
L['key_face'] = (0x3c, 0x3c, 0x44)
L['key_edge'] = (0x57, 0x57, 0x5f)
L['key_lo'] = (0x15, 0x15, 0x1a)                   # bottom bevel shadow
L['fn_face'] = (0x33, 0x33, 0x3a)
L['done_face'] = (0x58, 0x58, 0x62)
L['shift_face'] = (0x6a, 0x6a, 0x74)
L['badge_face'] = (0xe2, 0xe2, 0xe8)               # face-button circles (light)
L['badge_text'] = (0x17, 0x17, 0x1b)
L['pill_face'] = (0x24, 0x24, 0x2b)                # shoulder/start pills (dark)
L['pill_edge'] = (0x6f, 0x6f, 0x78)
L['pill_text'] = (0xc9, 0xc9, 0xcf)
L['caret'] = (0xff, 0xff, 0xff)
L['textbox'] = (56, 84, 528, 42)                   # x,y,w,h (border incl.)
L['title_pos'] = (58, 46)
L['counter_y'] = 50

KX0, KW, KGAP, KH, VGAP = 44, 48, 8, 48, 8         # 8px gaps BOTH axes (user note)
ROWY = [142, 198, 254, 310]

letters_rows = [           # bottom-right dead zone gets common punctuation
    "1234567890",
    "qwertyuiop",
    "asdfghjkl'",
    "zxcvbnm,.-",
]
sym_rows = [               # physical-keyboard shift pairs (lo, hi)
    [('1', '!'), ('2', '@'), ('3', '#'), ('4', '$'), ('5', '%'),
     ('6', '^'), ('7', '&'), ('8', '*'), ('9', '('), ('0', ')')],
    [('-', '_'), ('=', '+'), ('[', '{'), (']', '}'), (';', ':'),
     ('\'', '"'), (',', '<'), ('.', '>'), ('/', '?'), ('\\', '|')],
    [('`', '~')],
]

FN_Y = 366
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
        f.write("#define OVK_TITLE_POS %d, %d\n#define OVK_COUNTER_Y %d\n"
                % (tx, ty, L['counter_y']))
        f.write("static const ovk_key_t ovk_letters[] = {\n")
        for ri, row in enumerate(letters_rows):
            for ci, c in enumerate(row):
                emit_key(f, c, KX0 + ci * (KW + KGAP), ROWY[ri], KW, KH,
                         c, c.upper() if c.isalpha() else c, ACT_CHAR)
        f.write("};\n")
        f.write("static const ovk_key_t ovk_symbols[] = {\n")
        for ri, row in enumerate(sym_rows):
            for ci, (lo, hi) in enumerate(row):
                emit_key(f, lo, KX0 + ci * (KW + KGAP), ROWY[ri], KW, KH,
                         lo, hi, ACT_CHAR)
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
                                 fill=(0, 0, 0, int(0.68 * 255)))
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
    tx, ty = L['title_pos']
    text(tx, ty + 22, "Company name", L['title_c'], 0.75)
    text(500, L['counter_y'] + 22, "13/14", L['dim_c'], 0.58)
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
            face = L['sel_face'] if is_sel else L['key_face']
            edge = L['sel_edge'] if is_sel else L['key_edge']
            txt = L['sel_text'] if is_sel else L['text_c']
            if is_sel:
                rr(x - 2, y - 2, KW + 4, KH + 4, L['sel_edge'])
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
