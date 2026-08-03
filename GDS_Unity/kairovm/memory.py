"""Guest address-space management for the KairoVM ARM64 machine.

This module owns the emulated 64-bit address space: page mapping, an
mmap allocator and a malloc heap.  Everything here is deliberately
mechanical so that the *same* layout decisions can be reproduced by the
native ARM64 loader (see ../native/), where mmap/malloc are simply the
host's own implementations.

Layout (guest virtual addresses)
--------------------------------
  0x0000_0000_0000_0000  null guard page (unmapped)
  0x0000_0000_1000_0000  STUB   - one 8-byte trampoline per imported symbol
  0x0000_0000_2000_0000  ENV    - argv/envp/auxv, FILE structs, errno, misc
  0x0000_0000_4000_0000  LIB    - loaded ELF images (libil2cpp.so ...)
  0x0000_0000_8000_0000  STACK  - one 8 MiB stack per emulated thread
  0x0000_0001_0000_0000  HEAP   - malloc arena
  0x0000_0002_0000_0000  MMAP   - anonymous + file mmap arena (Boehm GC lives here)
"""

STUB_BASE = 0x10000000
STUB_SIZE = 0x00100000

ENV_BASE = 0x20000000
ENV_SIZE = 0x00400000

LIB_BASE = 0x40000000
LIB_SIZE = 0x20000000

STACK_BASE = 0x80000000
STACK_SIZE = 0x00800000          # 8 MiB per thread
STACK_GUARD = 0x00010000

HEAP_BASE = 0x100000000
HEAP_SIZE = 0x100000000          # 4 GiB of address space, mapped lazily

MMAP_BASE = 0x200000000
MMAP_SIZE = 0x200000000

# host-built managed objects (strings/arrays the platform layer hands to the
# game).  Kept above the mmap arena so the two can never collide.
UNITY_BASE = 0x400000000
UNITY_SIZE = 0x040000000

PAGE = 0x1000

PROT_READ = 1
PROT_WRITE = 2
PROT_EXEC = 4


def page_down(x):
    return x & ~(PAGE - 1)


def page_up(x):
    return (x + PAGE - 1) & ~(PAGE - 1)


class AddressSpace(object):
    """Tracks which guest pages are mapped and mirrors them into Unicorn."""

    def __init__(self, uc):
        self.uc = uc
        self.mapped = {}          # page-aligned base -> size
        self._ends = []           # sorted list of (start, end)

    # ------------------------------------------------------------- mapping
    def is_mapped(self, addr, size=1):
        a = page_down(addr)
        end = page_up(addr + size)
        while a < end:
            if a not in self._page_owner():
                return False
            a += PAGE
        return True

    def _page_owner(self):
        # Lazily materialised set of mapped pages.  Cheap enough: we map in
        # big chunks so the set stays small relative to total memory.
        if getattr(self, '_pages', None) is None:
            self._pages = set()
        return self._pages

    def map(self, addr, size, prot=PROT_READ | PROT_WRITE):
        base = page_down(addr)
        end = page_up(addr + size)
        pages = self._page_owner()
        # Map only the runs that are not already present.
        run_start = None
        a = base
        while a <= end:
            present = (a in pages) or a >= end
            if not present and run_start is None:
                run_start = a
            elif present and run_start is not None:
                self.uc.mem_map(run_start, a - run_start, prot)
                for p in range(run_start, a, PAGE):
                    pages.add(p)
                run_start = None
            a += PAGE
        return base

    def protect(self, addr, size, prot):
        base = page_down(addr)
        end = page_up(addr + size)
        if end > base:
            try:
                self.uc.mem_protect(base, end - base, prot)
            except Exception:
                pass

    def ensure(self, addr, size):
        """Map anything in [addr, addr+size) that is not mapped yet."""
        self.map(addr, size)


class Arena(object):
    """A simple bump/segregated-free-list allocator over a guest region."""

    #                16   32   48   64   96  128  192  256  384  512
    CLASSES = (16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
               1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384)

    def __init__(self, space, base, limit, name='heap'):
        self.space = space
        self.base = base
        self.cur = base
        self.limit = base + limit
        self.name = name
        self.free = {c: [] for c in self.CLASSES}
        self.big_free = []                # (size, addr) for > largest class
        self.sizes = {}                   # addr -> usable size
        self.total = 0

    def _class_for(self, n):
        for c in self.CLASSES:
            if n <= c:
                return c
        return None

    def _raw(self, size, align):
        addr = (self.cur + align - 1) & ~(align - 1)
        end = addr + size
        if end > self.limit:
            raise MemoryError('%s arena exhausted' % self.name)
        self.space.ensure(addr, size)
        self.cur = end
        self.total += size
        return addr

    def alloc(self, size, align=16):
        if size <= 0:
            size = 1
        cls = self._class_for(size)
        if cls is not None and align <= 16:
            pool = self.free[cls]
            if pool:
                addr = pool.pop()
                self.sizes[addr] = cls
                return addr
            addr = self._raw(cls, 16)
            self.sizes[addr] = cls
            return addr
        # Large allocation: look for an exact-ish reusable block.
        need = (size + 4095) & ~4095
        for i, (sz, addr) in enumerate(self.big_free):
            if sz >= need and (addr & (align - 1)) == 0 and sz <= need * 2:
                self.big_free.pop(i)
                self.sizes[addr] = sz
                return addr
        addr = self._raw(need, max(align, 16))
        self.sizes[addr] = need
        return addr

    def free_ptr(self, addr):
        if addr == 0:
            return
        sz = self.sizes.pop(addr, None)
        if sz is None:
            return                        # double free / foreign pointer
        if sz in self.free:
            self.free[sz].append(addr)
        else:
            self.big_free.append((sz, addr))

    def usable(self, addr):
        return self.sizes.get(addr, 0)


class MmapArena(object):
    """Page-granular allocator used for mmap()/munmap()."""

    def __init__(self, space, base, limit):
        self.space = space
        self.base = base
        self.cur = base
        self.limit = base + limit
        self.regions = {}                 # addr -> size

    def alloc(self, size, prot=PROT_READ | PROT_WRITE):
        size = page_up(size)
        addr = self.cur
        if addr + size > self.limit:
            raise MemoryError('mmap arena exhausted')
        self.cur = addr + size
        self.space.map(addr, size, prot)
        self.regions[addr] = size
        return addr

    def alloc_at(self, addr, size, prot=PROT_READ | PROT_WRITE):
        size = page_up(size)
        self.space.map(addr, size, prot)
        self.regions[addr] = size
        return addr

    def free(self, addr, size):
        # We intentionally do not unmap: IL2CPP/Boehm reuse patterns are
        # benign and keeping pages mapped avoids Unicorn map churn.
        self.regions.pop(addr, None)
