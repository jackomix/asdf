"""Minimal AArch64 ELF shared-object loader.

Parses a bionic .so directly out of its bytes (no pyelftools at runtime),
maps its PT_LOAD segments into a guest address space and applies the
dynamic relocations.  Undefined symbols are bound to caller-supplied
trampoline addresses.

The relocation handling here is deliberately identical in behaviour to
bionic's linker for the subset of relocation types Unity's IL2CPP output
actually uses, so the native R36S loader can reuse the same logic.
"""
import struct

# ---------------------------------------------------------------- constants
ET_DYN = 3
PT_LOAD = 1
PT_DYNAMIC = 2

DT_NULL = 0
DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_PLTGOT = 3
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_INIT = 12
DT_FINI = 13
DT_SONAME = 14
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_PLTREL = 20
DT_JMPREL = 23
DT_INIT_ARRAY = 25
DT_FINI_ARRAY = 26
DT_INIT_ARRAYSZ = 27
DT_FINI_ARRAYSZ = 28
DT_GNU_HASH = 0x6FFFFEF5
DT_RELACOUNT = 0x6FFFFFF9
DT_ANDROID_REL = 0x6000000F
DT_ANDROID_RELA = 0x60000011
DT_RELR = 0x6FFFFFF8
DT_RELRSZ = 0x6FFFFFF7

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027
R_AARCH64_TLS_TPREL64 = 1030
R_AARCH64_TLSDESC = 1031
R_AARCH64_IRELATIVE = 1032

SHN_UNDEF = 0

PF_X, PF_W, PF_R = 1, 2, 4


class Sym(object):
    __slots__ = ('name', 'value', 'size', 'info', 'shndx', 'bind', 'stype')

    def __init__(self, name, value, size, info, shndx):
        self.name = name
        self.value = value
        self.size = size
        self.info = info
        self.shndx = shndx
        self.bind = info >> 4
        self.stype = info & 0xF

    @property
    def defined(self):
        return self.shndx != SHN_UNDEF


class ElfImage(object):
    def __init__(self, path, name=None):
        self.path = path
        self.name = name or path.rsplit('/', 1)[-1]
        with open(path, 'rb') as fh:
            self.data = fh.read()
        self._parse()

    # -------------------------------------------------------------- parsing
    def _parse(self):
        d = self.data
        assert d[:4] == b'\x7fELF', 'not an ELF'
        assert d[4] == 2, 'only ELFCLASS64 supported'
        (self.e_type, self.e_machine, _ver, self.e_entry, self.e_phoff,
         self.e_shoff, _flags, _ehsz, self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from(
            '<HHIQQQIHHHHHH', d, 16)
        assert self.e_machine == 183, 'only AArch64 supported (got %d)' % self.e_machine

        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                struct.unpack_from('<IIQQQQQQ', d, off)
            self.phdrs.append(dict(type=p_type, flags=p_flags, offset=p_offset,
                                   vaddr=p_vaddr, filesz=p_filesz,
                                   memsz=p_memsz, align=p_align))

        loads = [p for p in self.phdrs if p['type'] == PT_LOAD]
        self.min_vaddr = min(p['vaddr'] for p in loads) & ~0xFFF
        self.max_vaddr = max(p['vaddr'] + p['memsz'] for p in loads)
        self.span = (self.max_vaddr - self.min_vaddr + 0xFFF) & ~0xFFF

        dynseg = [p for p in self.phdrs if p['type'] == PT_DYNAMIC]
        self.dyn = {}
        self.dyn_list = []
        if dynseg:
            off = dynseg[0]['offset']
            n = dynseg[0]['filesz'] // 16
            for i in range(n):
                tag, val = struct.unpack_from('<qQ', d, off + i * 16)
                if tag == DT_NULL:
                    break
                self.dyn_list.append((tag, val))
                self.dyn[tag] = val

        self.strtab_off = self._v2o(self.dyn.get(DT_STRTAB, 0))
        self.symtab_off = self._v2o(self.dyn.get(DT_SYMTAB, 0))
        self.syment = self.dyn.get(DT_SYMENT, 24)
        self.soname = self._dstr(self.dyn.get(DT_SONAME, 0))
        self.needed = [self._dstr(v) for t, v in self.dyn_list if t == DT_NEEDED]
        self._nsyms = self._count_syms()
        self.symbols = self._read_symbols()
        self.exports = {}
        for s in self.symbols:
            if s.defined and s.name and s.name not in self.exports:
                self.exports[s.name] = s

    def _v2o(self, vaddr):
        for p in self.phdrs:
            if p['type'] != PT_LOAD:
                continue
            if p['vaddr'] <= vaddr < p['vaddr'] + p['filesz']:
                return p['offset'] + (vaddr - p['vaddr'])
        return None

    def _dstr(self, off):
        if self.strtab_off is None:
            return ''
        base = self.strtab_off + off
        end = self.data.index(b'\0', base)
        return self.data[base:end].decode('utf-8', 'replace')

    def _count_syms(self):
        """Derive symbol count from DT_HASH (nchain) or DT_GNU_HASH."""
        if DT_HASH in self.dyn:
            o = self._v2o(self.dyn[DT_HASH])
            _nbucket, nchain = struct.unpack_from('<II', self.data, o)
            return nchain
        if DT_GNU_HASH in self.dyn:
            o = self._v2o(self.dyn[DT_GNU_HASH])
            nbuckets, symoffset, bloom_size, _shift = struct.unpack_from('<IIII', self.data, o)
            buckets_off = o + 16 + bloom_size * 8
            buckets = struct.unpack_from('<%dI' % nbuckets, self.data, buckets_off)
            last = max(buckets) if buckets else symoffset
            if last < symoffset:
                return symoffset
            chain_off = buckets_off + nbuckets * 4
            i = last
            while True:
                h = struct.unpack_from('<I', self.data, chain_off + (i - symoffset) * 4)[0]
                if h & 1:
                    return i + 1
                i += 1
        return 0

    def _read_symbols(self):
        out = []
        if self.symtab_off is None:
            return out
        d = self.data
        for i in range(self._nsyms):
            off = self.symtab_off + i * self.syment
            st_name, st_info, _st_other, st_shndx, st_value, st_size = \
                struct.unpack_from('<IBBHQQ', d, off)
            out.append(Sym(self._dstr(st_name), st_value, st_size, st_info, st_shndx))
        return out

    # ------------------------------------------------------------- loading
    def relocations(self):
        """Yield (r_offset, r_type, r_sym, r_addend) for every reloc."""
        d = self.data
        for tag_off, tag_sz in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if tag_off not in self.dyn:
                continue
            if tag_off == DT_JMPREL and self.dyn.get(DT_PLTREL) != DT_RELA:
                continue
            o = self._v2o(self.dyn[tag_off])
            n = self.dyn[tag_sz] // 24
            for i in range(n):
                r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, o + i * 24)
                yield r_offset, r_info & 0xFFFFFFFF, r_info >> 32, r_addend
        if DT_ANDROID_RELA in self.dyn or DT_RELR in self.dyn:
            raise NotImplementedError('packed/relr relocations not present in this build')

    def init_array(self, load_bias):
        out = []
        if DT_INIT in self.dyn and self.dyn[DT_INIT]:
            out.append(self.dyn[DT_INIT] + load_bias)
        if DT_INIT_ARRAY in self.dyn:
            o = self._v2o(self.dyn[DT_INIT_ARRAY])
            n = self.dyn.get(DT_INIT_ARRAYSZ, 0) // 8
            for i in range(n):
                v = struct.unpack_from('<Q', self.data, o + i * 8)[0]
                if v not in (0, 0xFFFFFFFFFFFFFFFF):
                    out.append(v + load_bias)
        return out


class LoadedImage(object):
    """An ElfImage placed at a concrete guest base address."""

    def __init__(self, img, load_bias):
        self.img = img
        self.bias = load_bias
        self.name = img.name
        self.base = load_bias + img.min_vaddr
        self.end = load_bias + img.max_vaddr

    def resolve(self, name):
        s = self.img.exports.get(name)
        if s is None:
            return None
        return self.bias + s.value

    def addr_to_sym(self, addr):
        best = None
        for s in self.img.symbols:
            if not s.defined or not s.name:
                continue
            v = self.bias + s.value
            if v <= addr and (s.size == 0 or addr < v + s.size):
                if best is None or v > best[1]:
                    best = (s.name, v)
        return best
