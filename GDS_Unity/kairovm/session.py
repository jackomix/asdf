"""Bring the shipped IL2CPP runtime up and expose reflection over it."""
import os
import struct
import sys
import time

from .boot import build, Il2CppRuntime, DEFAULT_PKG
from .machine import GuestError


class Session(object):
    """A live IL2CPP runtime: the real game assemblies, loaded and callable."""

    def __init__(self, apk='out/apk', verbose=1, echo_log=False, symbols=True):
        t0 = time.time()
        self.apk = apk
        self.pkg = DEFAULT_PKG
        self.rootfs = os.path.join(apk, '_rootfs')
        self.quit = False
        self._type_image = None
        self.m, self.host, self.li = build(apk, verbose=verbose, echo_log=echo_log)
        self.m.watch = False
        self.rt = Il2CppRuntime(self.m)
        self.meta = None
        if symbols:
            self._load_symbols()
        self.m.run_init_array(self.li)
        self.rt.set_data_dir('/apk/assets/bin/Data/Managed')
        self.rt.set_config_dir('/apk/assets/bin/Data/Managed/etc')
        self.rt.set_temp_dir('/data/data/%s/cache' % self.pkg)
        self.rt.set_commandline_arguments(['GameDevStory'])
        rc = self.rt.init()
        if rc != 1:
            raise SystemExit('il2cpp_init failed (%d)' % rc)
        # Boehm's stop-the-world needs POSIX signals; green threads are already
        # stopped whenever another one runs, so collection stays off.
        self.rt.gc_disable()
        self.domain = self.rt.domain()
        self.thread = self.rt.thread_attach(self.domain)
        self.images = {}
        for a in self.rt.assemblies():
            img = self.rt.assembly_image(a)
            self.images[self.rt.image_name(img)] = img
        print('[sess] runtime up in %.0fs  %d assemblies'
              % (time.time() - t0, len(self.images)))

    def _load_symbols(self):
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, os.path.join(here, '..', 'tools'))
        try:
            import il2cpp_meta as MD
            from symbols import SymbolTable
            self.meta = MD.load(os.path.join(
                self.apk, 'assets/bin/Data/Managed/Metadata/global-metadata.dat'))
            names = tuple(self.meta.string(self.meta.image_def(i).nameIndex)
                          for i in range(self.meta.count('images')))
            self.m.symbols = SymbolTable(self.apk, names)
        except Exception as e:                      # symbols are optional
            print('[sess] symbols unavailable: %r' % e)

    # ------------------------------------------------------------ reflection
    def cls(self, image, ns, name):
        k = self.rt.class_from_name(self.images[image], ns, name)
        if not k:
            raise KeyError('%s: %s.%s' % (image, ns, name))
        return k

    def type_image(self, full_name):
        """Assembly that declares `full_name`, straight from the metadata.

        Asking il2cpp_class_from_name for a type image by image is not safe:
        a few of the shipped assemblies fault inside the runtime's name
        comparison, and one bad guess takes the whole VM down.  The metadata
        already says where every type lives, so use it.
        """
        if self._type_image is None:
            self._type_image = {}
            m = self.meta
            if m is not None:
                for i in range(m.count('images')):
                    im = m.image_def(i)
                    nm = m.string(im.nameIndex)
                    for ti in range(im.typeStart, im.typeStart + im.typeCount):
                        self._type_image[m.type_name(ti)] = nm
        return self._type_image.get(full_name)

    def find_class(self, ns, name):
        full = '%s.%s' % (ns, name) if ns else name
        img_name = self.type_image(full)
        if img_name and img_name in self.images:
            k = self.rt.class_from_name(self.images[img_name], ns, name)
            if k:
                return k
        raise KeyError(full)

    def method(self, klass, name, argc=-1):
        mm = self.rt.method_from_name(klass, name, argc)
        if not mm:
            raise KeyError('method %s' % name)
        return mm

    def try_method(self, klass, name, argc=-1):
        return self.rt.method_from_name(klass, name, argc)

    def invoke(self, method, obj=0, args=()):
        return self.rt.invoke(method, obj, args)

    def call_method(self, klass, name, obj=0, args=(), argc=-1):
        return self.rt.invoke(self.method(klass, name, argc), obj, args)

    def field_offset(self, klass, name):
        f = self.rt.call('il2cpp_class_get_field_from_name', klass,
                         self.m.put_cstr(name))
        return self.rt.field_offset(f) if f else None

    def field_ptr(self, obj, klass, name):
        off = self.field_offset(klass, name)
        return None if off is None else obj + off

    # --------------------------------------------------------------- boxing
    def box_int(self, v):
        p = self.m.env.alloc(8)
        self.m.write64(p, v & 0xFFFFFFFFFFFFFFFF)
        return p

    def box_float(self, v):
        p = self.m.env.alloc(8)
        self.m.write(p, struct.pack('<f', v) + b'\0' * 4)
        return p

    def box_double(self, v):
        p = self.m.env.alloc(8)
        self.m.write(p, struct.pack('<d', v))
        return p

    def unbox_int(self, obj):
        return struct.unpack('<i', self.m.read(obj + 0x10, 4))[0]

    def unbox_double(self, obj):
        return struct.unpack('<d', self.m.read(obj + 0x10, 8))[0]

    def string(self, p):
        return self.rt.string(p)
