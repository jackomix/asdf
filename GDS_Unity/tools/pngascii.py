"""Print a PNG the VM produced as a luminance thumbnail (no image viewer here)."""
import struct
import sys
import zlib

RAMP = ' .:-=+*#%@'


def decode(path):
    d = open(path, 'rb').read()
    i, w, h, idat = 8, 0, 0, b''
    while i < len(d):
        ln = struct.unpack('>I', d[i:i + 4])[0]
        typ = d[i + 4:i + 8]
        data = d[i + 8:i + 8 + ln]
        if typ == b'IHDR':
            w, h = struct.unpack('>II', data[:8])
        elif typ == b'IDAT':
            idat += data
        i += 12 + ln
    raw = zlib.decompress(idat)
    st, out, prev, i = w * 4, [], bytearray(w * 4), 0
    for _ in range(h):
        f = raw[i]
        i += 1
        line = bytearray(raw[i:i + st])
        i += st
        if f == 1:
            for x in range(4, st):
                line[x] = (line[x] + line[x - 4]) & 255
        elif f == 2:
            for x in range(st):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(st):
                a = line[x - 4] if x >= 4 else 0
                line[x] = (line[x] + (a + prev[x]) // 2) & 255
        elif f == 4:
            for x in range(st):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]
                c = prev[x - 4] if x >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out.append(bytes(line))
        prev = line
    return w, h, out


def main():
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 else 96
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 36
    w, h, px = decode(path)
    print('%s  %dx%d' % (path, w, h))
    for r in range(rows):
        y = min(h - 1, r * h // rows)
        line = px[y]
        s = []
        for c in range(cols):
            x = min(w - 1, c * w // cols)
            o = x * 4
            lum = (line[o] * 30 + line[o + 1] * 59 + line[o + 2] * 11) // 100
            s.append(RAMP[min(len(RAMP) - 1, lum * len(RAMP) // 256)])
        print(''.join(s))


if __name__ == '__main__':
    main()
