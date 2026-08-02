"""Static ARM64 disassembly of managed methods inside libil2cpp.so.

Resolves `bl` targets against the full managed symbol table and annotates
adrp/add string references, which is how the IL2CPP generated code names the
internal calls it resolves.  This lets us read the shipped engine's control
flow without running it.
"""
import bisect
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import il2cpp_meta as MD                                   # noqa: E402
import il2cpp_bin as B                                     # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM        # noqa: E402


class Code(object):
    def __init__(self, apk='out/apk', assemblies=None):
        self.m = MD.load(os.path.join(
            apk, 'assets/bin/Data/Managed/Metadata/global-metadata.dat'))
        self.img = B.Image(os.path.join(apk, 'lib/arm64-v8a/libil2cpp.so'))
        self.images = {}
        for i in range(self.m.count('images')):
            im = self.m.image_def(i)
            self.images[self.m.string(im.nameIndex)] = im
        if assemblies is None:
            assemblies = list(self.images)
        self.by_addr = {}
        self.by_name = {}
        self.methods = []
        for a in assemblies:
            try:
                self._load(a)
            except (AssertionError, TypeError):
                pass
        self.addrs = sorted(self.by_addr)
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    def _load(self, asm):
        m, img = self.m, self.img
        im = self.images[asm]
        cands = B.find_codegen_module(img, asm)
        assert cands, 'no codegen module for ' + asm
        pva, cnt, mptrs, _ = cands[0]
        for ti in range(im.typeStart, im.typeStart + im.typeCount):
            t = m.type_def(ti)
            tname = m.type_name(ti)
            for k in range(t.method_count):
                md = m.method_def(t.methodStart + k)
                rid = (md.token & 0x00FFFFFF) - 1
                addr = img.ptr(mptrs + rid * img.ps) if 0 <= rid < cnt else None
                full = '%s::%s' % (tname, m.string(md.nameIndex))
                rec = dict(asm=asm, type=tname, name=m.string(md.nameIndex),
                           full=full, addr=addr, pcount=md.parameterCount,
                           flags=md.flags, iflags=md.iflags, token=md.token)
                self.methods.append(rec)
                self.by_name.setdefault(full, []).append(rec)
                if addr:
                    self.by_addr.setdefault(addr, rec)

    # ------------------------------------------------------------------
    def sym(self, va):
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return None, 0
        a = self.addrs[i]
        if va - a > 0x30000:
            return None, 0
        return self.by_addr[a], va - a

    def name(self, va):
        r, off = self.sym(va)
        if not r:
            return 'sub_%x' % va
        return r['full'] + ('+%#x' % off if off else '')

    def extent(self, addr):
        i = bisect.bisect_right(self.addrs, addr)
        nxt = self.addrs[i] if i < len(self.addrs) else addr + 0x400
        return max(4, min(nxt - addr, 0x8000))

    def find(self, pat):
        p = pat.lower()
        return [r for r in self.methods if p in r['full'].lower()]

    def resolve(self, spec):
        hits = [r for r in self.methods
                if r['full'] == spec or r['full'].endswith('::' + spec)]
        if not hits:
            hits = self.find(spec)
        return hits

    # ------------------------------------------------------------------
    def read(self, va, n):
        o = self.img.off(va)
        return self.img.data[o:o + n] if o is not None else b''

    def cstr_at(self, va):
        return self.img.cstr(va)

    def disasm(self, addr, size=None, out=sys.stdout, depth=0):
        size = size or self.extent(addr)
        code = self.read(addr, size)
        adrp = {}
        for ins in self.md.disasm(code, addr):
            note = ''
            op = ins.mnemonic
            if op == 'adrp':
                r, imm = [x.strip() for x in ins.op_str.split(',')]
                adrp[r] = int(imm.split()[0].lstrip('#'), 0)
            elif op == 'add' and ins.op_str.count(',') == 2:
                d, s, imm = [x.strip() for x in ins.op_str.split(',')]
                if s in adrp and imm.startswith('#'):
                    va = adrp[s] + int(imm[1:], 0)
                    adrp[d] = va
                    s2 = self.cstr_at(va)
                    if s2 and s2.isprintable() and len(s2) > 2:
                        note = '  ; "%s"' % s2[:120]
                    else:
                        note = '  ; =%#x %s' % (va, self.name(va))
            elif op == 'ldr' and '[' in ins.op_str:
                mo = re.match(r'(\w+), \[(\w+), #(0x[0-9a-f]+|\d+)\]', ins.op_str)
                if mo and mo.group(2) in adrp:
                    va = adrp[mo.group(2)] + int(mo.group(3), 0)
                    tgt = self.img.ptr(va)
                    if tgt:
                        note = '  ; [%#x] -> %s' % (va, self.name(tgt))
            elif op in ('bl', 'b') and ins.op_str.startswith('#'):
                tgt = int(ins.op_str[1:], 0)
                note = '  ; %s' % self.name(tgt)
            print('%s%08x  %-8s %s%s' % ('  ' * depth, ins.address, op,
                                         ins.op_str, note), file=out)

    def calls(self, addr, size=None):
        """set of bl targets"""
        size = size or self.extent(addr)
        out = []
        for ins in self.md.disasm(self.read(addr, size), addr):
            if ins.mnemonic == 'bl' and ins.op_str.startswith('#'):
                out.append(int(ins.op_str[1:], 0))
        return out

    def strings_in(self, addr, size=None):
        size = size or self.extent(addr)
        adrp, out = {}, []
        for ins in self.md.disasm(self.read(addr, size), addr):
            if ins.mnemonic == 'adrp':
                r, imm = [x.strip() for x in ins.op_str.split(',')]
                adrp[r] = int(imm.split()[0].lstrip('#'), 0)
            elif ins.mnemonic == 'add' and ins.op_str.count(',') == 2:
                d, s, imm = [x.strip() for x in ins.op_str.split(',')]
                if s in adrp and imm.startswith('#'):
                    va = adrp[s] + int(imm[1:], 0)
                    adrp[d] = va
                    t = self.cstr_at(va)
                    if t and len(t) > 2 and t.isprintable():
                        out.append(t)
        return out


def main():
    ap = sys.argv[1:]
    apk = 'out/apk'
    if ap and ap[0] == '--apk':
        apk = ap[1]; ap = ap[2:]
    c = Code(apk)
    if not ap:
        print('methods: %d (%d with code)' % (len(c.methods), len(c.by_addr)))
        return
    mode = 'dis'
    if ap[0].startswith('--'):
        mode = ap[0][2:]; ap = ap[1:]
    spec = ap[0]
    hits = c.resolve(spec)
    if mode == 'find':
        for r in hits[:80]:
            print('  %-72s %s' % (r['full'], hex(r['addr']) if r['addr'] else '-'))
        return
    for r in hits[:4]:
        if not r['addr']:
            print('%s : no code' % r['full']); continue
        print('\n;;; %s  @ %#x  (%d bytes)' % (r['full'], r['addr'], c.extent(r['addr'])))
        if mode == 'calls':
            seen = []
            for t in c.calls(r['addr']):
                n = c.name(t)
                if n not in seen:
                    seen.append(n)
            for n in seen:
                print('    -> %s' % n)
            for s in c.strings_in(r['addr']):
                print('    s "%s"' % s)
        else:
            c.disasm(r['addr'])


if __name__ == '__main__':
    main()
