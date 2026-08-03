/* =========================================================================
 * 05-image.js -- synchronous PNG decoder / encoder (inflate + deflate-store)
 *
 * Android's BitmapFactory.decodeByteArray() is synchronous while every
 * browser image API is asynchronous, so the port ships its own decoder.
 * This keeps the translated Dalvik code running exactly as it does on the
 * device (no promise plumbing inside the game's resource loader).
 * ========================================================================= */
'use strict';

var $img = (function () {

  /* --------------------------------------------------------- inflate */
  const LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35,
    43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4,
    4, 4, 4, 5, 5, 5, 5, 0];
  const DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
    385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  const DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9,
    9, 10, 10, 11, 11, 12, 12, 13, 13];
  const CLORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

  function buildHuff(lengths) {
    let max = 0;
    for (const l of lengths) if (l > max) max = l;
    const blCount = new Int32Array(max + 1);
    for (const l of lengths) if (l) blCount[l]++;
    const next = new Int32Array(max + 2);
    let code = 0;
    for (let b = 1; b <= max; b++) { code = (code + blCount[b - 1]) << 1; next[b] = code; }
    const codes = new Int32Array(lengths.length);
    for (let i = 0; i < lengths.length; i++) if (lengths[i]) codes[i] = next[lengths[i]]++;
    // build a fast lookup table
    const table = new Int32Array(1 << max).fill(-1);
    for (let i = 0; i < lengths.length; i++) {
      const len = lengths[i];
      if (!len) continue;
      let rev = 0, c = codes[i];
      for (let b = 0; b < len; b++) { rev = (rev << 1) | (c & 1); c >>= 1; }
      for (let j = rev; j < (1 << max); j += (1 << len)) table[j] = (i << 5) | len;
    }
    return { table, max };
  }

  function inflate(src, start, expected) {
    let pos = start || 0;
    let bitbuf = 0, bitcnt = 0;
    let out = new Uint8Array(expected || Math.max(1024, src.length * 4));
    let olen = 0;

    function need(n) {
      while (bitcnt < n) {
        bitbuf |= src[pos++] << bitcnt;
        bitcnt += 8;
      }
    }
    function bits(n) {
      if (n === 0) return 0;
      need(n);
      const v = bitbuf & ((1 << n) - 1);
      bitbuf >>>= n; bitcnt -= n;
      return v;
    }
    function decode(h) {
      need(h.max);
      const e = h.table[bitbuf & ((1 << h.max) - 1)];
      if (e < 0) throw new Error('bad huffman code');
      const len = e & 31;
      bitbuf >>>= len; bitcnt -= len;
      return e >> 5;
    }
    function grow(n) {
      if (olen + n <= out.length) return;
      let cap = out.length * 2;
      while (cap < olen + n) cap *= 2;
      const nb = new Uint8Array(cap);
      nb.set(out.subarray(0, olen));
      out = nb;
    }

    let fixedLit = null, fixedDist = null;
    for (;;) {
      const final = bits(1);
      const type = bits(2);
      if (type === 0) {
        bitbuf = 0; bitcnt = 0;
        const len = src[pos] | (src[pos + 1] << 8);
        pos += 4;
        grow(len);
        out.set(src.subarray(pos, pos + len), olen);
        olen += len; pos += len;
      } else {
        let lit, dist;
        if (type === 1) {
          if (!fixedLit) {
            const l = new Uint8Array(288);
            for (let i = 0; i < 144; i++) l[i] = 8;
            for (let i = 144; i < 256; i++) l[i] = 9;
            for (let i = 256; i < 280; i++) l[i] = 7;
            for (let i = 280; i < 288; i++) l[i] = 8;
            fixedLit = buildHuff(l);
            fixedDist = buildHuff(new Uint8Array(30).fill(5));
          }
          lit = fixedLit; dist = fixedDist;
        } else if (type === 2) {
          const hlit = bits(5) + 257, hdist = bits(5) + 1, hclen = bits(4) + 4;
          const clen = new Uint8Array(19);
          for (let i = 0; i < hclen; i++) clen[CLORDER[i]] = bits(3);
          const ch = buildHuff(clen);
          const lens = new Uint8Array(hlit + hdist);
          for (let i = 0; i < lens.length;) {
            const sym = decode(ch);
            if (sym < 16) lens[i++] = sym;
            else if (sym === 16) {
              const p = lens[i - 1], n = 3 + bits(2);
              for (let k = 0; k < n; k++) lens[i++] = p;
            } else if (sym === 17) {
              const n = 3 + bits(3);
              for (let k = 0; k < n; k++) lens[i++] = 0;
            } else {
              const n = 11 + bits(7);
              for (let k = 0; k < n; k++) lens[i++] = 0;
            }
          }
          lit = buildHuff(lens.subarray(0, hlit));
          dist = buildHuff(lens.subarray(hlit));
        } else {
          throw new Error('bad block type');
        }
        for (;;) {
          const sym = decode(lit);
          if (sym === 256) break;
          if (sym < 256) {
            grow(1);
            out[olen++] = sym;
          } else {
            const li = sym - 257;
            const len = LBASE[li] + bits(LEXT[li]);
            const ds = decode(dist);
            const d = DBASE[ds] + bits(DEXT[ds]);
            grow(len);
            let from = olen - d;
            for (let k = 0; k < len; k++) out[olen++] = out[from++];
          }
        }
      }
      if (final) break;
    }
    return out.subarray(0, olen);
  }

  /* ------------------------------------------------------------ PNG */
  function decodePNG(bytes) {
    const d = bytes;
    if (d[0] !== 0x89 || d[1] !== 0x50 || d[2] !== 0x4e || d[3] !== 0x47) return null;
    let p = 8;
    let w = 0, h = 0, depth = 8, color = 6, interlace = 0;
    let palette = null, trns = null;
    const idat = [];
    let idatLen = 0;
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    while (p < d.length) {
      const len = dv.getUint32(p); p += 4;
      const type = String.fromCharCode(d[p], d[p + 1], d[p + 2], d[p + 3]); p += 4;
      if (type === 'IHDR') {
        w = dv.getUint32(p); h = dv.getUint32(p + 4);
        depth = d[p + 8]; color = d[p + 9]; interlace = d[p + 12];
      } else if (type === 'PLTE') {
        palette = d.subarray(p, p + len);
      } else if (type === 'tRNS') {
        trns = d.subarray(p, p + len);
      } else if (type === 'IDAT') {
        idat.push(d.subarray(p, p + len));
        idatLen += len;
      } else if (type === 'IEND') {
        break;
      }
      p += len + 4;
    }
    let z;
    if (idat.length === 1) z = idat[0];
    else {
      z = new Uint8Array(idatLen);
      let o = 0;
      for (const c of idat) { z.set(c, o); o += c.length; }
    }
    const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[color];
    const bpl = Math.ceil(w * channels * depth / 8);
    const raw = inflate(z, 2, (bpl + 1) * h);
    if (interlace) throw new Error('interlaced PNG not supported');

    const out = new Uint32Array(w * h);          // 0xAABBGGRR (canvas order)
    const bpp = Math.max(1, Math.ceil(channels * depth / 8));
    const line = new Uint8Array(bpl);
    const prev = new Uint8Array(bpl);
    let rp = 0;
    for (let y = 0; y < h; y++) {
      const filter = raw[rp++];
      line.set(raw.subarray(rp, rp + bpl));
      rp += bpl;
      switch (filter) {
        case 0: break;
        case 1:
          for (let i = bpp; i < bpl; i++) line[i] = (line[i] + line[i - bpp]) & 255;
          break;
        case 2:
          for (let i = 0; i < bpl; i++) line[i] = (line[i] + prev[i]) & 255;
          break;
        case 3:
          for (let i = 0; i < bpl; i++) {
            const a = i >= bpp ? line[i - bpp] : 0;
            line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255;
          }
          break;
        case 4:
          for (let i = 0; i < bpl; i++) {
            const a = i >= bpp ? line[i - bpp] : 0;
            const b = prev[i];
            const c = i >= bpp ? prev[i - bpp] : 0;
            const pp = a + b - c;
            const pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c);
            const pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            line[i] = (line[i] + pr) & 255;
          }
          break;
        default: throw new Error('bad PNG filter ' + filter);
      }
      prev.set(line);
      const row = y * w;
      if (depth === 8) {
        switch (color) {
          case 0:
            for (let x = 0; x < w; x++) {
              const v = line[x];
              out[row + x] = 0xff000000 | (v << 16) | (v << 8) | v;
            }
            break;
          case 2:
            for (let x = 0; x < w; x++) {
              const i = x * 3;
              out[row + x] = 0xff000000 | (line[i + 2] << 16) | (line[i + 1] << 8) | line[i];
            }
            break;
          case 3:
            for (let x = 0; x < w; x++) {
              const idx = line[x], i = idx * 3;
              const a = trns && idx < trns.length ? trns[idx] : 255;
              out[row + x] = (a << 24) | (palette[i + 2] << 16) |
                             (palette[i + 1] << 8) | palette[i];
            }
            break;
          case 4:
            for (let x = 0; x < w; x++) {
              const v = line[x * 2], a = line[x * 2 + 1];
              out[row + x] = (a << 24) | (v << 16) | (v << 8) | v;
            }
            break;
          case 6:
            for (let x = 0; x < w; x++) {
              const i = x * 4;
              out[row + x] = (line[i + 3] << 24) | (line[i + 2] << 16) |
                             (line[i + 1] << 8) | line[i];
            }
            break;
        }
      } else if (depth === 4 || depth === 2 || depth === 1) {
        const per = 8 / depth, mask = (1 << depth) - 1;
        for (let x = 0; x < w; x++) {
          const b = line[(x / per) | 0];
          const shift = 8 - depth - (x % per) * depth;
          let v = (b >> shift) & mask;
          if (color === 3) {
            const i = v * 3;
            const a = trns && v < trns.length ? trns[v] : 255;
            out[row + x] = (a << 24) | (palette[i + 2] << 16) |
                           (palette[i + 1] << 8) | palette[i];
          } else {
            v = Math.round(v * 255 / mask);
            out[row + x] = 0xff000000 | (v << 16) | (v << 8) | v;
          }
        }
      } else if (depth === 16) {
        for (let x = 0; x < w; x++) {
          const i = x * channels * 2;
          const r = line[i], g = channels > 2 ? line[i + 2] : r,
                b = channels > 2 ? line[i + 4] : r;
          const a = (color === 6) ? line[i + 6] : 255;
          out[row + x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
      }
    }
    return { width: w, height: h, pixels: out };
  }

  /* --------------------------------------------------------- PNG encode */
  const crcTable = (function () {
    const t = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      t[n] = c;
    }
    return t;
  })();
  function crc32(buf, start, end) {
    let c = -1;
    for (let i = start; i < end; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return (c ^ -1) >>> 0;
  }
  function adler32(buf) {
    let a = 1, b = 0;
    for (let i = 0; i < buf.length; i++) {
      a = (a + buf[i]) % 65521;
      b = (b + a) % 65521;
    }
    return ((b << 16) | a) >>> 0;
  }

  /** encode RGBA pixels (Uint32Array, canvas order) into a PNG (stored blocks) */
  function encodePNG(px, w, h) {
    const raw = new Uint8Array((w * 4 + 1) * h);
    let o = 0;
    for (let y = 0; y < h; y++) {
      raw[o++] = 0;
      for (let x = 0; x < w; x++) {
        const v = px[y * w + x];
        raw[o++] = v & 255;
        raw[o++] = (v >> 8) & 255;
        raw[o++] = (v >> 16) & 255;
        raw[o++] = (v >>> 24) & 255;
      }
    }
    // zlib stream with stored deflate blocks
    const nblocks = Math.ceil(raw.length / 65535) || 1;
    const z = new Uint8Array(2 + raw.length + nblocks * 5 + 4);
    let p = 0;
    z[p++] = 0x78; z[p++] = 0x01;
    for (let i = 0; i < nblocks; i++) {
      const off = i * 65535;
      const len = Math.min(65535, raw.length - off);
      z[p++] = (i === nblocks - 1) ? 1 : 0;
      z[p++] = len & 255; z[p++] = len >> 8;
      z[p++] = (~len) & 255; z[p++] = (~len >> 8) & 255;
      z.set(raw.subarray(off, off + len), p);
      p += len;
    }
    const ad = adler32(raw);
    z[p++] = (ad >>> 24) & 255; z[p++] = (ad >>> 16) & 255;
    z[p++] = (ad >>> 8) & 255; z[p++] = ad & 255;

    const chunks = [];
    function chunk(type, data) {
      const b = new Uint8Array(12 + data.length);
      const dv = new DataView(b.buffer);
      dv.setUint32(0, data.length);
      for (let i = 0; i < 4; i++) b[4 + i] = type.charCodeAt(i);
      b.set(data, 8);
      dv.setUint32(8 + data.length, crc32(b, 4, 8 + data.length));
      chunks.push(b);
    }
    const ihdr = new Uint8Array(13);
    const idv = new DataView(ihdr.buffer);
    idv.setUint32(0, w); idv.setUint32(4, h);
    ihdr[8] = 8; ihdr[9] = 6;
    chunk('IHDR', ihdr);
    chunk('IDAT', z.subarray(0, p));
    chunk('IEND', new Uint8Array(0));
    let total = 8;
    for (const c of chunks) total += c.length;
    const out = new Uint8Array(total);
    out.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a], 0);
    let q = 8;
    for (const c of chunks) { out.set(c, q); q += c.length; }
    return out;
  }

  return { inflate, decodePNG, encodePNG, crc32, adler32 };
})();

if (typeof module !== 'undefined') module.exports = $img;
