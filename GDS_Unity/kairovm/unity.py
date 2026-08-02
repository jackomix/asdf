"""Mini-Unity: the host that replaces libunity.so for the recovered game code.

`libil2cpp.so` contains the whole IL2CPP runtime plus every line of Kairosoft's
C# compiled to ARM64.  It does not contain the *engine*: everything that
UnityEngine.dll declares `extern` is resolved at run time by name through
`il2cpp_add_internal_call()`, and on Android those implementations live in
libunity.so (16 MB of OpenGL ES, Android JNI, audio and scene management).

A port cannot use that binary - it is Android/GLES/JNI only.  So we supply the
implementations ourselves.  This module is a tiny engine: an object model, a
player loop, immediate-mode drawing capture, input and preferences, wired to
the exact internal-call names the shipped binary asks for.

Nothing of the game is reimplemented here.  Every one of these entry points is
a *platform* service; the game's own code (main.Main, kairo.unity.ui.Canvas,
form.GameForm, ...) executes as shipped ARM64 machine code on top of it.

The same contract is mirrored by native/kairo_unity.c for the ARM64 handheld
build, so the two hosts stay interchangeable.
"""
import json
import os
import struct
import sys
import time
from collections import Counter, OrderedDict

from unicorn import arm64_const as A64

from . import icallsig
from .machine import TailCall, SKIP

# --------------------------------------------------------------------- table
IMPL = OrderedDict()          # exact signature -> handler
SHORT = OrderedDict()         # 'Type::Method' -> handler


def icall(*sigs):
    def deco(fn):
        for s in sigs:
            (IMPL if '(' in s else SHORT)[s] = fn
        return fn
    return deco


def f32(x):
    """Return-value marker: a 32-bit float result (goes in s0)."""
    return ('f32', float(x))


def f64(x):
    return ('f64', float(x))


# ------------------------------------------------------------------ objects
class UObj(object):
    """Native side of a UnityEngine.Object."""
    _next = 0x0BADF00D0000

    def __init__(self, host, kind, managed=0, name=''):
        UObj._next += 0x40
        self.handle = UObj._next
        self.kind = kind
        self.managed = managed
        self.name = name or kind
        self.host = host
        self.alive = True
        # component/hierarchy bookkeeping
        self.components = []
        self.children = []
        self.parent = None
        self.gameobject = None
        self.transform = None
        self.pos = [0.0, 0.0, 0.0]
        self.scale = [1.0, 1.0, 1.0]
        self.rot = [0.0, 0.0, 0.0, 1.0]
        self.active = True
        self.enabled = True
        self.data = {}
        host.objects[self.handle] = self

    def __repr__(self):
        return '<%s %s #%x>' % (self.kind, self.name, self.handle)


class Texture(UObj):
    def __init__(self, host, managed=0, w=0, h=0, fmt=4, name='Texture2D'):
        UObj.__init__(self, host, 'Texture2D', managed, name)
        self.w, self.h, self.fmt = w, h, fmt
        self.pixels = bytearray(w * h * 4)      # RGBA32
        self.filter = 0
        self.wrap = 0


class Mesh(UObj):
    def __init__(self, host, managed=0):
        UObj.__init__(self, host, 'Mesh', managed, 'Mesh')
        self.channels = {}
        self.indices = {}
        self.topology = {}


# -------------------------------------------------------------------- host
class UnityHost(object):
    # KeyCode values we care about
    KEY = {'left': 276, 'right': 275, 'up': 273, 'down': 274, 'return': 13,
           'escape': 27, 'space': 32, 'z': 122, 'x': 120}

    def __init__(self, sess, width=640, height=480, verbose=1, log_all=False):
        self.s = sess
        self.m = sess.m
        self.rt = sess.rt
        self.uc = sess.m.uc
        self.width = width
        self.height = height
        self.verbose = verbose
        self.log_all = log_all

        self.objects = {}
        self.by_managed = {}
        self.counts = Counter()
        self.unknown = Counter()
        self.trace = []
        self.max_trace = 4000
        self.sig_by_addr = {}
        self.installed = 0

        self.frame = 0
        self.t0 = 0.0
        self.now = 0.0
        self.dt = 1.0 / 60.0
        self.fixed_dt = 1.0 / 50.0
        self.target_fps = 60
        self.platform = 11               # RuntimePlatform.Android
        self.locale = ('en', 'US')
        self.density = 1.0
        self.dialogs = []                # modal dialogs the game asked for
        self.datadir = None
        self.jvm = None                  # lazily built by kairovm.androidjni

        self.prefs = {}
        self.prefs_path = None

        # immediate-mode drawing capture
        self.gl = []                     # finished primitive batches
        self.cur = None
        self.cur_color = (1.0, 1.0, 1.0, 1.0)
        self.cur_uv = (0.0, 0.0)
        self.cur_tex = None
        self.matrix_stack = []
        self.pixel_matrix = None
        self.viewport = (0, 0, width, height)
        self.clears = 0
        self.draw_calls = 0

        # input state (set by the front end)
        self.keys = set()
        self.keys_down = set()
        self.keys_up = set()
        self.mouse = (0.0, 0.0, 0.0)
        self.mouse_buttons = [False, False, False]
        self.touches = []

        self.strings = {}                # cache: python str -> guest string
        self.klass = {}
        self.arena = None
        self.scratch = 0

        # engine bookkeeping the icall handlers rely on
        self.resources = {}              # path -> managed object
        self.resource_requests = []
        self.guistyles = {}
        self.default_skin = 0
        self._shader = None
        self._props = {}
        self._prop_names = {}
        self.coroutines = []
        self.new_components = []
        self.pending_types = set()
        self.types = {}                  # managed System.Type -> Il2CppClass*
        self.by_il2cpp_type = {}         # Il2CppType* -> Il2CppClass*
        self.cur_material = None
        self.active_rt = None
        self.render_passes = []

    # --------------------------------------------------------- small utils
    def prop_id(self, name):
        i = self._props.get(name)
        if i is None:
            i = len(self._props) + 1
            self._props[name] = i
            self._prop_names[i] = name
        return i

    def prop_name(self, i):
        return self._prop_names.get(i, 'prop%d' % i)

    def default_shader(self, name=None):
        if self._shader is None:
            k = self.klass.get('UnityEngine.Shader')
            managed = self.alloc_object(k) if k else 0
            o = UObj(self, 'Shader', managed, name or 'Sprites/Default')
            if managed:
                self.bind(o, managed)
            self._shader = o
        return self._shader.managed

    def type_class(self, type_obj):
        """Il2CppClass* behind a managed System.Type.

        Handlers run inside a Unicorn hook and cannot re-enter the emulator,
        so `il2cpp_class_from_system_type` is off limits here.  Instead the
        Il2CppType* is read straight out of the reflection object
        (Il2CppReflectionType::type, right after the object header) and looked
        up in the table built by prebind_types().
        """
        if not type_obj:
            return 0
        k = self.types.get(type_obj)
        if k is not None:
            return k
        try:
            tp = self.m.read64(type_obj + 0x10)
        except Exception:
            tp = 0
        k = self.by_il2cpp_type.get(tp)
        if k:
            self.types[type_obj] = k
            return k
        self.pending_types.add(type_obj)
        return 0

    def prebind_types(self, images=None):
        """Map every Il2CppType* of the engine + game classes to its class.

        One pass at start-up buys `type_class()` a lock-free answer later,
        which is what AddComponent/GetComponent need while the guest is
        suspended inside an internal call.
        """
        rt = self.rt
        if images is None:
            images = [n for n in self.s.images
                      if n.startswith('UnityEngine.')
                      or n in ('Assembly-CSharp.dll', 'KairoLibrary.dll')]
        t0 = time.time()
        n = 0
        for name in images:
            img = self.s.images.get(name)
            if not img:
                continue
            try:
                cnt = rt.call('il2cpp_image_get_class_count', img)
            except Exception:
                continue
            for i in range(cnt):
                k = rt.call('il2cpp_image_get_class', img, i)
                if not k:
                    continue
                tp = rt.call('il2cpp_class_get_type', k)
                if tp:
                    self.by_il2cpp_type[tp] = k
                    n += 1
        if self.verbose:
            print('[unity] %d types pre-bound in %.0fs' % (n, time.time() - t0))
        return n

    def resolve_pending(self):
        """Called from the driver (outside the hook) - guest calls are legal."""
        if not self.pending_types:
            return
        for t in list(self.pending_types):
            try:
                self.types[t] = self.rt.call('il2cpp_class_from_system_type', t)
            except Exception:
                self.types[t] = 0
        self.pending_types.clear()

    # ------------------------------------------------------------ bootstrap
    def prepare(self):
        """Resolve everything the icall handlers need up front.

        Handlers run inside a Unicorn hook and therefore cannot re-enter the
        emulator; every guest call they might need is made here instead.
        """
        m, rt = self.m, self.rt
        from . import memory as M
        # a private arena for host-built managed objects (GC is off)
        self.arena = M.Arena(m.space, M.UNITY_BASE, M.UNITY_SIZE, 'unity')
        self.scratch = self.arena.alloc(0x10000)

        cor = self.s.images['mscorlib.dll']
        self.klass['String'] = rt.class_from_name(cor, 'System', 'String')
        self.klass['Object'] = rt.class_from_name(cor, 'System', 'Object')
        self.klass['Int32'] = rt.class_from_name(cor, 'System', 'Int32')
        self.klass['Byte'] = rt.class_from_name(cor, 'System', 'Byte')
        ue = self.s.images.get('UnityEngine.CoreModule.dll')
        for n in ('Object', 'GameObject', 'Transform', 'Camera', 'Component',
                  'Texture2D', 'Mesh', 'Material', 'Shader', 'MonoBehaviour',
                  'Behaviour', 'Color', 'Vector3', 'RenderTexture',
                  'TextAsset', 'Sprite', 'Coroutine', 'Texture'):
            k = rt.class_from_name(ue, 'UnityEngine', n)
            if k:
                self.klass['UnityEngine.' + n] = k
        for img, names in (('UnityEngine.TextRenderingModule.dll', ('Font',)),
                           ('UnityEngine.AudioModule.dll', ('AudioClip',)),
                           ('UnityEngine.IMGUIModule.dll',
                            ('GUIStyle', 'GUISkin'))):
            im = self.s.images.get(img)
            if not im:
                continue
            for n in names:
                k = rt.class_from_name(im, 'UnityEngine', n)
                if k:
                    self.klass['UnityEngine.' + n] = k

        # instance-size field offset inside Il2CppClass (version independent:
        # locate it by matching the value the API reports)
        self.size_off = self._find_size_offset()
        self.arr_byte = rt.call('il2cpp_array_class_get', self.klass['Byte'], 1)
        self.arr_int = rt.call('il2cpp_array_class_get', self.klass['Int32'], 1)
        ip = rt.class_from_name(cor, 'System', 'IntPtr')
        self.arr_intptr = (rt.call('il2cpp_array_class_get', ip, 1) if ip
                           else self.arr_int)
        self.arr_object = rt.call('il2cpp_array_class_get',
                                  self.klass['Object'], 1)
        self.arr_string = rt.call('il2cpp_array_class_get',
                                  self.klass['String'], 1)
        col = self.klass.get('UnityEngine.Color')
        self.arr_color = rt.call('il2cpp_array_class_get', col, 1) if col else 0
        self.f_cached_ptr = 0x10          # UnityEngine.Object.m_CachedPtr

        # singletons the engine expects to exist
        self.main_camera_go = self.make_object('GameObject', 'Main Camera',
                                               'UnityEngine.GameObject')
        self.main_camera_tf = self.make_object('Transform', 'Main Camera',
                                               'UnityEngine.Transform')
        self.main_camera = self.make_object('Camera', 'Main Camera',
                                            'UnityEngine.Camera')
        self.main_camera_go.transform = self.main_camera_tf
        self.main_camera_go.components = [self.main_camera]
        self.main_camera.gameobject = self.main_camera_go
        self.main_camera.transform = self.main_camera_tf
        self.main_camera_tf.gameobject = self.main_camera_go
        self.main_camera.data['orthographicSize'] = self.height / 2.0
        self.named = {'Main Camera': self.main_camera_go}

        self.prebind_types()
        self.open_data_dir()
        self.prefs_path = os.path.join(self.s.rootfs, 'playerprefs.json')
        if os.path.exists(self.prefs_path):
            try:
                self.prefs = json.load(open(self.prefs_path))
            except Exception:
                self.prefs = {}

    # ------------------------------------------------------------ resources
    def open_data_dir(self):
        """Mount assets/bin/Data so Resources.Load finds the shipped assets.

        Unity's own loader is in libunity.so; the serialized files it reads
        are right there in the APK, so we read them ourselves and hand the
        game the very same TextAsset / Texture2D objects it shipped with.
        """
        self.datadir = None
        here = os.path.dirname(os.path.abspath(__file__))
        tools = os.path.join(here, '..', 'tools')
        if tools not in sys.path:
            sys.path.insert(0, tools)
        try:
            from unityfs import DataDir
            self.datadir = DataDir(os.path.join(self.s.apk, 'assets/bin/Data'))
            if self.verbose:
                print('[unity] assets/bin/Data mounted: %d container entries'
                      % len(self.datadir.container))
        except Exception as e:
            print('[unity] no asset container (%r)' % (e,))

    def load_resource(self, path):
        """Resources.Load: managed object for a container path, or 0."""
        if not path:
            return 0
        key = path.strip('/').lower()
        if key in self.resources:
            return self.resources[key]
        self.resource_requests.append(path)
        got = 0
        if self.datadir is not None and key in self.datadir.container:
            try:
                got = self._build_asset(key)
            except Exception as e:
                print('[unity] asset %s failed: %r' % (key, e))
                got = 0
        if self.verbose > 1 and not got:
            print('[unity] Resources.Load("%s") -> null' % path)
        self.resources[key] = got
        return got

    def _build_asset(self, key):
        ent = self.datadir.load(key)
        if ent is None:
            return 0
        cid, parsed = ent
        name = key.rsplit('/', 1)[-1]
        if cid == 49:                                     # TextAsset
            data = parsed.get('bytes') or b''
            k = self.klass.get('UnityEngine.TextAsset')
            managed = self.alloc_object(k) if k else 0
            o = UObj(self, 'TextAsset', managed, parsed.get('name') or name)
            o.data['bytes'] = data
            if managed:
                self.bind(o, managed)
            if self.verbose:
                print('[unity] asset %-28s TextAsset %d bytes' % (key, len(data)))
            return managed
        if cid == 28:                                     # Texture2D
            w = parsed.get('width', 0)
            hgt = parsed.get('height', 0)
            k = self.klass.get('UnityEngine.Texture2D')
            managed = self.alloc_object(k) if k else 0
            t = Texture(self, managed, w, hgt, parsed.get('format', 4),
                        parsed.get('name') or name)
            t.data['raw'] = parsed.get('data') or b''
            if managed:
                self.bind(t, managed)
            if self.verbose:
                print('[unity] asset %-28s Texture2D %dx%d' % (key, w, hgt))
            return managed
        if cid == 83:                                     # AudioClip
            k = self.klass.get('UnityEngine.AudioClip')
            managed = self.alloc_object(k) if k else 0
            o = UObj(self, 'AudioClip', managed, parsed.get('name') or name)
            o.data.update(parsed or {})
            if managed:
                self.bind(o, managed)
            return managed
        if cid == 48:                                     # Shader
            return self.default_shader(name)
        return 0

    def managed_bytes(self, data):
        """byte[] in the managed heap holding `data`."""
        return self.new_array(self.arr_byte, len(data), 1, data)

    def _find_size_offset(self):
        """Byte offset of Il2CppClass::instance_size (differs per version)."""
        rt, m = self.rt, self.m
        probes = []
        for name in ('String', 'Int32', 'Object'):
            k = self.klass[name]
            probes.append((k, rt.call('il2cpp_class_instance_size', k)))
        for off in range(0x80, 0x140, 4):
            if all(m.read32(k + off) == v for k, v in probes):
                return off
        raise RuntimeError('could not locate Il2CppClass::instance_size')

    def instance_size(self, klass):
        return self.m.read32(klass + self.size_off)

    # -------------------------------------------------------- managed heap
    def alloc_object(self, klass):
        """Allocate a managed object without re-entering the VM."""
        size = max(0x18, self.instance_size(klass))
        p = self.arena.alloc((size + 15) & ~15)
        self.m.write(p, b'\0' * size)
        self.m.write64(p, klass)
        return p

    def new_string(self, s):
        if s is None:
            return 0
        cached = self.strings.get(s)
        if cached:
            return cached
        data = s.encode('utf-16-le')
        p = self.arena.alloc(0x18 + len(data) + 2)
        self.m.write(p, struct.pack('<QQi', self.klass['String'], 0, len(s))
                     + data + b'\0\0')
        self.strings[s] = p
        return p

    def read_string(self, p):
        if not p:
            return None
        n = self.m.read32(p + 0x10)
        if n < 0 or n > (1 << 20):
            return None
        return self.m.read(p + 0x14, n * 2).decode('utf-16-le', 'replace')

    def new_array(self, arr_klass, count, elem_size, data=b''):
        total = 0x20 + count * elem_size
        p = self.arena.alloc((total + 15) & ~15)
        self.m.write(p, b'\0' * total)
        self.m.write64(p, arr_klass)
        self.m.write64(p + 0x18, count)
        if data:
            self.m.write(p + 0x20, data[:count * elem_size])
        return p

    def array_len(self, p):
        return self.m.read64(p + 0x18) if p else 0

    def array_data(self, p):
        return p + 0x20

    # ------------------------------------------------------- object helpers
    def make_object(self, kind, name, klass_name=None, managed=None):
        """Create a native object and, unless given, its managed twin."""
        klass = self.klass.get(klass_name or ('UnityEngine.' + kind))
        if managed is None:
            managed = self.alloc_object(klass) if klass else 0
        o = UObj(self, kind, managed, name)
        if managed:
            self.bind(o, managed)
        return o

    def bind(self, o, managed):
        o.managed = managed
        self.by_managed[managed] = o
        self.m.write64(managed + self.f_cached_ptr, o.handle)
        return o

    def obj(self, managed):
        """Native object for a managed UnityEngine.Object (may be None)."""
        if not managed:
            return None
        o = self.by_managed.get(managed)
        if o is not None:
            return o
        h = self.m.read64(managed + self.f_cached_ptr)
        return self.objects.get(h)

    def obj_or_make(self, managed, kind='Object'):
        o = self.obj(managed)
        if o is None and managed:
            o = UObj(self, kind, managed, self.class_name(managed))
            self.bind(o, managed)
        return o

    def class_name(self, managed):
        try:
            k = self.m.read64(managed)
            n = self.m.cstr(self.m.read64(k + 0x10), 96)
            return n.decode('ascii', 'replace') if n else '?'
        except Exception:
            return '?'

    # -------------------------------------------------------- registration
    def install(self, apk_root, meta):
        """Point every internal call in the build at this host."""
        sigs = icallsig.load(apk_root, meta)
        m, rt = self.m, self.rt
        add = rt.f('il2cpp_add_internal_call')
        n = 0
        for sig in sigs:
            fn = IMPL.get(sig.raw) or SHORT.get(sig.short)
            addr = m.new_callback(self._make_stub(sig, fn), 'icall:' + sig.raw)
            self.sig_by_addr[addr] = sig
            m.call(add, m.put_cstr(sig.raw), addr)
            n += 1
        self.installed = n
        if self.verbose:
            print('[unity] %d internal calls registered, %d implemented'
                  % (n, sum(1 for s in sigs
                            if IMPL.get(s.raw) or SHORT.get(s.short))))
        return n

    def _make_stub(self, sig, fn):
        uc = self.uc
        slots = sig.slots
        name = sig.raw

        def stub(*_ignored):
            self.counts[name] += 1
            args = []
            for kind, i in slots:
                if kind == 'x':
                    args.append(uc.reg_read(getattr(A64, 'UC_ARM64_REG_X%d' % i)))
                elif kind == 'f':
                    raw = uc.reg_read(getattr(A64, 'UC_ARM64_REG_S%d' % i))
                    if isinstance(raw, int):
                        raw = struct.unpack('<f', struct.pack('<I', raw & 0xFFFFFFFF))[0]
                    args.append(raw)
                else:
                    raw = uc.reg_read(getattr(A64, 'UC_ARM64_REG_D%d' % i))
                    if isinstance(raw, int):
                        raw = struct.unpack('<d', struct.pack('<Q', raw))[0]
                    args.append(raw)
            this = uc.reg_read(A64.UC_ARM64_REG_X0) if not sig.static else 0
            if len(self.trace) < self.max_trace and (self.log_all or fn is None):
                self.trace.append(name)
            if fn is None:
                if self.unknown[name] == 0 and self.verbose > 1:
                    print('[unity] ?? %s' % name, file=sys.stderr)
                self.unknown[name] += 1
                return 0
            r = fn(self, this, args)
            if r is None:
                return 0
            if isinstance(r, tuple) and r and r[0] in ('f32', 'f64'):
                if r[0] == 'f32':
                    uc.reg_write(A64.UC_ARM64_REG_S0,
                                 struct.unpack('<I', struct.pack('<f', r[1]))[0])
                else:
                    uc.reg_write(A64.UC_ARM64_REG_D0,
                                 struct.unpack('<Q', struct.pack('<d', r[1]))[0])
                return SKIP
            if isinstance(r, float):
                uc.reg_write(A64.UC_ARM64_REG_S0,
                             struct.unpack('<I', struct.pack('<f', r))[0])
                return SKIP
            return r
        return stub

    # ------------------------------------------------------------ services
    def write_vec3(self, p, x, y, z):
        self.m.write(p, struct.pack('<fff', x, y, z))

    def write_vec2(self, p, x, y):
        self.m.write(p, struct.pack('<ff', x, y))

    def read_vec3(self, p):
        return struct.unpack('<fff', self.m.read(p, 12))

    def read_color(self, p):
        return struct.unpack('<ffff', self.m.read(p, 16))

    def save_prefs(self):
        if self.prefs_path:
            try:
                json.dump(self.prefs, open(self.prefs_path, 'w'))
            except Exception:
                pass

    def report(self, top=40):
        out = ['[unity] %d icalls made, %d distinct; %d unimplemented hits'
               % (sum(self.counts.values()), len(self.counts),
                  sum(self.unknown.values()))]
        for k, v in self.unknown.most_common(top):
            out.append('   MISSING %-70s %d' % (k, v))
        if self.jvm is not None:
            out.append(self.jvm.report(top))
        return '\n'.join(out)


# ==========================================================================
#  Application / player
# ==========================================================================
@icall('UnityEngine.Application::get_persistentDataPath()')
def _persist(h, this, a):
    return h.new_string('/data/data/%s/files' % h.s.pkg)


@icall('UnityEngine.Application::get_temporaryCachePath()')
def _tmp(h, this, a):
    return h.new_string('/data/data/%s/cache' % h.s.pkg)


@icall('UnityEngine.Application::get_dataPath()')
def _datapath(h, this, a):
    return h.new_string('/apk/assets/bin/Data')


@icall('UnityEngine.Application::get_streamingAssetsPath()')
def _streaming(h, this, a):
    return h.new_string('/apk/assets')


@icall('UnityEngine.Application::get_productName()')
def _product(h, this, a):
    return h.new_string('Game Dev Story')


@icall('UnityEngine.Application::get_companyName()')
def _company(h, this, a):
    return h.new_string('Kairosoft')


@icall('UnityEngine.Application::get_identifier()')
def _identifier(h, this, a):
    return h.new_string(h.s.pkg)


@icall('UnityEngine.Application::get_version()')
def _version(h, this, a):
    return h.new_string('2.6.9')


@icall('UnityEngine.Application::get_unityVersion()')
def _uversion(h, this, a):
    return h.new_string('2022.3.62f2')


@icall('UnityEngine.Application::get_platform()')
def _platform(h, this, a):
    return h.platform


@icall('UnityEngine.Application::get_isPlaying()',
       'UnityEngine.Application::get_isFocused()')
def _isplaying(h, this, a):
    return 1


@icall('UnityEngine.Application::get_isEditor()',
       'UnityEngine.Application::get_isBatchMode()',
       'UnityEngine.Application::get_isMobilePlatform()')
def _iseditor(h, this, a):
    return 0


@icall('UnityEngine.Application::get_systemLanguage()')
def _lang(h, this, a):
    return 10                      # SystemLanguage.English


@icall('UnityEngine.Application::get_internetReachability()')
def _net(h, this, a):
    return 0                       # NotReachable


@icall('UnityEngine.Application::get_targetFrameRate()')
def _gettarget(h, this, a):
    return h.target_fps


@icall('UnityEngine.Application::set_targetFrameRate(System.Int32)')
def _settarget(h, this, a):
    h.target_fps = a[0] if a[0] > 0 else 60


@icall('UnityEngine.Application::Quit(System.Int32)',
       'UnityEngine.Application::Quit()')
def _quit(h, this, a):
    h.s.quit = True


@icall('UnityEngine.Application::CanStreamedLevelBeLoaded(System.Int32)')
def _canstream(h, this, a):
    return 0


@icall('UnityEngine.Application::get_runInBackground()')
def _runbg(h, this, a):
    return 1


@icall('UnityEngine.Application::HasProLicense()')
def _prolic(h, this, a):
    return 1


# ------------------------------------------------------------------- Screen
@icall('UnityEngine.Screen::get_width()')
def _sw(h, this, a):
    return h.width


@icall('UnityEngine.Screen::get_height()')
def _sh(h, this, a):
    return h.height


@icall('UnityEngine.Screen::get_dpi()')
def _dpi(h, this, a):
    return f32(160.0)


@icall('UnityEngine.Screen::GetScreenOrientation()')
def _orient(h, this, a):
    return 4 if h.width >= h.height else 1     # LandscapeLeft / Portrait


@icall('UnityEngine.Screen::get_fullScreen()')
def _fs(h, this, a):
    return 1


@icall('UnityEngine.Screen::get_safeArea_Injected(UnityEngine.Rect&)')
def _safearea(h, this, a):
    h.m.write(a[0], struct.pack('<ffff', 0.0, 0.0, float(h.width), float(h.height)))


@icall('UnityEngine.Screen::GetResolution_Injected(UnityEngine.Resolution&)')
def _res(h, this, a):
    h.m.write(a[0], struct.pack('<iiii', h.width, h.height, h.target_fps, 1))


# --------------------------------------------------------------------- Time
@icall('UnityEngine.Time::get_time()', 'UnityEngine.Time::get_unscaledTime()',
       'UnityEngine.Time::get_realtimeSinceStartup()',
       'UnityEngine.Time::get_timeSinceLevelLoad()',
       'UnityEngine.Time::get_fixedTime()')
def _time(h, this, a):
    return f32(h.now)


@icall('UnityEngine.Time::get_realtimeSinceStartupAsDouble()',
       'UnityEngine.Time::get_timeAsDouble()',
       'UnityEngine.Time::get_unscaledTimeAsDouble()')
def _timed(h, this, a):
    return f64(h.now)


@icall('UnityEngine.Time::get_deltaTime()',
       'UnityEngine.Time::get_unscaledDeltaTime()',
       'UnityEngine.Time::get_smoothDeltaTime()')
def _dt(h, this, a):
    return f32(h.dt)


@icall('UnityEngine.Time::get_fixedDeltaTime()')
def _fdt(h, this, a):
    return f32(h.fixed_dt)


@icall('UnityEngine.Time::set_fixedDeltaTime(System.Single)')
def _sfdt(h, this, a):
    h.fixed_dt = a[0] or h.fixed_dt


@icall('UnityEngine.Time::get_frameCount()')
def _frames(h, this, a):
    return h.frame


@icall('UnityEngine.Time::get_timeScale()')
def _tscale(h, this, a):
    return f32(1.0)


# =========================================================== UnityEngine.Object
@icall('UnityEngine.Object::GetName(UnityEngine.Object)')
def _getname(h, this, a):
    o = h.obj(a[0])
    return h.new_string(o.name if o else '')


@icall('UnityEngine.Object::SetName(UnityEngine.Object,System.String)')
def _setname(h, this, a):
    o = h.obj_or_make(a[0])
    if o:
        o.name = h.read_string(a[1]) or ''


@icall('UnityEngine.Object::GetInstanceID()')
def _instid(h, this, a):
    o = h.obj_or_make(this)
    return (o.handle >> 6) & 0x7FFFFFFF if o else 0


@icall('UnityEngine.Object::Destroy(UnityEngine.Object,System.Single)',
       'UnityEngine.Object::DestroyImmediate(UnityEngine.Object,System.Boolean)')
def _destroy(h, this, a):
    o = h.obj(a[0])
    if o:
        o.alive = False
        if o.managed:
            h.m.write64(o.managed + h.f_cached_ptr, 0)


@icall('UnityEngine.Object::DontDestroyOnLoad(UnityEngine.Object)')
def _dontdestroy(h, this, a):
    return None


@icall('UnityEngine.Object::ToString(UnityEngine.Object)')
def _tostring(h, this, a):
    o = h.obj(a[0])
    return h.new_string(o.name if o else 'null')


@icall('UnityEngine.Object::FindObjectsOfType(System.Type,System.Boolean)')
def _findobjs(h, this, a):
    return h.new_array(h.arr_int, 0, 8)


@icall('UnityEngine.Object::CurrentThreadIsMainThread()')
def _ismain(h, this, a):
    return 1


# ============================================================== GameObject
@icall('UnityEngine.GameObject::Internal_CreateGameObject(UnityEngine.GameObject,System.String)')
def _newgo(h, this, a):
    name = h.read_string(a[1]) or 'GameObject'
    o = UObj(h, 'GameObject', a[0], name)
    h.bind(o, a[0])
    tf = h.make_object('Transform', name, 'UnityEngine.Transform')
    tf.gameobject = o
    o.transform = tf
    h.named.setdefault(name, o)
    return None


@icall('UnityEngine.GameObject::get_transform()')
def _gotf(h, this, a):
    o = h.obj_or_make(this, 'GameObject')
    if o.transform is None:
        tf = h.make_object('Transform', o.name, 'UnityEngine.Transform')
        tf.gameobject = o
        o.transform = tf
    return o.transform.managed


@icall('UnityEngine.GameObject::Find(System.String)')
def _gofind(h, this, a):
    name = h.read_string(a[0])
    o = h.named.get(name)
    return o.managed if o else 0


@icall('UnityEngine.GameObject::SetActive(System.Boolean)')
def _gosetactive(h, this, a):
    o = h.obj_or_make(this, 'GameObject')
    o.active = bool(a[0])


@icall('UnityEngine.GameObject::get_activeSelf()',
       'UnityEngine.GameObject::get_activeInHierarchy()')
def _gogetactive(h, this, a):
    o = h.obj_or_make(this, 'GameObject')
    return 1 if o.active else 0


@icall('UnityEngine.GameObject::get_layer()')
def _golayer(h, this, a):
    return 0


@icall('UnityEngine.GameObject::set_layer(System.Int32)')
def _gosetlayer(h, this, a):
    return None


@icall('UnityEngine.GameObject::GetComponent(System.Type)',
       'UnityEngine.GameObject::GetComponentInChildren(System.Type,System.Boolean)',
       'UnityEngine.GameObject::TryGetComponentInternal(System.Type)',
       'UnityEngine.GameObject::GetComponentInParent(System.Type,System.Boolean)')
def _gogetcomp(h, this, a):
    o = h.obj_or_make(this, 'GameObject')
    want = h.type_class(a[0])
    for c in o.components:
        if not want or h.m.read64(c.managed) == want:
            return c.managed
    return 0


@icall('UnityEngine.Component::get_gameObject()')
def _cgo(h, this, a):
    o = h.obj_or_make(this, 'Component')
    if o.gameobject is None:
        go = h.make_object('GameObject', o.name, 'UnityEngine.GameObject')
        go.components.append(o)
        o.gameobject = go
    return o.gameobject.managed


@icall('UnityEngine.Component::get_transform()')
def _ctf(h, this, a):
    o = h.obj_or_make(this, 'Component')
    if o.transform is None:
        go = o.gameobject
        if go is None:
            go = h.make_object('GameObject', o.name, 'UnityEngine.GameObject')
            go.components.append(o)
            o.gameobject = go
        if go.transform is None:
            go.transform = h.make_object('Transform', o.name,
                                         'UnityEngine.Transform')
            go.transform.gameobject = go
        o.transform = go.transform
    return o.transform.managed


@icall('UnityEngine.Behaviour::get_enabled()')
def _benabled(h, this, a):
    o = h.obj_or_make(this, 'Behaviour')
    return 1 if o.enabled else 0


@icall('UnityEngine.Behaviour::set_enabled(System.Boolean)')
def _bsetenabled(h, this, a):
    o = h.obj_or_make(this, 'Behaviour')
    o.enabled = bool(a[0])


@icall('UnityEngine.Behaviour::get_isActiveAndEnabled()')
def _bactive(h, this, a):
    return 1


# =============================================================== Transform
def _tf(h, this):
    return h.obj_or_make(this, 'Transform')


@icall('UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)',
       'UnityEngine.Transform::get_localPosition_Injected(UnityEngine.Vector3&)')
def _tfgetpos(h, this, a):
    o = _tf(h, this)
    h.write_vec3(a[0], *o.pos)


@icall('UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)',
       'UnityEngine.Transform::set_localPosition_Injected(UnityEngine.Vector3&)')
def _tfsetpos(h, this, a):
    o = _tf(h, this)
    o.pos = list(h.read_vec3(a[0]))


@icall('UnityEngine.Transform::get_localScale_Injected(UnityEngine.Vector3&)',
       'UnityEngine.Transform::get_lossyScale_Injected(UnityEngine.Vector3&)')
def _tfgetscale(h, this, a):
    h.write_vec3(a[0], *_tf(h, this).scale)


@icall('UnityEngine.Transform::set_localScale_Injected(UnityEngine.Vector3&)')
def _tfsetscale(h, this, a):
    _tf(h, this).scale = list(h.read_vec3(a[0]))


@icall('UnityEngine.Transform::get_rotation_Injected(UnityEngine.Quaternion&)',
       'UnityEngine.Transform::get_localRotation_Injected(UnityEngine.Quaternion&)')
def _tfgetrot(h, this, a):
    h.m.write(a[0], struct.pack('<ffff', *_tf(h, this).rot))


@icall('UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)',
       'UnityEngine.Transform::set_localRotation_Injected(UnityEngine.Quaternion&)')
def _tfsetrot(h, this, a):
    _tf(h, this).rot = list(struct.unpack('<ffff', h.m.read(a[0], 16)))


@icall('UnityEngine.Transform::get_childCount()')
def _tfchildren(h, this, a):
    return len(_tf(h, this).children)


@icall('UnityEngine.Transform::GetChild(System.Int32)')
def _tfchild(h, this, a):
    o = _tf(h, this)
    if 0 <= a[0] < len(o.children):
        return o.children[a[0]].managed
    return 0


@icall('UnityEngine.Transform::GetParent()', 'UnityEngine.Transform::get_parent()')
def _tfparent(h, this, a):
    o = _tf(h, this)
    return o.parent.managed if o.parent else 0


@icall('UnityEngine.Transform::SetParent(UnityEngine.Transform,System.Boolean)',
       'UnityEngine.Transform::set_parent(UnityEngine.Transform)')
def _tfsetparent(h, this, a):
    o = _tf(h, this)
    p = h.obj(a[0]) if a and a[0] else None
    if o.parent and o in o.parent.children:
        o.parent.children.remove(o)
    o.parent = p
    if p is not None:
        p.children.append(o)


@icall('UnityEngine.Transform::get_localToWorldMatrix_Injected(UnityEngine.Matrix4x4&)',
       'UnityEngine.Transform::get_worldToLocalMatrix_Injected(UnityEngine.Matrix4x4&)')
def _tfmatrix(h, this, a):
    ident = [1.0 if i % 5 == 0 else 0.0 for i in range(16)]
    h.m.write(a[0], struct.pack('<16f', *ident))


# ================================================================== Camera
@icall('UnityEngine.Camera::get_main()')
def _cammain(h, this, a):
    return h.main_camera.managed


@icall('UnityEngine.Camera::get_current()')
def _camcur(h, this, a):
    return h.main_camera.managed


@icall('UnityEngine.Camera::get_allCamerasCount()')
def _camcount(h, this, a):
    return 1


@icall('UnityEngine.Camera::set_orthographicSize(System.Single)')
def _camsetortho(h, this, a):
    h.obj_or_make(this, 'Camera').data['orthographicSize'] = a[0]


@icall('UnityEngine.Camera::get_orthographicSize()')
def _camortho(h, this, a):
    return f32(h.obj_or_make(this, 'Camera').data.get('orthographicSize',
                                                      h.height / 2.0))


@icall('UnityEngine.Camera::get_pixelWidth()')
def _campw(h, this, a):
    return f32(float(h.width))


@icall('UnityEngine.Camera::get_pixelHeight()')
def _camph(h, this, a):
    return f32(float(h.height))


@icall('UnityEngine.Camera::get_scaledPixelWidth()')
def _camspw(h, this, a):
    return h.width


@icall('UnityEngine.Camera::get_scaledPixelHeight()')
def _camsph(h, this, a):
    return h.height


@icall('UnityEngine.Camera::get_aspect()')
def _camaspect(h, this, a):
    return f32(float(h.width) / max(1.0, float(h.height)))


@icall('UnityEngine.Camera::get_depth()')
def _camdepth(h, this, a):
    return f32(h.obj_or_make(this, 'Camera').data.get('depth', 0.0))


@icall('UnityEngine.Camera::set_depth(System.Single)')
def _camsetdepth(h, this, a):
    h.obj_or_make(this, 'Camera').data['depth'] = a[0]


@icall('UnityEngine.Camera::get_pixelRect_Injected(UnityEngine.Rect&)',
       'UnityEngine.Camera::get_rect_Injected(UnityEngine.Rect&)')
def _campixelrect(h, this, a):
    h.m.write(a[0], struct.pack('<ffff', 0.0, 0.0, float(h.width), float(h.height)))


# =================================================================== Input
@icall('UnityEngine.Input::GetKeyInt(UnityEngine.KeyCode)')
def _getkey(h, this, a):
    return 1 if a[0] in h.keys else 0


@icall('UnityEngine.Input::GetKeyDownInt(UnityEngine.KeyCode)')
def _getkeydown(h, this, a):
    return 1 if a[0] in h.keys_down else 0


@icall('UnityEngine.Input::GetKeyUpInt(UnityEngine.KeyCode)')
def _getkeyup(h, this, a):
    return 1 if a[0] in h.keys_up else 0


@icall('UnityEngine.Input::get_anyKey()', 'UnityEngine.Input::get_anyKeyDown()')
def _anykey(h, this, a):
    return 1 if h.keys else 0


@icall('UnityEngine.Input::get_mousePosition_Injected(UnityEngine.Vector3&)')
def _mousepos(h, this, a):
    h.write_vec3(a[0], *h.mouse)


@icall('UnityEngine.Input::get_mouseScrollDelta_Injected(UnityEngine.Vector2&)')
def _mousescroll(h, this, a):
    h.write_vec2(a[0], 0.0, 0.0)


@icall('UnityEngine.Input::GetMouseButton(System.Int32)',
       'UnityEngine.Input::GetMouseButtonDown(System.Int32)',
       'UnityEngine.Input::GetMouseButtonUp(System.Int32)')
def _mousebtn(h, this, a):
    i = a[0]
    return 1 if 0 <= i < 3 and h.mouse_buttons[i] else 0


@icall('UnityEngine.Input::get_touchCount()')
def _touchcount(h, this, a):
    return len(h.touches)


@icall('UnityEngine.Input::get_touchSupported()')
def _touchsup(h, this, a):
    return 0


@icall('UnityEngine.Input::get_mousePresent()')
def _mousepresent(h, this, a):
    return 1


@icall('UnityEngine.Input::get_multiTouchEnabled()')
def _multitouch(h, this, a):
    return 0


@icall('UnityEngine.Input::set_multiTouchEnabled(System.Boolean)',
       'UnityEngine.Input::CheckDisabled()',
       'UnityEngine.Input::ClearLastPenContactEvent()')
def _inputnop(h, this, a):
    return None


@icall('UnityEngine.Input::get_inputString()',
       'UnityEngine.Input::get_compositionString()')
def _inputstr(h, this, a):
    return h.new_string('')


@icall('UnityEngine.Input::GetTouch_Injected(System.Int32,UnityEngine.Touch&)')
def _gettouch(h, this, a):
    h.m.write(a[1], b'\0' * 60)


@icall('UnityEngine.Input::GetJoystickNames()')
def _joynames(h, this, a):
    return h.new_array(h.arr_int, 0, 8)


# ============================================================= PlayerPrefs
@icall('UnityEngine.PlayerPrefs::GetString(System.String,System.String)')
def _ppgets(h, this, a):
    k = h.read_string(a[0])
    v = h.prefs.get(k)
    return h.new_string(v) if isinstance(v, str) else a[1]


@icall('UnityEngine.PlayerPrefs::SetString(System.String,System.String)',
       'UnityEngine.PlayerPrefs::TrySetString(System.String,System.String)')
def _ppsets(h, this, a):
    h.prefs[h.read_string(a[0])] = h.read_string(a[1])
    return 1


@icall('UnityEngine.PlayerPrefs::GetInt(System.String,System.Int32)')
def _ppgeti(h, this, a):
    v = h.prefs.get(h.read_string(a[0]))
    return int(v) if isinstance(v, (int, float)) else a[1]


@icall('UnityEngine.PlayerPrefs::TrySetInt(System.String,System.Int32)',
       'UnityEngine.PlayerPrefs::SetInt(System.String,System.Int32)')
def _ppseti(h, this, a):
    h.prefs[h.read_string(a[0])] = a[1]
    return 1


@icall('UnityEngine.PlayerPrefs::GetFloat(System.String,System.Single)')
def _ppgetf(h, this, a):
    v = h.prefs.get(h.read_string(a[0]))
    return f32(float(v) if isinstance(v, (int, float)) else a[0 + 1])


@icall('UnityEngine.PlayerPrefs::TrySetFloat(System.String,System.Single)',
       'UnityEngine.PlayerPrefs::SetFloat(System.String,System.Single)')
def _ppsetf(h, this, a):
    h.prefs[h.read_string(a[0])] = a[1]
    return 1


@icall('UnityEngine.PlayerPrefs::HasKey(System.String)')
def _pphas(h, this, a):
    return 1 if h.read_string(a[0]) in h.prefs else 0


@icall('UnityEngine.PlayerPrefs::DeleteKey(System.String)')
def _ppdel(h, this, a):
    h.prefs.pop(h.read_string(a[0]), None)


@icall('UnityEngine.PlayerPrefs::DeleteAll()')
def _ppdelall(h, this, a):
    h.prefs.clear()


@icall('UnityEngine.PlayerPrefs::Save()')
def _ppsave(h, this, a):
    h.save_prefs()


# ============================================================== SystemInfo
@icall('UnityEngine.SystemInfo::GetDeviceModel()')
def _devmodel(h, this, a):
    return h.new_string('R36S')


@icall('UnityEngine.SystemInfo::GetDeviceName()')
def _devname(h, this, a):
    return h.new_string('handheld')


@icall('UnityEngine.SystemInfo::GetOperatingSystem()')
def _devos(h, this, a):
    return h.new_string('Linux ARM64')


@icall('UnityEngine.SystemInfo::GetDeviceUniqueIdentifier()')
def _devuid(h, this, a):
    return h.new_string('kairovm-0000')


@icall('UnityEngine.SystemInfo::GetGraphicsDeviceType()')
def _gfxtype(h, this, a):
    return 11                       # OpenGLES3


@icall('UnityEngine.SystemInfo::GetSystemMemorySize()')
def _memsize(h, this, a):
    return 2048


@icall('UnityEngine.SystemInfo::GetGraphicsMemorySize()')
def _gfxmem(h, this, a):
    return 512


@icall('UnityEngine.SystemInfo::GetProcessorCount()')
def _cpucount(h, this, a):
    return 4


@icall('UnityEngine.SystemInfo::SupportsTextureFormat(UnityEngine.TextureFormat)',
       'UnityEngine.SystemInfo::SupportsRenderTextureFormat(UnityEngine.RenderTextureFormat)')
def _supports(h, this, a):
    return 1


@icall('UnityEngine.SystemInfo::GetDeviceType()')
def _devtype(h, this, a):
    return 1                        # Handheld


# ================================================================= logging
@icall('UnityEngine.DebugLogHandler::Internal_Log(UnityEngine.LogType,UnityEngine.LogOption,System.String,UnityEngine.Object)')
def _dlog(h, this, a):
    msg = h.read_string(a[2])
    print('[game] %s' % msg)


@icall('UnityEngine.DebugLogHandler::Internal_LogException(System.Exception,UnityEngine.Object)')
def _dlogex(h, this, a):
    print('[game] EXCEPTION %s' % h.rt.describe_exception(a[0]))


@icall('UnityEngine.Debug::get_isDebugBuild()')
def _isdebug(h, this, a):
    return 0


# =========================================================== immediate mode
GL_MODE = {0: 'triangles', 1: 'triangle_strip', 2: 'quads', 3: 'lines',
           4: 'line_strip'}


@icall('UnityEngine.GL::Begin(System.Int32)')
def _glbegin(h, this, a):
    h.cur = {'mode': a[0], 'tex': h.cur_tex, 'v': []}


@icall('UnityEngine.GL::End()')
def _glend(h, this, a):
    if h.cur and h.cur['v']:
        h.gl.append(h.cur)
        h.draw_calls += 1
    h.cur = None


@icall('UnityEngine.GL::Vertex3(System.Single,System.Single,System.Single)')
def _glvertex(h, this, a):
    if h.cur is not None:
        h.cur['v'].append((a[0], a[1], a[2], h.cur_uv[0], h.cur_uv[1],
                           h.cur_color))


@icall('UnityEngine.GL::TexCoord3(System.Single,System.Single,System.Single)')
def _gltexcoord(h, this, a):
    h.cur_uv = (a[0], a[1])


@icall('UnityEngine.GL::ImmediateColor(System.Single,System.Single,System.Single,System.Single)')
def _glcolor(h, this, a):
    h.cur_color = (a[0], a[1], a[2], a[3])


@icall('UnityEngine.GL::GLClear_Injected(System.Boolean,System.Boolean,UnityEngine.Color&,System.Single)')
def _glclear(h, this, a):
    h.clears += 1
    h.gl.append({'mode': -1, 'clear': h.read_color(a[2])})


@icall('UnityEngine.GL::PushMatrix()', 'UnityEngine.GL::PopMatrix()',
       'UnityEngine.GL::LoadOrtho()', 'UnityEngine.GL::LoadPixelMatrix()',
       'UnityEngine.GL::LoadProjectionMatrix_Injected(UnityEngine.Matrix4x4&)',
       'UnityEngine.GL::SetViewMatrix_Injected(UnityEngine.Matrix4x4&)',
       'UnityEngine.GL::LoadIdentity()', 'UnityEngine.GL::Flush()',
       'UnityEngine.GL::InvalidateState()')
def _glnop(h, this, a):
    return None


@icall('UnityEngine.GL::GLLoadPixelMatrixScript(System.Single,System.Single,System.Single,System.Single)')
def _glpixelmatrix(h, this, a):
    h.pixel_matrix = tuple(a)


@icall('UnityEngine.GL::Viewport_Injected(UnityEngine.Rect&)')
def _glviewport(h, this, a):
    x, y, w, hh = struct.unpack('<ffff', h.m.read(a[0], 16))
    h.viewport = (x, y, w, hh)


@icall('UnityEngine.GL::get_wireframe()', 'UnityEngine.GL::get_sRGBWrite()')
def _glflags(h, this, a):
    return 0


@icall('UnityEngine.GL::set_wireframe(System.Boolean)',
       'UnityEngine.GL::set_sRGBWrite(System.Boolean)',
       'UnityEngine.GL::set_invertCulling(System.Boolean)')
def _glsetflags(h, this, a):
    return None


# =============================================================== Texture2D
def _tex(h, managed, w=0, hh=0):
    o = h.obj(managed)
    if o is None:
        o = Texture(h, managed, w, hh)
        h.bind(o, managed)
    return o


@icall('UnityEngine.Texture2D::Internal_CreateImpl(UnityEngine.Texture2D,System.Int32,System.Int32,System.Int32,UnityEngine.Experimental.Rendering.GraphicsFormat,UnityEngine.TextureColorSpace,UnityEngine.Experimental.Rendering.TextureCreationFlags,System.IntPtr,System.String)')
def _texcreate(h, this, a):
    managed, w, hh = a[0], a[1], a[2]
    t = Texture(h, managed, w, hh, a[4], h.read_string(a[8]) or 'Texture2D')
    h.bind(t, managed)
    return 1


@icall('UnityEngine.Texture2D::ReinitializeImpl(System.Int32,System.Int32)',
       'UnityEngine.Texture2D::ReinitializeWithFormatImpl(System.Int32,System.Int32,UnityEngine.Experimental.Rendering.GraphicsFormat,System.Boolean)',
       'UnityEngine.Texture2D::ReinitializeWithTextureFormatImpl(System.Int32,System.Int32,UnityEngine.TextureFormat,System.Boolean)')
def _texresize(h, this, a):
    t = _tex(h, this, a[0], a[1])
    t.w, t.h = a[0], a[1]
    t.pixels = bytearray(t.w * t.h * 4)
    return 1


@icall('UnityEngine.Texture::GetDataWidth()')
def _texw(h, this, a):
    return _tex(h, this).w


@icall('UnityEngine.Texture::GetDataHeight()')
def _texh(h, this, a):
    return _tex(h, this).h


@icall('UnityEngine.Texture2D::get_format()')
def _texfmt(h, this, a):
    return getattr(_tex(h, this), 'fmt', 4)


@icall('UnityEngine.Texture2D::get_isReadable()', 'UnityEngine.Texture::get_isReadable()')
def _texreadable(h, this, a):
    return 1


@icall('UnityEngine.Texture::get_filterMode()')
def _texfilter(h, this, a):
    return getattr(_tex(h, this), 'filter', 0)


@icall('UnityEngine.Texture::set_filterMode(UnityEngine.FilterMode)')
def _texsetfilter(h, this, a):
    _tex(h, this).filter = a[0]


@icall('UnityEngine.Texture::get_wrapMode()')
def _texwrap(h, this, a):
    return getattr(_tex(h, this), 'wrap', 0)


@icall('UnityEngine.Texture::set_wrapMode(UnityEngine.TextureWrapMode)',
       'UnityEngine.Texture::set_anisoLevel(System.Int32)',
       'UnityEngine.Texture::set_mipMapBias(System.Single)')
def _texsetwrap(h, this, a):
    return None


@icall('UnityEngine.Texture::get_texelSize_Injected(UnityEngine.Vector2&)')
def _textexel(h, this, a):
    t = _tex(h, this)
    h.write_vec2(a[0], 1.0 / max(1, t.w), 1.0 / max(1, t.h))


@icall('UnityEngine.Texture2D::ApplyImpl(System.Boolean,System.Boolean)',
       'UnityEngine.Texture2D::Compress(System.Boolean)',
       'UnityEngine.Texture2D::MarkNonReadable()')
def _texapply(h, this, a):
    t = _tex(h, this)
    t.dirty = True


@icall('UnityEngine.Texture2D::SetAllPixels32(UnityEngine.Color32[],System.Int32)')
def _texsetall32(h, this, a):
    t = _tex(h, this)
    n = h.array_len(a[0])
    data = h.m.read(h.array_data(a[0]), min(n * 4, t.w * t.h * 4))
    t.pixels[:len(data)] = data
    t.dirty = True


@icall('UnityEngine.Texture2D::SetBlockOfPixels32(System.Int32,System.Int32,System.Int32,System.Int32,UnityEngine.Color32[],System.Int32)')
def _texsetblock32(h, this, a):
    t = _tex(h, this)
    x, y, w, hh = a[0], a[1], a[2], a[3]
    src = h.m.read(h.array_data(a[4]), w * hh * 4)
    for row in range(hh):
        dst = ((y + row) * t.w + x) * 4
        t.pixels[dst:dst + w * 4] = src[row * w * 4:(row + 1) * w * 4]
    t.dirty = True


@icall('UnityEngine.Texture2D::SetPixelsImpl(System.Int32,System.Int32,System.Int32,System.Int32,UnityEngine.Color[],System.Int32,System.Int32)')
def _texsetpixels(h, this, a):
    t = _tex(h, this)
    x, y, w, hh = a[0], a[1], a[2], a[3]
    src = h.m.read(h.array_data(a[4]), w * hh * 16)
    out = bytearray(w * hh * 4)
    for i in range(w * hh):
        r, g, b, al = struct.unpack_from('<ffff', src, i * 16)
        out[i * 4:i * 4 + 4] = bytes((int(max(0.0, min(1.0, r)) * 255),
                                      int(max(0.0, min(1.0, g)) * 255),
                                      int(max(0.0, min(1.0, b)) * 255),
                                      int(max(0.0, min(1.0, al)) * 255)))
    for row in range(hh):
        dst = ((y + row) * t.w + x) * 4
        t.pixels[dst:dst + w * 4] = out[row * w * 4:(row + 1) * w * 4]
    t.dirty = True


@icall('UnityEngine.Texture2D::SetPixelDataImplArray(System.Array,System.Int32,System.Int32,System.Int32,System.Int32)')
def _texsetdata(h, this, a):
    t = _tex(h, this)
    n = h.array_len(a[0])
    data = h.m.read(h.array_data(a[0]), min(n, len(t.pixels)))
    t.pixels[:len(data)] = data
    t.dirty = True


@icall('UnityEngine.Texture2D::LoadRawTextureDataImplArray(System.Byte[])')
def _texloadraw(h, this, a):
    t = _tex(h, this)
    n = h.array_len(a[0])
    data = h.m.read(h.array_data(a[0]), min(n, len(t.pixels)))
    t.pixels[:len(data)] = data
    t.dirty = True


@icall('UnityEngine.Texture2D::GetPixels(System.Int32,System.Int32,System.Int32,System.Int32,System.Int32)')
def _texgetpixels(h, this, a):
    t = _tex(h, this)
    x, y, w, hh = a[0], a[1], a[2], a[3]
    buf = bytearray(w * hh * 16)
    for row in range(hh):
        for col in range(w):
            i = ((y + row) * t.w + x + col) * 4
            px = t.pixels[i:i + 4] if i + 4 <= len(t.pixels) else b'\0\0\0\0'
            struct.pack_into('<ffff', buf, (row * w + col) * 16,
                             px[0] / 255.0, px[1] / 255.0,
                             px[2] / 255.0, px[3] / 255.0)
    return h.new_array(h.arr_color, w * hh, 16, bytes(buf))


@icall('UnityEngine.Texture2D::GetPixels32(System.Int32)')
def _texgetpixels32(h, this, a):
    t = _tex(h, this)
    return h.new_array(h.arr_int, t.w * t.h, 4, bytes(t.pixels))


@icall('UnityEngine.Texture2D::GetRawTextureData()')
def _texgetraw(h, this, a):
    t = _tex(h, this)
    return h.new_array(h.arr_byte, len(t.pixels), 1, bytes(t.pixels))


@icall('UnityEngine.Texture2D::GetRawImageDataSize()')
def _texrawsize(h, this, a):
    return len(_tex(h, this).pixels)


@icall('UnityEngine.Texture2D::get_whiteTexture()', 'UnityEngine.Texture2D::get_blackTexture()',
       'UnityEngine.Texture2D::get_grayTexture()', 'UnityEngine.Texture2D::get_redTexture()',
       'UnityEngine.Texture2D::get_normalTexture()', 'UnityEngine.Texture2D::get_linearGrayTexture()')
def _texbuiltin(h, this, a):
    key = 'builtin-texture'
    t = h.objects.get(h.__dict__.get(key))
    if t is None:
        t = Texture(h, h.alloc_object(h.klass['UnityEngine.Texture2D']), 4, 4)
        t.pixels = bytearray(b'\xff' * (4 * 4 * 4))
        h.bind(t, t.managed)
        h.__dict__[key] = t.handle
    return t.managed


@icall('UnityEngine.ImageConversion::LoadImage(UnityEngine.Texture2D,System.Byte[],System.Boolean)')
def _loadimage(h, this, a):
    """PNG/JPEG decode - the engine uses this for images it stores itself."""
    t = _tex(h, a[0])
    n = h.array_len(a[1])
    data = h.m.read(h.array_data(a[1]), n)
    try:
        from .png import decode_png
        w, hh, px = decode_png(data)
    except Exception:
        return 0
    t.w, t.h, t.pixels = w, hh, bytearray(px)
    t.dirty = True
    return 1


# ==================================================================== Mesh
def _mesh(h, managed):
    o = h.obj(managed)
    if o is None:
        o = Mesh(h, managed)
        h.bind(o, managed)
    return o


@icall('UnityEngine.Mesh::Internal_Create(UnityEngine.Mesh)')
def _meshcreate(h, this, a):
    m = Mesh(h, a[0])
    h.bind(m, a[0])


@icall('UnityEngine.Mesh::ClearImpl(System.Boolean)')
def _meshclear(h, this, a):
    m = _mesh(h, this)
    m.channels.clear()
    m.indices.clear()


@icall('UnityEngine.Mesh::SetArrayForChannelImpl(UnityEngine.Rendering.VertexAttribute,UnityEngine.Rendering.VertexAttributeFormat,System.Int32,System.Array,System.Int32,System.Int32,System.Int32,UnityEngine.Rendering.MeshUpdateFlags)')
def _meshsetchannel(h, this, a):
    m = _mesh(h, this)
    channel, fmt, dim, arr, count = a[0], a[1], a[2], a[3], a[4]
    esz = 4 * dim
    m.channels[channel] = h.m.read(h.array_data(arr), count * esz) if arr else b''


@icall('UnityEngine.Mesh::SetIndicesImpl(System.Int32,UnityEngine.MeshTopology,UnityEngine.Rendering.IndexFormat,System.Array,System.Int32,System.Int32,System.Boolean,System.Int32)')
def _meshsetindices(h, this, a):
    m = _mesh(h, this)
    sub, topo, fmt, arr, start, count = a[0], a[1], a[2], a[3], a[4], a[5]
    esz = 2 if fmt == 0 else 4
    m.indices[sub] = h.m.read(h.array_data(arr) + start * esz, count * esz)
    m.topology[sub] = (topo, esz)


@icall('UnityEngine.Mesh::get_vertexCount()')
def _meshvcount(h, this, a):
    return len(_mesh(h, this).channels.get(0, b'')) // 12


@icall('UnityEngine.Mesh::get_subMeshCount()')
def _meshsubcount(h, this, a):
    return max(1, len(_mesh(h, this).indices))


@icall('UnityEngine.Mesh::get_canAccess()', 'UnityEngine.Mesh::HasVertexAttribute(UnityEngine.Rendering.VertexAttribute)')
def _meshaccess(h, this, a):
    return 1


@icall('UnityEngine.Mesh::MarkDynamicImpl()',
       'UnityEngine.Mesh::RecalculateBoundsImpl(UnityEngine.Rendering.MeshUpdateFlags)',
       'UnityEngine.Mesh::SetSubMeshesImpl(System.Array)')
def _meshnop(h, this, a):
    return None


@icall('UnityEngine.Graphics::Internal_DrawMeshNow2_Injected(UnityEngine.Mesh,System.Int32,UnityEngine.Matrix4x4&)')
def _drawmeshnow(h, this, a):
    m = h.obj(a[0])
    mat = struct.unpack('<16f', h.m.read(a[2], 64))
    h.gl.append({'mode': -2, 'mesh': m, 'matrix': mat, 'tex': h.cur_tex})
    h.draw_calls += 1


@icall('UnityEngine.Graphics::Internal_DrawTexture(UnityEngine.Internal_DrawTextureArguments&)')
def _drawtexture(h, this, a):
    x, y, w, hh = struct.unpack('<ffff', h.m.read(a[0], 16))
    tex = h.m.read64(a[0] + 16)
    h.gl.append({'mode': -3, 'rect': (x, y, w, hh), 'tex': h.obj(tex)})
    h.draw_calls += 1


# ================================================================ Material
@icall('UnityEngine.Material::CreateWithShader(UnityEngine.Material,UnityEngine.Shader)',
       'UnityEngine.Material::CreateWithMaterial(UnityEngine.Material,UnityEngine.Material)',
       'UnityEngine.Material::CreateWithString(UnityEngine.Material)')
def _matcreate(h, this, a):
    o = UObj(h, 'Material', a[0], 'Material')
    h.bind(o, a[0])


@icall('UnityEngine.Material::SetPass(System.Int32)')
def _matsetpass(h, this, a):
    o = h.obj_or_make(this, 'Material')
    h.cur_tex = o.data.get('_MainTex')
    h.cur_material = o
    return 1


@icall('UnityEngine.Material::SetTextureImpl(System.Int32,UnityEngine.Texture)')
def _matsettex(h, this, a):
    o = h.obj_or_make(this, 'Material')
    o.data[h.prop_name(a[0])] = h.obj(a[1])
    if h.prop_name(a[0]) == '_MainTex':
        o.data['_MainTex'] = h.obj(a[1])


@icall('UnityEngine.Material::GetTextureImpl(System.Int32)')
def _matgettex(h, this, a):
    o = h.obj_or_make(this, 'Material')
    t = o.data.get(h.prop_name(a[0]))
    return t.managed if t else 0


@icall('UnityEngine.Material::SetColorImpl_Injected(System.Int32,UnityEngine.Color&)')
def _matsetcolor(h, this, a):
    o = h.obj_or_make(this, 'Material')
    o.data[h.prop_name(a[0])] = h.read_color(a[1])


@icall('UnityEngine.Material::SetFloatImpl(System.Int32,System.Single)')
def _matsetfloat(h, this, a):
    h.obj_or_make(this, 'Material').data[h.prop_name(a[0])] = a[1]


@icall('UnityEngine.Material::GetFloatImpl(System.Int32)')
def _matgetfloat(h, this, a):
    return f32(h.obj_or_make(this, 'Material').data.get(h.prop_name(a[0]), 0.0))


@icall('UnityEngine.Material::HasProperty(System.Int32)')
def _mathasprop(h, this, a):
    return 1


@icall('UnityEngine.Material::SetMatrixImpl_Injected(System.Int32,UnityEngine.Matrix4x4&)',
       'UnityEngine.Material::EnableKeyword(System.String)',
       'UnityEngine.Material::DisableKeyword(System.String)',
       'UnityEngine.Material::SetShaderKeywords(System.String[])',
       'UnityEngine.Material::set_shader(UnityEngine.Shader)',
       'UnityEngine.Material::SetIntImpl(System.Int32,System.Int32)',
       'UnityEngine.Material::set_renderQueue(System.Int32)')
def _matnop(h, this, a):
    return None


@icall('UnityEngine.Material::get_renderQueue()')
def _matqueue(h, this, a):
    return 3000


@icall('UnityEngine.Material::get_shader()')
def _matshader(h, this, a):
    return h.default_shader()


@icall('UnityEngine.Shader::PropertyToID(System.String)', 'UnityEngine.Shader::TagToID(System.String)')
def _proptoid(h, this, a):
    name = h.read_string(a[0]) or ''
    return h.prop_id(name)


@icall('UnityEngine.Shader::get_isSupported()')
def _shadersupported(h, this, a):
    return 1


@icall('UnityEngine.Shader::get_renderQueue()')
def _shaderqueue(h, this, a):
    return 3000


@icall('UnityEngine.ResourcesAPIInternal::FindShaderByName(System.String)',
       'UnityEngine.Shader::FindShaderByName(System.String)')
def _findshader(h, this, a):
    return h.default_shader(h.read_string(a[0]))


# =============================================================== Resources
@icall('UnityEngine.ResourcesAPIInternal::Load(System.String,System.Type)')
def _resload(h, this, a):
    return h.load_resource(h.read_string(a[0]))


@icall('UnityEngine.TextAsset::get_bytes()')
def _textasset_bytes(h, this, a):
    # Unity hands out a *fresh copy* of the asset every time.  The game
    # decrypts in place (AssetReader.GetData -> Encrypter.Decode), so handing
    # back a shared array would decrypt the same buffer twice.
    o = h.obj(this)
    data = o.data.get('bytes', b'') if o is not None else b''
    return h.managed_bytes(data)


@icall('UnityEngine.TextAsset::Internal_CreateInstance(UnityEngine.TextAsset,System.String)')
def _textasset_create(h, this, a):
    o = UObj(h, 'TextAsset', a[0], 'TextAsset')
    o.data['bytes'] = (h.read_string(a[1]) or '').encode('utf-8')
    h.bind(o, a[0])
    return None


@icall('UnityEngine.ResourcesAPIInternal::UnloadAsset(UnityEngine.Object)',
       'UnityEngine.Resources::UnloadUnusedAssets()')
def _resunload(h, this, a):
    return 0


@icall('UnityEngine.Resources::GetBuiltinResource(System.Type,System.String)')
def _resbuiltin(h, this, a):
    return 0


# ==================================================================== IMGUI
@icall('UnityEngine.GUIUtility::Internal_GetDefaultSkin(System.Int32)')
def _guiskin(h, this, a):
    return h.default_skin


@icall('UnityEngine.GUIUtility::get_pixelsPerPoint()')
def _guippp(h, this, a):
    return f32(1.0)


@icall('UnityEngine.GUIUtility::Internal_GetHotControl()',
       'UnityEngine.GUIUtility::Internal_GetKeyboardControl()',
       'UnityEngine.GUIUtility::get_guiDepth()')
def _guizero(h, this, a):
    return 0


@icall('UnityEngine.GUIStyle::Internal_Create(UnityEngine.GUIStyle)')
def _guistylecreate(h, this, a):
    o = UObj(h, 'GUIStyle', 0, 'GUIStyle')
    o.data['state'] = h.arena.alloc(0x100)
    h.m.write(o.data['state'], b'\0' * 0x100)
    h.guistyles[a[0]] = o
    return o.handle


@icall('UnityEngine.GUIStyle::GetStyleStatePtr(System.Int32)')
def _guistate(h, this, a):
    o = h.objects.get(this)
    if o is None:
        p = h.arena.alloc(0x100)
        h.m.write(p, b'\0' * 0x100)
        return p
    return o.data.setdefault('state%d' % a[0], h.arena.alloc(0x100))


@icall('UnityEngine.GUIStyle::GetRectOffsetPtr(System.Int32)')
def _guirect(h, this, a):
    p = h.arena.alloc(16)
    h.m.write(p, b'\0' * 16)
    return p


@icall('UnityEngine.GUIStyle::get_fontSize()')
def _guifontsize(h, this, a):
    o = h.objects.get(this)
    return o.data.get('fontSize', 12) if o else 12


@icall('UnityEngine.GUIStyle::set_fontSize(System.Int32)')
def _guisetfontsize(h, this, a):
    o = h.objects.get(this)
    if o:
        o.data['fontSize'] = a[0]


@icall('UnityEngine.GUIStyle::get_alignment()')
def _guialign(h, this, a):
    return 0


@icall('UnityEngine.GUIStyle::get_font()')
def _guifont(h, this, a):
    o = h.objects.get(this)
    return o.data.get('font', 0) if o else 0


@icall('UnityEngine.GUIStyle::set_font(UnityEngine.Font)')
def _guisetfont(h, this, a):
    o = h.objects.get(this)
    if o:
        o.data['font'] = a[0]


@icall('UnityEngine.GUIStyle::Internal_CalcSize_Injected(UnityEngine.GUIContent,UnityEngine.Vector2&)',
       'UnityEngine.GUIStyle::Internal_CalcMinMaxWidth_Injected(UnityEngine.GUIContent,UnityEngine.Vector2&)')
def _guicalcsize(h, this, a):
    h.write_vec2(a[1], 8.0, 12.0)


@icall('UnityEngine.GUIStyle::Internal_GetLineHeight(System.IntPtr)')
def _guilineheight(h, this, a):
    return f32(12.0)


@icall('UnityEngine.Font::Internal_CreateFont(UnityEngine.Font,System.String)')
def _fontcreate(h, this, a):
    o = UObj(h, 'Font', a[0], h.read_string(a[1]) or 'Font')
    h.bind(o, a[0])


@icall('UnityEngine.Font::get_dynamic()')
def _fontdynamic(h, this, a):
    return 1


@icall('UnityEngine.Font::get_fontSize()')
def _fontsize(h, this, a):
    return 12


@icall('UnityEngine.Font::HasCharacter(System.Int32)')
def _fonthaschar(h, this, a):
    return 1


@icall('UnityEngine.Event::Internal_Create(System.Int32)')
def _eventcreate(h, this, a):
    p = h.arena.alloc(0x80)
    h.m.write(p, b'\0' * 0x80)
    return p


@icall('UnityEngine.Event::Internal_Destroy(System.IntPtr)',
       'UnityEngine.Event::Internal_Use()',
       'UnityEngine.Event::CopyFromPtr(System.IntPtr)',
       'UnityEngine.Event::Internal_SetNativeEvent(System.IntPtr)')
def _eventnop(h, this, a):
    return None


@icall('UnityEngine.Event::get_type()', 'UnityEngine.Event::get_rawType()')
def _eventtype(h, this, a):
    return 7                     # EventType.Repaint


@icall('UnityEngine.Event::PopEvent(UnityEngine.Event)')
def _eventpop(h, this, a):
    return 0


# ============================================================ MonoBehaviour
@icall('UnityEngine.MonoBehaviour::StartCoroutineManaged2(System.Collections.IEnumerator)')
def _startcoroutine(h, this, a):
    co = h.alloc_object(h.klass.get('UnityEngine.Coroutine') or h.klass['Object'])
    h.coroutines.append([a[0], co])
    return co


@icall('UnityEngine.MonoBehaviour::StopAllCoroutines()')
def _stopcoroutines(h, this, a):
    h.coroutines = []


@icall('UnityEngine.MonoBehaviour::StopCoroutineManaged(UnityEngine.Coroutine)',
       'UnityEngine.MonoBehaviour::StopCoroutineFromEnumeratorManaged(System.Collections.IEnumerator)',
       'UnityEngine.MonoBehaviour::StopCoroutine(System.String)',
       'UnityEngine.MonoBehaviour::set_useGUILayout(System.Boolean)',
       'UnityEngine.MonoBehaviour::OnCancellationTokenCreated()')
def _mbnop(h, this, a):
    return None


@icall('UnityEngine.MonoBehaviour::get_useGUILayout()')
def _usegui(h, this, a):
    return 1


@icall('UnityEngine.MonoBehaviour::IsObjectMonoBehaviour(UnityEngine.Object)')
def _ismb(h, this, a):
    return 1


@icall('UnityEngine.Coroutine::ReleaseCoroutine(System.IntPtr)')
def _releasecoroutine(h, this, a):
    return None


@icall('UnityEngine.GameObject::Internal_AddComponentWithType(System.Type)')
def _addcomponent(h, this, a):
    go = h.obj_or_make(this, 'GameObject')
    klass = h.type_class(a[0])
    if not klass:
        h.pending_types.add(a[0])
        return 0
    managed = h.alloc_object(klass)
    c = UObj(h, 'Component', managed, h.class_name(managed))
    h.bind(c, managed)
    c.gameobject = go
    c.transform = go.transform
    go.components.append(c)
    h.new_components.append(c)
    return managed


# ==========================================================================
#  Android JNI - the game's Java layer still calls into it off Android too.
#  Implemented by kairovm/androidjni.py (imported at the bottom of this file).
# ==========================================================================


# =========================================================================
#  Late additions: everything the first full boot asked for and missed.
# =========================================================================
@icall('UnityEngine.SystemInfo::SupportsTextureFormatNative(UnityEngine.TextureFormat)',
       'UnityEngine.SystemInfo::SupportsRenderTextureFormatNative(UnityEngine.RenderTextureFormat)',
       'UnityEngine.SystemInfo::SupportsBlendingOnRenderTextureFormatNative(UnityEngine.RenderTextureFormat)',
       'UnityEngine.SystemInfo::IsFormatSupported(UnityEngine.Experimental.Rendering.GraphicsFormat,UnityEngine.Experimental.Rendering.GraphicsFormatUsage)')
def _supportsformat(h, this, a):
    return 1


@icall('UnityEngine.Experimental.Rendering.GraphicsFormatUtility::IsCompressedFormat_Native_TextureFormat(UnityEngine.TextureFormat)',
       'UnityEngine.Experimental.Rendering.GraphicsFormatUtility::IsCompressedFormat_Native_GraphicsFormat(UnityEngine.Experimental.Rendering.GraphicsFormat)',
       'UnityEngine.Experimental.Rendering.GraphicsFormatUtility::CanDecompressFormat(UnityEngine.Experimental.Rendering.GraphicsFormat,System.Boolean)')
def _iscompressed(h, this, a):
    return 0


@icall('UnityEngine.QualitySettings::get_antiAliasing()',
       'UnityEngine.QualitySettings::get_vSyncCount()',
       'UnityEngine.QualitySettings::get_masterTextureLimit()',
       'UnityEngine.QualitySettings::get_anisotropicFiltering()')
def _qualityget(h, this, a):
    return 0


@icall('UnityEngine.QualitySettings::set_vSyncCount(System.Int32)',
       'UnityEngine.QualitySettings::set_antiAliasing(System.Int32)',
       'UnityEngine.QualitySettings::set_masterTextureLimit(System.Int32)',
       'UnityEngine.Screen::set_fullScreen(System.Boolean)',
       'UnityEngine.Screen::SetResolution(System.Int32,System.Int32,UnityEngine.FullScreenMode,UnityEngine.RefreshRate)',
       'UnityEngine.Screen::set_sleepTimeout(System.Int32)')
def _screenset(h, this, a):
    return None


@icall('UnityEngine.Screen::get_fullScreen()')
def _fullscreen(h, this, a):
    return 1


@icall('UnityEngine.PlayerPrefs::TrySetSetString(System.String,System.String)')
def _prefstryset(h, this, a):
    h.prefs[h.read_string(a[0])] = h.read_string(a[1])
    return 1


@icall('UnityEngine.PlayerPrefs::TrySetInt(System.String,System.Int32)',
       'UnityEngine.PlayerPrefs::TrySetFloat(System.String,System.Single)')
def _prefstrysetnum(h, this, a):
    h.prefs[h.read_string(a[0])] = a[1]
    return 1


@icall('UnityEngine.Internal.InputUnsafeUtility::GetAxis(System.String)',
       'UnityEngine.Internal.InputUnsafeUtility::GetAxisRaw(System.String)')
def _getaxis(h, this, a):
    name = (h.read_string(a[0]) or '').lower()
    x = 0.0
    if 'horizontal' in name:
        x = (1.0 if 'right' in h.keys else 0.0) - (1.0 if 'left' in h.keys else 0.0)
    elif 'vertical' in name:
        x = (1.0 if 'up' in h.keys else 0.0) - (1.0 if 'down' in h.keys else 0.0)
    return f32(x)


@icall('UnityEngine.Internal.InputUnsafeUtility::GetButton(System.String)',
       'UnityEngine.Internal.InputUnsafeUtility::GetButtonDown(System.String)',
       'UnityEngine.Internal.InputUnsafeUtility::GetButtonUp(System.String)')
def _getbutton(h, this, a):
    return 0


@icall('UnityEngine.AudioSource::set_playOnAwake(System.Boolean)',
       'UnityEngine.AudioSource::set_loop(System.Boolean)',
       'UnityEngine.AudioSource::set_volume(System.Single)',
       'UnityEngine.AudioSource::set_pitch(System.Single)',
       'UnityEngine.AudioSource::set_clip(UnityEngine.AudioClip)',
       'UnityEngine.AudioSource::set_mute(System.Boolean)',
       'UnityEngine.AudioSource::set_time(System.Single)',
       'UnityEngine.AudioSource::Play(System.UInt64)',
       'UnityEngine.AudioSource::PlayOneShotHelper(UnityEngine.AudioSource,UnityEngine.AudioClip,System.Single)',
       'UnityEngine.AudioSource::Stop(System.Boolean)',
       'UnityEngine.AudioSource::Pause()',
       'UnityEngine.AudioSource::UnPause()',
       'UnityEngine.AudioListener::set_volume(System.Single)',
       'UnityEngine.AudioListener::set_pause(System.Boolean)')
def _audionop(h, this, a):
    return None


@icall('UnityEngine.AudioSource::get_isPlaying()',
       'UnityEngine.AudioSource::get_loop()',
       'UnityEngine.AudioSource::get_mute()')
def _audiofalse(h, this, a):
    return 0


@icall('UnityEngine.AudioSource::get_volume()',
       'UnityEngine.AudioSource::get_time()',
       'UnityEngine.AudioSource::get_pitch()',
       'UnityEngine.AudioListener::get_volume()')
def _audiozero(h, this, a):
    return f32(1.0)


@icall('UnityEngine.AudioClip::get_length()',
       'UnityEngine.AudioClip::get_samples()',
       'UnityEngine.AudioClip::get_channels()',
       'UnityEngine.AudioClip::get_frequency()',
       'UnityEngine.AudioClip::get_loadState()')
def _clipinfo(h, this, a):
    return 0


# GUIStyleState colours travel through a by-ref Color; keep them per object.
@icall('UnityEngine.GUIStyleState::set_textColor_Injected(UnityEngine.Color&)')
def _stylestate_set(h, this, a):
    h.guistyles[('textColor', this)] = h.read_color(a[0])
    return None


@icall('UnityEngine.GUIStyleState::get_textColor_Injected(UnityEngine.Color&)')
def _stylestate_get(h, this, a):
    c = h.guistyles.get(('textColor', this), (1.0, 1.0, 1.0, 1.0))
    h.m.write(a[0], struct.pack('<ffff', *c))
    return None




@icall('UnityEngine.Experimental.Rendering.GraphicsFormatUtility::GetGraphicsFormat_Native_TextureFormat(UnityEngine.TextureFormat,System.Boolean)')
def _gfxformat(h, this, a):
    # TextureFormat -> GraphicsFormat, for the handful the game builds.
    srgb = bool(a[1])
    return {3: 24 if srgb else 23,      # RGB24
            4: 8 if srgb else 7,        # RGBA32
            5: 10 if srgb else 9,       # ARGB32
            1: 1,                       # Alpha8
            }.get(a[0], 8 if srgb else 7)


@icall('UnityEngine.Experimental.Rendering.GraphicsFormatUtility::IsCrunchFormat(UnityEngine.TextureFormat)',
       'UnityEngine.Experimental.Rendering.GraphicsFormatUtility::IsSRGBFormat(UnityEngine.Experimental.Rendering.GraphicsFormat)')
def _iscrunch(h, this, a):
    return 0


@icall('UnityEngine.QualitySettings::GetQualityLevel()')
def _qualitylevel(h, this, a):
    return 0


@icall('UnityEngine.QualitySettings::get_names()')
def _qualitynames(h, this, a):
    return h.new_array(h.arr_string, 1, 8,
                       struct.pack('<Q', h.new_string('Medium')))




# =========================================================================
#  Prefabs, cloning and off-screen rendering.
#
#  kairo.unity.graphics.Offscreen instantiates Prefabs/Offscreen (a
#  GameObject carrying a Camera), points a RenderTexture at it and reads the
#  pixels back.  The prefab really is in the shipped container, so it is
#  rebuilt here from the serialized file rather than invented.
# =========================================================================
@icall('UnityEngine.Object::Internal_CloneSingle(UnityEngine.Object)',
       'UnityEngine.Object::Internal_CloneSingleWithParent(UnityEngine.Object,UnityEngine.Transform,System.Boolean)')
def _clone(h, this, a):
    src = h.obj(a[0])
    if src is None:
        return 0
    klass = h.m.read64(a[0])
    managed = h.alloc_object(klass)
    h.m.write(managed, h.m.read(a[0], max(0x18, h.instance_size(klass))))
    o = UObj(h, src.kind, managed, src.name + '(Clone)')
    h.bind(o, managed)
    o.data = dict(src.data)
    o.pos, o.scale, o.rot = list(src.pos), list(src.scale), list(src.rot)
    if src.kind == 'GameObject':
        tf = h.make_object('Transform', o.name)
        tf.gameobject = o
        o.transform = tf
        for c in src.components:
            ck = h.m.read64(c.managed) if c.managed else 0
            cm = h.alloc_object(ck) if ck else 0
            if cm:
                h.m.write(cm, h.m.read(c.managed,
                                       max(0x18, h.instance_size(ck))))
            nc = UObj(h, c.kind, cm, c.name)
            if cm:
                h.bind(nc, cm)
            nc.data = dict(c.data)
            nc.gameobject = o
            nc.transform = tf
            o.components.append(nc)
    return managed


# ------------------------------------------------------------ RenderTexture
@icall('UnityEngine.RenderTexture::Internal_Create(UnityEngine.RenderTexture)')
def _rt_create(h, this, a):
    t = Texture(h, a[0], 0, 0, 4, 'RenderTexture')
    t.kind = 'RenderTexture'
    h.bind(t, a[0])
    return None


@icall('UnityEngine.RenderTexture::set_width(System.Int32)')
def _rt_setw(h, this, a):
    t = h.obj_or_make(this, 'RenderTexture')
    t.w = a[0]
    return None


@icall('UnityEngine.RenderTexture::set_height(System.Int32)')
def _rt_seth(h, this, a):
    t = h.obj_or_make(this, 'RenderTexture')
    t.h = a[0]
    return None


@icall('UnityEngine.RenderTexture::get_width()')
def _rt_getw(h, this, a):
    t = h.obj(this)
    return getattr(t, 'w', 0) if t else 0


@icall('UnityEngine.RenderTexture::get_height()')
def _rt_geth(h, this, a):
    t = h.obj(this)
    return getattr(t, 'h', 0) if t else 0


@icall('UnityEngine.RenderTexture::SetActive(UnityEngine.RenderTexture)')
def _rt_setactive(h, this, a):
    h.active_rt = h.obj(a[0])
    return None


@icall('UnityEngine.RenderTexture::GetActive()')
def _rt_getactive(h, this, a):
    rt = getattr(h, 'active_rt', None)
    return rt.managed if rt else 0


@icall('UnityEngine.RenderTexture::set_enableRandomWrite(System.Boolean)',
       'UnityEngine.RenderTexture::set_depthStencilFormat(UnityEngine.Experimental.Rendering.GraphicsFormat)',
       'UnityEngine.RenderTexture::SetColorFormat(UnityEngine.Experimental.Rendering.GraphicsFormat)',
       'UnityEngine.RenderTexture::SetMipMapCount(System.Int32)',
       'UnityEngine.RenderTexture::SetSRGBReadWrite(System.Boolean)',
       'UnityEngine.RenderTexture::Release()',
       'UnityEngine.RenderTexture::ReleaseTemporary(UnityEngine.RenderTexture)',
       'UnityEngine.Graphics::Internal_SetNullRT()')
def _rt_nop(h, this, a):
    return None


@icall('UnityEngine.RenderTexture::GetColorFormat(System.Boolean)')
def _rt_colorformat(h, this, a):
    return 8


@icall('UnityEngine.RenderTexture::SupportsStencil(UnityEngine.RenderTexture)')
def _rt_stencil(h, this, a):
    return 1


@icall('UnityEngine.Camera::Render()')
def _camera_render(h, this, a):
    """A camera pass: hand the batch list to the front end and start a new one."""
    h.render_passes.append(h.gl)
    h.gl = []
    h.draw_calls += 1
    return None


@icall('UnityEngine.Texture2D::ReadPixelsImpl_Injected(UnityEngine.Rect&,System.Int32,System.Int32,System.Boolean)')
def _readpixels(h, this, a):
    t = h.obj(this)
    if t is not None and getattr(t, 'w', 0) and getattr(t, 'h', 0):
        need = t.w * t.h * 4
        if len(t.pixels) < need:
            t.pixels = bytearray(need)
    return None


@icall('UnityEngine.Component::GetComponentFastPath(System.Type,System.IntPtr)',
       'UnityEngine.GameObject::GetComponentFastPath(System.Type,System.IntPtr)',
       'UnityEngine.GameObject::TryGetComponentFastPath(System.Type,System.IntPtr)')
def _getcomponentfast(h, this, a):
    """Unity writes the result through the IntPtr out-parameter."""
    o = h.obj(this)
    if o is None:
        return None
    go = o if o.kind == 'GameObject' else (o.gameobject or o)
    klass = h.type_class(a[0])
    found = 0
    for c in ([go.transform] if go.transform else []) + list(go.components):
        if not c or not c.managed:
            continue
        if not klass or h.m.read64(c.managed) == klass:
            found = c.managed
            break
    if a[1]:
        h.m.write64(a[1], found)
    return None


from . import androidjni                                          # noqa: E402,F401
