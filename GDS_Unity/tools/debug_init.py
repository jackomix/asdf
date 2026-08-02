"""Probe il2cpp_init: hook interesting guest addresses and dump IL2CPP state."""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from unicorn import UC_HOOK_CODE                                   # noqa: E402
from unicorn import arm64_const as A64                             # noqa: E402
from kairovm.boot import build, Il2CppRuntime, DEFAULT_PKG         # noqa: E402


def cls_info(m, k):
    if not k:
        return 'NULL'
    name = m.cstr(m.read64(k + 0x10), 200) or b'?'
    ns = m.cstr(m.read64(k + 0x18), 200) or b''
    return '%s%s%s  static_fields=%#x rgctx=%#x  flags@0x131=%#x 0x135=%#x' % (
        ns.decode('latin-1'), '.' if ns else '', name.decode('latin-1'),
        m.read64(k + 0xB8), m.read64(k + 0xC0),
        m.read8(k + 0x131), m.read8(k + 0x135))


def main():
    apk = sys.argv[1] if len(sys.argv) > 1 else 'out/apk'
    m, host, li = build(apk, verbose=1, trace=True)
    host.log_echo = True
    rt = Il2CppRuntime(m)
    m.run_init_array(li)

    B = li.bias
    hits = {}

    def probe(uc, address, size, ud):
        va = address - B
        n = hits.get(va, 0) + 1
        hits[va] = n
        if va == 0x13a50f4:
            method = uc.reg_read(A64.UC_ARM64_REG_X0)
            name = m.cstr(m.read64(method + 0x18), 200)
            klass = m.read64(method + 0x20)
            print('>>> generic static @%#x  method=%s' %
                  (va, (name or b'?').decode('latin-1')))
            print('    MethodInfo %#x  klass %#x' % (method, klass))
            print('    klass: %s' % cls_info(m, klass))
            gc = m.read64(klass + 0x60)      # generic_class
            print('    generic_class=%#x  typeDefinition=%#x' % (gc, m.read64(klass + 0x68)))
            if gc:
                t = m.read64(gc)             # Il2CppGenericClass.type
                print('    gc.type=%#x  cached_class=%#x' % (t, m.read64(gc + 0x18)))
        elif va == 0xd0dbd0 and n <= 8:
            k = uc.reg_read(A64.UC_ARM64_REG_X0)
            print('    [Class::Init?] %s' % cls_info(m, k))

    m.uc.hook_add(UC_HOOK_CODE, probe, begin=B + 0x13a50f4, end=B + 0x13a50f7)
    m.uc.hook_add(UC_HOOK_CODE, probe, begin=B + 0xd0dbd0, end=B + 0xd0dbd3)

    rt.set_data_dir('/apk/assets/bin/Data/Managed')
    rt.set_config_dir('/apk/assets/bin/Data/Managed/etc')
    rt.set_temp_dir('/data/data/%s/cache' % DEFAULT_PKG)
    rt.set_commandline_arguments(['GameDevStory'])
    print('[dbg] il2cpp_init ...')
    t0 = time.time()
    try:
        rc = rt.init()
        print('[dbg] il2cpp_init -> %d (%.0fs)' % (rc, time.time() - t0))
    except Exception as e:
        print('[dbg] FAILED after %.0fs: %s' % (time.time() - t0, e))
    print('[dbg] hits: %r' % {hex(k): v for k, v in hits.items()})


if __name__ == '__main__':
    main()
