"""Join global-metadata.dat with libil2cpp.so: every managed method -> native address."""
import sys, struct
import os as _os; sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import il2cpp_meta as MD
import il2cpp_bin as B

META = '/tmp/u/apk/assets/bin/Data/Managed/Metadata/global-metadata.dat'
SO = '/tmp/u/apk/lib/arm64-v8a/libil2cpp.so'


def build(assemblies=('Assembly-CSharp.dll', 'KairoLibrary.dll')):
    m = MD.load(META)
    img = B.Image(SO)
    images = {}
    for i in range(m.count('images')):
        im = m.image_def(i)
        images[m.string(im.nameIndex)] = im

    out = {}
    for asm in assemblies:
        im = images[asm]
        cands = B.find_codegen_module(img, asm)
        assert cands, 'no codegen module for ' + asm
        pva, cnt, mptrs, _ = cands[0]
        methods = []
        for ti in range(im.typeStart, im.typeStart + im.typeCount):
            t = m.type_def(ti)
            tname = m.type_name(ti)
            for k in range(t.method_count):
                mi = t.methodStart + k
                md = m.method_def(mi)
                rid = (md.token & 0x00FFFFFF) - 1
                addr = None
                if 0 <= rid < cnt:
                    addr = img.ptr(mptrs + rid * img.ps)
                methods.append(dict(
                    index=mi, rid=rid, type=tname, name=m.string(md.nameIndex),
                    addr=addr, flags=md.flags, pcount=md.parameterCount,
                    token=md.token))
        out[asm] = dict(count=cnt, mptrs=mptrs, methods=methods)
    return m, img, out


if __name__ == '__main__':
    m, img, tabs = build()
    for asm, t in tabs.items():
        ms = t['methods']
        withaddr = [x for x in ms if x['addr']]
        nulls = [x for x in ms if not x['addr']]
        print('%-22s methods=%-6d resolved=%-6d null(abstract//generic)=%d'
              % (asm, len(ms), len(withaddr), len(nulls)))
        addrs = sorted({x['addr'] for x in withaddr})
        print('   address range 0x%x .. 0x%x' % (addrs[0], addrs[-1]))
