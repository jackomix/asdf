"""Dump the Kairosoft Unity engine surface so the host layer can be written.

Prints the fields/methods of kairo.unity.* (the shipped 'old engine ported
to C#' layer), of the game's forms, and lists every UnityEngine internal
call the build actually contains - that list is the platform contract the
host (browser today, SDL2/GLES on the R36S later) has to satisfy.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import il2cpp_meta as MD                                          # noqa: E402

METHOD_IMPL_INTERNAL_CALL = 0x1000
METHOD_ATTR_STATIC = 0x0010

INTERESTING = [
    ('KairoLibrary.dll', 'kairo.unity.ui', 'Canvas'),
    ('KairoLibrary.dll', 'kairo.unity.ui', 'Image'),
    ('KairoLibrary.dll', 'kairo.unity.ui', 'Graphics'),
    ('KairoLibrary.dll', 'kairo.unity.ui', 'IApplication'),
    ('KairoLibrary.dll', 'kairo.unity.ui', 'SurfaceBase'),
    ('KairoLibrary.dll', 'kairo.unity.ui', 'Font'),
    ('KairoLibrary.dll', 'kairo.unity.system', 'KairoBase'),
    ('KairoLibrary.dll', 'kairo.unity.io', 'RecordStore'),
    ('KairoLibrary.dll', 'kairo.unity.native', 'KairoPlugin'),
    ('KairoLibrary.dll', 'kairo.unity.form', 'FormManagerBase'),
    ('Assembly-CSharp.dll', 'main', 'Main'),
    ('Assembly-CSharp.dll', 'main', 'AppData'),
    ('Assembly-CSharp.dll', 'form', 'BootForm'),
    ('Assembly-CSharp.dll', 'form', 'TitleForm'),
    ('Assembly-CSharp.dll', 'surface', 'GameView'),
    ('Assembly-CSharp.dll', 'surface', 'SurfaceManager'),
]


def main():
    apk = sys.argv[1] if len(sys.argv) > 1 else 'out/apk'
    meta = os.path.join(apk, 'assets/bin/Data/Managed/Metadata/global-metadata.dat')
    m = MD.load(meta)

    images = {}
    for i in range(m.count('images')):
        im = m.image_def(i)
        images[m.string(im.nameIndex)] = im

    # ---------------------------------------------------------- type index
    by_key = {}
    for asm, im in images.items():
        for ti in range(im.typeStart, im.typeStart + im.typeCount):
            t = m.type_def(ti)
            key = (asm, m.string(t.namespaceIndex), m.string(t.nameIndex))
            by_key[key] = (ti, t)

    want = sys.argv[2] if len(sys.argv) > 2 else None
    targets = INTERESTING
    if want:
        targets = [k for k in by_key if want.lower() in
                   ('%s.%s' % (k[1], k[2])).lower()]

    for key in targets:
        if key not in by_key:
            print('!! missing %s' % (key,))
            continue
        ti, t = by_key[key]
        print('\n=== %s.%s   (%s)  fields=%d methods=%d' %
              (key[1], key[2], key[0], t.field_count, t.method_count))
        names = [m.string(m.field_def(fi).nameIndex)
                 for fi in range(t.fieldStart, t.fieldStart + t.field_count)]
        for i in range(0, len(names), 4):
            print('    fields  ' + '  '.join('%-26s' % x for x in names[i:i + 4]))
        mm = []
        for k in range(t.method_count):
            md = m.method_def(t.methodStart + k)
            ps = [m.string(m.param_def(pi).nameIndex)
                  for pi in range(md.parameterStart,
                                  md.parameterStart + md.parameterCount)]
            tag = '*' if md.iflags & METHOD_IMPL_INTERNAL_CALL else ''
            pre = 'S:' if md.flags & METHOD_ATTR_STATIC else ''
            mm.append('%s%s%s(%s)' % (pre, m.string(md.nameIndex), tag, ','.join(ps)))
        for i in range(0, len(mm), 3):
            print('    method  ' + '  '.join('%-34s' % x for x in mm[i:i + 3]))

    # ------------------------------------------------ internal call census
    print('\n=== UnityEngine internal calls present in this build ===')
    icalls = []
    for asm, im in images.items():
        for ti in range(im.typeStart, im.typeStart + im.typeCount):
            t = m.type_def(ti)
            tn = m.type_name(ti)
            for k in range(t.method_count):
                md = m.method_def(t.methodStart + k)
                if md.iflags & METHOD_IMPL_INTERNAL_CALL:
                    icalls.append('%s::%s' % (tn, m.string(md.nameIndex)))
    print('total: %d' % len(icalls))
    groups = {}
    for c in icalls:
        groups.setdefault(c.split('::')[0], []).append(c)
    for g in sorted(groups, key=lambda x: -len(groups[x]))[:40]:
        print('   %-58s %d' % (g, len(groups[g])))


if __name__ == '__main__':
    main()
