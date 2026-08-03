"""List the declared fields of a managed type (metadata only, no VM)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import il2cpp_meta as MD                                          # noqa: E402


def main():
    apk = 'out/apk'
    args = sys.argv[1:]
    if args and args[0] == '--apk':
        apk, args = args[1], args[2:]
    pat = args[0] if args else ''
    m = MD.load(os.path.join(
        apk, 'assets/bin/Data/Managed/Metadata/global-metadata.dat'))
    for ti in range(m.count('typeDefinitions')):
        name = m.type_name(ti)
        if pat.lower() not in name.lower():
            continue
        t = m.type_def(ti)
        print('%s  (%d fields, %d methods)' % (name, t.field_count,
                                               t.method_count))
        for k in range(t.field_count):
            f = m.field_def(t.fieldStart + k)
            print('    %3d  %s' % (k, m.string(f.nameIndex)))


if __name__ == '__main__':
    main()
