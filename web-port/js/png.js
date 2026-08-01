/* ============================================================================
 * png.js — synchronous PNG decoder for BitmapFactory.decodeByteArray.
 *
 * The game's engine decodes .png payloads embedded in its .dat asset packs
 * inside a single VM call, so the browser's (async) HTMLImageElement decode
 * cannot be used. Implemented: DEFLATE (RFC 1951) + zlib wrapper + PNG
 * unfiltering for 8-bit gray / RGB / palette / gray+alpha / RGBA, plus
 * sub-8-bit palette & gray depths. Adam7 interlace is rejected (unused by
 * the Kairosoft toolchain — throws loudly so it can be added if ever met).
 * ========================================================================== */
'use strict';

const PNG = (() => {

  /* ---------------- DEFLATE ---------------- */
  const CLEN_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
  const CLEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const CLEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  const CDIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  const CDIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

  function buildHuffman(lengths) {
    // canonical huffman; returns {table: Map code(len,code)->sym, maxLen}
    let maxLen = 0;
    for (const l of lengths) if (l > maxLen) maxLen = l;
    const blCount = new Array(maxLen + 1).fill(0);
    for (const l of lengths) if (l > 0) blCount[l]++;
    const nextCode = new Array(maxLen + 1).fill(0);
    let code = 0;
    for (let i = 1; i <= maxLen; i++) {
      code = (code + blCount[i - 1]) << 1;
      nextCode[i] = code;
    }
    const table = new Map();
    for (let i = 0; i < lengths.length; i++) {
      const l = lengths[i];
      if (l > 0) {
        const c = nextCode[l]++;
        table.set((l << 16) | c, i);
      }
    }
    return { table, maxLen };
  }

  function inflate(data) {
    let pos = 0, bitPos = 0;
    let out = new Uint8Array(Math.max(1024, data.length * 4));
    let outLen = 0;
    const grow = (need) => {
      if (outLen + need <= out.length) return;
      const n = new Uint8Array(Math.max(out.length * 2, outLen + need * 2));
      n.set(out.subarray(0, outLen));
      out = n;
    };
    const readBit = () => {
      const v = (data[pos] >> bitPos) & 1;
      bitPos++;
      if (bitPos === 8) { bitPos = 0; pos++; }
      return v;
    };
    const readBits = (n) => {
      let v = 0;
      for (let i = 0; i < n; i++) { v |= ((data[pos] >> bitPos) & 1) << i; bitPos++; if (bitPos === 8) { bitPos = 0; pos++; } }
      return v;
    };
    const readSym = (hf) => {
      let code = 0, len = 0;
      for (;;) {
        code = (code << 1) | ((data[pos] >> bitPos) & 1);
        bitPos++;
        if (bitPos === 8) { bitPos = 0; pos++; }
        len++;
        const sym = hf.table.get((len << 16) | code);
        if (sym !== undefined) return sym;
        if (len > hf.maxLen) throw new Error('bad huffman code');
      }
    };

    let final = false;
    while (!final) {
      final = readBit() === 1;
      const type = readBits(2);
      if (type === 0) {
        if (bitPos !== 0) { bitPos = 0; pos++; }
        const len = data[pos] | (data[pos + 1] << 8);
        pos += 4;
        grow(len);
        out.set(data.subarray(pos, pos + len), outLen);
        pos += len; outLen += len;
      } else if (type === 1 || type === 2) {
        let litHf, distHf;
        if (type === 1) {
          const litLen = new Array(288).fill(8);
          for (let i = 144; i < 256; i++) litLen[i] = 9;
          for (let i = 256; i < 280; i++) litLen[i] = 7;
          litHf = buildHuffman(litLen);
          distHf = buildHuffman(new Array(30).fill(5));
        } else {
          const hlit = readBits(5) + 257;
          const hdist = readBits(5) + 1;
          const hclen = readBits(4) + 4;
          const clenLen = new Array(19).fill(0);
          for (let i = 0; i < hclen; i++) clenLen[CLEN_ORDER[i]] = readBits(3);
          const clenHf = buildHuffman(clenLen);
          const lens = [];
          while (lens.length < hlit + hdist) {
            const sym = readSym(clenHf);
            if (sym === 16) {
              const rep = readBits(2) + 3;
              const prev = lens[lens.length - 1];
              for (let i = 0; i < rep; i++) lens.push(prev);
            } else if (sym === 17) {
              const rep = readBits(3) + 3;
              for (let i = 0; i < rep; i++) lens.push(0);
            } else if (sym === 18) {
              const rep = readBits(7) + 11;
              for (let i = 0; i < rep; i++) lens.push(0);
            } else lens.push(sym);
          }
          litHf = buildHuffman(lens.slice(0, hlit));
          distHf = buildHuffman(lens.slice(hlit));
        }
        for (;;) {
          const sym = readSym(litHf);
          if (sym === 256) break;
          if (sym < 256) {
            grow(1);
            out[outLen++] = sym;
          } else {
            const lb = CLEN_BASE[sym - 257], le = CLEN_EXTRA[sym - 257];
            const len = lb + readBits(le);
            const dSym = readSym(distHf);
            const db = CDIST_BASE[dSym], de = CDIST_EXTRA[dSym];
            const dist = db + readBits(de);
            grow(len);
            let src = outLen - dist;
            for (let i = 0; i < len; i++) out[outLen++] = out[src++];
          }
        }
      } else {
        throw new Error('deflate: bad block type ' + type);
      }
    }
    return out.subarray(0, outLen);
  }

  /* ---------------- zlib ---------------- */
  function zlibDecode(data, off) {
    // 2-byte header, deflate stream; adler32 ignored
    return inflate(data.subarray(off + 2), 0);
  }

  /* ---------------- PNG ---------------- */
  function isPNG(bytes) {
    return bytes.length >= 8 && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47;
  }
  function isJPEG(bytes) {
    return bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8;
  }

  function decode(bytes) {
    if (!isPNG(bytes)) return null;
    let off = 8;
    let width = 0, height = 0, bitDepth = 8, colorType = 6, interlace = 0;
    let palette = null, trns = null;
    const idat = [];
    while (off + 8 <= bytes.length) {
      const len = (bytes[off] << 24 | bytes[off + 1] << 16 | bytes[off + 2] << 8 | bytes[off + 3]) >>> 0;
      const type = String.fromCharCode(bytes[off + 4], bytes[off + 5], bytes[off + 6], bytes[off + 7]);
      const body = bytes.subarray(off + 8, off + 8 + len);
      if (type === 'IHDR') {
        width = (body[0] << 24 | body[1] << 16 | body[2] << 8 | body[3]) >>> 0;
        height = (body[4] << 24 | body[5] << 16 | body[6] << 8 | body[7]) >>> 0;
        bitDepth = body[8]; colorType = body[9];
        interlace = body[12];
      } else if (type === 'PLTE') {
        palette = body;
      } else if (type === 'tRNS') {
        trns = body;
      } else if (type === 'IDAT') {
        for (let i = 0; i < body.length; i++) idat.push(body[i]);
      } else if (type === 'IEND') break;
      off += 12 + len;
    }
    if (interlace !== 0) throw new Error('interlaced PNG unsupported (Adam7)');
    const raw = zlibDecode(Uint8Array.from(idat), 0);

    const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[colorType];
    if (channels === undefined) throw new Error('png colorType ' + colorType);
    const bpp = Math.max(1, Math.ceil(channels * bitDepth / 8));
    const scanlen = Math.ceil(channels * bitDepth * width / 8);
    const out = new Uint8Array(width * height * 4);

    let prev = new Uint8Array(scanlen);
    let pos = 0;
    // for sub-byte packing extraction
    const getPacked = (row, i, depth) => {
      const perByte = 8 / depth;
      const byte = row[(i / perByte) | 0];
      const shift = 8 - depth * ((i % perByte) + 1);
      const mask = (1 << depth) - 1;
      return (byte >> shift) & mask;
    };

    for (let y = 0; y < height; y++) {
      const filter = raw[pos++];
      const row = raw.subarray(pos, pos + scanlen);
      pos += scanlen;
      const cur = new Uint8Array(scanlen);
      for (let x = 0; x < scanlen; x++) {
        const a = x >= bpp ? cur[x - bpp] : 0;
        const b = prev[x];
        const c = x >= bpp ? prev[x - bpp] : 0;
        let v = row[x];
        switch (filter) {
          case 0: break;
          case 1: v = (v + a) & 0xff; break;
          case 2: v = (v + b) & 0xff; break;
          case 3: v = (v + ((a + b) >> 1)) & 0xff; break;
          case 4: {
            const p = a + b - c;
            const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
            v = (v + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c)) & 0xff;
            break;
          }
          default: throw new Error('png filter ' + filter);
        }
        cur[x] = v;
      }

      // expand to RGBA
      for (let x = 0; x < width; x++) {
        const di = (y * width + x) * 4;
        if (colorType === 6) {
          out[di] = cur[x * 4]; out[di + 1] = cur[x * 4 + 1]; out[di + 2] = cur[x * 4 + 2]; out[di + 3] = cur[x * 4 + 3];
        } else if (colorType === 2) {
          out[di] = cur[x * 3]; out[di + 1] = cur[x * 3 + 1]; out[di + 2] = cur[x * 3 + 2]; out[di + 3] = 255;
          if (trns) {
            const tr = (trns[0] << 8) | trns[1], tg = (trns[2] << 8) | trns[3], tb = (trns[4] << 8) | trns[5];
            if (cur[x * 3] === tr && cur[x * 3 + 1] === tg && cur[x * 3 + 2] === tb) out[di + 3] = 0;
          }
        } else if (colorType === 3) {
          const idx = bitDepth === 8 ? cur[x] : getPacked(cur, x, bitDepth);
          out[di] = palette[idx * 3]; out[di + 1] = palette[idx * 3 + 1]; out[di + 2] = palette[idx * 3 + 2];
          out[di + 3] = trns && idx < trns.length ? trns[idx] : 255;
        } else if (colorType === 0) {
          let g;
          if (bitDepth === 8) g = cur[x];
          else if (bitDepth === 16) g = cur[x * 2];
          else g = Math.round(getPacked(cur, x, bitDepth) * 255 / ((1 << bitDepth) - 1));
          out[di] = g; out[di + 1] = g; out[di + 2] = g; out[di + 3] = 255;
        } else if (colorType === 4) {
          out[di] = cur[x * 2]; out[di + 1] = cur[x * 2]; out[di + 2] = cur[x * 2];
          out[di + 3] = cur[x * 2 + 1];
        }
      }
      prev = cur;
    }
    return { w: width, h: height, rgba: out };
  }

  return { decode, isPNG, isJPEG };
})();

if (typeof module !== 'undefined') module.exports = PNG;
