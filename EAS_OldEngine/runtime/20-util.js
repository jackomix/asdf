/* =========================================================================
 * 20-util.js -- java.util.*  (Vector, Set, Queue, Iterator, Random, Date,
 * Calendar, Locale, Pattern)
 *
 * java.util.Random reproduces the exact 48-bit LCG of the JDK so that the
 * game's procedural generation matches the original bit for bit.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  iface('java/util/Collection');
  iface('java/util/List', ['java/util/Collection']);
  iface('java/util/Set', ['java/util/Collection']);
  iface('java/util/Queue', ['java/util/Collection']);
  iface('java/util/Iterator');
  iface('java/util/Enumeration');
  iface('java/util/Map');

  function mkIterator(arrFn) {
    const It = $rt.classes['$Iterator'];
    const o = new It();
    o.$src = arrFn;
    o.$i = 0;
    return o;
  }

  def('$Iterator', null, {
    ctor() { this.$src = null; this.$i = 0; },
    impl: ['java/util/Iterator', 'java/util/Enumeration'],
    m: {
      'hasNext()Z': function () { return this.$i < this.$src().length ? 1 : 0; },
      'next()Ljava/lang/Object;': function () {
        const a = this.$src();
        if (this.$i >= a.length) $rt.raise('java/util/NoSuchElementException');
        return a[this.$i++];
      },
      'hasMoreElements()Z': function () { return this.$i < this.$src().length ? 1 : 0; },
      'nextElement()Ljava/lang/Object;': function () { return this.$src()[this.$i++]; },
      'remove()V': function () {},
    },
  });
  $rt.throwable('java/util/NoSuchElementException', 'java/lang/RuntimeException');
  $rt.mkIterator = mkIterator;

  /* ------------------------------------------------------------- Vector */
  const listSpec = {
    ctor() { this.$a = []; },
    impl: ['java/util/List', 'java/util/Collection', 'java/lang/Iterable'],
    m: {
      '<init>()V': function () { this.$a = []; return this; },
      '<init>(I)V': function () { this.$a = []; return this; },
      '<init>(II)V': function () { this.$a = []; return this; },
      'size()I': function () { return this.$a.length; },
      'isEmpty()Z': function () { return this.$a.length === 0 ? 1 : 0; },
      'add(Ljava/lang/Object;)Z': function (o) { this.$a.push(o); return 1; },
      'add(ILjava/lang/Object;)V': function (i, o) { this.$a.splice(i, 0, o); },
      'addElement(Ljava/lang/Object;)V': function (o) { this.$a.push(o); },
      'insertElementAt(Ljava/lang/Object;I)V': function (o, i) { this.$a.splice(i, 0, o); },
      'get(I)Ljava/lang/Object;': function (i) { return this.$idx(i); },
      'elementAt(I)Ljava/lang/Object;': function (i) { return this.$idx(i); },
      'firstElement()Ljava/lang/Object;': function () {
        if (!this.$a.length) $rt.raise('java/util/NoSuchElementException');
        return this.$a[0];
      },
      'lastElement()Ljava/lang/Object;': function () {
        if (!this.$a.length) $rt.raise('java/util/NoSuchElementException');
        return this.$a[this.$a.length - 1];
      },
      'set(ILjava/lang/Object;)Ljava/lang/Object;': function (i, o) {
        const old = this.$a[i]; this.$a[i] = o; return old;
      },
      'setElementAt(Ljava/lang/Object;I)V': function (o, i) { this.$idx(i); this.$a[i] = o; },
      'remove(I)Ljava/lang/Object;': function (i) { return this.$a.splice(i, 1)[0]; },
      'removeElementAt(I)V': function (i) { this.$idx(i); this.$a.splice(i, 1); },
      'remove(Ljava/lang/Object;)Z': function (o) { return this.$rm(o); },
      'removeElement(Ljava/lang/Object;)Z': function (o) { return this.$rm(o); },
      'removeAllElements()V': function () { this.$a.length = 0; },
      'clear()V': function () { this.$a.length = 0; },
      'contains(Ljava/lang/Object;)Z': function (o) { return this.$ix(o) >= 0 ? 1 : 0; },
      'indexOf(Ljava/lang/Object;)I': function (o) { return this.$ix(o); },
      'lastIndexOf(Ljava/lang/Object;)I': function (o) {
        for (let i = this.$a.length - 1; i >= 0; i--) if ($rt.jEquals(o, this.$a[i])) return i;
        return -1;
      },
      'iterator()Ljava/util/Iterator;': function () { const s = this; return mkIterator(() => s.$a); },
      'elements()Ljava/util/Enumeration;': function () { const s = this; return mkIterator(() => s.$a); },
      'toArray()[Ljava/lang/Object;': function () {
        return $rt.arr.obj('[Ljava/lang/Object;', this.$a);
      },
      'copyInto([Ljava/lang/Object;)V': function (a) {
        for (let i = 0; i < this.$a.length; i++) a[i] = this.$a[i];
      },
      'toString()Ljava/lang/String;': function () {
        return '[' + this.$a.map($rt.jToString).join(', ') + ']';
      },
    },
  };
  const Vector = def('java/util/Vector', null, listSpec);
  Vector.prototype.$idx = function (i) {
    if (i < 0 || i >= this.$a.length) {
      $rt.raise('java/lang/ArrayIndexOutOfBoundsException', '' + i);
    }
    return this.$a[i];
  };
  Vector.prototype.$ix = function (o) {
    for (let i = 0; i < this.$a.length; i++) if ($rt.jEquals(o, this.$a[i])) return i;
    return -1;
  };
  Vector.prototype.$rm = function (o) {
    const i = this.$ix(o);
    if (i < 0) return 0;
    this.$a.splice(i, 1);
    return 1;
  };
  const ArrayList = def('java/util/ArrayList', null, listSpec);
  ArrayList.prototype.$idx = Vector.prototype.$idx;
  ArrayList.prototype.$ix = Vector.prototype.$ix;
  ArrayList.prototype.$rm = Vector.prototype.$rm;

  /* ----------------------------------------------------- LinkedList/Queue */
  const LinkedList = def('java/util/LinkedList', null, {
    ctor() { this.$a = []; },
    impl: ['java/util/List', 'java/util/Queue', 'java/util/Collection',
           'java/lang/Iterable'],
    m: Object.assign({}, listSpec.m, {
      'offer(Ljava/lang/Object;)Z': function (o) { this.$a.push(o); return 1; },
      'poll()Ljava/lang/Object;': function () {
        return this.$a.length ? this.$a.shift() : null;
      },
      'peek()Ljava/lang/Object;': function () {
        return this.$a.length ? this.$a[0] : null;
      },
      'addFirst(Ljava/lang/Object;)V': function (o) { this.$a.unshift(o); },
      'addLast(Ljava/lang/Object;)V': function (o) { this.$a.push(o); },
      'removeFirst()Ljava/lang/Object;': function () { return this.$a.shift(); },
    }),
  });
  LinkedList.prototype.$idx = Vector.prototype.$idx;
  LinkedList.prototype.$ix = Vector.prototype.$ix;
  LinkedList.prototype.$rm = Vector.prototype.$rm;

  /* ---------------------------------------------------------- HashSet */
  const HashSet = def('java/util/HashSet', null, {
    ctor() { this.$a = []; },
    impl: ['java/util/Set', 'java/util/Collection', 'java/lang/Iterable'],
    m: {
      '<init>()V': function () { this.$a = []; return this; },
      '<init>(I)V': function () { this.$a = []; return this; },
      'add(Ljava/lang/Object;)Z': function (o) {
        if (this.$ix(o) >= 0) return 0;
        this.$a.push(o); return 1;
      },
      'remove(Ljava/lang/Object;)Z': function (o) { return this.$rm(o); },
      'contains(Ljava/lang/Object;)Z': function (o) { return this.$ix(o) >= 0 ? 1 : 0; },
      'isEmpty()Z': function () { return this.$a.length === 0 ? 1 : 0; },
      'size()I': function () { return this.$a.length; },
      'clear()V': function () { this.$a.length = 0; },
      'iterator()Ljava/util/Iterator;': function () { const s = this; return mkIterator(() => s.$a); },
      'toString()Ljava/lang/String;': function () {
        return '[' + this.$a.map($rt.jToString).join(', ') + ']';
      },
    },
  });
  HashSet.prototype.$ix = Vector.prototype.$ix;
  HashSet.prototype.$rm = Vector.prototype.$rm;

  def('java/util/HashMap', null, {
    ctor() { this.$k = []; this.$v = []; },
    impl: ['java/util/Map'],
    m: {
      '<init>()V': function () { this.$k = []; this.$v = []; return this; },
      'put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;': function (k, v) {
        for (let i = 0; i < this.$k.length; i++) {
          if ($rt.jEquals(k, this.$k[i])) { const o = this.$v[i]; this.$v[i] = v; return o; }
        }
        this.$k.push(k); this.$v.push(v); return null;
      },
      'get(Ljava/lang/Object;)Ljava/lang/Object;': function (k) {
        for (let i = 0; i < this.$k.length; i++) if ($rt.jEquals(k, this.$k[i])) return this.$v[i];
        return null;
      },
      'containsKey(Ljava/lang/Object;)Z': function (k) {
        for (let i = 0; i < this.$k.length; i++) if ($rt.jEquals(k, this.$k[i])) return 1;
        return 0;
      },
      'size()I': function () { return this.$k.length; },
      'clear()V': function () { this.$k = []; this.$v = []; },
    },
  });

  /* -------------------------------------------------------------- Random */
  const MASK = (1n << 48n) - 1n;
  const MULT = 0x5DEECE66Dn;
  def('java/util/Random', null, {
    ctor() { this.$seed = (BigInt(Date.now()) ^ MULT) & MASK; },
    m: {
      '<init>()V': function () {
        this.$seed = (BigInt(Date.now()) ^ MULT) & MASK; return this;
      },
      '<init>(J)V': function (s) { this.$seed = (BigInt(s) ^ MULT) & MASK; return this; },
      'setSeed(J)V': function (s) { this.$seed = (BigInt(s) ^ MULT) & MASK; },
      'nextInt()I': function () { return this.$next(32); },
      'nextInt(I)I': function (bound) {
        if (bound <= 0) $rt.raise('java/lang/IllegalArgumentException', 'bound must be positive');
        if ((bound & -bound) === bound) {
          return Number((BigInt(bound) * BigInt(this.$next(31))) >> 31n) | 0;
        }
        let bits, val;
        do { bits = this.$next(31); val = bits % bound; }
        while (bits - val + (bound - 1) < 0);
        return val;
      },
      'nextLong()J': function () {
        return BigInt.asIntN(64, (BigInt(this.$next(32)) << 32n) + BigInt(this.$next(32)));
      },
      'nextBoolean()Z': function () { return this.$next(1); },
      'nextFloat()F': function () { return Math.fround(this.$next(24) / (1 << 24)); },
      'nextDouble()D': function () {
        return ((this.$next(26) * 134217728) + this.$next(27)) / 9007199254740992;
      },
    },
  });
  $rt.classes['java/util/Random'].prototype.$next = function (bits) {
    this.$seed = (this.$seed * MULT + 0xBn) & MASK;
    return Number(BigInt.asIntN(32, this.$seed >> BigInt(48 - bits)));
  };

  def('java/security/SecureRandom', 'java/util/Random', {
    m: {
      '<init>()V': function () {
        const b = new Uint32Array(2);
        if (typeof crypto !== 'undefined' && crypto.getRandomValues) crypto.getRandomValues(b);
        else { b[0] = Math.random() * 4294967296; b[1] = Math.random() * 4294967296; }
        this.$seed = ((BigInt(b[0]) << 16n) ^ BigInt(b[1])) & MASK;
        return this;
      },
    },
  });

  /* ---------------------------------------------------------------- Date */
  def('java/util/Date', null, {
    ctor() { this.$d = new Date(); },
    m: {
      '<init>()V': function () { this.$d = new Date(); return this; },
      '<init>(J)V': function (t) { this.$d = new Date(Number(t)); return this; },
      'getTime()J': function () { return BigInt(this.$d.getTime()); },
      'setTime(J)V': function (t) { this.$d = new Date(Number(t)); },
      'getYear()I': function () { return this.$d.getFullYear() - 1900; },
      'getMonth()I': function () { return this.$d.getMonth(); },
      'getDate()I': function () { return this.$d.getDate(); },
      'getDay()I': function () { return this.$d.getDay(); },
      'getHours()I': function () { return this.$d.getHours(); },
      'getMinutes()I': function () { return this.$d.getMinutes(); },
      'getSeconds()I': function () { return this.$d.getSeconds(); },
      'toString()Ljava/lang/String;': function () { return this.$d.toString(); },
    },
  });

  /* ------------------------------------------------------------ Calendar */
  def('java/util/Calendar', null, {
    ctor() { this.$d = new Date(); },
    sf: { YEAR: 1, MONTH: 2, DATE: 5, DAY_OF_MONTH: 5, DAY_OF_WEEK: 7,
          HOUR: 10, HOUR_OF_DAY: 11, MINUTE: 12, SECOND: 13, MILLISECOND: 14 },
    m: {
      'setTime(Ljava/util/Date;)V': function (d) { this.$d = new Date(d.$d.getTime()); },
      'getTime()Ljava/util/Date;': function () {
        const D = new ($rt.classes['java/util/Date'])();
        D.$d = new Date(this.$d.getTime());
        return D;
      },
      'setTimeInMillis(J)V': function (t) { this.$d = new Date(Number(t)); },
      'getTimeInMillis()J': function () { return BigInt(this.$d.getTime()); },
      'add(II)V': function (f, v) {
        const d = this.$d;
        switch (f) {
          case 1: d.setFullYear(d.getFullYear() + v); break;
          case 2: d.setMonth(d.getMonth() + v); break;
          case 5: d.setDate(d.getDate() + v); break;
          case 11: d.setHours(d.getHours() + v); break;
          case 12: d.setMinutes(d.getMinutes() + v); break;
          case 13: d.setSeconds(d.getSeconds() + v); break;
        }
      },
      'get(I)I': function (f) {
        const d = this.$d;
        switch (f) {
          case 1: return d.getFullYear();
          case 2: return d.getMonth();
          case 5: return d.getDate();
          case 7: return d.getDay() + 1;
          case 10: return d.getHours() % 12;
          case 11: return d.getHours();
          case 12: return d.getMinutes();
          case 13: return d.getSeconds();
          case 14: return d.getMilliseconds();
          case 6: {
            const s = new Date(d.getFullYear(), 0, 0);
            return Math.floor((d - s) / 86400000);
          }
          default: return 0;
        }
      },
    },
    s: {
      'getInstance()Ljava/util/Calendar;': function () {
        return new ($rt.classes['java/util/Calendar'])();
      },
    },
  });

  /* -------------------------------------------------------------- Locale */
  const Locale = def('java/util/Locale', null, {
    ctor() { this.$lang = 'en'; this.$country = 'US'; },
    m: {
      '<init>(Ljava/lang/String;)V': function (l) { this.$lang = l; this.$country = ''; return this; },
      '<init>(Ljava/lang/String;Ljava/lang/String;)V': function (l, c) {
        this.$lang = l; this.$country = c; return this;
      },
      'getLanguage()Ljava/lang/String;': function () { return this.$lang; },
      'getCountry()Ljava/lang/String;': function () { return this.$country; },
      'equals(Ljava/lang/Object;)Z': function (o) {
        return (o && o.$lang === this.$lang && o.$country === this.$country) ? 1 : 0;
      },
      'toString()Ljava/lang/String;': function () {
        return this.$country ? this.$lang + '_' + this.$country : this.$lang;
      },
    },
    s: {
      'getDefault()Ljava/util/Locale;': function () {
        return $rt.classes['java/util/Locale'].$default;
      },
    },
  });
  function mkLocale(l, c) {
    const o = new Locale();
    o.$lang = l; o.$country = c;
    return o;
  }
  Locale.s_JAPAN = mkLocale('ja', 'JP');
  Locale.s_JAPANESE = mkLocale('ja', '');
  Locale.s_ENGLISH = mkLocale('en', '');
  Locale.s_US = mkLocale('en', 'US');
  Locale.$default = Locale.s_US;
  $rt.setLocale = function (tag) {
    const p = String(tag || 'en-US').split(/[-_]/);
    Locale.$default = mkLocale(p[0].toLowerCase(), (p[1] || '').toUpperCase());
  };

  /* ------------------------------------------------------------- Pattern */
  def('java/util/regex/Pattern', null, {
    s: {
      'quote(Ljava/lang/String;)Ljava/lang/String;':
        (s) => String(s).replace(/[.*+?^${}()|[\]\\]/g, '\\$&'),
      'compile(Ljava/lang/String;)Ljava/util/regex/Pattern;': function (p) {
        const o = new ($rt.classes['java/util/regex/Pattern'])();
        o.$re = p;
        return o;
      },
      'matches(Ljava/lang/String;Ljava/lang/CharSequence;)Z': (p, s) =>
        (new RegExp('^(?:' + p + ')$').test($rt.jToString(s)) ? 1 : 0),
    },
  });
})($rt);
