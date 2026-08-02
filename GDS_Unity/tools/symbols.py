"""Build a native-address -> managed-method symbol table for libil2cpp.so.

Joins global-metadata.dat (v31) with the Il2CppCodeGenModule tables found
in the shipped binary.  Used to symbolise VM faults and to let the runtime
call any managed method by name.
"""
import bisect
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import il2cpp_meta as MD                                   # noqa: E402
import il2cpp_bin as B                                     # noqa: E402

ALL_ASSEMBLIES = ('Assembly-CSharp.dll', 'KairoLibrary.dll', 'mscorlib.dll',
                  'UnityEngine.CoreModule.dll')


class SymbolTable(object):
    def __init__(self, apk_root, assemblies=ALL_ASSEMBLIES):
        self.meta_path = os.path.join(
            apk_root, 'assets/bin/Data/Managed/Metadata/global-metadata.dat')
        self.so_path = os.path.join(apk_root, 'lib/arm64-v8a/libil2cpp.so')
        self.m = MD.load(self.meta_path)
        self.img = B.Image(self.so_path)
        self.by_addr = {}
        self.by_name = {}
        self.methods = []
        self._images = {}
        for i in range(self.m.count('images')):
            im = self.m.image_def(i)
            self._images[self.m.string(im.nameIndex)] = im
        for a in assemblies:
            if a in self._images:
                try:
                    self._load_assembly(a)
                except AssertionError:
                    pass
        self._sorted = sorted(self.by_addr)

    def _load_assembly(self, asm):
        m, img = self.m, self.img
        im = self._images[asm]
        cands = B.find_codegen_module(img, asm)
        assert cands, 'no codegen module for ' + asm
        pva, cnt, mptrs, _ = cands[0]
        for ti in range(im.typeStart, im.typeStart + im.typeCount):
            t = m.type_def(ti)
            tname = m.type_name(ti)
            for k in range(t.method_count):
                mi = t.methodStart + k
                md = m.method_def(mi)
                rid = (md.token & 0x00FFFFFF) - 1
                addr = img.ptr(mptrs + rid * img.ps) if 0 <= rid < cnt else None
                full = '%s::%s' % (tname, m.string(md.nameIndex))
                rec = dict(asm=asm, type=tname, name=m.string(md.nameIndex),
                           full=full, addr=addr, pcount=md.parameterCount,
                           flags=md.flags, token=md.token, index=mi)
                self.methods.append(rec)
                self.by_name.setdefault(full, []).append(rec)
                if addr:
                    self.by_addr.setdefault(addr, rec)

    # ------------------------------------------------------------------
    def lookup(self, vaddr):
        """Nearest managed method at or below `vaddr`."""
        if not self._sorted:
            return None
        i = bisect.bisect_right(self._sorted, vaddr) - 1
        if i < 0:
            return None
        a = self._sorted[i]
        if vaddr - a > 0x20000:
            return None
        return self.by_addr[a], vaddr - a

    def describe(self, vaddr):
        r = self.lookup(vaddr)
        if not r:
            return None
        rec, off = r
        return '%s+%#x' % (rec['full'], off) if off else rec['full']

    def find(self, pattern):
        pat = pattern.lower()
        return [r for r in self.methods if pat in r['full'].lower()]


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else 'out/apk'
    st = SymbolTable(root)
    print('symbols: %d methods, %d with code' % (len(st.methods), len(st.by_addr)))
    if len(sys.argv) > 2:
        q = sys.argv[2]
        if q.startswith('0x'):
            print(st.describe(int(q, 16)))
        else:
            for r in st.find(q)[:60]:
                print('  %-70s %s' % (r['full'], hex(r['addr']) if r['addr'] else '-'))
