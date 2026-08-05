"""Offline scan: every type in the game assemblies + method-name heuristics."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import il2cpp_meta as MD

apk = sys.argv[1] if len(sys.argv) > 1 else 'out/apk'
m = MD.load(os.path.join(apk, 'assets/bin/Data/Managed/Metadata/global-metadata.dat'))
images = {}
for i in range(m.count('images')):
    im = m.image_def(i); images[m.string(im.nameIndex)] = im

MB_HOOKS = {'Awake','Start','Update','LateUpdate','FixedUpdate','OnGUI','OnEnable',
            'OnDisable','OnDestroy','OnApplicationPause','OnApplicationQuit',
            'OnApplicationFocus','OnRenderImage','OnPostRender','OnPreRender'}
for asm in ('Assembly-CSharp.dll','KairoLibrary.dll'):
    im = images[asm]
    print('=== %s : %d types' % (asm, im.typeCount))
    for ti in range(im.typeStart, im.typeStart+im.typeCount):
        t = m.type_def(ti)
        names = set()
        for k in range(t.method_count):
            names.add(m.string(m.method_def(t.methodStart+k).nameIndex))
        hit = names & MB_HOOKS
        if hit:
            print('  %-52s  %s' % (m.type_name(ti), ' '.join(sorted(hit))))
