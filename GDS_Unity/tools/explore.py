"""Boot the IL2CPP runtime, then reflect over and call real game code.

This proves the port is executing Kairosoft's shipped implementation
rather than a reimplementation: every method invoked here is the ARM64
code compiled from their C# sources inside libil2cpp.so.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from kairovm.boot import build, Il2CppRuntime, DEFAULT_PKG        # noqa: E402
from kairovm.machine import GuestError                            # noqa: E402
from unicorn import arm64_const as A64                            # noqa: E402


class Session(object):
    def __init__(self, apk='out/apk', verbose=1, echo_log=True):
        t0 = time.time()
        self.m, self.host, self.li = build(apk, verbose=verbose, echo_log=echo_log)
        self.m.watch = False
        self.rt = Il2CppRuntime(self.m)
        self.m.run_init_array(self.li)
        self.rt.set_data_dir('/apk/assets/bin/Data/Managed')
        self.rt.set_config_dir('/apk/assets/bin/Data/Managed/etc')
        self.rt.set_temp_dir('/data/data/%s/cache' % DEFAULT_PKG)
        self.rt.set_commandline_arguments(['GameDevStory'])
        rc = self.rt.init()
        if rc != 1:
            raise SystemExit('il2cpp_init failed (%d)' % rc)
        # Boehm's stop-the-world relies on POSIX signal delivery to
        # suspend peer threads; green threads are already stopped whenever
        # another one runs, so we keep collection off until the scheduler
        # grows real safepoint stack scanning.
        self.rt.gc_disable()
        self.domain = self.rt.domain()
        self.thread = self.rt.thread_attach(self.domain)
        print('[sess] runtime up in %.0fs  domain=%#x thread=%#x'
              % (time.time() - t0, self.domain, self.thread))
        self.images = {}
        for a in self.rt.assemblies():
            img = self.rt.assembly_image(a)
            self.images[self.rt.image_name(img)] = img

    # ------------------------------------------------------------------
    def image(self, name):
        return self.images[name]

    def cls(self, image, ns, name):
        k = self.rt.class_from_name(self.images[image], ns, name)
        if not k:
            raise KeyError('%s: %s.%s' % (image, ns, name))
        return k

    def classes(self, image):
        img = self.images[image]
        n = self.rt.image_class_count(img)
        out = []
        for i in range(n):
            k = self.rt.image_class(img, i)
            out.append((k, self.rt.class_namespace(k), self.rt.class_name(k)))
        return out

    def method(self, klass, name, argc=-1):
        mm = self.rt.method_from_name(klass, name, argc)
        if not mm:
            raise KeyError('method %s' % name)
        return mm

    def invoke(self, method, obj=0, args=()):
        return self.rt.invoke(method, obj, args)

    def box_int(self, v):
        p = self.m.env.alloc(8)
        self.m.write64(p, v & 0xFFFFFFFFFFFFFFFF)
        return p

    def box_double(self, v):
        p = self.m.env.alloc(8)
        self.m.write(p, struct.pack('<d', v))
        return p

    def unbox_double(self, obj):
        return struct.unpack('<d', self.m.read(obj + 0x10, 8))[0]

    def unbox_int(self, obj):
        return struct.unpack('<i', self.m.read(obj + 0x10, 4))[0]

    def unbox_long(self, obj):
        return struct.unpack('<q', self.m.read(obj + 0x10, 8))[0]


def main():
    apk = sys.argv[1] if len(sys.argv) > 1 else 'out/apk'
    s = Session(apk)
    rt, m = s.rt, s.m

    print('\n[sess] loaded images: %d' % len(s.images))
    for n in ('Assembly-CSharp.dll', 'KairoLibrary.dll'):
        img = s.images.get(n)
        print('   %-26s classes=%d' % (n, rt.image_class_count(img)) if img
              else '   %-26s MISSING' % n)

    # ------------------------------------------------------- Kairosoft types
    print('\n[sess] Kairosoft classes in Assembly-CSharp.dll:')
    for k, ns, nm in s.classes('Assembly-CSharp.dll')[:400]:
        if ns and not ns.startswith('System'):
            print('   %-16s %s' % (ns, nm))

    # ---------------------------------------------- call real engine methods
    print('\n[sess] --- calling the shipped Kairosoft engine ---')

    jmath = s.cls('KairoLibrary.dll', 'java.lang', 'JMath')
    print('  java.lang.JMath = %#x' % jmath)
    rt.runtime_class_init(jmath)
    print('    methods: %s' % ', '.join(rt.method_name(x) for x in rt.methods(jmath)))
    for name, arg in (('Abs', -7.25), ('Sqrt', 144.0), ('Ceil', 2.1), ('Floor', 2.9)):
        try:
            mm = s.method(jmath, name, 1)
            r = s.invoke(mm, 0, [s.box_double(arg)])
            print('    JMath.%s(%s) = %s' % (name, arg, s.unbox_double(r)))
        except (KeyError, GuestError) as e:
            print('    JMath.%s -> %s' % (name, e))

    # JString: Kairosoft's transliterated java.lang.String helpers
    try:
        jstr = s.cls('KairoLibrary.dll', 'java.lang', 'JString')
        rt.runtime_class_init(jstr)
        print('  java.lang.JString = %#x, methods:' % jstr)
        for mm in rt.methods(jstr)[:24]:
            print('     %s' % rt.method_name(mm))
    except KeyError as e:
        print('  JString missing: %s' % e)

    # JRandom: the deterministic RNG the whole simulation depends on
    try:
        jr = s.cls('KairoLibrary.dll', 'java.util', 'JRandom')
        rt.runtime_class_init(jr)
        ctor = rt.method_from_name(jr, '.ctor', 1)
        obj = rt.call('il2cpp_object_new', jr)
        if ctor:
            rt.invoke(ctor, obj, [s.box_int(12345)])
        else:
            rt.invoke(rt.method_from_name(jr, '.ctor', 0), obj, [])
        nxt = rt.method_from_name(jr, 'NextInt', 1)
        vals = []
        for _ in range(8):
            r = rt.invoke(nxt, obj, [s.box_int(100)])
            vals.append(s.unbox_int(r))
        print('  java.util.JRandom(12345).nextInt(100) x8 = %r' % vals)
    except (KeyError, GuestError) as e:
        print('  JRandom -> %s' % e)

    print('\n[sess] guest instructions executed: %d' % m.insns)
    if m.missing:
        print('[sess] unimplemented imports: %r'
              % sorted(m.missing.items(), key=lambda kv: -kv[1])[:15])


if __name__ == '__main__':
    main()
