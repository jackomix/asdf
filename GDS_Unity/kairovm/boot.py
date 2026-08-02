"""Boot libil2cpp.so inside KairoVM and bring the IL2CPP runtime up."""
import argparse
import os
import sys
import time

from .machine import Machine, GuestError
from .bionic import Bionic
from unicorn import arm64_const as A64


DEFAULT_PKG = 'net.kairosoft.android.gamedev3en'


class Il2CppRuntime(object):
    """Thin binding to the il2cpp C embedding API running inside the VM."""

    def __init__(self, m):
        self.m = m
        self._cache = {}

    def f(self, name):
        a = self._cache.get(name)
        if a is None:
            a = self.m.sym(name)
            self._cache[name] = a
        return a

    def call(self, name, *args, **kw):
        return self.m.call(self.f(name), *args, **kw)

    # ------------------------------------------------------------ startup
    def set_data_dir(self, path):
        self.call('il2cpp_set_data_dir', self.m.put_cstr(path))

    def set_config_dir(self, path):
        self.call('il2cpp_set_config_dir', self.m.put_cstr(path))

    def set_temp_dir(self, path):
        self.call('il2cpp_set_temp_dir', self.m.put_cstr(path))

    def set_config(self, name):
        self.call('il2cpp_set_config', self.m.put_cstr(name))

    def set_commandline_arguments(self, argv):
        m = self.m
        arr = m.env.alloc(8 * (len(argv) + 1))
        for i, s in enumerate(argv):
            m.write64(arr + i * 8, m.put_cstr(s))
        m.write64(arr + len(argv) * 8, 0)
        self.call('il2cpp_set_commandline_arguments', len(argv), arr, 0)

    def init(self, domain='IL2CPP Root Domain'):
        return self.call('il2cpp_init', self.m.put_cstr(domain))

    def gc_disable(self):
        self.call('il2cpp_gc_disable')

    def gc_enable(self):
        self.call('il2cpp_gc_enable')

    # ------------------------------------------------------------ reflection
    def domain(self):
        return self.call('il2cpp_domain_get')

    def thread_attach(self, dom):
        return self.call('il2cpp_thread_attach', dom)

    def assembly_open(self, name):
        return self.call('il2cpp_domain_assembly_open', self.domain(),
                         self.m.put_cstr(name))

    def assemblies(self):
        m = self.m
        cnt = m.env.alloc(8)
        arr = self.call('il2cpp_domain_get_assemblies', self.domain(), cnt)
        n = m.read64(cnt)
        return [m.read64(arr + i * 8) for i in range(n)]

    def assembly_image(self, asm_):
        return self.call('il2cpp_assembly_get_image', asm_)

    def image_name(self, img):
        return self._s(self.call('il2cpp_image_get_name', img))

    def image_class_count(self, img):
        return self.call('il2cpp_image_get_class_count', img)

    def image_class(self, img, i):
        return self.call('il2cpp_image_get_class', img, i)

    def class_from_name(self, img, ns, name):
        return self.call('il2cpp_class_from_name', img,
                         self.m.put_cstr(ns), self.m.put_cstr(name))

    def class_name(self, k):
        return self._s(self.call('il2cpp_class_get_name', k))

    def class_namespace(self, k):
        return self._s(self.call('il2cpp_class_get_namespace', k))

    def method_from_name(self, k, name, argc):
        return self.call('il2cpp_class_get_method_from_name', k,
                         self.m.put_cstr(name), argc)

    def method_name(self, mth):
        return self._s(self.call('il2cpp_method_get_name', mth))

    def methods(self, k):
        m = self.m
        it = m.env.alloc(8)
        m.write64(it, 0)
        out = []
        while True:
            mm = self.call('il2cpp_class_get_methods', k, it)
            if not mm:
                break
            out.append(mm)
        return out

    def fields(self, k):
        m = self.m
        it = m.env.alloc(8)
        m.write64(it, 0)
        out = []
        while True:
            f = self.call('il2cpp_class_get_fields', k, it)
            if not f:
                break
            out.append(f)
        return out

    def field_name(self, f):
        return self._s(self.call('il2cpp_field_get_name', f))

    def field_offset(self, f):
        return self.call('il2cpp_field_get_offset', f)

    def runtime_class_init(self, k):
        return self.call('il2cpp_runtime_class_init', k)

    def invoke(self, method, obj=0, args=(), catch=True):
        m = self.m
        argv = 0
        if args:
            argv = m.env.alloc(8 * len(args))
            for i, v in enumerate(args):
                m.write64(argv + i * 8, v)
        exc = m.env.alloc(8)
        m.write64(exc, 0)
        r = self.call('il2cpp_runtime_invoke', method, obj, argv, exc)
        e = m.read64(exc)
        if e and catch:
            raise GuestError('managed exception: %s' % self.describe_exception(e))
        return r

    def describe_exception(self, exc):
        try:
            k = self.m.read64(exc)
            name = self.class_name(k)
            msg = self.m.read64(exc + 0x18)
            return '%s: %s' % (name, self.string(msg) if msg else '')
        except Exception:
            return '<exception @%#x>' % exc

    def string_new(self, s):
        return self.call('il2cpp_string_new', self.m.put_cstr(s))

    def string(self, obj):
        if not obj:
            return None
        m = self.m
        n = m.read32(obj + 0x10)
        chars = m.read(obj + 0x14, n * 2)
        return chars.decode('utf-16-le', 'replace')

    def _s(self, p):
        v = self.m.cstr(p)
        return v.decode('utf-8', 'replace') if v else None


def build(apk_root, pkg=DEFAULT_PKG, verbose=0, echo_log=True, trace=False):
    """Load libil2cpp.so and return (machine, bionic, runtime)."""
    lib = os.path.join(apk_root, 'lib/arm64-v8a/libil2cpp.so')
    if not os.path.exists(lib):
        raise SystemExit('missing %s - run tools/extract_apk.sh first' % lib)

    m = Machine(verbose=verbose, trace=trace)
    rootfs = os.path.join(apk_root, '_rootfs')
    data_dir = '/data/app/%s/lib/arm64/../../base.apk/assets/bin/Data' % pkg
    # Build a virtual Android filesystem that points at the extracted APK.
    os.makedirs(rootfs, exist_ok=True)
    apk_mount = os.path.join(rootfs, 'apk')
    if not os.path.islink(apk_mount) and not os.path.exists(apk_mount):
        os.symlink(os.path.abspath(apk_root), apk_mount)
    files_dir = os.path.join(rootfs, 'data/data', pkg, 'files')
    os.makedirs(files_dir, exist_ok=True)

    host = Bionic(m, rootfs, cwd='/data/data/%s' % pkg)
    host.log_echo = echo_log
    host._env.update({
        'LANG': 'en_US.UTF-8',
        'ANDROID_DATA': '/data',
        'ANDROID_ROOT': '/system',
        'TMPDIR': '/data/data/%s/cache' % pkg,
        'HOME': '/data/data/%s' % pkg,
    })

    li = m.load(lib)
    m.bootstrap_main_thread()
    return m, host, li


def main(argv=None):
    ap = argparse.ArgumentParser(description='Boot Game Dev Story (Unity/IL2CPP) in KairoVM')
    ap.add_argument('--apk', default='out/apk', help='extracted APK root')
    ap.add_argument('-v', '--verbose', action='count', default=0)
    ap.add_argument('--no-init', action='store_true', help='stop after loading the image')
    ap.add_argument('--dump-classes', action='store_true')
    ap.add_argument('--trace', action='store_true', help='record basic-block trace')
    ap.add_argument('--symbols', action='store_true', default=True)
    args = ap.parse_args(argv)

    t0 = time.time()
    m, host, li = build(args.apk, verbose=max(args.verbose, 1), trace=args.trace)
    if args.symbols:
        try:
            sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
            from symbols import SymbolTable
            import il2cpp_meta as _MD
            meta = _MD.load(os.path.join(
                args.apk, 'assets/bin/Data/Managed/Metadata/global-metadata.dat'))
            names = tuple(meta.string(meta.image_def(i).nameIndex)
                          for i in range(meta.count('images')))
            m.symbols = SymbolTable(args.apk, names)
            print('[boot] symbolizer: %d managed methods with code'
                  % len(m.symbols.by_addr))
        except Exception as e:
            print('[boot] symbol table unavailable: %r' % e)
    print('[boot] libil2cpp.so loaded in %.1fs' % (time.time() - t0))

    rt = Il2CppRuntime(m)
    print('[boot] running %d static initialisers' % len(m.init_array(li)))
    m.run_init_array(li)
    print('[boot] init_array done (%.1fs)' % (time.time() - t0))

    if args.no_init:
        return 0

    data_dir = '/apk/assets/bin/Data/Managed'
    rt.set_data_dir(data_dir)
    rt.set_config_dir('/apk/assets/bin/Data/Managed/etc')
    rt.set_temp_dir('/data/data/%s/cache' % DEFAULT_PKG)
    rt.set_commandline_arguments(['GameDevStory'])
    print('[boot] calling il2cpp_init ...')
    t1 = time.time()
    rc = rt.init()
    print('[boot] il2cpp_init -> %d in %.1fs' % (rc, time.time() - t1))

    if m.missing:
        print('[boot] unimplemented imports hit: %r' % sorted(m.missing.items(),
                                                              key=lambda kv: -kv[1])[:20])
    return 0


if __name__ == '__main__':
    sys.exit(main())
