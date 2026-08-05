"""PNG/JPEG-header image decoding for the VM's Texture2D backing store.

`UnityEngine.ImageConversion.LoadImage` is a real engine entry point: the
shipped `kairo.unity.ui.Image.DecodeByteArray` hands it the raw bytes it
pulled out of the game's own archives and expects a filled Texture2D back.
There is no imaging library in this sandbox, so the decoder lives here.

Only what the game actually ships is supported: non-interlaced and Adam7
PNG, bit depths 1/2/4/8/16, colour types 0/2/3/4/6, tRNS transparency.
Everything comes back as tightly packed straight-alpha RGBA8, top row first,
which is the layout `kairovm.unity.Texture.pixels` uses.
"""
import struct
import zlib

PNG_MAGIC = b'\x89PNG\r\n\x1a\n'

# channels per colour type
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}

# Adam7: (x offset, y offset, x step, y step)
_ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
          (0, 2, 2, 2), (1, 0, 2, 2), (0, 1, 1, 1))


class ImageError(Exception):
    pass


def is_png(data):
    return len(data) >= 8 and bytes(data[:8]) == PNG_MAGIC


def is_jpeg(data):
    return len(data) >= 3 and bytes(data[:3]) == b'\xff\xd8\xff'


# --------------------------------------------------------------- unfiltering
def _unfilter(raw, width, height, bpp, stride):
    """Reverse the five PNG scanline filters in place, row by row."""
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if len(line) < stride:                          # truncated stream
            line.extend(b'\0' * (stride - len(line)))
        if ft == 0:
            pass
        elif ft == 1:                                   # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:                                   # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:                                   # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:                                   # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa = p - a if p > a else a - p
                pb = p - b if p > b else b - p
                pc = p - c if p > c else c - p
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                line[i] = (line[i] + pr) & 0xFF
        else:
            raise ImageError('bad PNG filter %d' % ft)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def _scale(v, depth):
    if depth == 8:
        return v
    if depth == 16:
        return v >> 8
    if depth == 1:
        return 255 if v else 0
    if depth == 2:
        return v * 85
    if depth == 4:
        return v * 17
    raise ImageError('bad bit depth %d' % depth)


def _samples(line, width, chans, depth):
    """Pull `width * chans` samples out of one unfiltered scanline."""
    n = width * chans
    if depth == 8:
        return line[:n]
    if depth == 16:
        return [line[i * 2] for i in range(n)]
    out = []
    per = 8 // depth
    mask = (1 << depth) - 1
    for i in range(n):
        byte = line[i // per]
        shift = 8 - depth * (i % per + 1)
        out.append((byte >> shift) & mask)
    return out


def decode_png(data):
    """Decode a PNG into (width, height, bytearray of RGBA8)."""
    data = bytes(data)
    if not is_png(data):
        raise ImageError('not a PNG')
    pos = 8
    width = height = depth = ctype = 0
    interlace = 0
    palette = b''
    trns = None
    idat = []
    while pos + 8 <= len(data):
        (length,) = struct.unpack_from('>I', data, pos)
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b'IHDR':
            (width, height, depth, ctype, _comp, _filt,
             interlace) = struct.unpack('>IIBBBBB', body)
        elif tag == b'PLTE':
            palette = body
        elif tag == b'tRNS':
            trns = body
        elif tag == b'IDAT':
            idat.append(body)
        elif tag == b'IEND':
            break
    if not width or not height:
        raise ImageError('no IHDR')
    chans = _CHANNELS.get(ctype)
    if chans is None:
        raise ImageError('bad colour type %d' % ctype)
    raw = zlib.decompress(b''.join(idat))

    rgba = bytearray(width * height * 4)
    if interlace == 0:
        passes = [(0, 0, 1, 1, width, height)]
    elif interlace == 1:
        passes = []
        for ox, oy, sx, sy in _ADAM7:
            pw = (width - ox + sx - 1) // sx
            ph = (height - oy + sy - 1) // sy
            passes.append((ox, oy, sx, sy, pw, ph))
    else:
        raise ImageError('bad interlace %d' % interlace)

    off = 0
    for ox, oy, sx, sy, pw, ph in passes:
        if pw <= 0 or ph <= 0:
            continue
        bits = pw * chans * depth
        stride = (bits + 7) // 8
        bpp = max(1, (chans * depth + 7) // 8)
        need = ph * (stride + 1)
        block = _unfilter(raw[off:off + need], pw, ph, bpp, stride)
        off += need
        for y in range(ph):
            line = block[y * stride:(y + 1) * stride]
            s = _samples(line, pw, chans, depth)
            dy = oy + y * sy
            if dy >= height:
                break
            for x in range(pw):
                dx = ox + x * sx
                if dx >= width:
                    break
                i = x * chans
                if ctype == 3:                              # palette
                    p = s[i]
                    o = p * 3
                    r = palette[o] if o + 2 < len(palette) else 0
                    g = palette[o + 1] if o + 2 < len(palette) else 0
                    b = palette[o + 2] if o + 2 < len(palette) else 0
                    a = trns[p] if trns and p < len(trns) else 255
                elif ctype == 0:                            # grey
                    r = g = b = _scale(s[i], depth)
                    a = 255
                    if trns and len(trns) >= 2:
                        key = struct.unpack('>H', trns[:2])[0]
                        if s[i] == (key if depth == 16 else key & 0xFF):
                            a = 0
                elif ctype == 4:                            # grey + alpha
                    r = g = b = _scale(s[i], depth)
                    a = _scale(s[i + 1], depth)
                elif ctype == 2:                            # rgb
                    r = _scale(s[i], depth)
                    g = _scale(s[i + 1], depth)
                    b = _scale(s[i + 2], depth)
                    a = 255
                    if trns and len(trns) >= 6:
                        kr, kg, kb = struct.unpack('>HHH', trns[:6])
                        if depth != 16:
                            kr, kg, kb = kr & 0xFF, kg & 0xFF, kb & 0xFF
                        if (s[i], s[i + 1], s[i + 2]) == (kr, kg, kb):
                            a = 0
                else:                                       # rgba
                    r = _scale(s[i], depth)
                    g = _scale(s[i + 1], depth)
                    b = _scale(s[i + 2], depth)
                    a = _scale(s[i + 3], depth)
                o = (dy * width + dx) * 4
                rgba[o] = r
                rgba[o + 1] = g
                rgba[o + 2] = b
                rgba[o + 3] = a
    return width, height, rgba


def decode(data):
    """Decode whatever `ImageConversion.LoadImage` was handed."""
    if is_png(data):
        return decode_png(data)
    if is_jpeg(data):
        w, h = jpeg_size(data)
        raise ImageError('JPEG %dx%d not supported' % (w, h))
    raise ImageError('unknown image format %r' % (bytes(data[:8]),))


def jpeg_size(data):
    """Width/height out of the first SOF marker (no pixel decode)."""
    i = 2
    n = len(data)
    while i + 9 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack_from('>HH', data, i + 5)
            return w, h
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        (seg,) = struct.unpack_from('>H', data, i + 2)
        i += 2 + seg
    return 0, 0


# ------------------------------------------------------------------- writing
def write_png(path, width, height, rgba):
    """Minimal RGBA8 PNG writer (no third-party imaging library here)."""
    with open(path, 'wb') as fh:
        fh.write(encode_png(width, height, rgba))
    return True


def encode_png(width, height, rgba):
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)                                  # filter: none
        rows += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, body):
        c = struct.pack('>I', len(body)) + tag + body
        return c + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (PNG_MAGIC
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6,
                                         0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(rows), 6))
            + chunk(b'IEND', b''))
