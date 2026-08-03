"""List managed methods (name + native address) matching a pattern."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import symbols as S                                        # noqa: E402


def main():
    apk = 'out/apk'
    args = sys.argv[1:]
    if args and args[0] == '--apk':
        apk, args = args[1], args[2:]
    pat = args[0] if args else ''
    st = S.SymbolTable(apk)
    for r in st.find(pat):
        print('%-12s %-64s %s' % (hex(r['addr']) if r['addr'] else '-',
                                  r['full'], r['asm']))


if __name__ == '__main__':
    main()
