"""Find every managed method that branches to a given method.

The player loop we provide has to call the same entry points Unity's own loop
would; knowing which shipped method drives, say, MeshManager.RenderMesh tells
us which callback the loop is still missing rather than guessing at it.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from disasm import Code                                           # noqa: E402


def main():
    args = sys.argv[1:]
    apk = 'out/apk'
    if args and args[0] == '--apk':
        apk, args = args[1], args[2:]
    if not args:
        print('usage: callers.py <method-substring-or-hex-address>')
        return
    c = Code(apk)
    spec = args[0]
    if spec.startswith('0x'):
        targets = {int(spec, 16)}
    else:
        targets = {r['addr'] for r in c.resolve(spec) if r['addr']}
    if not targets:
        print('no such method: %s' % spec)
        return
    print('targets: %s' % ', '.join('%#x %s' % (t, c.name(t))
                                    for t in sorted(targets)))
    seen = []
    for r in c.methods:
        a = r['addr']
        if not a:
            continue
        try:
            hits = [t for t in c.calls(a) if t in targets]
        except Exception:
            continue
        if hits:
            seen.append(r['full'])
    for n in sorted(set(seen)):
        print('  %s' % n)
    print('%d callers' % len(set(seen)))


if __name__ == '__main__':
    main()
