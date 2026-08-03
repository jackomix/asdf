/* =========================================================================
 * 25-io.js -- java.io.*, java.net.*, java.security.*
 *
 * File I/O is backed by a small virtual file system ($rt.fs) that persists
 * into localStorage, which is the browser equivalent of the app's private
 * data directory used for save games.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  $rt.throwable('java/io/IOException', 'java/lang/Exception');
  $rt.throwable('java/io/FileNotFoundException', 'java/io/IOException');
  $rt.throwable('java/io/EOFException', 'java/io/IOException');
  $rt.throwable('java/io/UnsupportedEncodingException', 'java/io/IOException');
  $rt.throwable('java/io/InterruptedIOException', 'java/io/IOException');

  /* ------------------------------------------------- virtual file system */
  const FS_PREFIX = 'eas.fs:';
  const memfs = Object.create(null);
  const store = (function () {
    try {
      if (typeof localStorage !== 'undefined') {
        localStorage.setItem(FS_PREFIX + '$probe', '1');
        localStorage.removeItem(FS_PREFIX + '$probe');
        return localStorage;
      }
    } catch (e) { /* private mode */ }
    return null;
  })();

  const fs = {
    norm(p) { return String(p).replace(/\/+/g, '/').replace(/^\.\//, ''); },
    read(p) {
      p = fs.norm(p);
      if (p in memfs) return memfs[p];
      if (store) {
        const v = store.getItem(FS_PREFIX + p);
        if (v !== null) { memfs[p] = $rt.b64ToBytes(v); return memfs[p]; }
      }
      return null;
    },
    write(p, u8) {
      p = fs.norm(p);
      memfs[p] = u8;
      if (store) {
        try { store.setItem(FS_PREFIX + p, $rt.bytesToB64(u8)); }
        catch (e) { console.warn('[fs] quota exceeded for ' + p); }
      }
    },
    exists(p) { return fs.read(p) !== null; },
    remove(p) {
      p = fs.norm(p);
      delete memfs[p];
      if (store) store.removeItem(FS_PREFIX + p);
    },
    list(dir) {
      const out = new Set();
      const pre = dir ? fs.norm(dir).replace(/\/$/, '') + '/' : '';
      const add = (k) => {
        if (!k.startsWith(pre)) return;
        const rest = k.slice(pre.length);
        if (!rest) return;
        out.add(rest.split('/')[0]);
      };
      Object.keys(memfs).forEach(add);
      if (store) {
        for (let i = 0; i < store.length; i++) {
          const k = store.key(i);
          if (k && k.startsWith(FS_PREFIX)) add(k.slice(FS_PREFIX.length));
        }
      }
      return Array.from(out);
    },
  };
  $rt.fs = fs;

  /* ------------------------------------------------------------- streams */
  function u8(bytes) {
    if (bytes === null || bytes === undefined) return null;
    return bytes instanceof Uint8Array ? bytes
      : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }
  $rt.u8 = u8;

  const InputStream = def('java/io/InputStream', null, {
    ctor() { this.$b = new Uint8Array(0); this.$p = 0; this.$mark = 0; },
    m: {
      '<init>()V': function () { return this; },
      'read()I': function () {
        return this.$p < this.$b.length ? this.$b[this.$p++] : -1;
      },
      'read([B)I': function (dst) {
        return this[mangle('read([BII)I')](dst, 0, dst.length);
      },
      'read([BII)I': function (dst, off, len) {
        if (this.$p >= this.$b.length) return len === 0 ? 0 : -1;
        const n = Math.min(len, this.$b.length - this.$p);
        const d = u8(dst);
        d.set(this.$b.subarray(this.$p, this.$p + n), off);
        this.$p += n;
        return n;
      },
      'skip(J)J': function (n) {
        const k = Math.min(Number(n), this.$b.length - this.$p);
        this.$p += k;
        return BigInt(k);
      },
      'available()I': function () { return this.$b.length - this.$p; },
      'close()V': function () {},
      'mark(I)V': function () { this.$mark = this.$p; },
      'reset()V': function () { this.$p = this.$mark; },
      'markSupported()Z': function () { return 1; },
    },
  });

  def('java/io/ByteArrayInputStream', 'java/io/InputStream', {
    m: {
      '<init>([B)V': function (b) { this.$b = u8(b); this.$p = 0; return this; },
      '<init>([BII)V': function (b, o, n) {
        this.$b = u8(b).subarray(o, o + n); this.$p = 0; return this;
      },
    },
  });

  def('java/io/FileInputStream', 'java/io/InputStream', {
    m: {
      '<init>(Ljava/lang/String;)V': function (p) {
        const d = fs.read(p);
        if (d === null) $rt.raise('java/io/FileNotFoundException', p);
        this.$b = d; this.$p = 0; return this;
      },
      '<init>(Ljava/io/File;)V': function (f) {
        return this[mangle('<init>(Ljava/lang/String;)V')](f.$path);
      },
    },
  });

  def('java/io/DataInputStream', 'java/io/InputStream', {
    ctor() { this.$in = null; },
    m: {
      '<init>(Ljava/io/InputStream;)V': function (is) { this.$in = is; return this; },
      'read()I': function () { return $rt.invoke(this.$in, 'read()I', []); },
      'read([B)I': function (b) { return $rt.invoke(this.$in, 'read([B)I', [b]); },
      'read([BII)I': function (b, o, n) {
        return $rt.invoke(this.$in, 'read([BII)I', [b, o, n]);
      },
      'available()I': function () { return $rt.invoke(this.$in, 'available()I', []); },
      'close()V': function () { $rt.invoke(this.$in, 'close()V', []); },
      'skip(J)J': function (n) { return $rt.invoke(this.$in, 'skip(J)J', [n]); },
      'readByte()B': function () {
        const v = $rt.invoke(this.$in, 'read()I', []);
        if (v < 0) $rt.raise('java/io/EOFException');
        return (v << 24) >> 24;
      },
      'readUnsignedByte()I': function () {
        const v = $rt.invoke(this.$in, 'read()I', []);
        if (v < 0) $rt.raise('java/io/EOFException');
        return v;
      },
      'readBoolean()Z': function () { return this[mangle('readUnsignedByte()I')]() ? 1 : 0; },
      'readShort()S': function () {
        const a = this[mangle('readUnsignedByte()I')]();
        const b = this[mangle('readUnsignedByte()I')]();
        return (((a << 8) | b) << 16) >> 16;
      },
      'readUnsignedShort()I': function () {
        const a = this[mangle('readUnsignedByte()I')]();
        const b = this[mangle('readUnsignedByte()I')]();
        return (a << 8) | b;
      },
      'readChar()C': function () { return this[mangle('readUnsignedShort()I')](); },
      'readInt()I': function () {
        return (this[mangle('readUnsignedShort()I')]() << 16) |
                this[mangle('readUnsignedShort()I')]();
      },
      'readLong()J': function () {
        return BigInt.asIntN(64,
          (BigInt(this[mangle('readInt()I')]() >>> 0) << 32n) |
           BigInt(this[mangle('readInt()I')]() >>> 0));
      },
      'readFully([B)V': function (b) {
        for (let i = 0; i < b.length; i++) b[i] = this[mangle('readByte()B')]();
      },
    },
  });

  const OutputStream = def('java/io/OutputStream', null, {
    ctor() { this.$o = []; },
    m: {
      '<init>()V': function () { this.$o = []; return this; },
      'write(I)V': function (b) { this.$o.push(b & 0xff); },
      'write([B)V': function (b) {
        this[mangle('write([BII)V')](b, 0, b.length);
      },
      'write([BII)V': function (b, off, len) {
        for (let i = 0; i < len; i++) {
          $rt.invoke(this, 'write(I)V', [b[off + i] & 0xff]);
        }
      },
      'flush()V': function () {},
      'close()V': function () {},
    },
  });

  def('java/io/ByteArrayOutputStream', 'java/io/OutputStream', {
    ctor() { this.$buf = new Uint8Array(256); this.$n = 0; },
    m: {
      '<init>()V': function () { this.$buf = new Uint8Array(256); this.$n = 0; return this; },
      '<init>(I)V': function (n) {
        this.$buf = new Uint8Array(Math.max(16, n)); this.$n = 0; return this;
      },
      'write(I)V': function (b) { this.$grow(1); this.$buf[this.$n++] = b & 0xff; },
      'write([B)V': function (b) { this[mangle('write([BII)V')](b, 0, b.length); },
      'write([BII)V': function (b, off, len) {
        this.$grow(len);
        this.$buf.set(u8(b).subarray(off, off + len), this.$n);
        this.$n += len;
      },
      'toByteArray()[B': function () {
        return new Int8Array(this.$buf.buffer.slice(0, this.$n));
      },
      'size()I': function () { return this.$n; },
      'reset()V': function () { this.$n = 0; },
      'toString()Ljava/lang/String;': function () {
        return new TextDecoder().decode(this.$buf.subarray(0, this.$n));
      },
    },
  });
  $rt.classes['java/io/ByteArrayOutputStream'].prototype.$grow = function (extra) {
    if (this.$n + extra <= this.$buf.length) return;
    let cap = this.$buf.length * 2;
    while (cap < this.$n + extra) cap *= 2;
    const nb = new Uint8Array(cap);
    nb.set(this.$buf.subarray(0, this.$n));
    this.$buf = nb;
  };

  def('java/io/FileOutputStream', 'java/io/OutputStream', {
    ctor() { this.$path = null; this.$buf = new Uint8Array(256); this.$n = 0; },
    m: {
      '<init>(Ljava/lang/String;)V': function (p) {
        this.$path = fs.norm(p); this.$n = 0; return this;
      },
      '<init>(Ljava/io/File;)V': function (f) {
        this.$path = fs.norm(f.$path); this.$n = 0; return this;
      },
      '<init>(Ljava/lang/String;Z)V': function (p, append) {
        this.$path = fs.norm(p);
        this.$n = 0;
        if (append) {
          const old = fs.read(this.$path);
          if (old) { this.$buf = old.slice(); this.$n = old.length; }
        }
        return this;
      },
      'write(I)V': function (b) { this.$grow(1); this.$buf[this.$n++] = b & 0xff; },
      'write([B)V': function (b) { this[mangle('write([BII)V')](b, 0, b.length); },
      'write([BII)V': function (b, off, len) {
        this.$grow(len);
        this.$buf.set(u8(b).subarray(off, off + len), this.$n);
        this.$n += len;
      },
      'flush()V': function () { this.$commit(); },
      'close()V': function () { this.$commit(); },
    },
  });
  const FOS = $rt.classes['java/io/FileOutputStream'];
  FOS.prototype.$grow = $rt.classes['java/io/ByteArrayOutputStream'].prototype.$grow;
  FOS.prototype.$commit = function () {
    if (this.$path) fs.write(this.$path, this.$buf.slice(0, this.$n));
  };

  def('java/io/DataOutputStream', 'java/io/OutputStream', {
    ctor() { this.$out = null; },
    m: {
      '<init>(Ljava/io/OutputStream;)V': function (o) { this.$out = o; return this; },
      'write(I)V': function (b) { $rt.invoke(this.$out, 'write(I)V', [b]); },
      'write([B)V': function (b) { $rt.invoke(this.$out, 'write([B)V', [b]); },
      'write([BII)V': function (b, o, n) {
        $rt.invoke(this.$out, 'write([BII)V', [b, o, n]);
      },
      'writeByte(I)V': function (b) { $rt.invoke(this.$out, 'write(I)V', [b]); },
      'writeShort(I)V': function (v) {
        $rt.invoke(this.$out, 'write(I)V', [(v >> 8) & 0xff]);
        $rt.invoke(this.$out, 'write(I)V', [v & 0xff]);
      },
      'writeInt(I)V': function (v) {
        for (let i = 3; i >= 0; i--) $rt.invoke(this.$out, 'write(I)V', [(v >> (i * 8)) & 0xff]);
      },
      'flush()V': function () { $rt.invoke(this.$out, 'flush()V', []); },
      'close()V': function () { $rt.invoke(this.$out, 'close()V', []); },
    },
  });

  /* ---------------------------------------------------------------- File */
  def('java/io/File', null, {
    ctor() { this.$path = ''; },
    m: {
      '<init>(Ljava/lang/String;)V': function (p) { this.$path = fs.norm(p); return this; },
      '<init>(Ljava/lang/String;Ljava/lang/String;)V': function (d, n) {
        this.$path = fs.norm(d + '/' + n); return this;
      },
      '<init>(Ljava/io/File;Ljava/lang/String;)V': function (d, n) {
        this.$path = fs.norm(d.$path + '/' + n); return this;
      },
      'getPath()Ljava/lang/String;': function () { return this.$path; },
      'getAbsolutePath()Ljava/lang/String;': function () {
        return this.$path.charAt(0) === '/' ? this.$path : '/' + this.$path;
      },
      'getName()Ljava/lang/String;': function () {
        return this.$path.substring(this.$path.lastIndexOf('/') + 1);
      },
      'exists()Z': function () { return fs.exists(this.$path) ? 1 : 0; },
      'isDirectory()Z': function () { return fs.list(this.$path).length ? 1 : 0; },
      'mkdirs()Z': function () { return 1; },
      'mkdir()Z': function () { return 1; },
      'delete()Z': function () { fs.remove(this.$path); return 1; },
      'length()J': function () {
        const d = fs.read(this.$path);
        return BigInt(d ? d.length : 0);
      },
      'list()[Ljava/lang/String;': function () {
        return $rt.arr.obj('[Ljava/lang/String;', fs.list(this.$path));
      },
      'toString()Ljava/lang/String;': function () { return this.$path; },
    },
  });

  /* ----------------------------------------------------------- java.net */
  // There is no socket API in a browser sandbox; the game treats a failing
  // connection exactly like an offline device, which is what we emulate.
  def('java/net/URLConnection', null, {});
  def('java/net/HttpURLConnection', 'java/net/URLConnection', {
    ctor() { this.$url = ''; },
    m: {
      'setDoInput(Z)V': function () {},
      'setDoOutput(Z)V': function () {},
      'setRequestMethod(Ljava/lang/String;)V': function () {},
      'setRequestProperty(Ljava/lang/String;Ljava/lang/String;)V': function () {},
      'setConnectTimeout(I)V': function () {},
      'setReadTimeout(I)V': function () {},
      'connect()V': function () {
        $rt.raise('java/io/IOException', 'network unavailable: ' + this.$url);
      },
      'getResponseCode()I': function () {
        $rt.raise('java/io/IOException', 'network unavailable');
      },
      'getContentLength()I': function () { return -1; },
      'getInputStream()Ljava/io/InputStream;': function () {
        $rt.raise('java/io/IOException', 'network unavailable');
      },
      'getOutputStream()Ljava/io/OutputStream;': function () {
        $rt.raise('java/io/IOException', 'network unavailable');
      },
      'disconnect()V': function () {},
    },
  });
  def('java/net/URL', null, {
    ctor() { this.$url = ''; },
    m: {
      '<init>(Ljava/lang/String;)V': function (s) { this.$url = s; return this; },
      'openConnection()Ljava/net/URLConnection;': function () {
        const c = new ($rt.classes['java/net/HttpURLConnection'])();
        c.$url = this.$url;
        return c;
      },
      'toString()Ljava/lang/String;': function () { return this.$url; },
    },
  });

  /* ------------------------------------------------------ java.security */
  $rt.throwable('java/security/GeneralSecurityException', 'java/lang/Exception');
  $rt.throwable('java/security/NoSuchAlgorithmException',
                'java/security/GeneralSecurityException');
  $rt.throwable('java/security/InvalidKeyException',
                'java/security/GeneralSecurityException');
  $rt.throwable('java/security/SignatureException',
                'java/security/GeneralSecurityException');
  $rt.throwable('java/security/spec/InvalidKeySpecException',
                'java/security/GeneralSecurityException');
  iface('java/security/Key');
  iface('java/security/PublicKey', ['java/security/Key']);
  iface('java/security/spec/KeySpec');
  def('java/security/spec/X509EncodedKeySpec', null, {
    impl: ['java/security/spec/KeySpec'],
    m: { '<init>([B)V': function (b) { this.$b = b; return this; } },
  });
  def('java/security/KeyFactory', null, {
    s: {
      'getInstance(Ljava/lang/String;)Ljava/security/KeyFactory;': function () {
        return new ($rt.classes['java/security/KeyFactory'])();
      },
    },
    m: {
      'generatePublic(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;':
        function (spec) {
          const k = new ($rt.classes['$PublicKey'])();
          k.$spec = spec;
          return k;
        },
    },
  });
  def('$PublicKey', null, { impl: ['java/security/PublicKey', 'java/security/Key'] });
  def('java/security/Signature', null, {
    s: {
      'getInstance(Ljava/lang/String;)Ljava/security/Signature;': function () {
        return new ($rt.classes['java/security/Signature'])();
      },
    },
    m: {
      'initVerify(Ljava/security/PublicKey;)V': function () {},
      'update([B)V': function () {},
      'verify([B)Z': function () { return 0; },
    },
  });
})($rt);
