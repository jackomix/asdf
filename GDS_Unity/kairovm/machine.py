"""KairoVM: an AArch64 user-space machine that runs Android .so files.

The machine loads bionic shared objects, binds their undefined symbols to
Python-implemented trampolines and executes the real ARM64 machine code
through Unicorn.  Nothing about the game is reimplemented - the shipped
libil2cpp.so executes instruction for instruction.

The interesting property for this project is that the *contract* between
the guest code and the host (the list of libc/libm/libdl/liblog symbols
in bionic.py, plus the file-system and threading model) is exactly what a
native ARM64 loader on a Linux handheld has to provide.  Swapping Unicorn
for a real CPU therefore does not change the port, only the backend.
"""
import collections
import struct
import sys
import threading
import time

from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_BLOCK, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED,
                     UC_HOOK_MEM_FETCH_UNMAPPED,
                     UC_PROT_ALL, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC)
from unicorn import arm64_const as A64

from . import memory as M
from .elfimage import (ElfImage, LoadedImage, R_AARCH64_ABS64, R_AARCH64_GLOB_DAT,
                       R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE, R_AARCH64_IRELATIVE,
                       R_AARCH64_TLS_TPREL64, PF_X, PF_W, PF_R, PT_LOAD)

try:
    from keystone import Ks, KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN
    _KS = Ks(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN)
except Exception:                                            # pragma: no cover
    _KS = None


def asm(src, addr=0):
    if _KS is None:
        raise RuntimeError('keystone-engine is required to build guest thunks')
    enc, _ = _KS.asm(src, addr)
    return bytes(enc)


ARG_REGS = [A64.UC_ARM64_REG_X0, A64.UC_ARM64_REG_X1, A64.UC_ARM64_REG_X2,
            A64.UC_ARM64_REG_X3, A64.UC_ARM64_REG_X4, A64.UC_ARM64_REG_X5,
            A64.UC_ARM64_REG_X6, A64.UC_ARM64_REG_X7]
FARG_REGS = [A64.UC_ARM64_REG_D0, A64.UC_ARM64_REG_D1, A64.UC_ARM64_REG_D2,
             A64.UC_ARM64_REG_D3, A64.UC_ARM64_REG_D4, A64.UC_ARM64_REG_D5,
             A64.UC_ARM64_REG_D6, A64.UC_ARM64_REG_D7]

# Guest addresses reserved for control flow bookkeeping.
RETURN_MAGIC = M.ENV_BASE + 0x1000        # LR for host->guest calls
# Hand-written AArch64 helper thunks must sit within a single B-instruction
# (+/-128 MiB) of the import stub table, and *outside* the stub hook range.
STUB_TABLE_END = M.STUB_BASE + 0x10000    # 8192 import trampolines
THUNK_BASE = M.STUB_BASE + 0x10000
THUNK_SIZE = 0x8000


class GuestError(Exception):
    pass


class TailCall(object):
    """Host stub result: continue execution at `addr` keeping the caller's LR.

    Lets a host implementation hand control to real guest code (for example
    il2cpp_string_new) without re-entering the emulator, which Unicorn does
    not allow from inside a hook.
    """
    __slots__ = ('addr',)

    def __init__(self, addr):
        self.addr = addr


class _Skip(object):
    """Host stub result: the handler has already set the return registers."""
    __repr__ = lambda self: '<SKIP>'


SKIP = _Skip()


class Thread(object):
    _next_id = 1

    def __init__(self, machine, entry=0, arg=0, name='thread'):
        self.machine = machine
        self.id = Thread._next_id
        Thread._next_id += 1
        self.name = name
        self.entry = entry
        self.arg = arg
        self.ctx = None
        self.state = 'new'                # new | ready | running | blocked | done
        self.ev = threading.Event()
        self.py = None
        self.blocked_on = None
        self.wake_count = 0
        self.retval = 0
        self.tls = {}                     # pthread key -> value
        self.errno_ptr = 0
        self.wake_at = 0.0
        self.join_waiters = []
        base = M.STACK_BASE + (self.id - 1) * (M.STACK_SIZE + M.STACK_GUARD)
        machine.space.map(base, M.STACK_SIZE)
        self.stack_lo = base
        self.stack_hi = base + M.STACK_SIZE
        self.sp = self.stack_hi - 0x200
        self.tls_block = machine.env.alloc(0x400)
        machine.write64(self.tls_block + 8 * 1, self.id)          # TLS_SLOT_THREAD_ID
        machine.write64(self.tls_block + 8 * 5, 0x0BADC0DEDEADBEEF)  # STACK_GUARD
        self.errno_ptr = machine.env.alloc(8)


class Machine(object):
    def __init__(self, verbose=0, trace=False):
        self.verbose = verbose
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.space = M.AddressSpace(self.uc)
        # Enable FP/SIMD at EL0 (Unicorn boots with FP traps enabled).
        try:
            self.uc.reg_write(A64.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        except Exception:
            pass

        self.space.map(M.STUB_BASE, M.STUB_SIZE, UC_PROT_ALL)
        self.space.map(M.ENV_BASE, M.ENV_SIZE, UC_PROT_ALL)
        self.env = M.Arena(self.space, M.ENV_BASE + 0x10000, M.ENV_SIZE - 0x10000, 'env')
        self.heap = M.Arena(self.space, M.HEAP_BASE, M.HEAP_SIZE, 'malloc')
        self.mmap = M.MmapArena(self.space, M.MMAP_BASE, M.MMAP_SIZE)

        self.images = []
        self.by_name = {}
        self.lib_cursor = M.LIB_BASE

        # trampolines
        self.stubs = {}                   # name -> guest address
        self.stub_impl = {}               # guest address -> python callable
        self.stub_name = {}               # guest address -> name
        self.stub_cursor = M.STUB_BASE
        self.missing = {}                 # name -> hit count (unimplemented)
        self.passthrough = set()          # stubs that hold real guest code

        # threads
        self.threads = []
        self.current = None
        self.main_thread = None
        self._switch_request = None
        self._stop_reason = None

        self.exit_code = None
        self.host = None                  # bionic.Bionic instance
        self.symbols = None               # tools.symbols.SymbolTable
        self.trace = trace
        self.insns = 0
        self.default_budget = 4000 * 1000 * 1000
        self.slice_insns = 50 * 1000 * 1000
        self.watch = True
        self._yield_flag = False
        self._retry_call = False
        self._sched_lock = threading.RLock()
        self._spurious = 0

        self.uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=M.STUB_BASE,
                         end=STUB_TABLE_END - 1)
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                         UC_HOOK_MEM_FETCH_UNMAPPED, self._on_unmapped)
        self.blocks = collections.deque(maxlen=96)
        if trace:
            self.uc.hook_add(UC_HOOK_BLOCK, self._on_block)

        # return-magic page must be executable and contain a trap
        self.uc.mem_write(RETURN_MAGIC, asm('brk #0xffff'))

    # ------------------------------------------------------------ memory io
    def read(self, addr, size):
        return bytes(self.uc.mem_read(addr, size))

    def write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def read64(self, addr):
        return struct.unpack('<Q', self.uc.mem_read(addr, 8))[0]

    def read32(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def read16(self, addr):
        return struct.unpack('<H', self.uc.mem_read(addr, 2))[0]

    def read8(self, addr):
        return self.uc.mem_read(addr, 1)[0]

    def write64(self, addr, v):
        self.uc.mem_write(addr, struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF))

    def write32(self, addr, v):
        self.uc.mem_write(addr, struct.pack('<I', v & 0xFFFFFFFF))

    def write16(self, addr, v):
        self.uc.mem_write(addr, struct.pack('<H', v & 0xFFFF))

    def write8(self, addr, v):
        self.uc.mem_write(addr, bytes([v & 0xFF]))

    def cstr(self, addr, limit=1 << 20):
        if not addr:
            return None
        out = bytearray()
        while len(out) < limit:
            chunk = bytes(self.uc.mem_read(addr + len(out), 64))
            i = chunk.find(b'\0')
            if i >= 0:
                out += chunk[:i]
                break
            out += chunk
        return bytes(out)

    def put_cstr(self, s, arena=None):
        if isinstance(s, str):
            s = s.encode('utf-8')
        arena = arena or self.env
        p = arena.alloc(len(s) + 1)
        self.write(p, s + b'\0')
        return p

    # --------------------------------------------------------------- loading
    def load(self, path, name=None):
        img = ElfImage(path, name)
        bias = self.lib_cursor
        self.lib_cursor = (bias + img.span + 0x10000 + 0xFFFF) & ~0xFFFF
        li = LoadedImage(img, bias)
        self._map_image(li)
        self.images.append(li)
        self.by_name[li.name] = li
        return li

    def _map_image(self, li):
        img = li.img
        span = img.span
        buf = bytearray(span)
        for p in img.phdrs:
            if p['type'] != PT_LOAD:
                continue
            off = p['vaddr'] - img.min_vaddr
            buf[off:off + p['filesz']] = img.data[p['offset']:p['offset'] + p['filesz']]
        self._relocate(li, buf)
        base = li.bias + img.min_vaddr
        self.space.map(base, span, UC_PROT_ALL)
        self.uc.mem_write(base, bytes(buf))
        if self.verbose:
            print('[vm] mapped %s at %#x..%#x (%.1f MiB)' %
                  (li.name, base, base + span, span / 1048576.0))

    def _relocate(self, li, buf):
        img = li.img
        bias = li.bias
        lo = img.min_vaddr
        syms = img.symbols
        n_rel = 0
        n_imp = 0
        pack = struct.pack_into
        for r_offset, r_type, r_sym, r_addend in img.relocations():
            where = r_offset - lo
            if r_type == R_AARCH64_RELATIVE:
                pack('<Q', buf, where, (bias + r_addend) & 0xFFFFFFFFFFFFFFFF)
                n_rel += 1
                continue
            sym = syms[r_sym] if r_sym < len(syms) else None
            if r_type in (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64):
                if sym is None:
                    value = 0
                elif sym.defined:
                    value = bias + sym.value
                else:
                    value = self.import_stub(sym.name)
                    n_imp += 1
                pack('<Q', buf, where, (value + r_addend) & 0xFFFFFFFFFFFFFFFF)
            elif r_type == R_AARCH64_IRELATIVE:
                # ifunc: point at the resolver; we resolve lazily on call.
                pack('<Q', buf, where, (bias + r_addend) & 0xFFFFFFFFFFFFFFFF)
            elif r_type == R_AARCH64_TLS_TPREL64:
                pack('<Q', buf, where, 0)
            else:
                raise GuestError('unhandled reloc type %d at %#x' % (r_type, r_offset))
        if self.verbose:
            print('[vm] %s: %d relative, %d imports bound' % (li.name, n_rel, n_imp))

    # ----------------------------------------------------------- trampolines
    def import_stub(self, name):
        """Return the guest address of the trampoline for an imported symbol."""
        a = self.stubs.get(name)
        if a is not None:
            return a
        a = self.stub_cursor
        self.stub_cursor += 8
        if a >= STUB_TABLE_END:
            raise GuestError('import stub table overflow')
        self.uc.mem_write(a, asm('brk #1') + asm('ret'))
        self.stubs[name] = a
        self.stub_name[a] = name
        return a

    def bind(self, name, fn):
        """Attach a Python implementation to an imported symbol."""
        a = self.import_stub(name)
        self.stub_impl[a] = fn
        return a

    def new_callback(self, fn, name='<callback>'):
        """Allocate a fresh guest-callable address backed by a Python fn."""
        a = self.stub_cursor
        self.stub_cursor += 8
        self.uc.mem_write(a, asm('brk #1') + asm('ret'))
        self.stub_impl[a] = fn
        self.stub_name[a] = name
        return a

    # ------------------------------------------------------------ hooks
    def _on_stub(self, uc, address, size, ud):
        if address in self.passthrough:
            return                        # real ARM64 thunk lives here
        fn = self.stub_impl.get(address)
        if fn is None:
            name = self.stub_name.get(address, '??@%#x' % address)
            self.missing[name] = self.missing.get(name, 0) + 1
            if self.verbose > 1 or self.missing[name] == 1:
                print('[vm] !! unimplemented import: %s (lr=%#x)' %
                      (name, uc.reg_read(A64.UC_ARM64_REG_LR)), file=sys.stderr)
            uc.reg_write(A64.UC_ARM64_REG_X0, 0)
            uc.reg_write(A64.UC_ARM64_REG_PC, uc.reg_read(A64.UC_ARM64_REG_LR))
            return
        args = [uc.reg_read(r) for r in ARG_REGS]
        try:
            ret = fn(*args)
        except SwitchOut:
            raise
        except Exception as exc:            # surface host bugs loudly
            print('[vm] host function %s raised: %r' %
                  (self.stub_name.get(address), exc), file=sys.stderr)
            import traceback
            traceback.print_exc()
            uc.emu_stop()
            self._stop_reason = ('host-error', exc)
            return
        if ret is None:
            ret = 0
        if ret is SKIP:
            uc.reg_write(A64.UC_ARM64_REG_PC, uc.reg_read(A64.UC_ARM64_REG_LR))
            return
        if isinstance(ret, TailCall):
            uc.reg_write(A64.UC_ARM64_REG_PC, ret.addr)
            return
        if isinstance(ret, tuple):
            for i, v in enumerate(ret):
                uc.reg_write(ARG_REGS[i], v & 0xFFFFFFFFFFFFFFFF)
        elif isinstance(ret, float):
            uc.reg_write(A64.UC_ARM64_REG_D0, ret)
        else:
            uc.reg_write(A64.UC_ARM64_REG_X0, ret & 0xFFFFFFFFFFFFFFFF)
        if self._retry_call:
            # Re-run this very call once the thread is scheduled again.
            self._retry_call = False
            uc.reg_write(A64.UC_ARM64_REG_PC, address)
            return
        uc.reg_write(A64.UC_ARM64_REG_PC, uc.reg_read(A64.UC_ARM64_REG_LR))

    def _on_intr(self, uc, intno, ud):
        pc = uc.reg_read(A64.UC_ARM64_REG_PC)
        if intno == 2:                     # SVC
            if self.host is not None:
                self.host.do_svc()
            return
        # BRK
        try:
            insn = struct.unpack('<I', uc.mem_read(pc, 4))[0]
        except Exception:
            insn = 0
        imm = (insn >> 5) & 0xFFFF
        if imm == 0xFFFF and pc == RETURN_MAGIC:
            uc.emu_stop()
            return
        self._stop_reason = ('brk', pc, imm)
        print('[vm] BRK #%#x at %#x  %s' % (imm, pc, self.describe(pc)), file=sys.stderr)
        uc.emu_stop()

    def _on_unmapped(self, uc, access, address, size, value, ud):
        pc = uc.reg_read(A64.UC_ARM64_REG_PC)
        print('[vm] unmapped access type=%d addr=%#x size=%d pc=%#x %s' %
              (access, address, size, pc, self.describe(pc)), file=sys.stderr)
        self.dump_state('segv')
        self._stop_reason = ('segv', address, pc)
        return False

    def _on_block(self, uc, address, size, ud):
        self.blocks.append(address)

    # ------------------------------------------------------- method tracing
    def enable_method_trace(self, assemblies=('Assembly-CSharp.dll',
                                              'KairoLibrary.dll'), depth=8192,
                            watch=()):
        """Record every managed method of the game's own assemblies as it is
        entered.  Restricted to their address range so the hook costs nothing
        while the runtime or corlib is running."""
        if self.symbols is None:
            return False
        want = {}
        for a, rec in self.symbols.by_addr.items():
            if rec['asm'] in assemblies:
                want[a] = rec['full']
        if not want:
            return False
        li = self.images[0]
        lo, hi = min(want) + li.bias, max(want) + li.bias
        self.method_trace = collections.deque(maxlen=depth)
        self.trace_names = {a + li.bias: n for a, n in want.items()}
        names = self.trace_names
        trace = self.method_trace
        self.trace_watch = list(watch)
        watching = {}
        if watch:
            for a, n in names.items():
                if any(w in n for w in watch):
                    watching[a] = n
            print('[vm] watching %d methods' % len(watching))
        self.trace_seq = [0]
        seq = self.trace_seq
        logging = {}
        for a, n in names.items():
            if n.startswith('kairo.unity.util.Log::') and n.split('::')[1] in (
                    'Info', 'Error', 'Warn', 'Debug', 'Verbose'):
                logging[a] = n.split('::')[1]
        gstr = self.guest_string
        reg0 = A64.UC_ARM64_REG_X0
        reg1 = A64.UC_ARM64_REG_X1

        def cb(uc, address, size, ud):
            n = names.get(address)
            if n is not None:
                trace.append(n)
                seq[0] += 1
                if address in watching:
                    print('[watch] %6d  %s' % (seq[0], n))
                lvl = logging.get(address)
                if lvl is not None:
                    s = gstr(uc.reg_read(reg0)) or gstr(uc.reg_read(reg1))
                    if s:
                        print('[game.%s] %s' % (lvl.lower(), s))
        self.uc.hook_add(UC_HOOK_BLOCK, cb, begin=lo, end=hi)
        if self.verbose:
            print('[vm] method trace armed over %#x..%#x (%d methods)'
                  % (lo, hi, len(want)))
        return True

    def guest_string(self, p):
        """Decode an Il2CppString* if `p` plausibly is one, else None."""
        if not p or p & 1:
            return None
        try:
            n = self.read32(p + 0x10)
            if n <= 0 or n > 4096:
                return None
            return self.read(p + 0x14, n * 2).decode('utf-16-le', 'replace')
        except Exception:
            return None

    def recent_methods(self, n=40):
        t = getattr(self, 'method_trace', None)
        if not t:
            return []
        out, last = [], None
        for x in list(t)[-n * 4:]:
            if x != last:
                out.append(x)
                last = x
        return out[-n:]

    def backtrace(self, limit=32):
        """Walk the AArch64 frame-pointer chain."""
        uc = self.uc
        out = ['pc  %#018x  %s' % (uc.reg_read(A64.UC_ARM64_REG_PC),
                                   self.describe(uc.reg_read(A64.UC_ARM64_REG_PC))),
               'lr  %#018x  %s' % (uc.reg_read(A64.UC_ARM64_REG_LR),
                                   self.describe(uc.reg_read(A64.UC_ARM64_REG_LR)))]
        fp = uc.reg_read(A64.UC_ARM64_REG_X29)
        for _ in range(limit):
            if not fp or fp < 0x1000:
                break
            try:
                nfp = self.read64(fp)
                lr = self.read64(fp + 8)
            except Exception:
                break
            if not lr:
                break
            out.append('    %#018x  %s' % (lr, self.describe(lr)))
            if nfp <= fp:
                break
            fp = nfp
        return out

    def probe_class(self, ptr):
        """If `ptr` looks like an Il2CppClass, return 'Namespace.Name'."""
        try:
            if ptr < 0x1000:
                return None
            name = self.cstr(self.read64(ptr + 0x10), 128)
            ns = self.cstr(self.read64(ptr + 0x18), 128)
        except Exception:
            return None
        if not name:
            return None
        try:
            n = name.decode('ascii')
            v = ns.decode('ascii') if ns else ''
        except Exception:
            return None
        ok = lambda t: all(32 < c < 127 for c in t.encode()) if t else True
        if not n or not ok(n) or not ok(v) or len(n) > 120:
            return None
        if not (n[0].isalpha() or n[0] in '_<'):
            return None
        return ('%s.%s' % (v, n)) if v else n

    def probe_object(self, ptr):
        """If `ptr` looks like an Il2CppObject, name its class."""
        try:
            k = self.read64(ptr)
        except Exception:
            return None
        c = self.probe_class(k)
        return ('obj<%s>' % c) if c else None

    def dump_state(self, tag=''):
        uc = self.uc
        print('=== guest state %s ===' % tag, file=sys.stderr)
        for i in range(0, 31, 3):
            row = []
            for j in range(i, min(i + 3, 31)):
                row.append('x%-2d=%#018x' % (j, uc.reg_read(
                    getattr(A64, 'UC_ARM64_REG_X%d' % j))))
            print('  ' + '  '.join(row), file=sys.stderr)
        print('  sp =%#018x  lr =%#018x  pc =%#018x' % (
            uc.reg_read(A64.UC_ARM64_REG_SP), uc.reg_read(A64.UC_ARM64_REG_LR),
            uc.reg_read(A64.UC_ARM64_REG_PC)), file=sys.stderr)
        for j in range(31):
            v = uc.reg_read(getattr(A64, 'UC_ARM64_REG_X%d' % j))
            hint = self.probe_class(v) or self.probe_object(v)
            if hint:
                print('  x%-2d -> %s' % (j, hint), file=sys.stderr)
        for line in self.backtrace():
            print('  bt ' + line, file=sys.stderr)
        if self.blocks:
            print('  last blocks:', file=sys.stderr)
            for a in list(self.blocks)[-24:]:
                print('    %#018x  %s' % (a, self.describe(a)), file=sys.stderr)

    # -------------------------------------------------------------- symbols
    def describe(self, addr):
        if self.symbols is not None:
            for li in self.images:
                if li.base <= addr < li.end:
                    d = self.symbols.describe(addr - li.bias)
                    if d:
                        return d
        for li in self.images:
            if li.base <= addr < li.end:
                s = li.addr_to_sym(addr)
                if s:
                    return '%s!%s+%#x' % (li.name, s[0], addr - s[1])
                return '%s+%#x' % (li.name, addr - li.bias)
        if M.STUB_BASE <= addr < M.STUB_BASE + M.STUB_SIZE:
            return 'stub:%s' % self.stub_name.get(addr & ~7, '?')
        return '?'

    def sym(self, name):
        for li in self.images:
            a = li.resolve(name)
            if a is not None:
                return a
        raise KeyError(name)

    # ------------------------------------------------------------ execution
    def call(self, addr, *args, **kw):
        """Call a guest function with the AAPCS64 integer calling convention.

        Runs in instruction slices so a runaway guest loop can be observed
        and bounded instead of hanging the host.
        """
        uc = self.uc
        floats = kw.get('floats') or []
        budget = kw.get('count', 0) or self.default_budget
        slice_n = kw.get('slice_insns', self.slice_insns)
        watch = kw.get('watch', self.watch)
        saved = [uc.reg_read(r) for r in ARG_REGS]
        saved_sp = uc.reg_read(A64.UC_ARM64_REG_SP)
        saved_lr = uc.reg_read(A64.UC_ARM64_REG_LR)
        for i, v in enumerate(args[:8]):
            uc.reg_write(ARG_REGS[i], v & 0xFFFFFFFFFFFFFFFF)
        for i, v in enumerate(floats[:8]):
            uc.reg_write(FARG_REGS[i], v)
        if uc.reg_read(A64.UC_ARM64_REG_SP) == 0:
            uc.reg_write(A64.UC_ARM64_REG_SP, self.current.sp)
        uc.reg_write(A64.UC_ARM64_REG_LR, RETURN_MAGIC)
        self._stop_reason = None
        pc = addr
        done = 0
        t0 = time.time()
        while True:
            n = slice_n if (slice_n and (not budget or budget - done > slice_n)) \
                else (budget - done if budget else 0)
            try:
                uc.emu_start(pc, RETURN_MAGIC, timeout=0, count=n)
            except UcError as e:
                bad = uc.reg_read(A64.UC_ARM64_REG_PC)
                raise GuestError('%s at pc=%#x (%s)' % (e, bad, self.describe(bad)))
            self.insns += n
            done += n
            pc = uc.reg_read(A64.UC_ARM64_REG_PC)
            if self._stop_reason and self._stop_reason[0] in ('segv', 'brk', 'host-error'):
                raise GuestError('guest fault: %r' % (self._stop_reason,))
            if self._yield_flag:
                self._yield_flag = False
                self._park()
                pc = uc.reg_read(A64.UC_ARM64_REG_PC)
                done = 0
                continue
            if self.exit_code is not None:
                break
            if pc == RETURN_MAGIC or pc == 0:
                break
            if watch:
                print('[vm] ... %s  (%d Minsn, %.0fs)' %
                      (self.describe(pc), done // 1000000, time.time() - t0),
                      file=sys.stderr)
                sys.stderr.flush()
            if budget and done >= budget:
                raise GuestError('instruction budget (%d) exhausted at %#x (%s)' %
                                 (budget, pc, self.describe(pc)))
        ret = uc.reg_read(A64.UC_ARM64_REG_X0)
        for i, v in enumerate(saved):
            uc.reg_write(ARG_REGS[i], v)
        uc.reg_write(A64.UC_ARM64_REG_SP, saved_sp)
        uc.reg_write(A64.UC_ARM64_REG_LR, saved_lr)
        return ret

    def call_double(self, addr, *args, **kw):
        self.call(addr, *args, **kw)
        return self.uc.reg_read(A64.UC_ARM64_REG_D0)

    # ------------------------------------------------------------- threading
    def bootstrap_main_thread(self):
        t = Thread(self, name='main')
        t.state = 'running'
        self.threads.append(t)
        self.main_thread = t
        self.current = t
        self.uc.reg_write(A64.UC_ARM64_REG_SP, t.sp)
        self.uc.reg_write(A64.UC_ARM64_REG_TPIDR_EL0, t.tls_block)
        return t

    # ------------------------------------------------------- green threads
    #
    # Each guest thread is backed by a host Python thread, but only one is
    # ever inside emu_start(): switching saves/restores the Unicorn CPU
    # context and hands a token to the next runnable thread.  This models
    # exactly what a real OS does for the native R36S build, where these
    # become genuine pthreads.

    def spawn(self, entry, arg, name='thread'):
        t = Thread(self, entry, arg, name)
        t.state = 'ready'
        self.threads.append(t)
        t.py = threading.Thread(target=self._thread_body, args=(t,),
                                name='guest-%d' % t.id, daemon=True)
        t.py.start()
        if self.verbose:
            print('[vm] pthread_create -> tid %d entry=%s' %
                  (t.id, self.describe(entry)))
        return t

    def _thread_body(self, t):
        t.ev.wait()
        t.ev.clear()
        uc = self.uc
        uc.reg_write(A64.UC_ARM64_REG_SP, t.sp)
        uc.reg_write(A64.UC_ARM64_REG_TPIDR_EL0, t.tls_block)
        try:
            t.retval = self.call(t.entry, t.arg)
        except BaseException as e:
            if self.verbose:
                print('[vm] tid %d exited: %s' % (t.id, e), file=sys.stderr)
        t.state = 'done'
        self.wake_object(t, all_=True)
        self._hand_off()

    # ------------------------------------------------------------ blocking
    def block_current(self, obj, note='', retry=False):
        """Mark the running thread blocked on `obj` and yield.

        With retry=True the guest re-executes the blocking call after it is
        woken, which gives correct semaphore/futex semantics without having
        to resume execution in the middle of a host function.
        """
        t = self.current
        t.state = 'blocked'
        t.blocked_on = obj
        if self.verbose > 1:
            print('[vm] tid %d blocks on %r %s' % (t.id, obj, note), file=sys.stderr)
        self._yield_flag = True
        self._retry_call = retry
        self.uc.emu_stop()

    def yield_current(self):
        self.current.state = 'ready'
        self._yield_flag = True
        self.uc.emu_stop()

    def wake_object(self, obj, n=1, all_=False):
        woke = 0
        for t in self.threads:
            if t.state == 'blocked' and t.blocked_on == obj:
                t.state = 'ready'
                t.blocked_on = None
                woke += 1
                if not all_ and woke >= n:
                    break
        return woke

    def has_other_runnable(self):
        cur = self.current
        return any(t.state == 'ready' for t in self.threads if t is not cur)

    def thread_report(self):
        return ', '.join('#%d %s%s' % (t.id, t.state,
                                       (' on %r' % (t.blocked_on,)) if t.blocked_on else '')
                         for t in self.threads)

    def _pick_next(self):
        cur = self.current
        order = self.threads
        i = order.index(cur) if cur in order else -1
        for k in range(1, len(order) + 1):
            t = order[(i + k) % len(order)]
            if t.state == 'ready':
                return t
        # Nothing runnable: hand a spurious wake to a *different* blocked
        # thread if there is one (futex / condvar semantics permit this),
        # otherwise resume the caller.
        for k in range(1, len(order) + 1):
            t = order[(i + k) % len(order)]
            if t.state == 'blocked' and t is not cur:
                t.state = 'ready'
                t.blocked_on = None
                self._spurious += 1
                return t
        return None

    def _park(self):
        """Called on the yielding thread: switch out, wait to be resumed."""
        me = self.current
        nxt = self._pick_next()
        if nxt is None or nxt is me:
            self._spurious += 1
            if self._spurious > 20000:
                raise GuestError('all guest threads deadlocked [%s]'
                                 % self.thread_report())
            me.state = 'running'
            return
        self._spurious = 0
        me.ctx = self.uc.context_save()
        self._resume(nxt)
        me.ev.wait()
        me.ev.clear()
        self.current = me
        me.state = 'running'

    def _hand_off(self):
        """Called by a thread that will not run again."""
        nxt = self._pick_next()
        if nxt is not None and nxt is not self.current:
            self._resume(nxt)

    def _resume(self, t):
        self.current = t
        t.state = 'running'
        if t.ctx is not None:
            self.uc.context_restore(t.ctx)
            t.ctx = None
        t.ev.set()

    def run_pending_threads(self, max_steps=0):
        """Let every ready thread make progress (used between frames)."""
        for _ in range(len(self.threads)):
            if not any(t.state == 'ready' for t in self.threads
                       if t is not self.current):
                break
            self.yield_current()

    def init_array(self, li):
        """Read DT_INIT_ARRAY from *relocated* guest memory (file bytes are 0)."""
        from .elfimage import DT_INIT, DT_INIT_ARRAY, DT_INIT_ARRAYSZ
        out = []
        dyn = li.img.dyn
        if dyn.get(DT_INIT):
            out.append(dyn[DT_INIT] + li.bias)
        base = dyn.get(DT_INIT_ARRAY)
        if base:
            n = dyn.get(DT_INIT_ARRAYSZ, 0) // 8
            for i in range(n):
                v = self.read64(li.bias + base + i * 8)
                if v not in (0, 0xFFFFFFFFFFFFFFFF):
                    out.append(v)
        return out

    def run_init_array(self, li):
        for fn in self.init_array(li):
            if self.verbose:
                print('[vm] init %s' % self.describe(fn))
            self.call(fn, 0, 0, 0)


class SwitchOut(Exception):
    """Raised inside a host call that must block the current green thread."""
