"""Locate IL2CPP code-registration tables inside a stripped libil2cpp.so.

ET_DYN ELFs store almost every pointer in .data.rel.ro as an R_AARCH64_RELATIVE
relocation whose *addend* carries the real value, so the raw file bytes there
are zero. We therefore build a relocation map first and read pointers through
it. The Il2CppCodeGenModule for each assembly begins with a pointer to the
module-name C string, which gives us a reliable anchor to find the tables
without any exported symbol.
"""
import struct
from elftools.elf.elffile import ELFFile

R_AARCH64_RELATIVE = 1027
R_ARM_RELATIVE = 23


class Image(object):
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.e = ELFFile(self.f)
        self.is64 = self.e.elfclass == 64
        self.ps = 8 if self.is64 else 4
        self.f.seek(0)
        self.data = self.f.read()
        self._segs = [(s['p_vaddr'], s['p_filesz'], s['p_offset'])
                      for s in self.e.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.relocs = {}
        self._load_relocs()

    # --------------------------------------------------------- addressing
    def off(self, va):
        """virtual address -> file offset (None if not backed by file)"""
        for vaddr, filesz, offset in self._segs:
            if vaddr <= va < vaddr + filesz:
                return offset + (va - vaddr)
        return None

    def _load_relocs(self):
        want = R_AARCH64_RELATIVE if self.is64 else R_ARM_RELATIVE
        for sec in self.e.iter_sections():
            if sec.header['sh_type'] not in ('SHT_RELA', 'SHT_REL'):
                continue
            rela = sec.header['sh_type'] == 'SHT_RELA'
            ent = sec.header['sh_entsize']
            base = sec.header['sh_offset']
            n = sec.header['sh_size'] // ent
            for i in range(n):
                o = base + i * ent
                if self.is64:
                    r_off, r_info = struct.unpack_from('<QQ', self.data, o)
                    rtype = r_info & 0xFFFFFFFF
                    add = struct.unpack_from('<q', self.data, o + 16)[0] if rela else 0
                else:
                    r_off, r_info = struct.unpack_from('<II', self.data, o)
                    rtype = r_info & 0xFF
                    add = struct.unpack_from('<i', self.data, o + 8)[0] if rela else 0
                if rtype == want:
                    self.relocs[r_off] = add

    def ptr(self, va):
        """read a pointer at virtual address `va`, honouring relocations.
        RELA r_offset is itself a virtual address in an ET_DYN image."""
        if va in self.relocs:
            return self.relocs[va]
        o = self.off(va)
        if o is None:
            return None
        if self.is64:
            return struct.unpack_from('<Q', self.data, o)[0]
        return struct.unpack_from('<I', self.data, o)[0]

    def u32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from('<I', self.data, o)[0]

    def cstr(self, va, limit=256):
        o = self.off(va)
        if o is None:
            return None
        end = self.data.find(b'\0', o, o + limit)
        if end < 0:
            return None
        return self.data[o:end].decode('utf-8', 'replace')

    # ------------------------------------------------------------ scanning
    def find_bytes(self, needle):
        out, i = [], self.data.find(needle)
        while i >= 0:
            out.append(i)
            i = self.data.find(needle, i + 1)
        return out

    def va_of_offset(self, off):
        for vaddr, filesz, offset in self._segs:
            if offset <= off < offset + filesz:
                return vaddr + (off - offset)
        return None

    def find_pointers_to(self, target_va):
        """every virtual address whose stored pointer equals target_va"""
        hits = [r_va for r_va, add in self.relocs.items() if add == target_va]
        # also non-relocated (prelinked) pointers stored directly in the file
        pat = struct.pack('<Q' if self.is64 else '<I', target_va)
        for o in self.find_bytes(pat):
            va = self.va_of_offset(o)
            if va is not None and va not in self.relocs:
                hits.append(va)
        return sorted(set(hits))


# ---------------------------------------------------------- CodeGenModule
# struct Il2CppCodeGenModule (Unity 2021+/metadata v29-31)
#   0  const char*                     moduleName
#   8  uint32                          methodPointerCount
#  16  Il2CppMethodPointer*            methodPointers
#  24  uint32                          adjustorThunkCount
#  32  Il2CppTokenAdjustorThunkPair*   adjustorThunks
#  40  int32*                          invokerIndices
#  48  uint32                          reversePInvokeWrapperCount
#  56  ...                             reversePInvokeWrapperIndices
CGM_NAME, CGM_MCOUNT, CGM_MPTRS = 0, 8, 16


def find_codegen_module(img, module_name):
    """Find the Il2CppCodeGenModule whose moduleName is `module_name`."""
    needle = module_name.encode() + b'\0'
    cands = []
    for off in img.find_bytes(needle):
        # must be a standalone string (preceded by NUL)
        if off > 0 and img.data[off - 1] != 0:
            continue
        sva = img.va_of_offset(off)
        if sva is None:
            continue
        for pva in img.find_pointers_to(sva):
            cnt = img.u32(pva + CGM_MCOUNT)
            mp = img.ptr(pva + CGM_MPTRS)
            if cnt and 0 < cnt < 200000 and mp and img.off(mp) is not None:
                cands.append((pva, cnt, mp, sva))
    return cands
