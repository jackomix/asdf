"""A miniature Java runtime behind UnityEngine.AndroidJNI.

Kairosoft's C# layer (java.lang.JThread, java.util.JLocale,
java.native.JavaPlugin, kairo.unity.util.DisplayMetrics, ...) is a port of
their old Java engine, so it still reaches the platform through
AndroidJavaObject / AndroidJavaClass.  Those types are pure managed code
inside the shipped libil2cpp.so; the only thing that is missing off Android
is the ~160 `UnityEngine.AndroidJNI::*` internal calls they bottom out in.

This module supplies them.  It is not an emulator of Dalvik: it is a handle
table plus a small dispatch table for exactly the classes the game asks for
(java.util.Locale, java.lang.System, android.util.DisplayMetrics,
com.unity3d.player.UnityPlayer, ...).  Anything unknown is answered with a
type-correct default derived from the JNI descriptor and recorded in
`jvm.unknown`, so a run tells us precisely what still needs a body instead of
dying on a NullReferenceException.

Unity's own managed reflection layer (AndroidReflection) is routed through
`com/unity3d/player/ReflectionHelper`, whose four static methods are
implemented here, so `AndroidJavaObject.Call<T>("name", args)` resolves method
IDs exactly the way it does on a device.
"""
import struct
import time
from collections import Counter

from . import unity as U
from .unity import icall

# --------------------------------------------------------------- registries
METHODS = {}          # (class, method-name) -> handler
FIELDS = {}           # (class, field-name) -> value | callable
TEMPLATE = {}         # class -> callable(jvm) -> dict of instance fields


def jmethod(cls, *names):
    cls = cls.replace('.', '/')

    def deco(fn):
        for n in names:
            METHODS[(cls, n)] = fn
        return fn
    return deco


def jfield(cls, name, value):
    FIELDS[(cls.replace('.', '/'), name)] = value


def jtemplate(cls):
    def deco(fn):
        TEMPLATE[cls.replace('.', '/')] = fn
        return fn
    return deco


# ------------------------------------------------------------- descriptors
def split_descriptor(sig):
    """'(Ljava/lang/String;IZ)V' -> (['Ljava/lang/String;', 'I', 'Z'], 'V')"""
    if not sig or '(' not in sig:
        return [], 'V'
    inner = sig[sig.index('(') + 1:sig.index(')')]
    ret = sig[sig.index(')') + 1:] or 'V'
    args, i = [], 0
    while i < len(inner):
        c = inner[i]
        if c == '[':
            j = i
            while j < len(inner) and inner[j] == '[':
                j += 1
            if inner[j] == 'L':
                k = inner.index(';', j)
                args.append(inner[i:k + 1])
                i = k + 1
            else:
                args.append(inner[i:j + 1])
                i = j + 1
        elif c == 'L':
            k = inner.index(';', i)
            args.append(inner[i:k + 1])
            i = k + 1
        else:
            args.append(c)
            i += 1
    return args, ret


def class_of(desc):
    """'Ljava/lang/String;' -> 'java/lang/String'"""
    if desc.startswith('L') and desc.endswith(';'):
        return desc[1:-1]
    return desc


# ------------------------------------------------------------------- values
class JRef(object):
    """One entry in the JNI handle table."""

    def __init__(self, handle, cls, value=None, fields=None):
        self.handle = handle
        self.cls = cls
        self.value = value                     # str / list / python payload
        self.fields = fields if fields is not None else {}

    def __repr__(self):
        v = '' if self.value is None else ' %r' % (self.value,)
        return '<j %s%s #%x>' % (self.cls, v, self.handle)


class JVM(object):
    """The handle table and dispatcher."""

    BASE = 0x4A564D00                          # 'JVM\0' - never a real pointer

    def __init__(self, host):
        self.h = host
        self.refs = {}
        self.next = self.BASE
        self.classes = {}
        self.statics = {}                      # class -> {field: value}
        self.calls = Counter()
        self.unknown = Counter()
        self.trace = []
        self.max_trace = 400
        self.thread = None
        self.log = getattr(host, 'verbose', 1) > 1
        self.list_calls = getattr(host, 'verbose', 1) > 1

    # ------------------------------------------------------------- handles
    def _new(self, cls, value=None, fields=None):
        self.next += 8
        r = JRef(self.next, cls, value, fields)
        self.refs[r.handle] = r
        return r

    def ref(self, handle):
        return self.refs.get(handle)

    def cls_ref(self, name):
        """jclass for a binary class name (cached: classes are identities)."""
        name = (name or '').replace('.', '/')
        r = self.classes.get(name)
        if r is None:
            r = self._new('java/lang/Class', name)
            self.classes[name] = r
        return r

    def obj(self, cls, value=None, **fields):
        cls = cls.replace('.', '/')
        tpl = TEMPLATE.get(cls)
        f = tpl(self) if tpl else {}
        f.update(fields)
        return self._new(cls, value, f)

    def string(self, s):
        return self._new('java/lang/String', '' if s is None else str(s))

    def array(self, elem, items):
        return self._new('[' + elem, list(items))

    def name_of(self, handle):
        r = self.refs.get(handle)
        if r is None:
            return '?'
        return r.value if r.cls == 'java/lang/Class' else r.cls

    def text(self, handle):
        r = self.refs.get(handle)
        if r is None:
            return None
        if r.cls == 'java/lang/String':
            return r.value
        return None if r.value is None else str(r.value)

    # ------------------------------------------------------------ jvalue[]
    def read_args(self, ptr, descs):
        """Decode a jvalue array against the parsed parameter descriptors."""
        out = []
        m = self.h.m
        for i, d in enumerate(descs):
            if not ptr:
                out.append(0)
                continue
            raw = m.read(ptr + i * 8, 8)
            if d == 'Z':
                out.append(1 if raw[0] else 0)
            elif d == 'B':
                out.append(struct.unpack('<b', raw[:1])[0])
            elif d == 'C':
                out.append(struct.unpack('<H', raw[:2])[0])
            elif d == 'S':
                out.append(struct.unpack('<h', raw[:2])[0])
            elif d == 'I':
                out.append(struct.unpack('<i', raw[:4])[0])
            elif d == 'J':
                out.append(struct.unpack('<q', raw)[0])
            elif d == 'F':
                out.append(struct.unpack('<f', raw[:4])[0])
            elif d == 'D':
                out.append(struct.unpack('<d', raw)[0])
            else:
                out.append(struct.unpack('<Q', raw)[0])
        return out

    # ------------------------------------------------------------ defaults
    def default_for(self, ret):
        if ret in ('V',):
            return None
        if ret in ('Z', 'B', 'C', 'S', 'I', 'J'):
            return 0
        if ret in ('F', 'D'):
            return 0.0
        if ret == 'Ljava/lang/String;':
            return ''
        if ret.startswith('['):
            return self.array(ret[1:], [])
        return self.obj(class_of(ret))

    # ------------------------------------------------------------ dispatch
    def invoke(self, mid_handle, this_handle, argptr):
        """Run a Java method.  Returns a python value (see convert())."""
        mid = self.refs.get(mid_handle)
        if mid is None or mid.cls != 'jmethodID':
            self.unknown['<bad methodID>'] += 1
            return None
        cls, name, sig, static = mid.value
        descs, ret = split_descriptor(sig)
        args = self.read_args(argptr, descs)
        this = self.refs.get(this_handle)
        key = '%s.%s%s' % (cls, name, sig)
        self.calls[key] += 1
        if self.log and self.calls[key] == 1:
            print('[jni] %s' % key)
        if len(self.trace) < self.max_trace:
            self.trace.append(key)

        fn = METHODS.get((cls, name)) or METHODS.get(('*', name))
        if fn is None:
            # walk to java/lang/Object for the universal methods
            fn = METHODS.get(('java/lang/Object', name))
        if fn is None:
            self.unknown[key] += 1
            return self.default_for(ret)
        r = fn(self, this, args, sig)
        if r is NotImplemented:
            self.unknown[key] += 1
            return self.default_for(ret)
        return r

    def get_field(self, fid_handle, this_handle):
        fid = self.refs.get(fid_handle)
        if fid is None or fid.cls != 'jfieldID':
            return None
        cls, name, sig, static = fid.value
        self.calls['%s#%s' % (cls, name)] += 1
        this = self.refs.get(this_handle)
        if this is not None and name in this.fields:
            return this.fields[name]
        v = FIELDS.get((cls, name), NotImplemented)
        if v is NotImplemented:
            v = self.statics.get(cls, {}).get(name, NotImplemented)
        if v is NotImplemented:
            self.unknown['%s#%s' % (cls, name)] += 1
            return self.default_for(sig or 'Ljava/lang/Object;')
        return v(self) if callable(v) else v

    def set_field(self, fid_handle, this_handle, value):
        fid = self.refs.get(fid_handle)
        if fid is None:
            return
        cls, name, sig, static = fid.value
        this = self.refs.get(this_handle)
        if this is not None and not static:
            this.fields[name] = value
        else:
            self.statics.setdefault(cls, {})[name] = value

    # ---------------------------------------------------------- conversion
    def as_handle(self, v):
        """Python value -> jobject handle."""
        if v is None:
            return 0
        if isinstance(v, JRef):
            return v.handle
        if isinstance(v, str):
            return self.string(v).handle
        if isinstance(v, bool):
            return self.obj('java/lang/Boolean', 1 if v else 0).handle
        if isinstance(v, int):
            return v if v in self.refs else self.obj('java/lang/Integer', v).handle
        if isinstance(v, float):
            return self.obj('java/lang/Float', v).handle
        if isinstance(v, (list, tuple)):
            return self.array('Ljava/lang/Object;', v).handle
        return 0

    def as_text(self, v):
        if v is None:
            return None
        if isinstance(v, JRef):
            return v.value if isinstance(v.value, str) else None
        if isinstance(v, str):
            return v
        if isinstance(v, int) and v in self.refs:
            return self.text(v)
        return str(v)

    def as_int(self, v):
        if v is None:
            return 0
        if isinstance(v, bool):
            return 1 if v else 0
        if isinstance(v, (int,)):
            return v
        if isinstance(v, float):
            return int(v)
        if isinstance(v, JRef):
            return self.as_int(v.value)
        return 0

    def as_float(self, v):
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return float(v)
        if isinstance(v, JRef):
            return self.as_float(v.value)
        return 0.0

    def report(self, top=30):
        out = ['[jni] %d java calls, %d distinct; %d without a body'
               % (sum(self.calls.values()), len(self.calls),
                  sum(self.unknown.values()))]
        for k, v in self.unknown.most_common(top):
            out.append('   java?  %-64s %d' % (k, v))
        if self.list_calls:
            for k, v in sorted(self.calls.items()):
                out.append('   java   %-64s %d' % (k, v))
        return '\n'.join(out)


def vm(h):
    j = getattr(h, 'jvm', None)
    if j is None:
        j = JVM(h)
        h.jvm = j
    return j


# ==========================================================================
#  The classes the game actually touches
# ==========================================================================

# ------------------------------------------------- java.lang.Object basics
@jmethod('java/lang/Object', 'equals')
def _equals(j, this, a, sig):
    return 1 if (this is not None and a and this.handle == a[0]) else 0


@jmethod('java/lang/Object', 'hashCode')
def _hash(j, this, a, sig):
    return (this.handle & 0x7FFFFFFF) if this else 0


@jmethod('java/lang/Object', 'toString')
def _tostring(j, this, a, sig):
    if this is None:
        return 'null'
    if isinstance(this.value, str):
        return this.value
    return '%s@%x' % (this.cls.replace('/', '.'), this.handle)


@jmethod('java/lang/Object', 'getClass')
def _getclass(j, this, a, sig):
    return j.cls_ref(this.cls if this else 'java/lang/Object')


@jmethod('java/lang/Class', 'getName')
def _classname(j, this, a, sig):
    return (this.value or '').replace('/', '.') if this else ''


@jmethod('java/lang/Class', 'getSimpleName')
def _classsimple(j, this, a, sig):
    n = (this.value or '') if this else ''
    return n.rsplit('/', 1)[-1]


# -------------------------------------------------------- boxed primitives
def _boxer(cls):
    def make(j, this, a, sig):
        return j.obj(cls, a[0] if a else 0)
    return make


for _c in ('java/lang/Boolean', 'java/lang/Byte', 'java/lang/Character',
           'java/lang/Short', 'java/lang/Integer', 'java/lang/Long',
           'java/lang/Float', 'java/lang/Double'):
    METHODS[(_c, 'valueOf')] = _boxer(_c)


@jmethod('java/lang/Integer', 'intValue')
@jmethod('java/lang/Short', 'shortValue')
@jmethod('java/lang/Byte', 'byteValue')
def _intvalue(j, this, a, sig):
    return j.as_int(this.value if this else 0)


@jmethod('java/lang/Long', 'longValue')
def _longvalue(j, this, a, sig):
    return j.as_int(this.value if this else 0)


@jmethod('java/lang/Float', 'floatValue')
@jmethod('java/lang/Double', 'doubleValue')
def _floatvalue(j, this, a, sig):
    return j.as_float(this.value if this else 0)


@jmethod('java/lang/Boolean', 'booleanValue')
def _boolvalue(j, this, a, sig):
    return 1 if (this and this.value) else 0


# -------------------------------------------------------------- java.lang
@jmethod('java/lang/System', 'currentTimeMillis')
def _millis(j, this, a, sig):
    return int(time.time() * 1000)


@jmethod('java/lang/System', 'nanoTime')
def _nanos(j, this, a, sig):
    return int(time.time() * 1e9)


@jmethod('java/lang/System', 'getProperty')
def _getprop(j, this, a, sig):
    key = j.text(a[0]) if a else ''
    return {'os.name': 'Linux', 'os.arch': 'aarch64',
            'java.version': '0', 'line.separator': '\n',
            'file.separator': '/', 'user.dir': '/'}.get(key, '')


@jmethod('java/lang/System', 'gc', 'exit', 'arraycopy', 'setProperty')
def _sysnop(j, this, a, sig):
    return None


@jmethod('java/lang/Runtime', 'getRuntime')
def _runtime(j, this, a, sig):
    return j.obj('java/lang/Runtime')


@jmethod('java/lang/Runtime', 'maxMemory', 'totalMemory')
def _maxmem(j, this, a, sig):
    return 512 * 1024 * 1024


@jmethod('java/lang/Runtime', 'freeMemory')
def _freemem(j, this, a, sig):
    return 256 * 1024 * 1024


@jmethod('java/lang/Runtime', 'availableProcessors')
def _cpus(j, this, a, sig):
    return 4


@jmethod('java/lang/Thread', 'currentThread')
def _curthread(j, this, a, sig):
    if j.thread is None:
        j.thread = j.obj('java/lang/Thread', 'main')
    return j.thread


@jmethod('java/lang/Thread', 'getName')
def _threadname(j, this, a, sig):
    return (this.value if this and isinstance(this.value, str) else 'main')


@jmethod('java/lang/Thread', 'getId')
def _threadid(j, this, a, sig):
    return 1


@jmethod('java/lang/Thread', 'start', 'join', 'interrupt', 'setPriority',
         'setDaemon', 'sleep')
def _threadnop(j, this, a, sig):
    return None


@jmethod('java/lang/Thread', 'isAlive', 'isInterrupted', 'isDaemon')
def _threadfalse(j, this, a, sig):
    return 0


@jmethod('java/lang/String', 'length')
def _strlen(j, this, a, sig):
    return len(this.value or '') if this else 0


@jmethod('java/lang/Throwable', 'getMessage', 'getLocalizedMessage')
def _throwmsg(j, this, a, sig):
    return (this.value if this and isinstance(this.value, str) else '')


# -------------------------------------------------------------- java.util
@jmethod('java/util/Locale', 'getDefault')
def _localedefault(j, this, a, sig):
    lang, country = j.h.locale
    return j.obj('java/util/Locale', '%s_%s' % (lang, country),
                 language=lang, country=country)


@jmethod('java/util/Locale', 'getLanguage')
def _localelang(j, this, a, sig):
    return (this.fields.get('language') if this else None) or j.h.locale[0]


@jmethod('java/util/Locale', 'getCountry')
def _localecountry(j, this, a, sig):
    return (this.fields.get('country') if this else None) or j.h.locale[1]


@jmethod('java/util/Locale', 'getISO3Language')
def _localeiso3(j, this, a, sig):
    return {'en': 'eng', 'ja': 'jpn'}.get(j.h.locale[0], 'eng')


@jmethod('java/util/Locale', 'getDisplayLanguage', 'getDisplayName')
def _localedisplay(j, this, a, sig):
    return {'en': 'English', 'ja': 'Japanese'}.get(j.h.locale[0], 'English')


@jmethod('java/util/TimeZone', 'getDefault')
def _tzdefault(j, this, a, sig):
    return j.obj('java/util/TimeZone', 'UTC')


@jmethod('java/util/TimeZone', 'getID')
def _tzid(j, this, a, sig):
    return 'UTC'


@jmethod('java/util/TimeZone', 'getRawOffset', 'getDSTSavings', 'getOffset')
def _tzoffset(j, this, a, sig):
    return 0


@jmethod('java/util/TimeZone', 'inDaylightTime', 'useDaylightTime')
def _tzdst(j, this, a, sig):
    return 0


# ----------------------------------------------------------- android.os
jfield('android/os/Build', 'MODEL', 'R36S')
jfield('android/os/Build', 'MANUFACTURER', 'Kairosoft')
jfield('android/os/Build', 'DEVICE', 'kairovm')
jfield('android/os/Build', 'BRAND', 'kairovm')
jfield('android/os/Build', 'PRODUCT', 'gamedev3')
jfield('android/os/Build', 'HARDWARE', 'aarch64')
jfield('android/os/Build$VERSION', 'SDK_INT', 30)
jfield('android/os/Build$VERSION', 'RELEASE', '11')


@jmethod('android/os/Process', 'myPid', 'myUid', 'myTid')
def _pid(j, this, a, sig):
    return 1000


# ------------------------------------------------- android.util.DisplayMetrics
@jtemplate('android/util/DisplayMetrics')
def _dm(j):
    h = j.h
    return {'widthPixels': h.width, 'heightPixels': h.height,
            'density': h.density, 'densityDpi': int(160 * h.density),
            'scaledDensity': h.density, 'xdpi': 160.0 * h.density,
            'ydpi': 160.0 * h.density}


@jmethod('android/view/Display', 'getMetrics', 'getRealMetrics')
def _getmetrics(j, this, a, sig):
    target = j.refs.get(a[0]) if a else None
    if target is not None:
        target.fields.update(_dm(j))
    return None


@jmethod('android/view/Display', 'getWidth')
def _dispw(j, this, a, sig):
    return j.h.width


@jmethod('android/view/Display', 'getHeight')
def _disph(j, this, a, sig):
    return j.h.height


@jmethod('android/view/Display', 'getRotation')
def _disprot(j, this, a, sig):
    return 0


@jmethod('android/view/Display', 'getSize', 'getRealSize')
def _dispsize(j, this, a, sig):
    p = j.refs.get(a[0]) if a else None
    if p is not None:
        p.fields['x'] = j.h.width
        p.fields['y'] = j.h.height
    return None


@jtemplate('android/graphics/Point')
def _point(j):
    return {'x': 0, 'y': 0}


@jmethod('android/view/WindowManager', 'getDefaultDisplay')
def _defaultdisplay(j, this, a, sig):
    return j.obj('android/view/Display')


# ------------------------------------------- com.unity3d.player.UnityPlayer
def _activity(j):
    act = j.statics.setdefault('com/unity3d/player/UnityPlayer', {}).get('currentActivity')
    if act is None:
        act = j.obj('android/app/Activity')
        j.statics['com/unity3d/player/UnityPlayer']['currentActivity'] = act
    return act


jfield('com/unity3d/player/UnityPlayer', 'currentActivity', _activity)
jfield('com/unity3d/player/UnityPlayer', 'currentContext', _activity)


@jmethod('android/app/Activity', 'getWindowManager')
def _getwm(j, this, a, sig):
    return j.obj('android/view/WindowManager')


@jmethod('android/app/Activity', 'getApplicationContext', 'getBaseContext')
def _getctx(j, this, a, sig):
    return _activity(j)


@jmethod('android/app/Activity', 'getPackageName')
def _getpkg(j, this, a, sig):
    return j.h.s.pkg


@jmethod('android/app/Activity', 'runOnUiThread')
def _runonui(j, this, a, sig):
    # the runnable was created by managed code; the caller falls back to
    # running it inline when the UI thread check succeeds, so nothing to do.
    return None


@jmethod('android/app/Activity', 'getFilesDir', 'getCacheDir',
         'getExternalFilesDir')
def _filesdir(j, this, a, sig):
    return j.obj('java/io/File', '/data/data/%s/files' % j.h.s.pkg)


@jmethod('java/io/File', 'getAbsolutePath', 'getPath', 'toString')
def _filepath(j, this, a, sig):
    return (this.value if this and isinstance(this.value, str) else '/')


@jmethod('java/io/File', 'exists', 'mkdirs', 'isDirectory', 'canWrite')
def _fileflag(j, this, a, sig):
    return 1


@jmethod('android/app/Activity', 'getWindow', 'getResources',
         'getContentResolver', 'getSystemService', 'getPackageManager')
def _actservice(j, this, a, sig):
    return j.obj('java/lang/Object')


# ---------------------------------------------------- Unity ReflectionHelper
REFLECTION_HELPER = 'com/unity3d/player/ReflectionHelper'


@jmethod(REFLECTION_HELPER, 'getMethodID')
def _refl_method(j, this, a, sig):
    cls = j.name_of(a[0])
    name = j.text(a[1]) or ''
    desc = j.text(a[2]) or ''
    static = bool(a[3]) if len(a) > 3 else False
    mid = j._new('jmethodID', (cls, name, desc, static))
    return j.obj('java/lang/reflect/Method', mid)


@jmethod(REFLECTION_HELPER, 'getConstructorID')
def _refl_ctor(j, this, a, sig):
    cls = j.name_of(a[0])
    desc = j.text(a[1]) or '()V'
    mid = j._new('jmethodID', (cls, '<init>', desc, False))
    return j.obj('java/lang/reflect/Constructor', mid)


@jmethod(REFLECTION_HELPER, 'getFieldID')
def _refl_field(j, this, a, sig):
    cls = j.name_of(a[0])
    name = j.text(a[1]) or ''
    desc = j.text(a[2]) or ''
    static = bool(a[3]) if len(a) > 3 else False
    fid = j._new('jfieldID', (cls, name, desc, static))
    return j.obj('java/lang/reflect/Field', fid)


@jmethod(REFLECTION_HELPER, 'getFieldSignature')
def _refl_fieldsig(j, this, a, sig):
    f = j.refs.get(a[0]) if a else None
    fid = f.value if f is not None else None
    if isinstance(fid, JRef) and fid.value:
        cls, name, desc, static = fid.value
        if desc:
            return desc
        v = FIELDS.get((cls, name))
        if isinstance(v, str):
            return 'Ljava/lang/String;'
        if isinstance(v, int):
            return 'I'
    return 'Ljava/lang/Object;'


@jmethod(REFLECTION_HELPER, 'newProxyInstance')
def _refl_proxy(j, this, a, sig):
    return j.obj('java/lang/Object')


@jmethod(REFLECTION_HELPER, 'setNativeExceptionOnProxy')
def _refl_setexc(j, this, a, sig):
    return None


@jmethod('java/lang/reflect/Field', 'getDeclaringClass')
@jmethod('java/lang/reflect/Method', 'getDeclaringClass')
def _refl_declaring(j, this, a, sig):
    fid = this.value if this is not None else None
    if isinstance(fid, JRef) and fid.value:
        return j.cls_ref(fid.value[0])
    return j.cls_ref('java/lang/Object')


# =========================================================================
#  The internal calls themselves
# =========================================================================
def _mid(j, clazz, name, desc, static):
    return j._new('jmethodID', (j.name_of(clazz), name or '', desc or '',
                                static)).handle


def _fid(j, clazz, name, desc, static):
    return j._new('jfieldID', (j.name_of(clazz), name or '', desc or '',
                               static)).handle


@icall('UnityEngine.AndroidJNI::FindClass(System.String)')
def _findclass(h, this, a):
    j = vm(h)
    return j.cls_ref(h.read_string(a[0]) or '').handle


@icall('UnityEngine.AndroidJNI::GetObjectClass(System.IntPtr)')
def _objclass(h, this, a):
    j = vm(h)
    r = j.ref(a[0])
    return j.cls_ref(r.cls if r else 'java/lang/Object').handle


@icall('UnityEngine.AndroidJNI::GetSuperclass(System.IntPtr)')
def _superclass(h, this, a):
    return vm(h).cls_ref('java/lang/Object').handle


@icall('UnityEngine.AndroidJNI::GetMethodID(System.IntPtr,System.String,System.String)')
def _getmethodid(h, this, a):
    j = vm(h)
    return _mid(j, a[0], h.read_string(a[1]), h.read_string(a[2]), False)


@icall('UnityEngine.AndroidJNI::GetStaticMethodID(System.IntPtr,System.String,System.String)')
def _getstaticmethodid(h, this, a):
    j = vm(h)
    return _mid(j, a[0], h.read_string(a[1]), h.read_string(a[2]), True)


@icall('UnityEngine.AndroidJNI::GetFieldID(System.IntPtr,System.String,System.String)')
def _getfieldid(h, this, a):
    j = vm(h)
    return _fid(j, a[0], h.read_string(a[1]), h.read_string(a[2]), False)


@icall('UnityEngine.AndroidJNI::GetStaticFieldID(System.IntPtr,System.String,System.String)')
def _getstaticfieldid(h, this, a):
    j = vm(h)
    return _fid(j, a[0], h.read_string(a[1]), h.read_string(a[2]), True)


@icall('UnityEngine.AndroidJNI::FromReflectedMethod(System.IntPtr)',
       'UnityEngine.AndroidJNI::FromReflectedField(System.IntPtr)')
def _fromreflected(h, this, a):
    j = vm(h)
    r = j.ref(a[0])
    inner = r.value if r is not None else None
    if isinstance(inner, JRef):
        return inner.handle
    return j._new('jmethodID', ('java/lang/Object', '?', '()V', False)).handle


@icall('UnityEngine.AndroidJNI::ToReflectedMethod(System.IntPtr,System.IntPtr,System.Boolean)')
def _toreflectedmethod(h, this, a):
    j = vm(h)
    return j.obj('java/lang/reflect/Method', j.ref(a[1])).handle


@icall('UnityEngine.AndroidJNI::ToReflectedField(System.IntPtr,System.IntPtr,System.Boolean)')
def _toreflectedfield(h, this, a):
    j = vm(h)
    return j.obj('java/lang/reflect/Field', j.ref(a[1])).handle


# ------------------------------------------------------------------ strings
@icall('UnityEngine.AndroidJNI::NewStringFromStr(System.String)',
       'UnityEngine.AndroidJNI::NewStringUTF(System.String)')
def _newstring(h, this, a):
    return vm(h).string(h.read_string(a[0]) or '').handle


@icall('UnityEngine.AndroidJNI::NewString(System.Char[])')
def _newstringchars(h, this, a):
    j = vm(h)
    p = a[0]
    if not p:
        return j.string('').handle
    n = h.array_len(p)
    data = h.m.read(h.array_data(p), n * 2)
    return j.string(data.decode('utf-16-le', 'replace')).handle


@icall('UnityEngine.AndroidJNI::GetStringChars(System.IntPtr)',
       'UnityEngine.AndroidJNI::GetStringUTFChars(System.IntPtr)')
def _getstringchars(h, this, a):
    return h.new_string(vm(h).text(a[0]) or '')


@icall('UnityEngine.AndroidJNI::GetStringLength(System.IntPtr)')
def _getstringlength(h, this, a):
    return len(vm(h).text(a[0]) or '')


@icall('UnityEngine.AndroidJNI::GetStringUTFLength(System.IntPtr)')
def _getstringutflength(h, this, a):
    return len((vm(h).text(a[0]) or '').encode('utf-8'))


# ------------------------------------------------------------ construction
@icall('UnityEngine.AndroidJNI::NewObjectA(System.IntPtr,System.IntPtr,UnityEngine.jvalue*)')
def _newobject(h, this, a):
    j = vm(h)
    cls = j.name_of(a[0])
    mid = j.ref(a[1])
    o = j.obj(cls)
    if mid is not None and mid.cls == 'jmethodID':
        descs, _ = split_descriptor(mid.value[2])
        args = j.read_args(a[2], descs)
        fn = METHODS.get((cls, '<init>'))
        if fn is not None:
            fn(j, o, args, mid.value[2])
        elif args:
            o.value = args[0]
        j.calls['%s.<init>%s' % (cls, mid.value[2])] += 1
    return o.handle


@icall('UnityEngine.AndroidJNI::AllocObject(System.IntPtr)')
def _allocobject(h, this, a):
    j = vm(h)
    return j.obj(j.name_of(a[0])).handle


# ------------------------------------------------------------------- calls
def _call(h, a, want):
    """Shared body for every Call*Method / CallStatic*Method variant."""
    j = vm(h)
    v = j.invoke(a[1], a[0], a[2])
    if want == 'v':
        return None
    if want == 'i':
        return j.as_int(v) & 0xFFFFFFFFFFFFFFFF
    if want == 'z':
        return 1 if j.as_int(v) else 0
    if want == 'f':
        return float(j.as_float(v))
    if want == 'd':
        from .unity import f64
        return f64(j.as_float(v))
    if want == 's':
        t = j.as_text(v)
        return h.new_string(t) if t is not None else 0
    return j.as_handle(v)


for _pfx in ('', 'Static'):
    for _nm, _want in (('Void', 'v'), ('Int', 'i'), ('Long', 'i'),
                       ('Short', 'i'), ('SByte', 'i'), ('Char', 'i'),
                       ('Boolean', 'z'), ('Float', 'f'), ('Double', 'd'),
                       ('String', 's'), ('Object', 'o')):
        _sig = ('UnityEngine.AndroidJNI::Call%s%sMethodUnsafe'
                '(System.IntPtr,System.IntPtr,UnityEngine.jvalue*)'
                % (_pfx, _nm))

        def _mk(want=_want):
            def fn(h, this, a):
                return _call(h, a, want)
            return fn
        IMPL_FN = _mk()
        IMPL_FN.__name__ = 'jni_call_%s%s' % (_pfx.lower(), _nm.lower())
        icall(_sig)(IMPL_FN)


# ------------------------------------------------------------------ fields
def _field(h, a, want, static):
    j = vm(h)
    v = j.get_field(a[1], 0 if static else a[0])
    if want == 'i':
        return j.as_int(v) & 0xFFFFFFFFFFFFFFFF
    if want == 'z':
        return 1 if j.as_int(v) else 0
    if want == 'f':
        return float(j.as_float(v))
    if want == 'd':
        from .unity import f64
        return f64(j.as_float(v))
    if want == 's':
        t = j.as_text(v)
        return h.new_string(t) if t is not None else 0
    return j.as_handle(v)


for _pfx, _static in (('', False), ('Static', True)):
    for _nm, _want in (('Int', 'i'), ('Long', 'i'), ('Short', 'i'),
                       ('SByte', 'i'), ('Char', 'i'), ('Boolean', 'z'),
                       ('Float', 'f'), ('Double', 'd'), ('String', 's'),
                       ('Object', 'o')):
        _sig = ('UnityEngine.AndroidJNI::Get%s%sField'
                '(System.IntPtr,System.IntPtr)' % (_pfx, _nm))

        def _mkg(want=_want, static=_static):
            def fn(h, this, a):
                return _field(h, a, want, static)
            return fn
        _g = _mkg()
        _g.__name__ = 'jni_get_%s%s' % (_pfx.lower(), _nm.lower())
        icall(_sig)(_g)


def _setfield(h, a, static, conv):
    j = vm(h)
    j.set_field(a[1], 0 if static else a[0], conv(h, j, a[2]))
    return None


_SETCONV = {
    'Int': lambda h, j, v: v, 'Long': lambda h, j, v: v,
    'Short': lambda h, j, v: v, 'SByte': lambda h, j, v: v,
    'Char': lambda h, j, v: v, 'Boolean': lambda h, j, v: bool(v),
    'Float': lambda h, j, v: v, 'Double': lambda h, j, v: v,
    'String': lambda h, j, v: h.read_string(v),
    'Object': lambda h, j, v: j.ref(v) or v,
}
_SETTYPE = {'Int': 'System.Int32', 'Long': 'System.Int64',
            'Short': 'System.Int16', 'SByte': 'System.SByte',
            'Char': 'System.Char', 'Boolean': 'System.Boolean',
            'Float': 'System.Single', 'Double': 'System.Double',
            'String': 'System.String', 'Object': 'System.IntPtr'}

for _pfx, _static in (('', False), ('Static', True)):
    for _nm, _conv in _SETCONV.items():
        _sig = ('UnityEngine.AndroidJNI::Set%s%sField'
                '(System.IntPtr,System.IntPtr,%s)'
                % (_pfx, _nm, _SETTYPE[_nm]))

        def _mks(static=_static, conv=_conv):
            def fn(h, this, a):
                return _setfield(h, a, static, conv)
            return fn
        _s = _mks()
        _s.__name__ = 'jni_set_%s%s' % (_pfx.lower(), _nm.lower())
        icall(_sig)(_s)


# ------------------------------------------------------------------ arrays
@icall('UnityEngine.AndroidJNI::GetArrayLength(System.IntPtr)')
def _arraylen(h, this, a):
    r = vm(h).ref(a[0])
    return len(r.value) if r is not None and isinstance(r.value, list) else 0


@icall('UnityEngine.AndroidJNI::NewObjectArray(System.Int32,System.IntPtr,System.IntPtr)')
def _newobjarray(h, this, a):
    j = vm(h)
    return j.array('Ljava/lang/Object;', [a[2]] * max(0, a[0])).handle


@icall('UnityEngine.AndroidJNI::GetObjectArrayElement(System.IntPtr,System.Int32)')
def _getobjelem(h, this, a):
    r = vm(h).ref(a[0])
    if r is None or not isinstance(r.value, list) or not (0 <= a[1] < len(r.value)):
        return 0
    return vm(h).as_handle(r.value[a[1]])


@icall('UnityEngine.AndroidJNI::SetObjectArrayElement(System.IntPtr,System.Int32,System.IntPtr)')
def _setobjelem(h, this, a):
    r = vm(h).ref(a[0])
    if r is not None and isinstance(r.value, list) and 0 <= a[1] < len(r.value):
        r.value[a[1]] = a[2]
    return None


@icall('UnityEngine.AndroidJNI::ToObjectArray(System.IntPtr*,System.Int32,System.IntPtr)')
def _toobjarray(h, this, a):
    j = vm(h)
    n = a[1]
    items = [h.m.read64(a[0] + i * 8) for i in range(n)] if a[0] else []
    return j.array('Ljava/lang/Object;', items).handle


@icall('UnityEngine.AndroidJNI::FromObjectArray(System.IntPtr)')
def _fromobjarray(h, this, a):
    j = vm(h)
    r = j.ref(a[0])
    items = r.value if r is not None and isinstance(r.value, list) else []
    arr = h.new_array(h.arr_intptr, len(items), 8,
                      b''.join(struct.pack('<Q', j.as_handle(x)) for x in items))
    return arr


# --------------------------------------------------------- primitive arrays
#  Java arrays live in the handle table as python lists (bytes for byte[]);
#  these are the four shapes Unity's AndroidJNI exposes for them.
PRIM = {'Z': ('Z', 1, '<B'), 'B': ('B', 1, '<B'), 'SB': ('SB', 1, '<b'),
        'C': ('C', 2, '<H'), 'S': ('S', 2, '<h'), 'I': ('I', 4, '<i'),
        'J': ('J', 8, '<q'), 'F': ('F', 4, '<f'), 'D': ('D', 8, '<d')}


def _jarray_items(j, handle):
    r = j.ref(handle)
    if r is None:
        return None
    v = r.value
    if isinstance(v, (bytes, bytearray)):
        return list(v)
    return list(v) if isinstance(v, list) else None


@icall('UnityEngine.AndroidJNI::ToByteArray(System.Byte[])')
def _tobytearray(h, this, a):
    return vm(h).array('B', h.read_managed_array(a[0], 1)).handle


@icall('UnityEngine.AndroidJNI::ConvertToBooleanArray(System.Boolean[])')
def _toboolarray(h, this, a):
    return vm(h).array('Z', h.read_managed_array(a[0], 1, '<B')).handle


def _ptr_array(h, kind, ptr, count):
    _, size, fmt = PRIM[kind]
    raw = h.m.read(ptr, count * size) if (ptr and count > 0) else b''
    items = [struct.unpack_from(fmt, raw, i * size)[0] for i in range(count)]
    return vm(h).array(kind, items).handle


@icall('UnityEngine.AndroidJNI::ToSByteArray(System.SByte*,System.Int32)')
def _tosbytearray(h, this, a):
    return _ptr_array(h, 'SB', a[0], a[1])


@icall('UnityEngine.AndroidJNI::ToCharArray(System.Char*,System.Int32)')
def _tochararray(h, this, a):
    return _ptr_array(h, 'C', a[0], a[1])


@icall('UnityEngine.AndroidJNI::ToShortArray(System.Int16*,System.Int32)')
def _toshortarray(h, this, a):
    return _ptr_array(h, 'S', a[0], a[1])


@icall('UnityEngine.AndroidJNI::ToIntArray(System.Int32*,System.Int32)')
def _tointarray(h, this, a):
    return _ptr_array(h, 'I', a[0], a[1])


@icall('UnityEngine.AndroidJNI::ToLongArray(System.Int64*,System.Int32)')
def _tolongarray(h, this, a):
    return _ptr_array(h, 'J', a[0], a[1])


@icall('UnityEngine.AndroidJNI::ToFloatArray(System.Single*,System.Int32)')
def _tofloatarray(h, this, a):
    return _ptr_array(h, 'F', a[0], a[1])


@icall('UnityEngine.AndroidJNI::ToDoubleArray(System.Double*,System.Int32)')
def _todoublearray(h, this, a):
    return _ptr_array(h, 'D', a[0], a[1])


def _new_jarray(h, kind, n):
    n = max(0, n)
    zero = 0.0 if kind in ('F', 'D') else 0
    return vm(h).array(kind, [zero] * n).handle


@icall('UnityEngine.AndroidJNI::NewBooleanArray(System.Int32)')
def _newboolarray(h, this, a):
    return _new_jarray(h, 'Z', a[0])


@icall('UnityEngine.AndroidJNI::NewSByteArray(System.Int32)')
def _newsbytearray(h, this, a):
    return _new_jarray(h, 'SB', a[0])


@icall('UnityEngine.AndroidJNI::NewCharArray(System.Int32)')
def _newchararray(h, this, a):
    return _new_jarray(h, 'C', a[0])


@icall('UnityEngine.AndroidJNI::NewShortArray(System.Int32)')
def _newshortarray(h, this, a):
    return _new_jarray(h, 'S', a[0])


@icall('UnityEngine.AndroidJNI::NewIntArray(System.Int32)')
def _newintarray(h, this, a):
    return _new_jarray(h, 'I', a[0])


@icall('UnityEngine.AndroidJNI::NewLongArray(System.Int32)')
def _newlongarray(h, this, a):
    return _new_jarray(h, 'J', a[0])


@icall('UnityEngine.AndroidJNI::NewFloatArray(System.Int32)')
def _newfloatarray(h, this, a):
    return _new_jarray(h, 'F', a[0])


@icall('UnityEngine.AndroidJNI::NewDoubleArray(System.Int32)')
def _newdoublearray(h, this, a):
    return _new_jarray(h, 'D', a[0])


def _from_jarray(h, kind, handle):
    items = _jarray_items(vm(h), handle)
    if items is None:
        return 0
    return h.managed_prim_array(kind, items)


@icall('UnityEngine.AndroidJNI::FromByteArray(System.IntPtr)')
def _frombytearray(h, this, a):
    return _from_jarray(h, 'B', a[0])


@icall('UnityEngine.AndroidJNI::FromSByteArray(System.IntPtr)')
def _fromsbytearray(h, this, a):
    return _from_jarray(h, 'SB', a[0])


@icall('UnityEngine.AndroidJNI::FromBooleanArray(System.IntPtr)')
def _fromboolarray(h, this, a):
    return _from_jarray(h, 'Z', a[0])


@icall('UnityEngine.AndroidJNI::FromCharArray(System.IntPtr)')
def _fromchararray(h, this, a):
    return _from_jarray(h, 'C', a[0])


@icall('UnityEngine.AndroidJNI::FromShortArray(System.IntPtr)')
def _fromshortarray(h, this, a):
    return _from_jarray(h, 'S', a[0])


@icall('UnityEngine.AndroidJNI::FromIntArray(System.IntPtr)')
def _fromintarray(h, this, a):
    return _from_jarray(h, 'I', a[0])


@icall('UnityEngine.AndroidJNI::FromLongArray(System.IntPtr)')
def _fromlongarray(h, this, a):
    return _from_jarray(h, 'J', a[0])


@icall('UnityEngine.AndroidJNI::FromFloatArray(System.IntPtr)')
def _fromfloatarray(h, this, a):
    return _from_jarray(h, 'F', a[0])


@icall('UnityEngine.AndroidJNI::FromDoubleArray(System.IntPtr)')
def _fromdoublearray(h, this, a):
    return _from_jarray(h, 'D', a[0])


def _elem_get(h, handle, index):
    items = _jarray_items(vm(h), handle)
    if items is None or not (0 <= index < len(items)):
        return None
    return items[index]


@icall('UnityEngine.AndroidJNI::GetBooleanArrayElement(System.IntPtr,System.Int32)',
       'UnityEngine.AndroidJNI::GetSByteArrayElement(System.IntPtr,System.Int32)',
       'UnityEngine.AndroidJNI::GetCharArrayElement(System.IntPtr,System.Int32)',
       'UnityEngine.AndroidJNI::GetShortArrayElement(System.IntPtr,System.Int32)',
       'UnityEngine.AndroidJNI::GetIntArrayElement(System.IntPtr,System.Int32)',
       'UnityEngine.AndroidJNI::GetLongArrayElement(System.IntPtr,System.Int32)')
def _getintelem(h, this, a):
    v = _elem_get(h, a[0], a[1])
    return int(v) if v is not None else 0


@icall('UnityEngine.AndroidJNI::GetFloatArrayElement(System.IntPtr,System.Int32)')
def _getfloatelem(h, this, a):
    v = _elem_get(h, a[0], a[1])
    return U.f32(float(v) if v is not None else 0.0)


@icall('UnityEngine.AndroidJNI::GetDoubleArrayElement(System.IntPtr,System.Int32)')
def _getdoubleelem(h, this, a):
    v = _elem_get(h, a[0], a[1])
    return U.f64(float(v) if v is not None else 0.0)


@icall('UnityEngine.AndroidJNI::SetBooleanArrayElement(System.IntPtr,System.Int32,System.Boolean)',
       'UnityEngine.AndroidJNI::SetSByteArrayElement(System.IntPtr,System.Int32,System.SByte)',
       'UnityEngine.AndroidJNI::SetCharArrayElement(System.IntPtr,System.Int32,System.Char)',
       'UnityEngine.AndroidJNI::SetShortArrayElement(System.IntPtr,System.Int32,System.Int16)',
       'UnityEngine.AndroidJNI::SetIntArrayElement(System.IntPtr,System.Int32,System.Int32)',
       'UnityEngine.AndroidJNI::SetLongArrayElement(System.IntPtr,System.Int32,System.Int64)',
       'UnityEngine.AndroidJNI::SetFloatArrayElement(System.IntPtr,System.Int32,System.Single)',
       'UnityEngine.AndroidJNI::SetDoubleArrayElement(System.IntPtr,System.Int32,System.Double)')
def _setelem(h, this, a):
    r = vm(h).ref(a[0])
    if r is None:
        return None
    if isinstance(r.value, (bytes, bytearray)):
        r.value = list(r.value)
    if isinstance(r.value, list) and 0 <= a[1] < len(r.value):
        r.value[a[1]] = a[2]
    return None


# ------------------------------------------------------------------- refs
@icall('UnityEngine.AndroidJNI::NewGlobalRef(System.IntPtr)',
       'UnityEngine.AndroidJNI::NewLocalRef(System.IntPtr)',
       'UnityEngine.AndroidJNI::NewWeakGlobalRef(System.IntPtr)')
def _newref(h, this, a):
    return a[0]


@icall('UnityEngine.AndroidJNI::DeleteGlobalRef(System.IntPtr)',
       'UnityEngine.AndroidJNI::DeleteLocalRef(System.IntPtr)',
       'UnityEngine.AndroidJNI::DeleteWeakGlobalRef(System.IntPtr)',
       'UnityEngine.AndroidJNI::QueueDeleteGlobalRef(System.IntPtr)',
       'UnityEngine.AndroidJNI::ExceptionClear()',
       'UnityEngine.AndroidJNI::ExceptionDescribe()',
       'UnityEngine.AndroidJNI::AttachCurrentThread()',
       'UnityEngine.AndroidJNI::DetachCurrentThread()',
       'UnityEngine.AndroidJNI::UnregisterNatives(System.IntPtr)',
       'UnityEngine.AndroidJNIHelper::set_debug(System.Boolean)')
def _jninop(h, this, a):
    return 0


@icall('UnityEngine.AndroidJNI::PushLocalFrame(System.Int32)',
       'UnityEngine.AndroidJNI::PopLocalFrame(System.IntPtr)',
       'UnityEngine.AndroidJNI::EnsureLocalCapacity(System.Int32)',
       'UnityEngine.AndroidJNI::ExceptionOccurred()',
       'UnityEngine.AndroidJNI::GetQueueGlobalRefsCount()',
       'UnityEngine.AndroidJNIHelper::get_debug()')
def _jnizero(h, this, a):
    return 0


@icall('UnityEngine.AndroidJNI::GetVersion()')
def _jniversion(h, this, a):
    return 0x00010006


@icall('UnityEngine.AndroidJNI::GetJavaVM()')
def _javavm(h, this, a):
    return vm(h).obj('java/lang/JavaVM').handle


@icall('UnityEngine.AndroidJNI::IsSameObject(System.IntPtr,System.IntPtr)')
def _issame(h, this, a):
    return 1 if a[0] == a[1] else 0


@icall('UnityEngine.AndroidJNI::IsInstanceOf(System.IntPtr,System.IntPtr)',
       'UnityEngine.AndroidJNI::IsAssignableFrom(System.IntPtr,System.IntPtr)')
def _isinstance(h, this, a):
    j = vm(h)
    want = j.name_of(a[1])
    if want in ('java/lang/Object', '?'):
        return 1
    return 1 if j.name_of(a[0]) == want else 0


@icall('UnityEngine.AndroidJNI::Throw(System.IntPtr)',
       'UnityEngine.AndroidJNI::ThrowNew(System.IntPtr,System.String)',
       'UnityEngine.AndroidJNI::FatalError(System.String)')
def _jnithrow(h, this, a):
    return 0


# ======================================================================
#  kairo.android.plugin.* - Kairosoft's own Java glue (classes.dex).
#  Everything it does is host-side: window size, notifications, dialogs.
# ======================================================================
@jmethod('kairo/android/plugin/Config', 'init')
@jmethod('plugin/Config', 'init')
def _kconfig_init(j, this, a, sig):
    return None


@jmethod('kairo/android/plugin/Utility', 'getAppWidth')
def _kutil_w(j, this, a, sig):
    return j.h.width


@jmethod('kairo/android/plugin/Utility', 'getAppHeight')
def _kutil_h(j, this, a, sig):
    return j.h.height


@jmethod('kairo/android/plugin/Utility', 'getScaleRatio')
def _kutil_scale(j, this, a, sig):
    """(srcW, srcH, dstW, dstH) -> uniform fit ratio, as the Java code does."""
    if len(a) >= 4 and a[0] and a[1]:
        return min(float(a[2]) / float(a[0]), float(a[3]) / float(a[1]))
    return 1.0


@jmethod('kairo/android/plugin/Utility', 'getPackageName')
def _kutil_pkg(j, this, a, sig):
    return j.h.s.pkg


@jmethod('kairo/android/plugin/Utility', 'getVersionName')
def _kutil_ver(j, this, a, sig):
    return '2.6.9'


@jmethod('kairo/android/plugin/Utility', 'getVersionCode')
def _kutil_vercode(j, this, a, sig):
    return 269


@jmethod('kairo/android/plugin/Utility', 'showDialog')
def _kutil_dialog(j, this, a, sig):
    """A modal Java dialog.  Record it; answer 'not shown / cancelled'."""
    text = [j.text(x) for x in a if isinstance(x, int) and x in j.refs]
    j.h.dialogs.append([x for x in text if x])
    if j.h.verbose:
        print('[jni] showDialog %r' % (j.h.dialogs[-1],))
        for line in j.h.m.recent_methods(400):
            print('    at %s' % line)
    return 0


@jmethod('kairo/android/plugin/Utility', 'setParam', 'fullscreen',
         'cancelNotifications',
         'setNotification', 'vibrate', 'openUrl', 'share',
         'setKeepScreenOn', 'showInterstitial', 'hideAd', 'showAd',
         'setImmersiveMode', 'requestPermission')
def _kutil_nop(j, this, a, sig):
    return None


@jmethod('kairo/android/plugin/Utility', 'getParam')
def _kutil_getparam(j, this, a, sig):
    return ''


# --------------------------------------------------------------------------
#  SharedPreferences.  This is where the game's record store really lives:
#  kairo.unity.io.RecordStore keeps every save slot as a preference entry, so
#  these four calls are the whole persistence layer of the port.
# --------------------------------------------------------------------------
# kairo.android.plugin.util.StringUtil.ESCAPES, verified against the string
# literal the shipped C# port of the same helper indexes into.
ESCAPES = ',&@\\'


def _escape(s):
    """kairo.android.plugin.util.StringUtil.escape(), 1:1 with the dex."""
    if s is None:
        return ''
    out = []
    for ch in s:
        if ch in ESCAPES:
            out.append('\\')
        out.append(ch)
    return '"' + ''.join(out) + '"'


def _pref_bytes(j, handle):
    """A jbyteArray handle -> python bytes (None for a null reference)."""
    if not handle:
        return None
    r = j.ref(handle)
    if r is None:
        return None
    v = r.value
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    if isinstance(v, list):
        return bytes(x & 0xFF for x in v)
    if isinstance(v, str):
        return v.encode('utf-8')
    return None


@jmethod('kairo/android/plugin/Utility', 'putPreference', 'setPreference')
def _pref_put(j, this, a, sig):
    """Utility.putPreference(String,byte[]) -> Preference.put(...)."""
    key = j.text(a[0]) if a else None
    if key is None:
        return None
    data = _pref_bytes(j, a[1]) if len(a) > 1 else b''
    j.h.sharedprefs[key] = b'' if data is None else data
    j.h.save_shared()
    return None


@jmethod('kairo/android/plugin/Utility', 'getPreference')
def _pref_get(j, this, a, sig):
    """Utility.getPreference(String) -> byte[]; null when absent."""
    key = j.text(a[0]) if a else None
    v = j.h.sharedprefs.get(key)
    if v is None:
        return None
    return j.array('B', bytes(v))


@jmethod('kairo/android/plugin/Utility', 'existPreference')
def _pref_exist(j, this, a, sig):
    key = j.text(a[0]) if a else None
    return 1 if key in j.h.sharedprefs else 0


@jmethod('kairo/android/plugin/Utility', 'removePreference')
def _pref_remove(j, this, a, sig):
    key = j.text(a[0]) if a else None
    if key in j.h.sharedprefs:
        del j.h.sharedprefs[key]
        j.h.save_shared()
    return None


@jmethod('kairo/android/plugin/Utility', 'getPreferenceKeys')
def _pref_keys(j, this, a, sig):
    """Preference.getAllKeys() joined by StringUtil.getString(String[]).

    Note the shipped C# reader (kairo.unity.native.util.StringUtil.Unescape)
    maps the empty string to null and the caller then feeds that null to
    Enumerable.Contains, so a genuinely empty store throws inside the engine.
    On a device it never is empty: Utility.setNotificationBackground() and
    friends below write their `_plugin_*` keys during start-up, which is
    exactly why they must be real here instead of no-ops.
    """
    return ','.join(_escape(k) for k in sorted(j.h.sharedprefs))


# ---- the `_plugin_*` preferences kairo.android.plugin.Utility keeps ------
#      (keys, encodings and defaults read straight out of classes.dex)
def _pref_write(j, key, text):
    j.h.sharedprefs[key] = str(text).encode('utf-8')
    j.h.save_shared()


def _pref_read_text(j, key):
    v = j.h.sharedprefs.get(key)
    return None if v is None else bytes(v).decode('utf-8', 'replace')


def _pref_read_int(j, key, default):
    s = _pref_read_text(j, key)
    if s is None:
        return default
    try:
        return int(s.strip())
    except ValueError:                       # Java: NumberFormatException
        return default


@jmethod('kairo/android/plugin/Utility', 'setDebug')
def _kutil_setdebug(j, this, a, sig):
    _pref_write(j, '_plugin_debug', 1 if a and a[0] else 0)


@jmethod('kairo/android/plugin/Utility', 'isDebug')
def _kutil_isdebug(j, this, a, sig):
    return 1 if _pref_read_int(j, '_plugin_debug', 0) == 1 else 0


@jmethod('kairo/android/plugin/Utility', 'setLanguage')
def _kutil_setlang(j, this, a, sig):
    _pref_write(j, '_plugin_language', j.as_int(a[0]) if a else 0)


@jmethod('kairo/android/plugin/Utility', 'getLanguage')
def _kutil_getlang(j, this, a, sig):
    return _pref_read_int(j, '_plugin_language', 0)


@jmethod('kairo/android/plugin/Utility', 'setNotificationFilter')
def _kutil_setnotiflevel(j, this, a, sig):
    _pref_write(j, '_plugin_notification_level', j.as_int(a[0]) if a else 0)


@jmethod('kairo/android/plugin/Utility', 'getNotificationFilter')
def _kutil_notiffilter(j, this, a, sig):
    return _pref_read_int(j, '_plugin_notification_level', 3)


@jmethod('kairo/android/plugin/Utility', 'setNotificationBackground')
def _kutil_setnotifbg(j, this, a, sig):
    _pref_write(j, '_plugin_notification_background', 1 if a and a[0] else 0)


@jmethod('kairo/android/plugin/Utility', 'getNotificationBackground')
def _kutil_notifbg(j, this, a, sig):
    return 1 if _pref_read_int(j, '_plugin_notification_background', 0) == 1 else 0


@jmethod('kairo/android/plugin/Utility', 'setGCMRegistrationId')
def _kutil_setgcm(j, this, a, sig):
    _pref_write(j, '_registration_id', j.as_text(a[0]) if a else '')


@jmethod('kairo/android/plugin/Utility', 'getGCMRegistrationId')
def _kutil_getgcm(j, this, a, sig):
    return _pref_read_text(j, '_registration_id')


@jmethod('kairo/android/plugin/Utility', 'addNotificationData')
def _kutil_addnotifdata(j, this, a, sig):
    key = '_plugin_notification_data'
    cur = _pref_read_text(j, key) or ''
    if len(cur):
        cur += '\n'
    add = (j.as_text(a[-1]) or '') if a else ''
    _pref_write(j, key, cur + add.replace('\n', ''))


@jmethod('kairo/android/plugin/Utility', 'removeNotificationData')
def _kutil_rmnotifdata(j, this, a, sig):
    if j.h.sharedprefs.pop('_plugin_notification_data', None) is not None:
        j.h.save_shared()


@jmethod('kairo/android/plugin/Utility', 'getNotificationData')
def _kutil_notifdata(j, this, a, sig):
    raw = _pref_read_text(j, '_plugin_notification_data')
    if raw is None:
        return j.array('Ljava/lang/String;', [])
    items = [s.strip() for s in raw.split('\n')]
    return j.array('Ljava/lang/String;',
                   [j.string(s) for s in items if s])


@jmethod('kairo/android/plugin/Utility', 'isConnected', 'hasPermission')
def _kutil_true(j, this, a, sig):
    return 1


# ------------------------------------------------------------ android.opengl
jfield('android/opengl/GLES20', 'GL_EXTENSIONS', 0x1F03)
jfield('android/opengl/GLES20', 'GL_VENDOR', 0x1F00)
jfield('android/opengl/GLES20', 'GL_RENDERER', 0x1F01)
jfield('android/opengl/GLES20', 'GL_VERSION', 0x1F02)


@jmethod('android/opengl/GLES20', 'glGetString')
def _gl_getstring(j, this, a, sig):
    return {0x1F00: 'KairoVM', 0x1F01: 'KairoVM Software Renderer',
            0x1F02: 'OpenGL ES 3.0',
            0x1F03: ('GL_OES_texture_npot GL_OES_rgb8_rgba8 '
                     'GL_EXT_texture_format_BGRA8888')}.get(
                         a[0] if a else 0, '')
