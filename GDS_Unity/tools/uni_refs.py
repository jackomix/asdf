"""Which UnityEngine methods does each kairo/game type actually call?"""
import os, sys, collections
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
import disasm
c = disasm.Code('out/apk')
pref = sys.argv[1] if len(sys.argv)>1 else 'kairo.unity.ui.'
per = collections.Counter()
who = collections.defaultdict(set)
for r in c.methods:
    if not r['addr'] or not r['type'].startswith(pref):
        continue
    for t in c.calls(r['addr']):
        rec, off = c.sym(t)
        if rec and off == 0 and (rec['type'].startswith('UnityEngine') or rec['asm'].startswith('UnityEngine')):
            per[rec['full']] += 1
            who[rec['full']].add(r['full'])
for k, v in per.most_common(200):
    print('%-64s %4d   e.g. %s' % (k, v, sorted(who[k])[0]))
