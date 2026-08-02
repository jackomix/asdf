"""Bionic (Android libc/libm/libdl/liblog) shim for KairoVM.

Every symbol libil2cpp.so imports is implemented here.  This file *is*
the portability contract: on a real ARM64 Linux handheld (R36S / ArkOS /
dArkOSre) the same 264 symbols are satisfied by glibc plus a handful of
tiny compatibility shims, which is what ../native/kairo_native.c does.
Nothing game-specific lives here.
"""
import ctypes
import errno as _errno
import math
import os
import struct
import sys
import time

from unicorn import arm64_const as A64
from unicorn import UC_PROT_ALL

from . import memory as M
from .machine import asm, ARG_REGS, THUNK_BASE, THUNK_SIZE

S64 = lambda v: v - (1 << 64) if v >> 63 else v
S32 = lambda v: (v & 0xFFFFFFFF) - (1 << 32) if (v >> 31) & 1 else (v & 0xFFFFFFFF)
U64 = lambda v: v & 0xFFFFFFFFFFFFFFFF

O_ACCMODE = 0o3
O_RDONLY, O_WRONLY, O_RDWR = 0, 1, 2
O_CREAT = 0o100
O_EXCL = 0o200
O_TRUNC = 0o1000
O_APPEND = 0o2000


class VFile(object):
    def __init__(self, fd, fh, path):
        self.fd = fd
        self.fh = fh
        self.path = path


class Bionic(object):
    """Host implementation of the Android C runtime for the guest."""

    def __init__(self, machine, rootfs, cwd='/data/data/net.kairosoft.android.gamedev3en'):
        self.m = machine
        machine.host = self
        self.rootfs = os.path.abspath(rootfs)
        self.cwd = cwd
        self.files = {}
        self.next_fd = 16
        self.streams = {}                  # FILE* -> VFile
        self.dirs = {}
        self.tls_keys = {}
        self.next_tls_key = 1
        self.tls_dtors = {}
        self.mutexes = {}
        self.conds = {}
        self.sems = {}
        self.atexit = []
        self.locale = 0
        self.log = []
        self.log_echo = True
        self.futex_waits = 0
        self.sem_autoacks = 0
        self.attr_stacks = {}
        self.start_time = time.time()
        self.mono0 = time.monotonic()
        self._env = {}
        self._envp = 0
        self.thunks = {}
        self._thunk_cursor = THUNK_BASE
        self._setup_stdio()
        self._build_thunks()
        self.register_all()

    # --------------------------------------------------------------- paths
    def host_path(self, guest_path):
        if guest_path is None:
            return None
        p = guest_path
        if not p.startswith('/'):
            p = os.path.join(self.cwd, p)
        p = os.path.normpath(p)
        return os.path.join(self.rootfs, p.lstrip('/'))

    # --------------------------------------------------------------- stdio
    def _setup_stdio(self):
        m = self.m
        # bionic exports __sF as an array of 3 FILE structs (size 152 on LP64)
        self.FILE_SZ = 152
        self.sF = m.env.alloc(self.FILE_SZ * 3, align=16)
        m.write(self.sF, b'\0' * (self.FILE_SZ * 3))
        for i in range(3):
            fp = self.sF + i * self.FILE_SZ
            m.write32(fp + 112, i)         # _file offset in bionic FILE
            self.streams[fp] = VFile(i, None, ['<stdin>', '<stdout>', '<stderr>'][i])

    # ------------------------------------------------------- guest thunks
    def _thunk(self, name, source):
        addr = self._thunk_cursor
        code = asm(source, addr)
        self.m.write(addr, code)
        self._thunk_cursor = (addr + len(code) + 15) & ~15
        self.thunks[name] = addr
        return addr

    def _build_thunks(self):
        """Helpers that must call back into guest code.

        Implementing these as real ARM64 machine code avoids re-entering the
        emulator from inside a hook, which Unicorn does not support.
        """
        # pthread_once(once_control*, void(*init)(void))
        self._thunk('pthread_once', """
            ldr   w2, [x0]
            cbnz  w2, po_done
            mov   w2, #1
            str   w2, [x0]
            stp   x29, x30, [sp, #-16]!
            blr   x1
            ldp   x29, x30, [sp], #16
        po_done:
            mov   x0, #0
            ret
        """)

        # qsort(base, nmemb, size, cmp) - shell sort (no scratch buffer needed)
        self._thunk('qsort', """
            stp   x29, x30, [sp, #-96]!
            stp   x19, x20, [sp, #16]
            stp   x21, x22, [sp, #32]
            stp   x23, x24, [sp, #48]
            stp   x25, x26, [sp, #64]
            stp   x27, x28, [sp, #80]
            mov   x19, x0
            mov   x20, x1
            mov   x21, x2
            mov   x22, x3
            cmp   x20, #2
            b.lo  qs_out
            mov   x23, #1
        qs_gap:
            lsl   x26, x23, #1
            add   x26, x26, #1
            cmp   x26, x20
            b.hs  qs_pass
            mov   x23, x26
            b     qs_gap
        qs_pass:
            mov   x24, x23
        qs_i:
            cmp   x24, x20
            b.hs  qs_shrink
            mov   x25, x24
        qs_j:
            cmp   x25, x23
            b.lo  qs_next
            sub   x26, x25, x23
            mul   x27, x26, x21
            add   x27, x19, x27
            mul   x28, x25, x21
            add   x28, x19, x28
            mov   x0, x27
            mov   x1, x28
            blr   x22
            cmp   w0, #0
            b.le  qs_next
            mov   x26, #0
        qs_swap:
            ldrb  w0, [x27, x26]
            ldrb  w1, [x28, x26]
            strb  w1, [x27, x26]
            strb  w0, [x28, x26]
            add   x26, x26, #1
            cmp   x26, x21
            b.lo  qs_swap
            sub   x25, x25, x23
            b     qs_j
        qs_next:
            add   x24, x24, #1
            b     qs_i
        qs_shrink:
            lsr   x23, x23, #1
            cbz   x23, qs_out
            b     qs_pass
        qs_out:
            ldp   x27, x28, [sp, #80]
            ldp   x25, x26, [sp, #64]
            ldp   x23, x24, [sp, #48]
            ldp   x21, x22, [sp, #32]
            ldp   x19, x20, [sp, #16]
            ldp   x29, x30, [sp], #96
            ret
        """)

        # bsearch(key, base, nmemb, size, cmp)
        self._thunk('bsearch', """
            stp   x29, x30, [sp, #-96]!
            stp   x19, x20, [sp, #16]
            stp   x21, x22, [sp, #32]
            stp   x23, x24, [sp, #48]
            stp   x25, x26, [sp, #64]
            mov   x19, x0
            mov   x20, x1
            mov   x21, x2
            mov   x22, x3
            mov   x23, x4
            mov   x24, #0
        bs_loop:
            cmp   x24, x21
            b.hs  bs_miss
            add   x25, x24, x21
            lsr   x25, x25, #1
            mul   x26, x25, x22
            add   x26, x20, x26
            mov   x0, x19
            mov   x1, x26
            blr   x23
            cmp   w0, #0
            b.eq  bs_hit
            b.lt  bs_lo
            add   x24, x25, #1
            b     bs_loop
        bs_lo:
            mov   x21, x25
            b     bs_loop
        bs_hit:
            mov   x0, x26
            b     bs_out
        bs_miss:
            mov   x0, #0
        bs_out:
            ldp   x25, x26, [sp, #64]
            ldp   x23, x24, [sp, #48]
            ldp   x21, x22, [sp, #32]
            ldp   x19, x20, [sp, #16]
            ldp   x29, x30, [sp], #96
            ret
        """)

    # =====================================================================
    #  registration
    # =====================================================================
    def register_all(self):
        b = self.m.bind
        g = globals()
        for name in dir(self):
            if not name.startswith('c_'):
                continue
            sym = name[2:]
            b(sym, getattr(self, name))
        for sym, thunk in self.thunks.items():
            self._alias(sym, thunk)
        self._register_math()
        self._register_data_symbols()

    def _alias(self, sym, target):
        """Point an imported symbol directly at guest code (no host hook)."""
        stub = self.m.import_stub(sym)
        # b <target>
        assert abs(target - stub) < (1 << 27), 'thunk out of branch range'
        self.m.write(stub, asm('b #%d' % target, stub))
        self.m.stub_impl.pop(stub, None)
        self.m.passthrough.add(stub)

    def _register_data_symbols(self):
        m = self.m
        # __sF is a data symbol; patch the GOT entries that referenced its stub.
        for name, value in (('__sF', self.sF),
                            ('environ', self._environ_ptr()),
                            ('_ctype_', self._ctype_table())):
            stub = m.stubs.get(name)
            if stub is None:
                continue
            self._patch_data_import(name, value)

    def _patch_data_import(self, name, value):
        """Rewrite every GOT slot bound to `name`'s stub to hold `value`."""
        m = self.m
        stub = m.stubs.get(name)
        if stub is None:
            return
        for li in m.images:
            img = li.img
            for r_offset, r_type, r_sym, r_addend in img.relocations():
                if r_type not in (1025, 257, 1026):
                    continue
                sym = img.symbols[r_sym] if r_sym < len(img.symbols) else None
                if sym is None or sym.name != name or sym.defined:
                    continue
                m.write64(li.bias + r_offset, value + r_addend)

    def _environ_ptr(self):
        m = self.m
        arr = m.env.alloc(16)
        m.write64(arr, 0)
        m.write64(arr + 8, 0)
        p = m.env.alloc(8)
        m.write64(p, arr)
        self._envp = arr
        return arr

    def _ctype_table(self):
        m = self.m
        t = m.env.alloc(1024)
        m.write(t, b'\0' * 1024)
        return t + 128

    # --------------------------------------------------------------- math
    def _register_math(self):
        m = self.m
        F1 = {
            'sin': math.sin, 'cos': math.cos, 'tan': math.tan,
            'asin': math.asin, 'acos': math.acos, 'atan': math.atan,
            'exp': math.exp, 'log': math.log, 'log2': math.log2,
            'log10': math.log10, 'sqrt': math.sqrt, 'cbrt': lambda x: math.copysign(abs(x) ** (1 / 3.), x),
            'sinh': math.sinh, 'cosh': math.cosh, 'tanh': math.tanh,
            'ceil': math.ceil, 'floor': math.floor, 'logb': lambda x: float(math.frexp(x)[1] - 1) if x else float('-inf'),
            'expm1': math.expm1, 'log1p': math.log1p, 'exp2': lambda x: 2.0 ** x,
        }
        F2 = {'pow': math.pow, 'atan2': math.atan2, 'fmod': math.fmod,
              'hypot': math.hypot, 'copysign': math.copysign,
              'nextafter': lambda a, b: math.nextafter(a, b)}

        def mk1(fn, single=False):
            def impl(*args):
                x = self._d(0)
                if single:
                    x = self._f(0)
                try:
                    r = fn(x)
                except (ValueError, OverflowError):
                    r = float('nan')
                return self._retf(r, single)
            return impl

        def mk2(fn, single=False):
            def impl(*args):
                a = self._f(0) if single else self._d(0)
                bb = self._f(1) if single else self._d(1)
                try:
                    r = fn(a, bb)
                except (ValueError, OverflowError, ZeroDivisionError):
                    r = float('nan')
                return self._retf(r, single)
            return impl

        for n, fn in F1.items():
            m.bind(n, mk1(fn))
            m.bind(n + 'f', mk1(fn, True))
        for n, fn in F2.items():
            m.bind(n, mk2(fn))
            m.bind(n + 'f', mk2(fn, True))

        def c_modf(*a):
            x = self._d(0)
            frac, ip = math.modf(x)
            self.m.write(a[0], struct.pack('<d', ip))
            return self._retf(frac)
        # modf's pointer arg is x0
        m.bind('modf', lambda *a: (self.m.write(a[0], struct.pack('<d', math.modf(self._d(0))[1])),
                                   self._retf(math.modf(self._d(0))[0]))[1])
        m.bind('modff', lambda *a: (self.m.write(a[0], struct.pack('<f', math.modf(self._f(0))[1])),
                                    self._retf(math.modf(self._f(0))[0], True))[1])
        m.bind('scalbn', lambda *a: self._retf(math.ldexp(self._d(0), S32(a[0]))))
        m.bind('ldexp', lambda *a: self._retf(math.ldexp(self._d(0), S32(a[0]))))
        m.bind('ldexpf', lambda *a: self._retf(math.ldexp(self._f(0), S32(a[0])), True))
        m.bind('difftime', lambda *a: self._retf(float(S64(a[0]) - S64(a[1]))))

        def c_sincosf(*a):
            x = self._f(0)
            self.m.write(a[0], struct.pack('<f', math.sin(x)))
            self.m.write(a[1], struct.pack('<f', math.cos(x)))
            return 0
        m.bind('sincosf', c_sincosf)

        def c_sincos(*a):
            x = self._d(0)
            self.m.write(a[0], struct.pack('<d', math.sin(x)))
            self.m.write(a[1], struct.pack('<d', math.cos(x)))
            return 0
        m.bind('sincos', c_sincos)

        def c_div(*a):
            n, d = S32(a[0]), S32(a[1])
            q = int(n / d) if d else 0
            return (q & 0xFFFFFFFF) | ((n - q * d if d else 0) & 0xFFFFFFFF) << 32
        m.bind('div', c_div)
        m.bind('lldiv', lambda *a: (int(S64(a[0]) / S64(a[1])) if a[1] else 0,
                                    S64(a[0]) - int(S64(a[0]) / S64(a[1])) * S64(a[1]) if a[1] else 0))

    DREGS = [A64.UC_ARM64_REG_D0, A64.UC_ARM64_REG_D1, A64.UC_ARM64_REG_D2,
             A64.UC_ARM64_REG_D3, A64.UC_ARM64_REG_D4, A64.UC_ARM64_REG_D5,
             A64.UC_ARM64_REG_D6, A64.UC_ARM64_REG_D7]
    SREGS = [A64.UC_ARM64_REG_S0, A64.UC_ARM64_REG_S1, A64.UC_ARM64_REG_S2,
             A64.UC_ARM64_REG_S3, A64.UC_ARM64_REG_S4, A64.UC_ARM64_REG_S5,
             A64.UC_ARM64_REG_S6, A64.UC_ARM64_REG_S7]

    def _d(self, i):
        raw = self.m.uc.reg_read(self.DREGS[i])
        if isinstance(raw, float):
            return raw
        return struct.unpack('<d', struct.pack('<Q', raw & 0xFFFFFFFFFFFFFFFF))[0]

    def _f(self, i):
        raw = self.m.uc.reg_read(self.SREGS[i])
        if isinstance(raw, float):
            return raw
        return struct.unpack('<f', struct.pack('<I', raw & 0xFFFFFFFF))[0]

    def _retf(self, v, single=False):
        if single:
            self.m.uc.reg_write(A64.UC_ARM64_REG_S0,
                                struct.unpack('<I', struct.pack('<f', v))[0])
        else:
            self.m.uc.reg_write(A64.UC_ARM64_REG_D0,
                                struct.unpack('<Q', struct.pack('<d', v))[0])
        return 0

    # =====================================================================
    #  memory
    # =====================================================================
    def c_malloc(self, size, *a):
        return self.m.heap.alloc(size)

    def c_calloc(self, n, size, *a):
        total = n * size
        p = self.m.heap.alloc(total if total else 1)
        self.m.write(p, b'\0' * max(total, 1))
        return p

    def c_realloc(self, ptr, size, *a):
        if ptr == 0:
            return self.m.heap.alloc(size)
        if size == 0:
            self.m.heap.free_ptr(ptr)
            return 0
        old = self.m.heap.usable(ptr)
        if old >= size:
            return ptr
        new = self.m.heap.alloc(size)
        if old:
            self.m.write(new, self.m.read(ptr, old))
        self.m.heap.free_ptr(ptr)
        return new

    def c_free(self, ptr, *a):
        self.m.heap.free_ptr(ptr)
        return 0

    def c_memalign(self, align, size, *a):
        return self.m.heap.alloc(size, max(align, 16))

    def c_posix_memalign(self, out, align, size, *a):
        p = self.m.heap.alloc(size, max(align, 16))
        self.m.write64(out, p)
        return 0

    def c_malloc_usable_size(self, ptr, *a):
        return self.m.heap.usable(ptr)

    # ---------------------------------------------------------- mem/str ops
    def c_memcpy(self, d, s, n, *a):
        if n:
            self.m.write(d, self.m.read(s, n))
        return d

    def c_memmove(self, d, s, n, *a):
        if n:
            self.m.write(d, self.m.read(s, n))
        return d

    def c___memmove_chk(self, d, s, n, *a):
        return self.c_memmove(d, s, n)

    def c___memcpy_chk(self, d, s, n, *a):
        return self.c_memcpy(d, s, n)

    def c_memset(self, d, c, n, *a):
        if n:
            self.m.write(d, bytes([c & 0xFF]) * n)
        return d

    def c___memset_chk(self, d, c, n, *a):
        return self.c_memset(d, c, n)

    def c_memcmp(self, p, q, n, *a):
        if not n:
            return 0
        x, y = self.m.read(p, n), self.m.read(q, n)
        return 0 if x == y else (1 if x > y else 0xFFFFFFFFFFFFFFFF)

    def c_memchr(self, p, c, n, *a):
        if not n:
            return 0
        buf = self.m.read(p, n)
        i = buf.find(bytes([c & 0xFF]))
        return p + i if i >= 0 else 0

    def c_memrchr(self, p, c, n, *a):
        buf = self.m.read(p, n)
        i = buf.rfind(bytes([c & 0xFF]))
        return p + i if i >= 0 else 0

    def c_strlen(self, s, *a):
        return len(self.m.cstr(s) or b'')

    def c___strlen_chk(self, s, n, *a):
        return self.c_strlen(s)

    def c_strnlen(self, s, n, *a):
        buf = self.m.read(s, n)
        i = buf.find(b'\0')
        return n if i < 0 else i

    def c_strcpy(self, d, s, *a):
        v = self.m.cstr(s) or b''
        self.m.write(d, v + b'\0')
        return d

    def c_stpcpy(self, d, s, *a):
        v = self.m.cstr(s) or b''
        self.m.write(d, v + b'\0')
        return d + len(v)

    def c_strncpy(self, d, s, n, *a):
        v = (self.m.cstr(s) or b'')[:n]
        self.m.write(d, v + b'\0' * (n - len(v)))
        return d

    def c_strlcpy(self, d, s, n, *a):
        v = self.m.cstr(s) or b''
        if n:
            self.m.write(d, v[:n - 1] + b'\0')
        return len(v)

    def c_strcat(self, d, s, *a):
        cur = self.m.cstr(d) or b''
        v = self.m.cstr(s) or b''
        self.m.write(d + len(cur), v + b'\0')
        return d

    def c_strcmp(self, p, q, *a):
        x = self.m.cstr(p) or b''
        y = self.m.cstr(q) or b''
        return 0 if x == y else (1 if x > y else U64(-1))

    def c_strncmp(self, p, q, n, *a):
        x = (self.m.cstr(p) or b'')[:n]
        y = (self.m.cstr(q) or b'')[:n]
        return 0 if x == y else (1 if x > y else U64(-1))

    def c_strcasecmp(self, p, q, *a):
        x = (self.m.cstr(p) or b'').lower()
        y = (self.m.cstr(q) or b'').lower()
        return 0 if x == y else (1 if x > y else U64(-1))

    def c_strncasecmp(self, p, q, n, *a):
        x = (self.m.cstr(p) or b'')[:n].lower()
        y = (self.m.cstr(q) or b'')[:n].lower()
        return 0 if x == y else (1 if x > y else U64(-1))

    def c_strchr(self, s, c, *a):
        v = self.m.cstr(s) or b''
        ch = c & 0xFF
        if ch == 0:
            return s + len(v)
        i = v.find(bytes([ch]))
        return s + i if i >= 0 else 0

    def c_strrchr(self, s, c, *a):
        v = self.m.cstr(s) or b''
        ch = c & 0xFF
        if ch == 0:
            return s + len(v)
        i = v.rfind(bytes([ch]))
        return s + i if i >= 0 else 0

    def c_strstr(self, h, n, *a):
        x = self.m.cstr(h) or b''
        y = self.m.cstr(n) or b''
        i = x.find(y)
        return h + i if i >= 0 else 0

    def c_strdup(self, s, *a):
        v = self.m.cstr(s) or b''
        p = self.m.heap.alloc(len(v) + 1)
        self.m.write(p, v + b'\0')
        return p

    def c_strndup(self, s, n, *a):
        v = (self.m.cstr(s) or b'')[:n]
        p = self.m.heap.alloc(len(v) + 1)
        self.m.write(p, v + b'\0')
        return p

    def c_strspn(self, s, acc, *a):
        v = self.m.cstr(s) or b''
        st = set(self.m.cstr(acc) or b'')
        i = 0
        while i < len(v) and v[i] in st:
            i += 1
        return i

    def c_strcspn(self, s, rej, *a):
        v = self.m.cstr(s) or b''
        st = set(self.m.cstr(rej) or b'')
        i = 0
        while i < len(v) and v[i] not in st:
            i += 1
        return i

    def c_strpbrk(self, s, acc, *a):
        v = self.m.cstr(s) or b''
        st = set(self.m.cstr(acc) or b'')
        for i, ch in enumerate(v):
            if ch in st:
                return s + i
        return 0

    def c_strtok_r(self, s, delim, save, *a):
        d = set(self.m.cstr(delim) or b'')
        cur = s if s else self.m.read64(save)
        if not cur:
            return 0
        v = self.m.cstr(cur) or b''
        i = 0
        while i < len(v) and v[i] in d:
            i += 1
        if i >= len(v):
            self.m.write64(save, 0)
            return 0
        start = cur + i
        j = i
        while j < len(v) and v[j] not in d:
            j += 1
        if j < len(v):
            self.m.write8(cur + j, 0)
            self.m.write64(save, cur + j + 1)
        else:
            self.m.write64(save, 0)
        return start

    def c_strcoll_l(self, p, q, *a):
        return self.c_strcmp(p, q)

    def c_strxfrm_l(self, d, s, n, *a):
        v = self.m.cstr(s) or b''
        if n:
            self.m.write(d, v[:n - 1] + b'\0')
        return len(v)

    def c_strerror(self, e, *a):
        return self.m.put_cstr(os.strerror(e) if e else 'Success')

    def c_strerror_r(self, e, buf, n, *a):
        s = (os.strerror(e) if e else 'Success').encode()[:n - 1]
        self.m.write(buf, s + b'\0')
        return 0

    # wide char (used by mscorlib's globalization paths)
    def c_wcslen(self, s, *a):
        n = 0
        while self.m.read32(s + n * 4) != 0:
            n += 1
        return n

    def c_wmemcpy(self, d, s, n, *a):
        if n:
            self.m.write(d, self.m.read(s, n * 4))
        return d

    def c_wmemmove(self, d, s, n, *a):
        return self.c_wmemcpy(d, s, n)

    def c_wmemset(self, d, c, n, *a):
        self.m.write(d, struct.pack('<I', c & 0xFFFFFFFF) * n)
        return d

    def c_wmemcmp(self, p, q, n, *a):
        x, y = self.m.read(p, n * 4), self.m.read(q, n * 4)
        return 0 if x == y else (1 if x > y else U64(-1))

    def c_wmemchr(self, p, c, n, *a):
        pat = struct.pack('<I', c & 0xFFFFFFFF)
        buf = self.m.read(p, n * 4)
        for i in range(n):
            if buf[i * 4:i * 4 + 4] == pat:
                return p + i * 4
        return 0

    def c_wcscoll_l(self, p, q, *a):
        return 0

    def c_wcsxfrm_l(self, d, s, n, *a):
        return 0

    # =====================================================================
    #  ctype / locale
    # =====================================================================
    def c___ctype_get_mb_cur_max(self, *a):
        return 4

    def c_newlocale(self, mask, name, base, *a):
        return 1

    def c_freelocale(self, l, *a):
        return 0

    def c_uselocale(self, l, *a):
        prev = self.locale
        if l:
            self.locale = l
        return prev

    def c_setlocale(self, cat, name, *a):
        return self.m.put_cstr('C')

    def c_localeconv(self, *a):
        m = self.m
        if not hasattr(self, '_lconv'):
            self._lconv = m.env.alloc(0x100)
            m.write(self._lconv, b'\0' * 0x100)
            dot = m.put_cstr('.')
            empty = m.put_cstr('')
            m.write64(self._lconv + 0, dot)         # decimal_point
            m.write64(self._lconv + 8, empty)       # thousands_sep
            for i in range(2, 16):
                m.write64(self._lconv + i * 8, empty)
        return self._lconv

    def _ct(self, fn):
        return lambda *a: 1 if fn(chr(a[0] & 0xFF)) else 0

    def c_isdigit_l(self, c, *a):
        return 1 if 48 <= (c & 0xFF) <= 57 else 0

    def c_isxdigit_l(self, c, *a):
        return 1 if chr(c & 0xFF) in '0123456789abcdefABCDEF' else 0

    def c_islower_l(self, c, *a):
        return 1 if 97 <= (c & 0xFF) <= 122 else 0

    def c_isupper_l(self, c, *a):
        return 1 if 65 <= (c & 0xFF) <= 90 else 0

    def c_tolower_l(self, c, *a):
        return ord(chr(c & 0xFFFFFFFF).lower()) if c < 0x110000 else c

    def c_toupper_l(self, c, *a):
        return ord(chr(c & 0xFFFFFFFF).upper()) if c < 0x110000 else c

    def c_towlower(self, c, *a):
        return ord(chr(c).lower()) if c < 0x110000 else c

    def c_towupper(self, c, *a):
        return ord(chr(c).upper()) if c < 0x110000 else c

    def c_towlower_l(self, c, *a):
        return self.c_towlower(c)

    def c_towupper_l(self, c, *a):
        return self.c_towupper(c)

    def _wct(self, c, pred):
        try:
            ch = chr(c)
        except ValueError:
            return 0
        return 1 if pred(ch) else 0

    def c_iswalpha_l(self, c, *a):
        return self._wct(c, str.isalpha)

    def c_iswdigit_l(self, c, *a):
        return self._wct(c, lambda x: x.isdigit() and x.isascii())

    def c_iswlower_l(self, c, *a):
        return self._wct(c, str.islower)

    def c_iswupper_l(self, c, *a):
        return self._wct(c, str.isupper)

    def c_iswspace_l(self, c, *a):
        return self._wct(c, str.isspace)

    def c_iswprint_l(self, c, *a):
        return self._wct(c, str.isprintable)

    def c_iswcntrl_l(self, c, *a):
        return 1 if c < 32 or c == 127 else 0

    def c_iswblank_l(self, c, *a):
        return 1 if c in (32, 9) else 0

    def c_iswpunct_l(self, c, *a):
        return self._wct(c, lambda x: (not x.isalnum()) and x.isprintable() and not x.isspace())

    def c_iswxdigit_l(self, c, *a):
        return 1 if c < 128 and chr(c) in '0123456789abcdefABCDEF' else 0

    # multibyte
    def c_mbtowc(self, pwc, s, n, *a):
        if s == 0:
            return 0
        buf = self.m.read(s, min(n, 8))
        if not buf or buf[0] == 0:
            if pwc:
                self.m.write32(pwc, 0)
            return 0
        for ln in range(1, min(n, 5) + 1):
            try:
                ch = buf[:ln].decode('utf-8')
            except UnicodeDecodeError:
                continue
            if pwc:
                self.m.write32(pwc, ord(ch))
            return ln
        return U64(-1)

    def c_mbrtowc(self, pwc, s, n, st, *a):
        return self.c_mbtowc(pwc, s, n)

    def c_mbrlen(self, s, n, st, *a):
        return self.c_mbtowc(0, s, n)

    def c_btowc(self, c, *a):
        return c if c < 128 else U64(-1)

    def c_wctob(self, c, *a):
        return c if c < 128 else U64(-1)

    def c_wcrtomb(self, s, wc, st, *a):
        if s == 0:
            return 1
        try:
            enc = chr(wc).encode('utf-8')
        except ValueError:
            return U64(-1)
        self.m.write(s, enc)
        return len(enc)

    def c_mbsrtowcs(self, dst, src, n, st, *a):
        return self._mbs(dst, src, 1 << 30, n)

    def c_mbsnrtowcs(self, dst, src, nms, n, st, *a):
        return self._mbs(dst, src, nms, n)

    def _mbs(self, dst, srcp, nms, n):
        sp = self.m.read64(srcp) if srcp else 0
        if not sp:
            return 0
        raw = (self.m.cstr(sp) or b'')[:nms]
        try:
            text = raw.decode('utf-8', 'replace')
        except Exception:
            return U64(-1)
        if dst:
            out = text[:n]
            self.m.write(dst, b''.join(struct.pack('<I', ord(c)) for c in out))
            if len(out) < n:
                self.m.write32(dst + len(out) * 4, 0)
            self.m.write64(srcp, 0)
            return len(out)
        return len(text)

    def c_wcsnrtombs(self, dst, srcp, nwc, n, st, *a):
        sp = self.m.read64(srcp) if srcp else 0
        if not sp:
            return 0
        chars = []
        i = 0
        while i < nwc:
            v = self.m.read32(sp + i * 4)
            if v == 0:
                break
            chars.append(chr(v))
            i += 1
        enc = ''.join(chars).encode('utf-8')
        if dst:
            enc = enc[:n]
            self.m.write(dst, enc + b'\0')
            self.m.write64(srcp, 0)
        return len(enc)

    # =====================================================================
    #  conversion / printf
    # =====================================================================
    def _strto(self, s, endp, base, conv, signed=True):
        raw = self.m.cstr(s) or b''
        txt = raw.decode('latin-1')
        i = 0
        while i < len(txt) and txt[i] in ' \t\n\r\f\v':
            i += 1
        start = i
        if i < len(txt) and txt[i] in '+-':
            i += 1
        if base in (0, 16) and txt[i:i + 2].lower() == '0x':
            i += 2
            base = 16
        elif base == 0:
            base = 8 if (i < len(txt) and txt[i] == '0') else 10
        digits = '0123456789abcdefghijklmnopqrstuvwxyz'[:base]
        j = i
        while j < len(txt) and txt[j].lower() in digits:
            j += 1
        body = txt[start:j]
        try:
            val = int(body, base)
        except ValueError:
            val = 0
            j = start
        if endp:
            self.m.write64(endp, s + j)
        return val

    def c_atoi(self, s, *a):
        return U64(self._strto(s, 0, 10, int) & 0xFFFFFFFF)

    def c_atol(self, s, *a):
        return U64(self._strto(s, 0, 10, int))

    def c_atoll(self, s, *a):
        return U64(self._strto(s, 0, 10, int))

    def c_strtol(self, s, endp, base, *a):
        return U64(self._strto(s, endp, base, int))

    def c_strtoll(self, s, endp, base, *a):
        return U64(self._strto(s, endp, base, int))

    def c_strtoul(self, s, endp, base, *a):
        return U64(self._strto(s, endp, base, int))

    def c_strtoull(self, s, endp, base, *a):
        return U64(self._strto(s, endp, base, int))

    def c_strtoll_l(self, s, endp, base, loc, *a):
        return U64(self._strto(s, endp, base, int))

    def c_strtoull_l(self, s, endp, base, loc, *a):
        return U64(self._strto(s, endp, base, int))

    def _strtod(self, s, endp):
        raw = (self.m.cstr(s) or b'').decode('latin-1')
        i = 0
        while i < len(raw) and raw[i] in ' \t\n\r\f\v':
            i += 1
        j = i
        if j < len(raw) and raw[j] in '+-':
            j += 1
        seen = False
        while j < len(raw) and raw[j].isdigit():
            j += 1
            seen = True
        if j < len(raw) and raw[j] == '.':
            j += 1
            while j < len(raw) and raw[j].isdigit():
                j += 1
                seen = True
        if seen and j < len(raw) and raw[j] in 'eE':
            k = j + 1
            if k < len(raw) and raw[k] in '+-':
                k += 1
            if k < len(raw) and raw[k].isdigit():
                while k < len(raw) and raw[k].isdigit():
                    k += 1
                j = k
        try:
            v = float(raw[i:j]) if seen else 0.0
        except ValueError:
            v = 0.0
            j = i
        if endp:
            self.m.write64(endp, s + j)
        return v

    def c_strtod(self, s, endp, *a):
        return self._retf(self._strtod(s, endp))

    def c_strtof(self, s, endp, *a):
        return self._retf(self._strtod(s, endp), True)

    def c_strtold(self, s, endp, *a):
        return self._retf(self._strtod(s, endp))

    def c_strtold_l(self, s, endp, loc, *a):
        return self._retf(self._strtod(s, endp))

    def c_wcstod(self, s, endp, *a):
        return self._retf(0.0)

    def c_wcstof(self, s, endp, *a):
        return self._retf(0.0, True)

    def c_wcstold(self, s, endp, *a):
        return self._retf(0.0)

    def c_wcstol(self, s, endp, base, *a):
        return 0

    def c_wcstoll(self, s, endp, base, *a):
        return 0

    def c_wcstoul(self, s, endp, base, *a):
        return 0

    def c_wcstoull(self, s, endp, base, *a):
        return 0

    # ---- printf family -------------------------------------------------
    def _format(self, fmt_ptr, va_start_reg, va_list_ptr=None):
        fmt = (self.m.cstr(fmt_ptr) or b'').decode('latin-1')
        return self._do_format(fmt, VaArgs(self, va_start_reg, va_list_ptr))

    def _do_format(self, fmt, va):
        out = []
        i = 0
        n = len(fmt)
        while i < n:
            c = fmt[i]
            if c != '%':
                out.append(c)
                i += 1
                continue
            j = i + 1
            if j < n and fmt[j] == '%':
                out.append('%')
                i = j + 1
                continue
            spec = '%'
            while j < n and fmt[j] in "-+ #0'":
                spec += fmt[j]
                j += 1
            while j < n and (fmt[j].isdigit() or fmt[j] == '*'):
                if fmt[j] == '*':
                    spec += str(S32(va.int_arg()))
                else:
                    spec += fmt[j]
                j += 1
            if j < n and fmt[j] == '.':
                spec += '.'
                j += 1
                while j < n and (fmt[j].isdigit() or fmt[j] == '*'):
                    if fmt[j] == '*':
                        spec += str(S32(va.int_arg()))
                    else:
                        spec += fmt[j]
                    j += 1
            length = ''
            while j < n and fmt[j] in 'hlLqjzt':
                length += fmt[j]
                j += 1
            if j >= n:
                out.append(spec)
                break
            conv = fmt[j]
            j += 1
            try:
                out.append(self._one(spec, length, conv, va))
            except Exception:
                out.append('')
            i = j
        return ''.join(out)

    def _one(self, spec, length, conv, va):
        spec = spec.replace("'", '')
        if conv in 'di':
            v = va.int_arg()
            if length in ('', 'h', 'hh'):
                v = S32(v)
            else:
                v = S64(v)
            return (spec + 'd') % v
        if conv in 'uoxX':
            v = va.int_arg()
            if length in ('', 'h', 'hh'):
                v &= 0xFFFFFFFF
            return (spec + ('d' if conv == 'u' else conv)) % v
        if conv in 'eEfgGaA':
            return (spec + (conv if conv not in 'aA' else 'g')) % va.float_arg()
        if conv == 'c':
            return (spec + 'c') % chr(va.int_arg() & 0xFF)
        if conv == 's':
            p = va.int_arg()
            s = (self.m.cstr(p) or b'').decode('utf-8', 'replace') if p else '(null)'
            return (spec + 's') % s
        if conv == 'p':
            return '0x%x' % va.int_arg()
        if conv == 'n':
            p = va.int_arg()
            return ''
        if conv == 'm':
            return 'error'
        return spec + conv

    def c_snprintf(self, buf, size, fmt, *a):
        s = self._format(fmt, 3).encode('utf-8')
        if size:
            self.m.write(buf, s[:size - 1] + b'\0')
        return len(s)

    def c___snprintf_chk(self, buf, size, flag, slen, fmt, *a):
        s = self._format(fmt, 5).encode('utf-8')
        if size:
            self.m.write(buf, s[:size - 1] + b'\0')
        return len(s)

    def c_sprintf(self, buf, fmt, *a):
        s = self._format(fmt, 2).encode('utf-8')
        self.m.write(buf, s + b'\0')
        return len(s)

    def c___sprintf_chk(self, buf, flag, slen, fmt, *a):
        s = self._format(fmt, 4).encode('utf-8')
        self.m.write(buf, s + b'\0')
        return len(s)

    def c_vsnprintf(self, buf, size, fmt, ap, *a):
        s = self._do_format((self.m.cstr(fmt) or b'').decode('latin-1'),
                            VaList(self, ap)).encode('utf-8')
        if size:
            self.m.write(buf, s[:size - 1] + b'\0')
        return len(s)

    def c___vsnprintf_chk(self, buf, size, flag, slen, fmt, ap, *a):
        return self.c_vsnprintf(buf, size, fmt, ap)

    def c_vasprintf(self, out, fmt, ap, *a):
        s = self._do_format((self.m.cstr(fmt) or b'').decode('latin-1'),
                            VaList(self, ap)).encode('utf-8')
        p = self.m.heap.alloc(len(s) + 1)
        self.m.write(p, s + b'\0')
        self.m.write64(out, p)
        return len(s)

    def c_swprintf(self, buf, n, fmt, *a):
        return 0

    def c_printf(self, fmt, *a):
        s = self._format(fmt, 1)
        self._emit(1, s)
        return len(s)

    def c_puts(self, s, *a):
        self._emit(1, (self.m.cstr(s) or b'').decode('utf-8', 'replace') + '\n')
        return 0

    def c_fprintf(self, fp, fmt, *a):
        s = self._format(fmt, 2)
        self._emit(self._fd_of(fp), s)
        return len(s)

    def c_vfprintf(self, fp, fmt, ap, *a):
        s = self._do_format((self.m.cstr(fmt) or b'').decode('latin-1'), VaList(self, ap))
        self._emit(self._fd_of(fp), s)
        return len(s)

    def c_vprintf(self, fmt, ap, *a):
        s = self._do_format((self.m.cstr(fmt) or b'').decode('latin-1'), VaList(self, ap))
        self._emit(1, s)
        return len(s)

    def c_fputs(self, s, fp, *a):
        self._emit(self._fd_of(fp), (self.m.cstr(s) or b'').decode('utf-8', 'replace'))
        return 0

    def c_fputc(self, ch, fp, *a):
        self._emit(self._fd_of(fp), chr(ch & 0xFF))
        return ch

    def c_sscanf(self, s, fmt, *a):
        return self._sscanf(s, fmt, list(a))

    def c_vsscanf(self, s, fmt, ap, *a):
        return 0

    def _sscanf(self, s, fmt, args):
        import re
        text = (self.m.cstr(s) or b'').decode('latin-1')
        f = (self.m.cstr(fmt) or b'').decode('latin-1')
        pos = 0
        ai = 0
        n = 0
        i = 0
        while i < len(f):
            c = f[i]
            if c == '%':
                i += 1
                width = ''
                while i < len(f) and f[i].isdigit():
                    width += f[i]
                    i += 1
                while i < len(f) and f[i] in 'hlLqjzt':
                    i += 1
                if i >= len(f):
                    break
                conv = f[i]
                i += 1
                while pos < len(text) and text[pos].isspace() and conv != 'c':
                    pos += 1
                if conv in 'diu':
                    mm = re.match(r'[+-]?\d+', text[pos:])
                    if not mm:
                        break
                    self.m.write32(args[ai], int(mm.group()) & 0xFFFFFFFF)
                    pos += mm.end()
                elif conv in 'xX':
                    mm = re.match(r'[0-9a-fA-F]+', text[pos:])
                    if not mm:
                        break
                    self.m.write32(args[ai], int(mm.group(), 16) & 0xFFFFFFFF)
                    pos += mm.end()
                elif conv in 'efg':
                    mm = re.match(r'[+-]?\d*\.?\d+(?:[eE][+-]?\d+)?', text[pos:])
                    if not mm:
                        break
                    self.m.write(args[ai], struct.pack('<f', float(mm.group())))
                    pos += mm.end()
                elif conv == 's':
                    mm = re.match(r'\S+', text[pos:])
                    if not mm:
                        break
                    self.m.write(args[ai], mm.group().encode() + b'\0')
                    pos += mm.end()
                else:
                    break
                ai += 1
                n += 1
            elif c.isspace():
                while pos < len(text) and text[pos].isspace():
                    pos += 1
                i += 1
            else:
                if pos < len(text) and text[pos] == c:
                    pos += 1
                    i += 1
                else:
                    break
        return n

    def _emit(self, fd, text):
        if fd == 2:
            sys.stderr.write(text)
        else:
            sys.stdout.write(text)

    # =====================================================================
    #  files
    # =====================================================================
    def _fd_of(self, fp):
        vf = self.streams.get(fp)
        return vf.fd if vf else 1

    def c_open(self, path, flags, mode=0, *a):
        return self._open(self.m.cstr(path).decode(), flags, mode)

    def c_open64(self, path, flags, mode=0, *a):
        return self._open(self.m.cstr(path).decode(), flags, mode)

    def c_openat(self, dirfd, path, flags, mode=0, *a):
        return self._open(self.m.cstr(path).decode(), flags, mode)

    def _open(self, gpath, flags, mode):
        hp = self.host_path(gpath)
        acc = flags & O_ACCMODE
        pymode = 'rb'
        if acc == O_WRONLY:
            pymode = 'ab' if flags & O_APPEND else 'wb'
        elif acc == O_RDWR:
            pymode = 'r+b'
        try:
            if (flags & O_CREAT) and not os.path.exists(hp):
                os.makedirs(os.path.dirname(hp), exist_ok=True)
                open(hp, 'wb').close()
            if (flags & O_TRUNC) and acc != O_RDONLY:
                pymode = 'w+b'
            fh = open(hp, pymode)
        except (IOError, OSError) as e:
            self.set_errno(e.errno or _errno.ENOENT)
            if self.m.verbose > 1:
                print('[fs] open FAIL %s -> %s' % (gpath, hp), file=sys.stderr)
            return U64(-1)
        fd = self.next_fd
        self.next_fd += 1
        self.files[fd] = VFile(fd, fh, gpath)
        if self.m.verbose:
            print('[fs] open %s -> fd %d (%d bytes)' %
                  (gpath, fd, os.path.getsize(hp)), file=sys.stderr)
        return fd

    def c_close(self, fd, *a):
        vf = self.files.pop(fd, None)
        if vf and vf.fh:
            vf.fh.close()
        return 0

    def c_read(self, fd, buf, n, *a):
        vf = self.files.get(fd)
        if not vf or not vf.fh:
            return 0
        data = vf.fh.read(n)
        if data:
            self.m.write(buf, data)
        return len(data)

    def c_write(self, fd, buf, n, *a):
        if fd in (1, 2):
            self._emit(fd, self.m.read(buf, n).decode('utf-8', 'replace'))
            return n
        vf = self.files.get(fd)
        if not vf or not vf.fh:
            return U64(-1)
        vf.fh.write(self.m.read(buf, n))
        return n

    def c_lseek(self, fd, off, whence, *a):
        vf = self.files.get(fd)
        if not vf or not vf.fh:
            return U64(-1)
        return vf.fh.seek(S64(off), whence)

    def c_lseek64(self, fd, off, whence, *a):
        return self.c_lseek(fd, off, whence)

    def c_ftruncate(self, fd, size, *a):
        vf = self.files.get(fd)
        if vf and vf.fh:
            vf.fh.truncate(size)
        return 0

    def c_fsync(self, fd, *a):
        return 0

    def c_dup(self, fd, *a):
        return fd

    def c_pipe(self, arr, *a):
        self.m.write32(arr, 900)
        self.m.write32(arr + 4, 901)
        return 0

    def c_isatty(self, fd, *a):
        return 0

    def c_access(self, path, mode, *a):
        hp = self.host_path(self.m.cstr(path).decode())
        return 0 if os.path.exists(hp) else U64(-1)

    def _stat_into(self, buf, st):
        m = self.m
        m.write(buf, b'\0' * 128)
        m.write64(buf + 0, getattr(st, 'st_dev', 0))
        m.write64(buf + 8, getattr(st, 'st_ino', 0))
        m.write32(buf + 16, st.st_mode)
        m.write32(buf + 20, getattr(st, 'st_nlink', 1))
        m.write32(buf + 24, 0)
        m.write32(buf + 28, 0)
        m.write64(buf + 40, 0)
        m.write64(buf + 48, st.st_size)
        m.write32(buf + 56, 4096)
        m.write64(buf + 64, (st.st_size + 511) // 512)
        m.write64(buf + 72, int(st.st_atime))
        m.write64(buf + 88, int(st.st_mtime))
        m.write64(buf + 104, int(st.st_ctime))
        return 0

    def c_stat(self, path, buf, *a):
        hp = self.host_path(self.m.cstr(path).decode())
        try:
            return self._stat_into(buf, os.stat(hp))
        except OSError as e:
            self.set_errno(e.errno)
            return U64(-1)

    def c_lstat(self, path, buf, *a):
        return self.c_stat(path, buf)

    def c_fstat(self, fd, buf, *a):
        vf = self.files.get(fd)
        if not vf or not vf.fh:
            return U64(-1)
        try:
            return self._stat_into(buf, os.fstat(vf.fh.fileno()))
        except OSError as e:
            self.set_errno(e.errno)
            return U64(-1)

    def c_fstat64(self, fd, buf, *a):
        return self.c_fstat(fd, buf)

    def c_mkdir(self, path, mode, *a):
        hp = self.host_path(self.m.cstr(path).decode())
        try:
            os.makedirs(hp, exist_ok=True)
            return 0
        except OSError:
            return U64(-1)

    def c_rmdir(self, path, *a):
        return 0

    def c_unlink(self, path, *a):
        hp = self.host_path(self.m.cstr(path).decode())
        try:
            os.unlink(hp)
            return 0
        except OSError:
            return U64(-1)

    def c_rename(self, a_, b_, *a):
        try:
            os.rename(self.host_path(self.m.cstr(a_).decode()),
                      self.host_path(self.m.cstr(b_).decode()))
            return 0
        except OSError:
            return U64(-1)

    def c_remove(self, path, *a):
        return self.c_unlink(path)

    def c_chmod(self, path, mode, *a):
        return 0

    def c_fchmod(self, fd, mode, *a):
        return 0

    def c_link(self, a_, b_, *a):
        return U64(-1)

    def c_symlink(self, a_, b_, *a):
        return U64(-1)

    def c_readlink(self, path, buf, n, *a):
        return U64(-1)

    def c_realpath(self, path, out, *a):
        s = self.m.cstr(path) or b''
        if out:
            self.m.write(out, s + b'\0')
            return out
        return self.m.put_cstr(s)

    def c_getcwd(self, buf, n, *a):
        s = self.cwd.encode()[:n - 1]
        self.m.write(buf, s + b'\0')
        return buf

    def c_utimes(self, path, times, *a):
        return 0

    def c_utime(self, path, times, *a):
        return 0

    def c_futimens(self, fd, times, *a):
        return 0

    def c_sendfile(self, out_fd, in_fd, off, count, *a):
        return U64(-1)

    def c_opendir(self, path, *a):
        gp = self.m.cstr(path).decode()
        hp = self.host_path(gp)
        try:
            names = sorted(os.listdir(hp))
        except OSError:
            return 0
        h = self.m.env.alloc(16)
        self.dirs[h] = {'names': names, 'i': 0, 'path': hp,
                        'ent': self.m.env.alloc(280)}
        return h

    def c_readdir(self, h, *a):
        d = self.dirs.get(h)
        if not d or d['i'] >= len(d['names']):
            return 0
        name = d['names'][d['i']]
        d['i'] += 1
        ent = d['ent']
        m = self.m
        m.write(ent, b'\0' * 280)
        m.write64(ent, d['i'])
        m.write64(ent + 8, d['i'])
        m.write16(ent + 16, 280)
        isdir = os.path.isdir(os.path.join(d['path'], name))
        m.write8(ent + 18, 4 if isdir else 8)
        m.write(ent + 19, name.encode()[:255] + b'\0')
        return ent

    def c_closedir(self, h, *a):
        self.dirs.pop(h, None)
        return 0

    # ---- FILE* ---------------------------------------------------------
    def c_fopen(self, path, mode, *a):
        gp = self.m.cstr(path).decode()
        md = (self.m.cstr(mode) or b'rb').decode()
        hp = self.host_path(gp)
        pym = md.replace('t', '')
        if 'b' not in pym:
            pym += 'b'
        try:
            fh = open(hp, pym)
        except (IOError, OSError) as e:
            self.set_errno(e.errno or _errno.ENOENT)
            return 0
        fp = self.m.env.alloc(self.FILE_SZ, align=16)
        self.m.write(fp, b'\0' * self.FILE_SZ)
        fd = self.next_fd
        self.next_fd += 1
        vf = VFile(fd, fh, gp)
        self.files[fd] = vf
        self.streams[fp] = vf
        if self.m.verbose:
            print('[fs] fopen %s (%s)' % (gp, md), file=sys.stderr)
        return fp

    def c_fdopen(self, fd, mode, *a):
        return 0

    def c_fclose(self, fp, *a):
        vf = self.streams.pop(fp, None)
        if vf and vf.fh:
            vf.fh.close()
            self.files.pop(vf.fd, None)
        return 0

    def c_fread(self, buf, size, n, fp, *a):
        vf = self.streams.get(fp)
        if not vf or not vf.fh:
            return 0
        data = vf.fh.read(size * n)
        if data:
            self.m.write(buf, data)
        return len(data) // size if size else 0

    def c_fwrite(self, buf, size, n, fp, *a):
        vf = self.streams.get(fp)
        total = size * n
        if not vf:
            return 0
        if vf.fh is None:
            self._emit(vf.fd, self.m.read(buf, total).decode('utf-8', 'replace'))
            return n
        vf.fh.write(self.m.read(buf, total))
        return n

    def c_fseek(self, fp, off, whence, *a):
        vf = self.streams.get(fp)
        if not vf or not vf.fh:
            return U64(-1)
        vf.fh.seek(S64(off), whence)
        return 0

    def c_fseeko(self, fp, off, whence, *a):
        return self.c_fseek(fp, off, whence)

    def c_ftell(self, fp, *a):
        vf = self.streams.get(fp)
        return vf.fh.tell() if vf and vf.fh else U64(-1)

    def c_ftello(self, fp, *a):
        return self.c_ftell(fp)

    def c_fflush(self, fp, *a):
        vf = self.streams.get(fp)
        if vf and vf.fh:
            try:
                vf.fh.flush()
            except Exception:
                pass
        sys.stdout.flush()
        return 0

    def c_feof(self, fp, *a):
        return 0

    def c_ferror(self, fp, *a):
        return 0

    def c_clearerr(self, fp, *a):
        return 0

    def c_setbuf(self, fp, buf, *a):
        return 0

    def c_setvbuf(self, fp, buf, mode, size, *a):
        return 0

    def c_fgets(self, buf, n, fp, *a):
        vf = self.streams.get(fp)
        if not vf or not vf.fh:
            return 0
        line = vf.fh.readline(n - 1)
        if not line:
            return 0
        self.m.write(buf, line + b'\0')
        return buf

    def c_fileno(self, fp, *a):
        return self._fd_of(fp)

    def c_fscanf(self, fp, fmt, *a):
        return 0

    def c_flock(self, fd, op, *a):
        return 0

    def c_fcntl(self, fd, cmd, *a):
        return 0

    def c_ioctl(self, fd, req, *a):
        return U64(-1)

    def c_select(self, n, r, w, e, t, *a):
        return 0

    def c___FD_SET_chk(self, fd, set_, n, *a):
        return 0

    def c___FD_ISSET_chk(self, fd, set_, n, *a):
        return 0

    def c___FD_CLR_chk(self, fd, set_, n, *a):
        return 0

    # =====================================================================
    #  memory mapping
    # =====================================================================
    def c_mmap(self, addr, length, prot, flags, fd, offset, *a):
        MAP_ANONYMOUS = 0x20
        MAP_FIXED = 0x10
        if fd not in (U64(-1), 0xFFFFFFFF) and not (flags & MAP_ANONYMOUS) and fd in self.files:
            vf = self.files[fd]
            p = self.m.mmap.alloc(length)
            keep = vf.fh.tell()
            vf.fh.seek(offset)
            data = vf.fh.read(length)
            vf.fh.seek(keep)
            if data:
                self.m.write(p, data)
            if self.m.verbose:
                print('[mm] mmap file %s off=%#x len=%#x -> %#x' %
                      (vf.path, offset, length, p), file=sys.stderr)
            return p
        if (flags & MAP_FIXED) and addr:
            return self.m.mmap.alloc_at(addr, length)
        return self.m.mmap.alloc(length)

    def c_mmap64(self, addr, length, prot, flags, fd, offset, *a):
        return self.c_mmap(addr, length, prot, flags, fd, offset)

    def c_munmap(self, addr, length, *a):
        self.m.mmap.free(addr, length)
        return 0

    def c_mprotect(self, addr, length, prot, *a):
        return 0

    def c_madvise(self, addr, length, adv, *a):
        return 0

    def c_msync(self, addr, length, flags, *a):
        return 0

    def c_getpagesize(self, *a):
        return 4096

    def c_sysconf(self, name, *a):
        # _SC_PAGESIZE=39 _SC_NPROCESSORS_ONLN=97 _SC_NPROCESSORS_CONF=96
        return {39: 4096, 96: 4, 97: 4, 30: 4096, 84: 4, 85: 4}.get(name, 4096)

    # =====================================================================
    #  process / env / time
    # =====================================================================
    def c_getpid(self, *a):
        return 1234

    def c_gettid(self, *a):
        return self.m.current.id if self.m.current else 1234

    def c_getuid(self, *a):
        return 10123

    def c_geteuid(self, *a):
        return 10123

    def c_getegid(self, *a):
        return 10123

    def c_getgid(self, *a):
        return 10123

    def c_gethostname(self, buf, n, *a):
        self.m.write(buf, b'localhost\0')
        return 0

    def c_uname(self, buf, *a):
        m = self.m
        m.write(buf, b'\0' * 390)
        for i, v in enumerate([b'Linux', b'localhost', b'4.19.0', b'#1 SMP', b'aarch64']):
            m.write(buf + i * 65, v + b'\0')
        return 0

    def c_getenv(self, name, *a):
        n = (self.m.cstr(name) or b'').decode()
        v = self._env.get(n)
        if v is None:
            return 0
        return self.m.put_cstr(v)

    def c_setenv(self, name, val, ow, *a):
        self._env[(self.m.cstr(name) or b'').decode()] = (self.m.cstr(val) or b'').decode()
        return 0

    def c_unsetenv(self, name, *a):
        self._env.pop((self.m.cstr(name) or b'').decode(), None)
        return 0

    def c_putenv(self, s, *a):
        return 0

    def c_getauxval(self, t, *a):
        return {6: 4096, 16: 0x887}.get(t, 0)

    def c_abort(self, *a):
        print('[vm] abort() called from %s' % self.m.describe(
            self.m.uc.reg_read(A64.UC_ARM64_REG_LR)), file=sys.stderr)
        self.m.exit_code = 134
        self.m.uc.emu_stop()
        return 0

    def c_exit(self, code, *a):
        self.m.exit_code = code
        self.m.uc.emu_stop()
        return 0

    def c__exit(self, code, *a):
        return self.c_exit(code)

    def c_raise(self, sig, *a):
        return 0

    def c_android_set_abort_message(self, msg, *a):
        print('[vm] abort message: %s' % (self.m.cstr(msg) or b'').decode('utf-8', 'replace'),
              file=sys.stderr)
        return 0

    def c___stack_chk_fail(self, *a):
        print('[vm] *** stack check failed at %s' %
              self.m.describe(self.m.uc.reg_read(A64.UC_ARM64_REG_LR)), file=sys.stderr)
        self.m.uc.emu_stop()
        return 0

    def c___cxa_atexit(self, fn, arg, dso, *a):
        self.atexit.append((fn, arg))
        return 0

    def c___cxa_finalize(self, dso, *a):
        return 0

    def c_atexit(self, fn, *a):
        self.atexit.append((fn, 0))
        return 0

    def c___errno(self, *a):
        t = self.m.current
        return t.errno_ptr if t else 0

    def set_errno(self, v):
        t = self.m.current
        if t:
            self.m.write32(t.errno_ptr, v & 0xFFFFFFFF)

    # ---- futex: Unity's Baselib blocks on raw futex syscalls -------------
    FUTEX_WAIT = 0
    FUTEX_WAKE = 1
    FUTEX_WAIT_BITSET = 9
    FUTEX_WAKE_BITSET = 10

    def _futex(self, uaddr, op, val, timeout=0):
        cmd = op & 0x7F
        if cmd in (self.FUTEX_WAIT, self.FUTEX_WAIT_BITSET):
            cur = self.m.read32(uaddr)
            if cur != (val & 0xFFFFFFFF):
                self.set_errno(_errno.EAGAIN)
                return U64(-11)
            self.futex_waits += 1
            self.m.block_current(('futex', uaddr))
            return 0
        if cmd in (self.FUTEX_WAKE, self.FUTEX_WAKE_BITSET):
            n = val if val < 0x7FFFFFFF else 1 << 30
            return self.m.wake_object(('futex', uaddr), n=n, all_=(n > 64))
        return 0

    def c_syscall(self, num, *a):
        # AArch64 syscall numbers
        if num == 98:       # futex
            return self._futex(a[0], a[1], a[2], a[3] if len(a) > 3 else 0)
        if num == 214:      # brk
            return 0
        if num == 178:      # gettid
            return self.c_gettid()
        if num == 172:      # getpid
            return 1234
        if num == 293:      # rseq
            return U64(-1)
        if num == 124:      # sched_yield
            self.m.yield_current()
            return 0
        if num == 278:      # getrandom
            buf, ln = a[0], a[1]
            self.m.write(buf, os.urandom(ln))
            return ln
        if num == 96:       # set_tid_address
            return self.c_gettid()
        if self.m.verbose > 1:
            print('[vm] unhandled syscall %d' % num, file=sys.stderr)
        return 0

    def do_svc(self):
        uc = self.m.uc
        num = uc.reg_read(A64.UC_ARM64_REG_X8)
        args = [uc.reg_read(r) for r in ARG_REGS]
        uc.reg_write(A64.UC_ARM64_REG_X0, U64(self.c_syscall(num, *args)))

    def c_sched_yield(self, *a):
        self.m.yield_current()
        return 0

    def c_sched_getaffinity(self, pid, sz, mask, *a):
        self.m.write(mask, b'\x0f' + b'\0' * (sz - 1))
        return 0

    def c_sched_setaffinity(self, *a):
        return 0

    def c_getpriority(self, *a):
        return 0

    def c_setpriority(self, *a):
        return 0

    def c_prctl(self, *a):
        return 0

    def c_ptrace(self, *a):
        return U64(-1)

    def c_pthread_atfork(self, *a):
        return 0

    def c_clock(self, *a):
        return int((time.monotonic() - self.mono0) * 1000000)

    def c_time(self, p, *a):
        t = int(time.time())
        if p:
            self.m.write64(p, t)
        return t

    def c_gettimeofday(self, tv, tz, *a):
        now = time.time()
        if tv:
            self.m.write64(tv, int(now))
            self.m.write64(tv + 8, int((now % 1) * 1e6))
        return 0

    def c_clock_gettime(self, clk, ts, *a):
        if clk in (1, 4, 5, 6, 7):        # MONOTONIC family
            now = time.monotonic() - self.mono0
        else:
            now = time.time()
        self.m.write64(ts, int(now))
        self.m.write64(ts + 8, int((now % 1) * 1e9))
        return 0

    def c_clock_getres(self, clk, ts, *a):
        if ts:
            self.m.write64(ts, 0)
            self.m.write64(ts + 8, 1)
        return 0

    def c_nanosleep(self, req, rem, *a):
        self.m.yield_current()
        return 0

    def c_usleep(self, us, *a):
        self.m.yield_current()
        return 0

    def c_sleep(self, s, *a):
        return 0

    def _tm_into(self, buf, t):
        m = self.m
        m.write(buf, b'\0' * 56)
        m.write32(buf + 0, t.tm_sec)
        m.write32(buf + 4, t.tm_min)
        m.write32(buf + 8, t.tm_hour)
        m.write32(buf + 12, t.tm_mday)
        m.write32(buf + 16, t.tm_mon - 1)
        m.write32(buf + 20, t.tm_year - 1900)
        m.write32(buf + 24, t.tm_wday if t.tm_wday < 6 else 0)
        m.write32(buf + 28, t.tm_yday - 1)
        m.write32(buf + 32, 0)
        return buf

    def c_localtime(self, tp, *a):
        if not hasattr(self, '_tmbuf'):
            self._tmbuf = self.m.env.alloc(64)
        return self._tm_into(self._tmbuf, time.localtime(self.m.read64(tp) if tp else 0))

    def c_localtime_r(self, tp, buf, *a):
        return self._tm_into(buf, time.localtime(self.m.read64(tp) if tp else 0))

    def c_gmtime(self, tp, *a):
        if not hasattr(self, '_tmbuf2'):
            self._tmbuf2 = self.m.env.alloc(64)
        return self._tm_into(self._tmbuf2, time.gmtime(self.m.read64(tp) if tp else 0))

    def c_gmtime_r(self, tp, buf, *a):
        return self._tm_into(buf, time.gmtime(self.m.read64(tp) if tp else 0))

    def c_mktime(self, tm, *a):
        m = self.m
        try:
            st = (m.read32(tm + 20) + 1900, m.read32(tm + 16) + 1, m.read32(tm + 12),
                  m.read32(tm + 8), m.read32(tm + 4), m.read32(tm + 0), 0, 1, -1)
            return int(time.mktime(st))
        except Exception:
            return U64(-1)

    def c_strftime(self, buf, n, fmt, tm, *a):
        f = (self.m.cstr(fmt) or b'').decode()
        try:
            st = time.struct_time((
                self.m.read32(tm + 20) + 1900, self.m.read32(tm + 16) + 1,
                self.m.read32(tm + 12), self.m.read32(tm + 8), self.m.read32(tm + 4),
                self.m.read32(tm + 0), 0, 1, 0))
            s = time.strftime(f, st).encode()
        except Exception:
            s = b''
        self.m.write(buf, s[:n - 1] + b'\0')
        return min(len(s), n - 1)

    def c_strftime_l(self, buf, n, fmt, tm, loc, *a):
        return self.c_strftime(buf, n, fmt, tm)

    def c_lrand48(self, *a):
        return int.from_bytes(os.urandom(4), 'little') & 0x7FFFFFFF

    def c_srand48(self, *a):
        return 0

    def c_rand(self, *a):
        return int.from_bytes(os.urandom(4), 'little') & 0x7FFFFFFF

    def c_srand(self, *a):
        return 0

    def c_arc4random(self, *a):
        return int.from_bytes(os.urandom(4), 'little')

    def c_arc4random_buf(self, buf, n, *a):
        self.m.write(buf, os.urandom(n))
        return 0

    # =====================================================================
    #  signals / setjmp
    # =====================================================================
    def c_signal(self, sig, h, *a):
        return 0

    def c_sigaction(self, sig, act, old, *a):
        return 0

    def c_sigaddset(self, s, n, *a):
        return 0

    def c_sigdelset(self, s, n, *a):
        return 0

    def c_sigemptyset(self, s, *a):
        if s:
            self.m.write(s, b'\0' * 8)
        return 0

    def c_sigfillset(self, s, *a):
        if s:
            self.m.write(s, b'\xff' * 8)
        return 0

    def c_sigsuspend(self, s, *a):
        return 0

    def c_sigaltstack(self, ss, old, *a):
        return 0

    def c_pthread_sigmask(self, how, set_, old, *a):
        return 0

    def c_sigprocmask(self, how, set_, old, *a):
        return 0

    JB_REGS = ['X19', 'X20', 'X21', 'X22', 'X23', 'X24', 'X25', 'X26',
               'X27', 'X28', 'X29', 'LR', 'SP']

    def c_setjmp(self, jb, *a):
        uc = self.m.uc
        for i, r in enumerate(self.JB_REGS):
            uc_reg = getattr(A64, 'UC_ARM64_REG_' + r)
            self.m.write64(jb + i * 8, uc.reg_read(uc_reg))
        self.m.write64(jb + 13 * 8, 0x5A5A5A5A)
        return 0

    def c__setjmp(self, jb, *a):
        return self.c_setjmp(jb)

    def c_sigsetjmp(self, jb, save, *a):
        return self.c_setjmp(jb)

    def c_longjmp(self, jb, val, *a):
        uc = self.m.uc
        for i, r in enumerate(self.JB_REGS):
            uc_reg = getattr(A64, 'UC_ARM64_REG_' + r)
            uc.reg_write(uc_reg, self.m.read64(jb + i * 8))
        uc.reg_write(A64.UC_ARM64_REG_X0, val if val else 1)
        uc.reg_write(A64.UC_ARM64_REG_PC, self.m.read64(jb + 11 * 8))
        return None

    def c_siglongjmp(self, jb, val, *a):
        return self.c_longjmp(jb, val)

    # =====================================================================
    #  pthreads (single-threaded / green-thread model)
    # =====================================================================
    def c_pthread_self(self, *a):
        t = self.m.current
        return t.tls_block if t else 0

    def c_pthread_equal(self, a_, b_, *a):
        return 1 if a_ == b_ else 0

    def c_pthread_getspecific(self, key, *a):
        t = self.m.current
        return t.tls.get(key, 0) if t else 0

    def c_pthread_setspecific(self, key, val, *a):
        t = self.m.current
        if t:
            t.tls[key] = val
        return 0

    def c_pthread_key_create(self, keyp, dtor, *a):
        k = self.next_tls_key
        self.next_tls_key += 1
        self.tls_dtors[k] = dtor
        self.m.write32(keyp, k)
        return 0

    def c_pthread_key_delete(self, key, *a):
        return 0

    def c_pthread_mutex_init(self, mtx, attr, *a):
        self.m.write64(mtx, 0)
        return 0

    def c_pthread_mutex_destroy(self, mtx, *a):
        return 0

    def c_pthread_mutex_lock(self, mtx, *a):
        return 0

    def c_pthread_mutex_trylock(self, mtx, *a):
        return 0

    def c_pthread_mutex_unlock(self, mtx, *a):
        return 0

    def c_pthread_mutexattr_init(self, a_, *a):
        return 0

    def c_pthread_mutexattr_destroy(self, a_, *a):
        return 0

    def c_pthread_mutexattr_settype(self, a_, t, *a):
        return 0

    def c_pthread_rwlock_init(self, l, a_, *a):
        return 0

    def c_pthread_rwlock_destroy(self, l, *a):
        return 0

    def c_pthread_rwlock_rdlock(self, l, *a):
        return 0

    def c_pthread_rwlock_wrlock(self, l, *a):
        return 0

    def c_pthread_rwlock_unlock(self, l, *a):
        return 0

    def c_pthread_cond_init(self, c, a_, *a):
        return 0

    def c_pthread_cond_destroy(self, c, *a):
        return 0

    def c_pthread_cond_signal(self, c, *a):
        self.m.wake_object(('cond', c), n=1)
        return 0

    def c_pthread_cond_broadcast(self, c, *a):
        self.m.wake_object(('cond', c), all_=True)
        return 0

    def c_pthread_cond_wait(self, c, mtx, *a):
        self.m.block_current(('cond', c))
        return 0

    def c_pthread_cond_timedwait(self, c, mtx, ts, *a):
        self.m.yield_current()
        return 110       # ETIMEDOUT

    def c_pthread_condattr_init(self, a_, *a):
        return 0

    def c_pthread_condattr_destroy(self, a_, *a):
        return 0

    def c_pthread_condattr_setclock(self, a_, c, *a):
        return 0

    def c_pthread_attr_init(self, a_, *a):
        if a_:
            self.m.write(a_, b'\0' * 56)
        return 0

    def c_pthread_attr_destroy(self, a_, *a):
        return 0

    def c_pthread_attr_setdetachstate(self, a_, s, *a):
        return 0

    def c_pthread_attr_setstacksize(self, a_, s, *a):
        return 0

    def _thread_by_handle(self, h):
        for t in self.m.threads:
            if t.tls_block == h or t.id == h:
                return t
        return None

    def c_pthread_attr_getstack(self, attr, addrp, sizep, *a):
        # pthread_attr_t is opaque (56 bytes on bionic/arm64); keep the
        # stack bounds in a host-side table instead of poking the struct.
        lo, sz = self.attr_stacks.get(attr, (0, 0))
        if not sz:
            t = self.m.current
            lo, sz = (t.stack_lo, M.STACK_SIZE) if t else (M.STACK_BASE, M.STACK_SIZE)
        self.m.write64(addrp, lo)
        self.m.write64(sizep, sz)
        return 0

    def c_pthread_getattr_np(self, th, attr, *a):
        t = self._thread_by_handle(th) or self.m.current
        self.attr_stacks[attr] = (t.stack_lo, M.STACK_SIZE)
        return 0

    def c_pthread_setname_np(self, th, name, *a):
        return 0

    def c_pthread_detach(self, th, *a):
        return 0

    def c_pthread_kill(self, th, sig, *a):
        return 0

    def c_pthread_exit(self, val, *a):
        return 0

    def c_pthread_create(self, thp, attr, fn, arg, *a):
        """Deferred threads: recorded, started by the scheduler."""
        t = self.m.spawn(fn, arg)
        if thp:
            self.m.write64(thp, t.tls_block)
        return 0

    def c_pthread_join(self, th, retp, *a):
        target = None
        for t in self.m.threads:
            if t.tls_block == th:
                target = t
                break
        if target is not None and target.state != 'done':
            self.m.block_current(target)
        if retp:
            self.m.write64(retp, target.retval if target else 0)
        return 0

    def c_sem_init(self, s, sh, val, *a):
        self.sems[s] = val
        return 0

    def c_sem_destroy(self, s, *a):
        self.sems.pop(s, None)
        return 0

    def c_sem_post(self, s, *a):
        self.sems[s] = self.sems.get(s, 0) + 1
        self.m.wake_object(('sem', s), n=1)
        return 0

    def c_sem_wait(self, s, *a):
        if self.sems.get(s, 0) > 0:
            self.sems[s] -= 1
            return 0
        if self.m.has_other_runnable():
            self.m.block_current(('sem', s), retry=True)
            return 0
        # No other guest thread can run, so the world is already stopped:
        # this is exactly the situation Boehm's GC_suspend_ack_sem waits
        # for, and acknowledging immediately is correct for a green-thread
        # VM (it becomes a real sem_wait in the native ARM64 build).
        self.sem_autoacks += 1
        return 0

    def c_sem_trywait(self, s, *a):
        if self.sems.get(s, 0) <= 0:
            self.set_errno(_errno.EAGAIN)
            return U64(-1)
        self.sems[s] -= 1
        return 0

    def c_sem_timedwait(self, s, ts, *a):
        if self.sems.get(s, 0) > 0:
            self.sems[s] -= 1
            return 0
        if self.m.has_other_runnable():
            self.m.block_current(('sem', s), retry=True)
            return 0
        self.sem_autoacks += 1
        return 0                      # world already stopped - acknowledge

    def c_sem_getvalue(self, s, out, *a):
        self.m.write32(out, self.sems.get(s, 0))
        return 0

    # =====================================================================
    #  dynamic linker
    # =====================================================================
    def c_dlopen(self, name, flags, *a):
        n = (self.m.cstr(name) or b'').decode() if name else '<self>'
        for li in self.m.images:
            if li.name in n or n.endswith(li.name):
                return li.base
        return self.m.images[0].base if self.m.images else 1

    def c_dlsym(self, handle, name, *a):
        n = (self.m.cstr(name) or b'').decode()
        for li in self.m.images:
            v = li.resolve(n)
            if v is not None:
                return v
        return 0

    def c_dlclose(self, h, *a):
        return 0

    def c_dlerror(self, *a):
        return 0

    def c_dladdr(self, addr, info, *a):
        for li in self.m.images:
            if li.base <= addr < li.end:
                m = self.m
                m.write64(info + 0, m.put_cstr('/data/app/lib/arm64/' + li.name))
                m.write64(info + 8, li.base)
                s = li.addr_to_sym(addr)
                m.write64(info + 16, m.put_cstr(s[0]) if s else 0)
                m.write64(info + 24, s[1] if s else 0)
                return 1
        return 0

    # liblog
    def c___android_log_print(self, prio, tag, fmt, *a):
        msg = self._format(fmt, 3)
        self._logcat(prio, tag, msg)
        return 0

    def c___android_log_write(self, prio, tag, text, *a):
        self._logcat(prio, tag, (self.m.cstr(text) or b'').decode('utf-8', 'replace'))
        return 0

    def c___android_log_vprint(self, prio, tag, fmt, ap, *a):
        msg = self._do_format((self.m.cstr(fmt) or b'').decode('latin-1'), VaList(self, ap))
        self._logcat(prio, tag, msg)
        return 0

    def c___android_log_assert(self, cond, tag, fmt, *a):
        self._logcat(7, tag, self._format(fmt, 3))
        return 0

    def _logcat(self, prio, tag, msg):
        t = (self.m.cstr(tag) or b'?').decode('utf-8', 'replace')
        entry = (prio, t, msg)
        self.log.append(entry)
        if self.log_echo:
            lvl = 'VVDIWEF'[min(max(prio, 0), 6)]
            sys.stderr.write('[logcat %s/%s] %s\n' % (lvl, t, msg.rstrip('\n')))

    def c_openlog(self, *a):
        return 0

    def c_closelog(self, *a):
        return 0

    def c_syslog(self, prio, fmt, *a):
        return 0

    def c___system_property_get(self, name, val, *a):
        n = (self.m.cstr(name) or b'').decode()
        props = {
            'ro.build.version.sdk': '30',
            'ro.build.version.release': '11',
            'ro.product.model': 'KairoVM',
            'ro.product.manufacturer': 'kairovm',
            'ro.product.cpu.abi': 'arm64-v8a',
            'ro.debuggable': '0',
        }
        v = props.get(n, '')
        self.m.write(val, v.encode() + b'\0')
        return len(v)

    def c___system_property_find(self, name, *a):
        return 0

    def c___system_property_read(self, pi, name, val, *a):
        return 0

    # sockets: the game's online features are inert offline
    def c_socket(self, *a):
        return U64(-1)

    def c_connect(self, *a):
        return U64(-1)

    def c_getaddrinfo(self, *a):
        return U64(-1)

    def c_freeaddrinfo(self, *a):
        return 0

    def c_gethostbyname(self, *a):
        return 0

    def c_gethostbyaddr(self, *a):
        return 0

    def c_inet_addr(self, *a):
        return 0

    def c_inet_ntop(self, *a):
        return 0

    def c_inet_pton(self, *a):
        return 0

    def c_if_nametoindex(self, *a):
        return 0

    def c_getsockname(self, *a):
        return U64(-1)

    def c_getpeername(self, *a):
        return U64(-1)

    def c_getsockopt(self, *a):
        return U64(-1)

    def c_setsockopt(self, *a):
        return U64(-1)

    def c_send(self, *a):
        return U64(-1)

    def c_recv(self, *a):
        return U64(-1)

    def c_bind(self, *a):
        return U64(-1)

    def c_listen(self, *a):
        return U64(-1)

    def c_accept(self, *a):
        return U64(-1)

    def c_shutdown(self, *a):
        return 0

    def c_poll(self, *a):
        return 0

    def c_statfs(self, path, buf, *a):
        self.m.write(buf, b'\0' * 120)
        self.m.write64(buf + 8, 4096)
        self.m.write64(buf + 16, 1 << 20)
        self.m.write64(buf + 24, 1 << 19)
        self.m.write64(buf + 32, 1 << 19)
        return 0

    def c_getpwuid(self, uid, *a):
        return 0

    def c_getpwuid_r(self, uid, pw, buf, n, res, *a):
        if res:
            self.m.write64(res, 0)
        return 0

    def c_basename(self, p, *a):
        s = (self.m.cstr(p) or b'')
        return self.m.put_cstr(s.rsplit(b'/', 1)[-1])

    def c_fnmatch(self, pat, s, flags, *a):
        import fnmatch as fn
        return 0 if fn.fnmatch((self.m.cstr(s) or b'').decode(),
                               (self.m.cstr(pat) or b'').decode()) else 1

    def c_truncate(self, *a):
        return 0

    def c_qsort_r(self, *a):
        return 0


class VaArgs(object):
    """Variadic arguments starting at register index `start` (AAPCS64)."""

    def __init__(self, host, start, _unused=None):
        self.h = host
        self.i = start
        self.fi = 0
        self.stack = host.m.uc.reg_read(A64.UC_ARM64_REG_SP)
        self.stack_off = 0

    def int_arg(self):
        if self.i < 8:
            v = self.h.m.uc.reg_read(ARG_REGS[self.i])
            self.i += 1
            return v
        v = self.h.m.read64(self.stack + self.stack_off)
        self.stack_off += 8
        return v

    def float_arg(self):
        regs = [A64.UC_ARM64_REG_D0, A64.UC_ARM64_REG_D1, A64.UC_ARM64_REG_D2,
                A64.UC_ARM64_REG_D3, A64.UC_ARM64_REG_D4, A64.UC_ARM64_REG_D5,
                A64.UC_ARM64_REG_D6, A64.UC_ARM64_REG_D7]
        if self.fi < 8:
            v = self.h.m.uc.reg_read(regs[self.fi])
            self.fi += 1
            if isinstance(v, float):
                return v
            return struct.unpack('<d', struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF))[0]
        raw = self.h.m.read64(self.stack + self.stack_off)
        self.stack_off += 8
        return struct.unpack('<d', struct.pack('<Q', raw))[0]


class VaList(object):
    """AArch64 va_list: {stack, gr_top, vr_top, gr_offs, vr_offs}."""

    def __init__(self, host, ap):
        self.h = host
        m = host.m
        self.stack = m.read64(ap + 0)
        self.gr_top = m.read64(ap + 8)
        self.vr_top = m.read64(ap + 16)
        self.gr_offs = S32(m.read32(ap + 24))
        self.vr_offs = S32(m.read32(ap + 28))

    def int_arg(self):
        m = self.h.m
        if self.gr_offs < 0:
            v = m.read64(self.gr_top + self.gr_offs)
            self.gr_offs += 8
            return v
        v = m.read64(self.stack)
        self.stack += 8
        return v

    def float_arg(self):
        m = self.h.m
        if self.vr_offs < 0:
            raw = m.read64(self.vr_top + self.vr_offs)
            self.vr_offs += 16
        else:
            raw = m.read64(self.stack)
            self.stack += 8
        return struct.unpack('<d', struct.pack('<Q', raw))[0]
