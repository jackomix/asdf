"""A small TrueType parser + scanline rasteriser.

The shipped engine draws its UI text two ways, and both need real glyphs on
the host side:

  * `UnityEngine.GUIStyle.Internal_Draw` with a *dynamic* font - on Android
    Unity resolves that to the platform font and rasterises inside libunity,
    which is not part of the APK; and
  * `kairo.android.plugin.Utility.makeFontTexture`, where the Java plugin
    paints a string with android.graphics.Paint and hands the pixels back.

Either way the font is the *device's*, so the port supplies one.  This module
turns a .ttf into glyph coverage bitmaps and advance widths with no external
dependency, which also makes `Internal_CalcSize` exact instead of guessed -
and the engine centres and clips its menus with those measurements.

Supported: glyf/loca outlines (simple + composite), cmap formats 0/4/6/12,
hmtx advances, unitsPerEm scaling.  Quadratic curves are flattened, coverage
is computed with 4x vertical supersampling and exact horizontal spans.
"""
import os
import struct

DEFAULT_FONTS = (
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
    '/usr/share/fonts/TTF/DejaVuSans.ttf',
    '/system/fonts/Roboto-Regular.ttf',
    '/system/fonts/DroidSans.ttf',
)

DEFAULT_BOLD = (
    '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
    '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf',
    '/system/fonts/Roboto-Bold.ttf',
)

SUBSAMPLES = 4


class FontError(Exception):
    pass


class Glyph(object):
    __slots__ = ('contours', 'advance', 'lsb', 'xmin', 'ymin', 'xmax', 'ymax')

    def __init__(self):
        self.contours = []
        self.advance = 0
        self.lsb = 0
        self.xmin = self.ymin = self.xmax = self.ymax = 0


class TrueType(object):
    def __init__(self, data):
        self.data = data = bytes(data)
        if len(data) < 12:
            raise FontError('truncated font')
        tag = data[:4]
        off = 0
        if tag == b'ttcf':
            off = struct.unpack_from('>I', data, 12)[0]
            tag = data[off:off + 4]
        if tag not in (b'\0\1\0\0', b'true', b'OTTO'):
            raise FontError('not a TrueType font (%r)' % (tag,))
        num = struct.unpack_from('>H', data, off + 4)[0]
        self.tables = {}
        for i in range(num):
            p = off + 12 + i * 16
            name = data[p:p + 4]
            toff, tlen = struct.unpack_from('>II', data, p + 8)
            self.tables[name] = (toff, tlen)
        if b'glyf' not in self.tables:
            raise FontError('no glyf table (CFF outlines unsupported)')

        head = self.tables[b'head'][0]
        self.units_per_em = struct.unpack_from('>H', data, head + 18)[0] or 1000
        self.index_to_loc = struct.unpack_from('>h', data, head + 50)[0]
        maxp = self.tables[b'maxp'][0]
        self.num_glyphs = struct.unpack_from('>H', data, maxp + 4)[0]
        hhea = self.tables[b'hhea'][0]
        self.ascent = struct.unpack_from('>h', data, hhea + 4)[0]
        self.descent = struct.unpack_from('>h', data, hhea + 6)[0]
        self.line_gap = struct.unpack_from('>h', data, hhea + 8)[0]
        self.num_hmetrics = struct.unpack_from('>H', data, hhea + 34)[0]
        self._loca = self._read_loca()
        self._cmap = self._read_cmap()
        self._glyph_cache = {}
        self._bitmap_cache = {}

    # ------------------------------------------------------------- tables
    def _read_loca(self):
        if b'loca' not in self.tables:
            return []
        off, _ = self.tables[b'loca']
        n = self.num_glyphs + 1
        if self.index_to_loc:
            return list(struct.unpack_from('>%dI' % n, self.data, off))
        short = struct.unpack_from('>%dH' % n, self.data, off)
        return [v * 2 for v in short]

    def _read_cmap(self):
        if b'cmap' not in self.tables:
            return {}
        base, _ = self.tables[b'cmap']
        data = self.data
        n = struct.unpack_from('>H', data, base + 2)[0]
        best = None
        for i in range(n):
            pid, eid, off = struct.unpack_from('>HHI', data, base + 4 + i * 8)
            score = {(3, 10): 5, (3, 1): 4, (0, 4): 4, (0, 3): 3,
                     (0, 6): 3, (3, 0): 2, (1, 0): 1}.get((pid, eid), 0)
            if best is None or score > best[0]:
                best = (score, base + off)
        if best is None:
            return {}
        return self._parse_subtable(best[1])

    def _parse_subtable(self, off):
        data = self.data
        fmt = struct.unpack_from('>H', data, off)[0]
        out = {}
        if fmt == 0:
            for c in range(256):
                out[c] = data[off + 6 + c]
        elif fmt == 4:
            segx2 = struct.unpack_from('>H', data, off + 6)[0]
            seg = segx2 // 2
            ends = struct.unpack_from('>%dH' % seg, data, off + 14)
            starts = struct.unpack_from('>%dH' % seg, data, off + 16 + segx2)
            deltas = struct.unpack_from('>%dh' % seg, data,
                                        off + 16 + segx2 * 2)
            ro_base = off + 16 + segx2 * 3
            ranges = struct.unpack_from('>%dH' % seg, data, ro_base)
            for i in range(seg):
                for c in range(starts[i], min(ends[i], 0xFFFF) + 1):
                    if ranges[i] == 0:
                        g = (c + deltas[i]) & 0xFFFF
                    else:
                        p = ro_base + i * 2 + ranges[i] + (c - starts[i]) * 2
                        if p + 1 >= len(data):
                            continue
                        g = struct.unpack_from('>H', data, p)[0]
                        if g:
                            g = (g + deltas[i]) & 0xFFFF
                    if g:
                        out[c] = g
        elif fmt == 6:
            first, count = struct.unpack_from('>HH', data, off + 6)
            for i in range(count):
                out[first + i] = struct.unpack_from('>H', data,
                                                    off + 10 + i * 2)[0]
        elif fmt == 12:
            ngroups = struct.unpack_from('>I', data, off + 12)[0]
            for i in range(ngroups):
                s, e, gs = struct.unpack_from('>III', data, off + 16 + i * 12)
                if e - s > 0x10000:
                    e = s + 0x10000
                for c in range(s, e + 1):
                    out[c] = gs + (c - s)
        return out

    def glyph_index(self, ch):
        return self._cmap.get(ord(ch) if isinstance(ch, str) else ch, 0)

    def advance_units(self, gid):
        if b'hmtx' not in self.tables:
            return self.units_per_em // 2
        off, _ = self.tables[b'hmtx']
        i = min(gid, self.num_hmetrics - 1)
        if i < 0:
            return 0
        return struct.unpack_from('>H', self.data, off + i * 4)[0]

    # ------------------------------------------------------------ outlines
    def glyph(self, gid, depth=0):
        g = self._glyph_cache.get(gid)
        if g is not None:
            return g
        g = Glyph()
        g.advance = self.advance_units(gid)
        self._glyph_cache[gid] = g
        if gid + 1 >= len(self._loca):
            return g
        gbase, _ = self.tables[b'glyf']
        start, end = self._loca[gid], self._loca[gid + 1]
        if end <= start:
            return g                                    # empty (e.g. space)
        off = gbase + start
        data = self.data
        ncont, g.xmin, g.ymin, g.xmax, g.ymax = struct.unpack_from(
            '>hhhhh', data, off)
        if ncont >= 0:
            g.contours = self._simple_glyph(off + 10, ncont)
        elif depth < 5:
            g.contours = self._composite_glyph(off + 10, depth)
        return g

    def _simple_glyph(self, p, ncont):
        data = self.data
        ends = struct.unpack_from('>%dH' % ncont, data, p)
        p += ncont * 2
        ilen = struct.unpack_from('>H', data, p)[0]
        p += 2 + ilen
        npts = (ends[-1] + 1) if ncont else 0
        flags = []
        while len(flags) < npts:
            f = data[p]
            p += 1
            flags.append(f)
            if f & 8:
                rep = data[p]
                p += 1
                flags.extend([f] * rep)
        flags = flags[:npts]
        xs = []
        v = 0
        for f in flags:
            if f & 2:
                d = data[p]
                p += 1
                v += d if f & 16 else -d
            elif not f & 16:
                v += struct.unpack_from('>h', data, p)[0]
                p += 2
            xs.append(v)
        ys = []
        v = 0
        for f in flags:
            if f & 4:
                d = data[p]
                p += 1
                v += d if f & 32 else -d
            elif not f & 32:
                v += struct.unpack_from('>h', data, p)[0]
                p += 2
            ys.append(v)
        contours = []
        s = 0
        for e in ends:
            pts = [(xs[i], ys[i], bool(flags[i] & 1))
                   for i in range(s, min(e + 1, npts))]
            if pts:
                contours.append(pts)
            s = e + 1
        return contours

    def _composite_glyph(self, p, depth):
        data = self.data
        out = []
        while True:
            flags, gi = struct.unpack_from('>HH', data, p)
            p += 4
            if flags & 1:                               # ARG_1_AND_2_ARE_WORDS
                a1, a2 = struct.unpack_from('>hh', data, p)
                p += 4
            else:
                a1, a2 = struct.unpack_from('>bb', data, p)
                p += 2
            sa = sd = 1.0
            sb = sc = 0.0
            if flags & 8:                               # WE_HAVE_A_SCALE
                sa = sd = _f2dot14(data, p)
                p += 2
            elif flags & 0x40:                          # X_AND_Y_SCALE
                sa = _f2dot14(data, p)
                sd = _f2dot14(data, p + 2)
                p += 4
            elif flags & 0x80:                          # TWO_BY_TWO
                sa = _f2dot14(data, p)
                sb = _f2dot14(data, p + 2)
                sc = _f2dot14(data, p + 4)
                sd = _f2dot14(data, p + 6)
                p += 8
            dx, dy = (a1, a2) if flags & 2 else (0, 0)
            sub = self.glyph(gi, depth + 1)
            for cont in sub.contours:
                out.append([(x * sa + y * sc + dx, x * sb + y * sd + dy, on)
                            for x, y, on in cont])
            if not flags & 0x20:                        # MORE_COMPONENTS
                break
        return out

    # --------------------------------------------------------- rasterising
    def render(self, gid, px):
        """(bitmap bytes, w, h, left, top, advance) at `px` pixels per em.

        `left`/`top` are the offsets from the pen position (baseline origin)
        to the top-left corner of the bitmap, y growing downwards.
        """
        key = (gid, round(px, 2))
        hit = self._bitmap_cache.get(key)
        if hit is not None:
            return hit
        g = self.glyph(gid)
        scale = float(px) / self.units_per_em
        adv = g.advance * scale
        edges = []
        for cont in g.contours:
            pts = _flatten(cont)
            for i in range(len(pts)):
                x0, y0 = pts[i]
                x1, y1 = pts[(i + 1) % len(pts)]
                if y0 != y1:
                    edges.append((x0 * scale, y0 * scale,
                                  x1 * scale, y1 * scale))
        if not edges:
            out = (b'', 0, 0, 0, 0, adv)
            self._bitmap_cache[key] = out
            return out
        xmin = min(min(e[0], e[2]) for e in edges)
        xmax = max(max(e[0], e[2]) for e in edges)
        ymin = min(min(e[1], e[3]) for e in edges)
        ymax = max(max(e[1], e[3]) for e in edges)
        left = int(xmin) - 1
        right = int(xmax) + 2
        bottom = int(ymin) - 1
        top = int(ymax) + 2
        w = right - left
        h = top - bottom
        if w <= 0 or h <= 0 or w > 4096 or h > 4096:
            out = (b'', 0, 0, 0, 0, adv)
            self._bitmap_cache[key] = out
            return out
        cov = bytearray(w * h)
        step = 1.0 / SUBSAMPLES
        add = 255 // SUBSAMPLES + 1
        for row in range(h):
            y_base = bottom + row
            acc = [0] * w
            for s in range(SUBSAMPLES):
                sy = y_base + (s + 0.5) * step
                xs = []
                for ex0, ey0, ex1, ey1 in edges:
                    if (ey0 <= sy < ey1) or (ey1 <= sy < ey0):
                        t = (sy - ey0) / (ey1 - ey0)
                        xs.append(ex0 + t * (ex1 - ex0))
                if not xs:
                    continue
                xs.sort()
                for i in range(0, len(xs) - 1, 2):
                    a, b = xs[i], xs[i + 1]
                    ia = int(a - left)
                    ib = int(b - left)
                    if ib < 0 or ia >= w:
                        continue
                    if ia == ib:
                        if 0 <= ia < w:
                            acc[ia] += int(add * (b - a))
                        continue
                    if 0 <= ia < w:
                        acc[ia] += int(add * (1.0 - ((a - left) - ia)))
                    for x in range(max(0, ia + 1), min(w, ib)):
                        acc[x] += add
                    if 0 <= ib < w:
                        acc[ib] += int(add * ((b - left) - ib))
            o = (h - 1 - row) * w                       # flip: y grows down
            for x in range(w):
                v = acc[x]
                cov[o + x] = 255 if v > 255 else v
        out = (bytes(cov), w, h, left, top, adv)
        self._bitmap_cache[key] = out
        return out

    # ------------------------------------------------------------ measuring
    def text_width(self, text, px):
        scale = float(px) / self.units_per_em
        total = 0
        for ch in text:
            total += self.advance_units(self.glyph_index(ch))
        return total * scale

    def line_height(self, px):
        scale = float(px) / self.units_per_em
        return (self.ascent - self.descent + self.line_gap) * scale

    def ascent_px(self, px):
        return self.ascent * float(px) / self.units_per_em


def _f2dot14(data, p):
    return struct.unpack_from('>h', data, p)[0] / 16384.0


def _flatten(contour, steps=6):
    """Quadratic contour -> polyline, inserting the implied on-curve points."""
    pts = []
    n = len(contour)
    if not n:
        return pts
    # rotate so the contour starts on-curve
    start = 0
    for i, (_x, _y, on) in enumerate(contour):
        if on:
            start = i
            break
    else:                                               # all off-curve
        x0, y0, _ = contour[0]
        xl, yl, _ = contour[-1]
        contour = [((x0 + xl) / 2.0, (y0 + yl) / 2.0, True)] + list(contour)
        start = 0
        n += 1
    seq = [contour[(start + i) % n] for i in range(n)]
    seq.append(seq[0])
    cur = (seq[0][0], seq[0][1])
    pts.append(cur)
    i = 1
    while i < len(seq):
        x, y, on = seq[i]
        if on:
            cur = (x, y)
            pts.append(cur)
            i += 1
            continue
        # off-curve control; the end point is the next on-curve point, or the
        # midpoint between two consecutive control points
        nx, ny, non = seq[i + 1] if i + 1 < len(seq) else seq[0]
        if not non:
            nx, ny = (x + nx) / 2.0, (y + ny) / 2.0
            step = 1
        else:
            step = 2
        for s in range(1, steps + 1):
            t = s / float(steps)
            it = 1.0 - t
            pts.append((it * it * cur[0] + 2 * it * t * x + t * t * nx,
                        it * it * cur[1] + 2 * it * t * y + t * t * ny))
        cur = (nx, ny)
        i += step
    return pts


# ------------------------------------------------------------------ loading
_CACHE = {}


def load(path):
    f = _CACHE.get(path)
    if f is None:
        with open(path, 'rb') as fh:
            f = TrueType(fh.read())
        _CACHE[path] = f
    return f


def system_font(bold=False):
    """First usable font on this host, or None.

    A shipped port bundles its own; this keeps the reference VM working on a
    plain Linux box (and on the handheld, where /usr/share/fonts exists too).
    """
    env = os.environ.get('KAIROVM_FONT_BOLD' if bold else 'KAIROVM_FONT')
    for p in ((env,) if env else ()) + (DEFAULT_BOLD if bold
                                        else ()) + DEFAULT_FONTS:
        if p and os.path.exists(p):
            try:
                return load(p)
            except Exception:
                continue
    return None
