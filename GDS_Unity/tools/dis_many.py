"""Dump disassembly for several methods in one metadata load."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import disasm

def main():
    apk = 'out/apk'
    args = sys.argv[1:]
    if args and args[0] == '--apk':
        apk = args[1]; args = args[2:]
    outdir = 'out/dis'
    c = disasm.Code(apk)
    print('[dis] loaded %d methods' % len(c.methods), flush=True)
    for spec in args:
        hits = c.resolve(spec)
        if not hits:
            print('[dis] NO MATCH %s' % spec, flush=True); continue
        fn = os.path.join(outdir, spec.replace('/', '_').replace(':', '.').replace('<','(').replace('>',')') + '.txt')
        with open(fn, 'w') as f:
            for r in hits[:6]:
                if not r['addr']:
                    print('%s : no code' % r['full'], file=f); continue
                print('\n;;; %s  @ %#x  (%d bytes)' % (r['full'], r['addr'], c.extent(r['addr'])), file=f)
                c.disasm(r['addr'], out=f)
        print('[dis] wrote %s (%d hits)' % (fn, len(hits)), flush=True)

main()
