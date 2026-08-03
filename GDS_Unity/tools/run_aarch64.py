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
    UC_ARM64_REG_LR, UC_ARM64_REG_X30


PAGE = 0x1000
STACK_TOP = 0x7fff0000
STACK_SZ = 0x100000
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
    # Map the whole image contiguous at CODE_BASE with RWX.
    uc.mem_map(CODE_BASE, span + 2 * PAGE, 7)
    for (p_offset, p_vaddr, p_filesz, p_memsz, p_flags) in segs:
        dst = CODE_BASE + (p_vaddr - minv)
        if p_filesz:
            uc.mem_write(dst, data[p_offset:p_offset + p_filesz])
        # bss already zero (fresh mapping)
    entry = CODE_BASE + (e_entry - minv)

    # Stack
    uc.mem_map(STACK_TOP - STACK_SZ, STACK_SZ + PAGE, 3)
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
    # argc, argv[], NULL, envp NULL
    sp = (STACK_TOP - 256) & ~(0xf)
    uc.mem_write(sp, struct.pack('<Q', len(argv)))
    ap = sp + 8
    for ptr in str_ptrs:
        uc.mem_write(ap, struct.pack('<Q', ptr)); ap += 8
    uc.mem_write(ap, struct.pack('<Q', 0))  # argv NULL
    ap += 8
    uc.mem_write(ap, struct.pack('<Q', 0))  # envp NULL
    uc.reg_write(UC_ARM64_REG_SP, sp)

    return uc, entry, minv, maxv, CODE_BASE, span


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else 'loader/loader_aarch64'
    argv = sys.argv[1:]
    uc, entry, minv, maxv, CODE_BASE, span = load(exe, argv)

    out = []

    def sys_write(uc, port, buf, size):
        if port == 1 or port == 2:
            out.append(uc.mem_read(buf, size).decode('utf-8', 'replace'))
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
        try:
            data = open(name, 'rb').read()
        except Exception as e:
            print('[host openat] %s -> %r' % (name, e), file=sys.stderr)
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

    def svc(uc):
        num = uc.reg_read(UC_ARM64_REG_X8)
        x0, x1, x2, x3, x4, x5 = [uc.reg_read(r) for r in
                                 (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                  UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5)]
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
        elif num == 215:  # access
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 257:  # openat
            uc.reg_write(UC_ARM64_REG_X0, sys_openat(uc, x0, x1, x2, x3))
        elif num == 63:  # read
            uc.reg_write(UC_ARM64_REG_X0, sys_read(uc, x0, x1, x2))
        elif num == 57:  # close
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 29:  # ioctl
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 35:  # nanosleep
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif num == 56:  # clone
            uc.reg_write(UC_ARM64_REG_X0, 0)  # pretend child/no thread
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
            try:
                if not addr or (flags & 0x10):  # MAP_FIXED, or no hint
                    tgt = addr if addr else getattr(uc, '_mmap_brk', 0x500000000)
                    uc.mem_map(tgt, length, prot if prot else 7)
                    uc.reg_write(UC_ARM64_REG_X0, tgt)
                    if not addr:
                        uc._mmap_brk = (tgt + length + 0x1000)
                else:
                    uc.mem_map(addr, length, prot if prot else 7)
                    uc.reg_write(UC_ARM64_REG_X0, addr)
            except Exception as e:
                print('[sys_mmap] %#x %#x -> %r' % (addr, length, e), file=sys.stderr)
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
        if _ic[0] > 400000000:
            print('[bench] instruction cap hit at pc=%#x' % addr, file=sys.stderr)
            uc.emu_stop()

    uc.hook_add(unicorn.UC_HOOK_CODE, _code)

    def _memerr(uc, access, addr, size, value, user):
        print('[unicorn MEM %s @%#x sz %d]' % (
            {0: 'READ', 1: 'WRITE', 2: 'FETCH', 3: 'READ*', 4: 'WRITE*'}.get(access, access), addr, size), file=sys.stderr)

    uc.hook_add(unicorn.UC_HOOK_MEM_INVALID, _memerr)
    print('[bench] entry=%#x minv=%#x maxv=%#x mapped %#x..%#x'
          % (entry, minv, maxv, CODE_BASE, CODE_BASE + span + 2 * PAGE),
          file=sys.stderr)

    try:
        uc.emu_start(entry, entry + 0x7fffffff)
    except SystemExit as e:
        out.append('\n%s\n' % e)
    except Exception as e:
        import traceback
        out.append('\n[unicorn aborted] %r\n' % e)
        out.append(traceback.format_exc())
    sys.stdout.write(''.join(out))


if __name__ == '__main__':
    main()
