"""Rasterise the draw batches the shipped engine emits.

Nothing here decides *what* is on screen: every triangle, texture, colour and
matrix comes out of the game's own ARM64 code running in the VM.  This module
only plays back the stream that `UnityHost` captured from the engine's calls
into Graphics/GL, so a frame can be looked at (PNG) or diffed without a GPU.

Batch kinds produced by kairovm.unity:

    mode >= 0   GL.Begin/Vertex3/End immediate primitive
    mode == -1  GL.Clear
    mode == -2  Graphics.DrawMeshNow(mesh, matrix)   <- the engine's main path
    mode == -3  Graphics.DrawTexture (IMGUI blit)
    mode == -4  GUIStyle.Draw (IMGUI text)
    mode == -5  GUIClip push        mode == -6  GUIClip pop
"""
import struct

from . import truetype
from .png import write_png                          # noqa: F401  (re-export)

# UnityEngine.GL uses the OpenGL primitive numbers
GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_QUADS = 1, 2, 4, 5, 7


# ------------------------------------------------------------------ helpers
def unpack_channel(mesh, channel, default_dim, default_fmt=0):
    """Decode one vertex stream into a list of float tuples."""
    raw = mesh.channels.get(channel)
    if not raw:
        return []
    dim, fmt, esz = mesh.layout.get(channel,
                                    (default_dim, default_fmt,
                                     default_dim * 4))
    n = len(raw) // esz if esz else 0
    out = []
    if fmt == 0:                                        # Float32
        for i in range(n):
            out.append(struct.unpack_from('<%df' % dim, raw, i * esz))
    elif fmt in (2, 6):                                 # UNorm8 / UInt8
        for i in range(n):
            v = struct.unpack_from('<%dB' % dim, raw, i * esz)
            out.append(tuple(c / 255.0 for c in v))
    elif fmt in (4, 8):                                 # UNorm16 / UInt16
        for i in range(n):
            v = struct.unpack_from('<%dH' % dim, raw, i * esz)
            out.append(tuple(c / 65535.0 for c in v))
    else:
        for i in range(n):
            out.append(struct.unpack_from('<%df' % min(dim, esz // 4),
                                          raw, i * esz))
    return out


def mesh_triangles(mesh):
    """(index triples, positions, uvs, colors) for every submesh."""
    pos = unpack_channel(mesh, 0, 3)
    uv = unpack_channel(mesh, 4, 2)
    col = unpack_channel(mesh, 3, 4, 2)
    tris = []
    for sub, raw in sorted(mesh.indices.items()):
        topo, esz = mesh.topology.get(sub, (0, 2))
        fmt = '<%d%s' % (len(raw) // esz, 'H' if esz == 2 else 'I')
        idx = struct.unpack(fmt, raw[:(len(raw) // esz) * esz])
        if topo == 0:                                   # Triangles
            for i in range(0, len(idx) - 2, 3):
                tris.append((idx[i], idx[i + 1], idx[i + 2]))
        elif topo == 1:                                 # Quads
            for i in range(0, len(idx) - 3, 4):
                a, b, c, d = idx[i:i + 4]
                tris.append((a, b, c))
                tris.append((a, c, d))
        elif topo == 2:                                 # Lines - draw as thin
            for i in range(0, len(idx) - 1, 2):
                tris.append((idx[i], idx[i + 1], idx[i + 1]))
    return tris, pos, uv, col


def transform(mat, x, y, z):
    """Unity Matrix4x4 is column major: m[c*4+r]."""
    if not mat:
        return x, y, z
    tx = mat[0] * x + mat[4] * y + mat[8] * z + mat[12]
    ty = mat[1] * x + mat[5] * y + mat[9] * z + mat[13]
    tz = mat[2] * x + mat[6] * y + mat[10] * z + mat[14]
    return tx, ty, tz


def intersect(a, b):
    if a is None:
        return tuple(b)
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    x0, y0 = max(ax, bx), max(ay, by)
    x1, y1 = min(ax + aw, bx + bw), min(ay + ah, by + bh)
    return (x0, y0, max(0.0, x1 - x0), max(0.0, y1 - y0))


def box(x, y, w, h, clip, width, height):
    """Integer pixel span of a GUI rect, cut down by the clip stack."""
    x0, y0, x1, y1 = x, y, x + w, y + h
    if clip:
        x0, y0 = max(x0, clip[0]), max(y0, clip[1])
        x1, y1 = min(x1, clip[0] + clip[2]), min(y1, clip[1] + clip[3])
    return (max(0, int(x0)), max(0, int(y0)),
            min(width, int(x1)), min(height, int(y1)))


# --------------------------------------------------------------- rasteriser
class Frame(object):
    def __init__(self, width, height, clear=(0, 0, 0, 255)):
        self.w = width
        self.h = height
        self.buf = bytearray(clear * (width * height))

    def clear(self, rgba):
        r, g, b, a = [max(0, min(255, int(c * 255 + 0.5))) for c in rgba]
        self.buf[:] = bytes((r, g, b, 255)) * (self.w * self.h)

    def blend(self, x, y, r, g, b, a):
        if a <= 0.003 or x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        o = (y * self.w + x) * 4
        if a >= 0.997:
            self.buf[o] = int(r * 255)
            self.buf[o + 1] = int(g * 255)
            self.buf[o + 2] = int(b * 255)
            self.buf[o + 3] = 255
            return
        ia = 1.0 - a
        self.buf[o] = int(r * 255 * a + self.buf[o] * ia)
        self.buf[o + 1] = int(g * 255 * a + self.buf[o + 1] * ia)
        self.buf[o + 2] = int(b * 255 * a + self.buf[o + 2] * ia)
        self.buf[o + 3] = min(255, int(a * 255 + self.buf[o + 3] * ia))

    # ------------------------------------------------------------- sampling
    @staticmethod
    def sample(tex, u, v):
        if tex is None or not getattr(tex, 'w', 0) or not getattr(tex, 'h', 0):
            return 1.0, 1.0, 1.0, 1.0
        px = getattr(tex, 'pixels', None)
        if not px:
            return 1.0, 1.0, 1.0, 1.0
        x = int(u * tex.w) % tex.w
        y = int((1.0 - v) * tex.h) % tex.h          # GL uv origin is bottom
        o = (y * tex.w + x) * 4
        if o + 3 >= len(px):
            return 1.0, 1.0, 1.0, 1.0
        return (px[o] / 255.0, px[o + 1] / 255.0, px[o + 2] / 255.0,
                px[o + 3] / 255.0)

    # ------------------------------------------------------------ triangles
    def triangle(self, p, uv, col, tex):
        (x0, y0), (x1, y1), (x2, y2) = p
        minx = max(0, int(min(x0, x1, x2)))
        maxx = min(self.w - 1, int(max(x0, x1, x2)) + 1)
        miny = max(0, int(min(y0, y1, y2)))
        maxy = min(self.h - 1, int(max(y0, y1, y2)) + 1)
        if minx > maxx or miny > maxy:
            return 0
        d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(d) < 1e-9:
            return 0
        inv = 1.0 / d
        painted = 0
        for y in range(miny, maxy + 1):
            fy = y + 0.5
            for x in range(minx, maxx + 1):
                fx = x + 0.5
                w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) * inv
                w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) * inv
                w2 = 1.0 - w0 - w1
                if w0 < -1e-4 or w1 < -1e-4 or w2 < -1e-4:
                    continue
                u = w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0]
                v = w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1]
                tr, tg, tb, ta = self.sample(tex, u, v)
                cr = w0 * col[0][0] + w1 * col[1][0] + w2 * col[2][0]
                cg = w0 * col[0][1] + w1 * col[1][1] + w2 * col[2][1]
                cb = w0 * col[0][2] + w1 * col[1][2] + w2 * col[2][2]
                ca = w0 * col[0][3] + w1 * col[1][3] + w2 * col[2][3]
                self.blend(x, y, tr * cr, tg * cg, tb * cb, ta * ca)
                painted += 1
        return painted

    # ----------------------------------------------------------------- text
    def text(self, font, s, x, y, px, rgba, clip=None):
        """Draw `s` with its baseline-left pen starting at (x, y)."""
        if font is None or not s:
            return 0
        r, g, b, a = rgba
        painted = 0
        pen = float(x)
        for ch in s:
            if ch in '\r\n':
                continue
            gid = font.glyph_index(ch)
            bm, gw, gh, left, top, adv = font.render(gid, px)
            if gw and gh:
                ox = int(pen) + left
                oy = int(y) - top
                for row in range(gh):
                    py = oy + row
                    if py < 0 or py >= self.h:
                        continue
                    if clip and not (clip[1] <= py < clip[1] + clip[3]):
                        continue
                    base = row * gw
                    for col in range(gw):
                        cov = bm[base + col]
                        if not cov:
                            continue
                        pxx = ox + col
                        if clip and not (clip[0] <= pxx < clip[0] + clip[2]):
                            continue
                        self.blend(pxx, py, r, g, b, a * cov / 255.0)
                        painted += 1
            pen += adv
        return painted

    def rect(self, x, y, w, h, rgba):
        r, g, b, a = rgba
        for yy in range(max(0, int(y)), min(self.h, int(y + h))):
            for xx in range(max(0, int(x)), min(self.w, int(x + w))):
                self.blend(xx, yy, r, g, b, a)

    def rgba(self):
        return bytes(self.buf)

    # ------------------------------------------------------------- preview
    RAMP = ' .:-=+*#%@'

    def ascii(self, cols=80, rows=30):
        """Luminance thumbnail, so a frame can be read out of a log file."""
        out = []
        for r in range(rows):
            line = []
            y0 = r * self.h // rows
            y1 = max(y0 + 1, (r + 1) * self.h // rows)
            for c in range(cols):
                x0 = c * self.w // cols
                x1 = max(x0 + 1, (c + 1) * self.w // cols)
                tot = n = 0
                for y in range(y0, y1):
                    base = y * self.w * 4
                    for x in range(x0, x1):
                        o = base + x * 4
                        tot += (self.buf[o] * 299 + self.buf[o + 1] * 587
                                + self.buf[o + 2] * 114) // 1000
                        n += 1
                v = tot // n if n else 0
                line.append(self.RAMP[min(len(self.RAMP) - 1,
                                          v * len(self.RAMP) // 256)])
            out.append(''.join(line))
        return '\n'.join(out)


WHITE = ((1.0, 1.0, 1.0, 1.0),) * 3


def render(batches, width, height, flip_y=True, stats=None,
           fonts=None, clear=(0, 0, 0, 255)):
    """Play a captured frame back into an RGBA image."""
    f = Frame(width, height, clear=clear)
    st = stats if stats is not None else {}
    st.setdefault('tris', 0)
    st.setdefault('pixels', 0)
    st.setdefault('batches', 0)

    def project(x, y):
        return x, (height - y) if flip_y else y

    font = fonts[0] if fonts else truetype.system_font()
    font_bold = (fonts[1] if fonts and len(fonts) > 1
                 else truetype.system_font(bold=True)) or font

    clips = []
    for b in batches:
        mode = b.get('mode')
        clip = clips[-1] if clips else None
        if mode == -1:
            f.clear(b.get('clear', (0, 0, 0, 1)))
            continue
        if mode == -2:
            mesh, mat, tex = b.get('mesh'), b.get('matrix'), b.get('tex')
            if mesh is None:
                continue
            tris, pos, uv, col = mesh_triangles(mesh)
            for ia, ib, ic in tris:
                if max(ia, ib, ic) >= len(pos):
                    continue
                pts, uvs, cols = [], [], []
                for i in (ia, ib, ic):
                    p = pos[i]
                    wx, wy, _wz = transform(mat, p[0], p[1],
                                            p[2] if len(p) > 2 else 0.0)
                    pts.append(project(wx, wy))
                    uvs.append(uv[i] if i < len(uv) else (0.0, 0.0))
                    c = col[i] if i < len(col) else (1.0, 1.0, 1.0, 1.0)
                    cols.append(tuple(c) + (1.0,) * (4 - len(c)))
                st['pixels'] += f.triangle(pts, uvs, cols, tex)
                st['tris'] += 1
            st['batches'] += 1
            continue
        if mode == -3:
            x, y, w, h = b.get('rect', (0, 0, 0, 0))
            tex = b.get('tex')
            col = b.get('color', (1.0, 1.0, 1.0, 1.0))
            src = b.get('uv')
            x0, y0, x1, y1 = box(x, y, w, h, clip, width, height)
            if tex is not None and getattr(tex, 'pixels', None):
                su, sv, sw, sh = src if src and src[2] and src[3] \
                    else (0.0, 0.0, 1.0, 1.0)
                for yy in range(y0, y1):
                    v = 1.0 - (sv + ((yy - y) / h if h else 0.0) * sh)
                    for xx in range(x0, x1):
                        u = su + ((xx - x) / w if w else 0.0) * sw
                        tr, tg, tb, ta = Frame.sample(tex, u, v)
                        f.blend(xx, yy, tr * col[0], tg * col[1], tb * col[2],
                                ta * col[3])
            else:
                for yy in range(y0, y1):
                    for xx in range(x0, x1):
                        f.blend(xx, yy, col[0], col[1], col[2], col[3])
            st['batches'] += 1
            continue
        if mode == -4:
            x, y, w, h = b.get('rect', (0, 0, 0, 0))
            s = b.get('text', '') or ''
            st.setdefault('text', []).append((s, (x, y, w, h)))
            size = b.get('font_size', 16) or 16
            fnt = font_bold if b.get('bold') else font
            if fnt is not None and s.strip():
                lines = s.replace('\r\n', '\n').replace('\r', '\n').split('\n')
                lh = fnt.line_height(size)
                asc = fnt.ascent_px(size)
                for i, line in enumerate(lines):
                    tw = fnt.text_width(line, size)
                    tx, ty = align_text(b.get('alignment', 0), x, y, w, h,
                                        tw, lh * len(lines))
                    # IMGUI rects are already in y-down screen space
                    st['pixels'] += f.text(fnt, line, tx, ty + lh * i + asc,
                                           size, b.get('color', (1.0,) * 4),
                                           clip)
            st['batches'] += 1
            continue
        if mode == -5:
            r = b.get('clip')
            if r:
                clips.append(intersect(clips[-1] if clips else None, r))
            st['batches'] += 1
            continue
        if mode == -6:
            if clips:
                clips.pop()
            st['batches'] += 1
            continue
        # immediate mode
        verts = b.get('v') or []
        tex = b.get('tex')
        idx = []
        if mode == GL_TRIANGLES:
            idx = [(i, i + 1, i + 2) for i in range(0, len(verts) - 2, 3)]
        elif mode == GL_QUADS:
            for i in range(0, len(verts) - 3, 4):
                idx += [(i, i + 1, i + 2), (i, i + 2, i + 3)]
        elif mode == GL_TRIANGLE_STRIP:
            idx = [(i, i + 1, i + 2) for i in range(len(verts) - 2)]
        for ia, ib, ic in idx:
            pts, uvs, cols = [], [], []
            for i in (ia, ib, ic):
                vx, vy, _vz, u, v, c = verts[i]
                pts.append(project(vx, vy))
                uvs.append((u, v))
                cols.append(c)
            st['pixels'] += f.triangle(pts, uvs, cols, tex)
            st['tris'] += 1
        st['batches'] += 1
    return f


# UnityEngine.TextAnchor
(UPPER_LEFT, UPPER_CENTER, UPPER_RIGHT, MIDDLE_LEFT, MIDDLE_CENTER,
 MIDDLE_RIGHT, LOWER_LEFT, LOWER_CENTER, LOWER_RIGHT) = range(9)


def align_text(anchor, x, y, w, h, tw, th):
    """Top-left of a `tw`x`th` text block inside the style's rect.

    The engine hands IMGUI a 1px-wide rect and lets the anchor place the
    string, which is why the alignment has to be honoured here.
    """
    col = anchor % 3
    row = anchor // 3
    if col == 1:
        x = x + (w - tw) / 2.0
    elif col == 2:
        x = x + w - tw
    if row == 1:
        y = y + (h - th) / 2.0
    elif row == 2:
        y = y + h - th
    return x, y
