"""Run a standalone aarch64-linux ELF under Unicorn (our local test bench).

This is only a *test harness* for the native R36S loader's C code: it lets us
run the exact aarch64 binary the device will run, here, without flashing the
handheld.  It maps PT_LOAD, sets up a stack + argc/argv, points SP, and
emulates from the entry point until exit()/brk.  Output goes through the same
write()/exit() the loader would make on the device.

It is deliberately not the game: there is no GPU, no JNI runtime yet - just
enough to prove the ELF relocation, init-array and a few syscalls work.
"""
import sys
import struct
import unicorn
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, \
    UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, \
    UC_ARM64_REG_X5, UC_ARM64_REG_X8, UC_ARM64_REG_PC, \
    UC_ARM64_REG_LR, UC_ARM64_REG_X30, UC_ARM64_REG_TPIDR_EL0


PAGE = 0x1000
STACK_TOP = 0x7fff0000
STACK_SZ = 0x800000  # 8 MB stack + headroom; IL2CPP runtime threads read just above top
CODE_BASE = 0x400000  # where we load the executable


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def load(path, argv):
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF', 'not ELF'
    # ELF header (minimal parse)
    e_phoff = struct.unpack_from('<Q', data, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', data, 0x36)[0]
    e_phnum = struct.unpack_from('<H', data, 0x38)[0]
    e_entry = struct.unpack_from('<Q', data, 0x18)[0]

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)

    minv = 0xffffffffffffffff
    maxv = 0
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from('<I', data, off)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset = struct.unpack_from('<Q', data, off + 8)[0]
        p_vaddr = struct.unpack_from('<Q', data, off + 16)[0]
        p_filesz = struct.unpack_from('<Q', data, off + 32)[0]
        p_memsz = struct.unpack_from('<Q', data, off + 40)[0]
        segs.append((p_offset, p_vaddr, p_filesz, p_memsz, 0))
        minv = min(minv, p_vaddr)
        maxv = max(maxv, p_vaddr + p_memsz)
    minv &= ~(PAGE - 1)
    maxv = align_up(maxv, PAGE)
    span = maxv - minv
    # Load a -fno-pie ET_EXEC at its OWN link base (minv), not at a made-up
    # CODE_BASE.  The loader is built -fno-pie, so its data section stores
    # *absolute* link-time addresses (e.g. the host_syms tab[] string pointers);
    # those only work if the image is mapped at its natural base.  PC-relative
    # refs would survive an offset, but absolute stored pointers would not.
    BASE = minv
    uc.mem_map(BASE, span + 2 * PAGE, 7)
    for (p_offset, p_vaddr, p_filesz, p_memsz, p_flags) in segs:
        dst = BASE + (p_vaddr - minv)  # == p_vaddr since BASE == minv
        if p_filesz:
            uc.mem_write(dst, data[p_offset:p_offset + p_filesz])
        # bss already zero (fresh mapping)
    entry = BASE + (e_entry - minv)    # == e_entry

    # Stack
    # Map a large flat region covering the stack plus generous headroom above.
    # The IL2CPP GC scans a window above the stack top (up to ~4 MB past the
    # entry SP), so the whole scan range must stay mapped.
    uc.mem_map(STACK_TOP - STACK_SZ, 0x5000000, 3)  # ~80 MB covering stack + headroom
    sp = STACK_TOP
    # Build argv strings at the very top of the stack region.
    arg_bytes = []
    for a in argv:
        b = a.encode() + b'\0'
        arg_bytes.append(b)
    # place strings
    str_ptrs = []
    p = STACK_TOP - 16
    for b in reversed(arg_bytes):
        p -= len(b)
        p &= ~(0xf)
        uc.mem_write(p, b)
        str_ptrs.append(p)
    str_ptrs = str_ptrs[::-1]
    # argc, argv[], NULL, envp NULL.  Leave lots of room: argv strings are
    # placed just below STACK_TOP and grow downward, so if they overflow the
    # argv-array region they'd corrupt the pointers.  Reserve 4 KB.
    sp = (STACK_TOP - 4096) & ~(0xf)
    uc.mem_write(sp, struct.pack('<Q', len(argv)))
    ap = sp + 8
    for ptr in str_ptrs:
        uc.mem_write(ap, struct.pack('<Q', ptr)); ap += 8
    uc.mem_write(ap, struct.pack('<Q', 0))  # argv NULL
    ap += 8
    uc.mem_write(ap, struct.pack('<Q', 0))  # envp NULL
    uc.reg_write(UC_ARM64_REG_SP, sp)
    # aarch64 Linux entry ABI: x0=argc, x1=argv (pointer to the argv array,
    # which begins right after the argc word we just wrote at `sp`).
    argv_ptr = sp + 8
    uc.reg_write(UC_ARM64_REG_X0, len(argv))
    uc.reg_write(UC_ARM64_REG_X1, argv_ptr)

    # TLS: aarch64 reads thread-local storage through tpidr_el0 (the thread
    # pointer).  The real kernel sets it per-thread; libunity.so's init_array
    # does `mrs x19, tpidr_el0; ldr x10, [x19, #0x28]`, so if it's 0 the first
    # TLS access faults.  Map a zeroed TLS block and point tpidr_el0 at it.
    # TLS block that libil2cpp's GC uses to find the current thread's stack
    # bounds.  bionic arm64 TLS layout (offsets in 8-byte slots):
    #   slot 1  = thread id
    #   slot 5  = stack guard (magic the GC checks)
    # We also store the stack base (lo) and size so pthread_attr_getstack /
    # pthread_getattr_np (which we stub) can return real bounds, letting the
    # GC's stack scan stay within the mapped region instead of walking off it.
    tls_base = 0x7f7e0000  # just below the stack region
    uc.mem_map(tls_base, 0x10000, 7)
    uc.mem_write(tls_base, struct.pack('<Q', 0))          # slot 0
    uc.mem_write(tls_base + 8, struct.pack('<Q', 1))      # slot 1: thread id
    uc.mem_write(tls_base + 40, struct.pack('<Q', 0x0BADC0DEDEADBEEF))  # slot 5: stack guard
    # slot 6/7: stack lo, stack hi (so thread stack lookup works)
    uc.mem_write(tls_base + 48, struct.pack('<Q', STACK_TOP - STACK_SZ))
    uc.mem_write(tls_base + 56, struct.pack('<Q', STACK_TOP))
    uc.reg_write(UC_ARM64_REG_TPIDR_EL0, tls_base)
    uc._tls_base = tls_base

    return uc, entry, minv, maxv, BASE, span, len(argv), argv_ptr


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else 'loader/loader_aarch64'
    argv = sys.argv[1:]
    uc, entry, minv, maxv, BASE, span, _argc, _argv_ptr = load(exe, argv)

    out = []

    def sys_write(uc, port, buf, size):
        if port == 1 or port == 2:
            s = uc.mem_read(buf, size).decode('utf-8', 'replace')
            out.append(s)
            sys.stdout.write(s); sys.stdout.flush()
        return size

    def sys_exit(uc, code):
        raise SystemExit('[guest exit %d]' % code)

    def sys_brk(uc, addr):
        # trivial brk: return a fixed heap pointer, ignore growth
        return 0x60000000

    def sys_fstat(uc, fd, buf):
        # minimal: st_size=0, regular file
        uc.mem_write(buf, struct.pack('<Q', 0) * 4 + b'\0' * 40)
        return 0

    def sys_openat(uc, dirfd, pathbuf, flags, mode):
        name = uc.mem_read(pathbuf, 4096).split(b'\0')[0].decode('utf-8', 'replace')
        # resolve relative to the repo root and to the loader's own directory
        import os
        repo = os.path.dirname(os.path.abspath(__file__)) + '/..'
        loader_dir = os.path.dirname(os.path.abspath(exe))
        cand = [name,
                os.path.join(repo, name),
                os.path.join(loader_dir, name),
                os.path.join(loader_dir, name[3:] if name.startswith('data/') else name)]
        for n in cand:
            try:
                data = open(n, 'rb').read()
                break
            except Exception:
                data = None
        if data is None:
            print('[host openat] %s -> not found' % name, file=sys.stderr)
            return -2
        import unicorn
        g = uc._guest_files = getattr(uc, '_guest_files', {})
        base = getattr(uc, '_guest_file_brk', 0xB0000000)
        span = (len(data) + 0x1000) & ~0xfff
        uc.mem_map(base, span + 0x2000, unicorn.UC_PROT_ALL)
        uc.mem_write(base, data)
        fd = max(g.keys(), default=99) + 1
        g[fd] = (base, len(data), 0)  # base, total, offset
        uc._guest_file_brk = base + span + 0x2000
        print('[host openat] %s -> fd %d, %d bytes @ %#x' % (name, fd, len(data), base), file=sys.stderr)
        return fd

    def sys_fstatat(uc, dirfd, pathbuf, buf, flags):
        g = getattr(uc, '_guest_files', {})
        # size lives at st_size; for aarch64 stat64 st_size is at offset 48
        sz = struct.pack('<Q', 0)
        for fd, (base, total, off) in g.items():
            try:
                uc.mem_write(buf + 48, struct.pack('<Q', total))
                return 0
            except Exception:
                pass
        return -1

    def sys_read(uc, fd, buf, size):
        g = getattr(uc, '_guest_files', {})
        if fd in g:
            base, total, off = g[fd]
            if off >= total:
                return 0
            n = min(size, total - off)
            try:
                raw = uc.mem_read(base + off, n)
                chunk = bytes(raw)
                uc.mem_write(buf, chunk)
                g[fd] = (base, total, off + n)
                return n
            except Exception as e:
                print('[sys_read] n=%d buf=%r type(chunk)=%s err=%r' % (n, buf, type(chunk).__name__, e), file=sys.stderr)
                return -1
        return -1

    def sys_lseek(uc, fd, off, whence):
        # whence: 0=SET, 1=CUR, 2=END.  Needed so read_all() can size a file
        # (SEEK_END) then rewind (SEEK_SET:0) before reading it whole.
        g = getattr(uc, '_guest_files', {})
        if fd not in g:
            return -1
        base, total, cur = g[fd]
        if whence == 2:       # END
            new = total + off
        elif whence == 1:     # CUR
            new = cur + off
        else:                 # SET
            new = off
        if new < 0:
            return -1
        g[fd] = (base, total, new)
        return new

    def svc(uc):
        num = uc.reg_read(UC_ARM64_REG_X8)
        x0, x1, x2, x3, x4, x5 = [uc.reg_read(r) for r in
                                 (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                  UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5)]
        if num != 64:  # don't spam on every write
            print('[svc %d] x0=%#x x1=%#x x2=%#x pc=%#x' % (num, x0, x1, x2, uc.reg_read(UC_ARM64_REG_PC)), file=sys.stderr)
        if num == 64:  # write
            uc.reg_write(UC_ARM64_REG_X0, sys_write(uc, x0, x1, x2))
        elif num == 93:  # exit
            sys_exit(uc, x0)
        elif num == 214:  # brk
            uc.reg_write(UC_ARM64_REG_X0, sys_brk(uc, x0))
        elif num == 78:  # getpid
            uc.reg_write(UC_ARM64_REG_X0, 1)
        elif num == 172:  # getpid (alias)
            uc.reg_write(UC_ARM64_REG_X0, 1)
        elif num == 79 or num == 179:  # getuid
            uc.reg_write(UC_ARM64_REG_X0, 1000)
        elif num == 165:  # gettid
            uc.reg_write(UC_ARM64_REG_X0, 100)
        elif num == 96:  # set_tid_address
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 113:  # set_robust_list
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 215:  # munmap (aarch64; no-op in bench - memory freed is reclaimed)
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 257 or num == 56:  # openat (56=aarch64, 257=x86_64 legacy)
            uc.reg_write(UC_ARM64_REG_X0, sys_openat(uc, x0, x1, x2, x3))
        elif num == 63:  # read
            uc.reg_write(UC_ARM64_REG_X0, sys_read(uc, x0, x1, x2))
        elif num == 62:  # lseek
            uc.reg_write(UC_ARM64_REG_X0, sys_lseek(uc, x0, x1, x2))
        elif num == 57:  # close
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 29:  # ioctl
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 35:  # nanosleep
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 122:  # fstatat
            uc.reg_write(UC_ARM64_REG_X0, sys_fstatat(uc, x0, x1, x2, x3))
        elif num == 174:  # rt_sigprocmask
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 124:  # sched_yield
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 98:  # futex
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 131:  # sigaltstack
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 134:  # rt_sigaction
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 160:  # uname
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 278:  # getrandom
            uc.reg_write(UC_ARM64_REG_X0, x2)
        elif num == 222:  # mmap (guest calling svc directly, e.g. the loader's heap)
            addr, length, prot, flags, fd, off = (x0, x1, x2, x3, x4, x5)
            prot = prot if prot else 7
            # The GC reserves a 4 GB managed heap.  Unicorn can't back 4 GB of
            # real RAM, so map the first page now and record a SPARSE region;
            # the memory-fault hook maps the rest lazily (like the real kernel
            # committing pages on demand).  Only do this for large maps; small
            # ones map eagerly.
            SPARSE_THRESH = 1024 * 1024 * 1024  # 1 GB: only huge GC reservations go sparse
            def _do_map(base, ln):
                if ln <= SPARSE_THRESH:
                    uc.mem_map(base, ln, prot)
                    return False
                uc.mem_map(base, PAGE, prot)   # map one page, rest sparse
                sp = getattr(uc, '_sparse', None)
                if sp is None:
                    sp = uc._sparse = []
                sp.append((base, base + ln, prot))
                return True
            try:
                if not addr or (flags & 0x10):  # MAP_FIXED, or no hint
                    tgt = addr if addr else getattr(uc, '_mmap_brk', 0x500000000)
                    _do_map(tgt, length)
                    uc.reg_write(UC_ARM64_REG_X0, tgt)
                    if not addr:
                        uc._mmap_brk = (tgt + length + 0x1000)
                else:
                    _do_map(addr, length)
                    uc.reg_write(UC_ARM64_REG_X0, addr)
            except Exception as e:
                # retry at a higher base once in case of overlap
                try:
                    tgt = getattr(uc, '_mmap_brk', 0x1000000000)
                    _do_map(tgt, length)
                    uc.reg_write(UC_ARM64_REG_X0, tgt)
                    uc._mmap_brk = tgt + length + 0x1000
                except Exception as e2:
                    print('[sys_mmap] %#x %#x -> %r / %r' % (addr, length, e, e2), file=sys.stderr)
                    uc.reg_write(UC_ARM64_REG_X0, -1)
        elif num == 226:  # mprotect
            try:
                uc.mem_protect(x0, x1, x2 if x2 else 7)
                uc.reg_write(UC_ARM64_REG_X0, 0)
            except Exception as e:
                print('[sys_mprotect] %#x %#x -> %r' % (x0, x1, e), file=sys.stderr)
                uc.reg_write(UC_ARM64_REG_X0, -1)
        else:
            print('[unicorn] UNHANDLED svc %d (x0=%#x x1=%#x x2=%#x)' % (num, x0, x1, x2), file=sys.stderr)
            uc.reg_write(UC_ARM64_REG_X0, -1)

    uc.hook_add(unicorn.UC_HOOK_INTR, lambda u, intno, u2: svc(u))

    # progress / spin detector: sample PC every N instructions and bail.
    _ic = [0]

    def _code(uc, addr, size, data):
        _ic[0] += 1
        if _ic[0] % 20000000 == 0:
            print('[bench] %dM instrs, pc=%#x' % (_ic[0] // 1000000, addr), file=sys.stderr)
        if _ic[0] > 2000000000:
            print('[bench] instruction cap hit at pc=%#x' % addr, file=sys.stderr)
            uc.emu_stop()

    uc.hook_add(unicorn.UC_HOOK_CODE, _code)

    # probe: log when exported mono_class_get_checked (libil2cpp+0xce5190) is
    # entered with x0==NULL, to catch who passes the NULL class.
    def _mcgc(uc, addr, size, data):
        from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30
        x0 = uc.reg_read(UC_ARM64_REG_X0)
        if x0 == 0:
            print('[mcgc] mono_class_get_checked(NULL) caller(x30)=%#x' % (
                uc.reg_read(UC_ARM64_REG_X30)), file=sys.stderr)
    uc.hook_add(unicorn.UC_HOOK_CODE, _mcgc, begin=0x200ce5190, end=0x200ce5194)

    # probe: log calls into libil2cpp+0xd11c14 (calls the crashing 0xcfcccc)
    # with their x0 (class) and x30 (caller) to trace the NULL origin.
    def _d11c14(uc, addr, size, data):
        from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30
        print('[d11c14] entered x0=%#x caller(x30)=%#x' % (
            uc.reg_read(UC_ARM64_REG_X0), uc.reg_read(UC_ARM64_REG_X30)), file=sys.stderr)
    uc.hook_add(unicorn.UC_HOOK_CODE, _d11c14, begin=0x200d11c14, end=0x200d11c18)

    def _memerr(uc, access, addr, size, value, user):
        # Lazy commit for sparse regions (GC heap reservation): if the faulting
        # address lies inside a sparse range, map a CHUNK covering the whole
        # access (plus margin) now and let the guest retry.  Zero pages are
        # fine (bss-like).  Mapping a chunk avoids single-page re-entry storms
        # on large heap writes.
        sp = getattr(uc, '_sparse', None)
        if sp:
            for (b, e, pr) in sp:
                if b <= addr < e:
                    try:
                        start = addr & ~(PAGE - 1)
                        # map up to 4 MB at once (clamp to the region)
                        chunk = 4 * 1024 * 1024
                        ln = min(chunk, e - start)
                        ln = max(ln, PAGE)
                        uc.mem_map(start, ln, pr)
                        return True   # handle the fault: retry the access
                    except Exception:
                        break
        print('[unicorn MEM %s @%#x sz %d pc=%#x]' % (
            {0: 'READ', 1: 'WRITE', 2: 'FETCH', 3: 'READ*', 4: 'WRITE*'}.get(access, access), addr, size,
            uc.reg_read(UC_ARM64_REG_PC)), file=sys.stderr)
        # dump full regs + frame-pointer backtrace
        try:
            from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6,
                UC_ARM64_REG_X7, UC_ARM64_REG_X8, UC_ARM64_REG_X9, UC_ARM64_REG_X10,
                UC_ARM64_REG_X11, UC_ARM64_REG_X12, UC_ARM64_REG_X13, UC_ARM64_REG_X14,
                UC_ARM64_REG_X15, UC_ARM64_REG_X16, UC_ARM64_REG_X17, UC_ARM64_REG_X18,
                UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22,
                UC_ARM64_REG_X23, UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26,
                UC_ARM64_REG_X27, UC_ARM64_REG_X28, UC_ARM64_REG_X29, UC_ARM64_REG_X30,
                UC_ARM64_REG_SP)
            regs = [uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6,
                UC_ARM64_REG_X7, UC_ARM64_REG_X8, UC_ARM64_REG_X9, UC_ARM64_REG_X10,
                UC_ARM64_REG_X11, UC_ARM64_REG_X12, UC_ARM64_REG_X13, UC_ARM64_REG_X14,
                UC_ARM64_REG_X15, UC_ARM64_REG_X16, UC_ARM64_REG_X17, UC_ARM64_REG_X18,
                UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22,
                UC_ARM64_REG_X23, UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26,
                UC_ARM64_REG_X27, UC_ARM64_REG_X28)]
            print('   regs: ' + ' '.join('x%d=%#x'%(i,v) for i,v in enumerate(regs)), file=sys.stderr)
            print('   x29(fc)=%#x x30(lr)=%#x sp=%#x' % (
                uc.reg_read(UC_ARM64_REG_X29), uc.reg_read(UC_ARM64_REG_X30),
                uc.reg_read(UC_ARM64_REG_SP)), file=sys.stderr)
            # walk frame-pointer chain: fp -> [fp] = prev fp, [fp+8] = saved lr
            fp = uc.reg_read(UC_ARM64_REG_X29); lr0 = uc.reg_read(UC_ARM64_REG_X30)
            print('   backtrace: [pc=%#x] -> lr=%#x' % (uc.reg_read(UC_ARM64_REG_PC), lr0), file=sys.stderr)
            for depth in range(16):
                try:
                    prevfp = uc.mem_read(fp, 8); savlr = uc.mem_read(fp + 8, 8)
                    prevfp = int.from_bytes(prevfp, 'little'); savlr = int.from_bytes(savlr, 'little')
                    print('     #%d fp=%#x saved_lr=%#x' % (depth + 1, prevfp, savlr), file=sys.stderr)
                    if prevfp == 0 or prevfp == fp: break
                    fp = prevfp
                except Exception:
                    break
        except Exception:
            pass
        return False

    uc.hook_add(unicorn.UC_HOOK_MEM_INVALID, _memerr)
    print('[bench] entry=%#x minv=%#x maxv=%#x mapped %#x..%#x'
          % (entry, minv, maxv, BASE, BASE + span + 2 * PAGE),
          file=sys.stderr)

    try:
        uc.emu_start(entry, entry + 0x7fffffff)
    except SystemExit as e:
        sys.stdout.write('\n%s\n' % e); sys.stdout.flush()
    except Exception as e:
        import traceback
        sys.stdout.write('\n[unicorn aborted] %r\n' % e); sys.stdout.flush()
        sys.stdout.write(traceback.format_exc()); sys.stdout.flush()


if __name__ == '__main__':
    main()
