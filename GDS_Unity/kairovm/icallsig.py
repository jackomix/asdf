"""Recover the IL2CPP internal-call contract straight out of libil2cpp.so.

Unity compiles every `extern` (internal call) method body into a stub that
resolves its native implementation by *name* at first use:

    static Input_GetKeyInt_ftn f;
    if (!f) f = (Input_GetKeyInt_ftn)il2cpp_codegen_resolve_icall(
                    "UnityEngine.Input::GetKeyInt(UnityEngine.KeyCode)");
    return f(k);

Those signature strings are still in the shipped binary, and
`il2cpp_add_internal_call()` is exported, so the complete platform surface the
game can ever ask for is enumerable ahead of time.  This module extracts it and
works out, from the AArch64 procedure call standard, which register each
argument arrives in.

The same table drives the emulator host (kairovm/unity.py) and the native
ARM64 host (native/), so both speak exactly the same contract.
"""
import os
import re

# a full IL2CPP icall name: Namespace.Type::Method(ArgType,ArgType)
ICALL_RE = re.compile(
    r'\A[A-Za-z_][\w.`<>+/\[\]]*::[\w.`<>+/\[\]]+\(.*\)\Z')
PRINTABLE = re.compile(rb'[\x20-\x7e]{6,300}')


def scan_binary(path):
    """Every internal-call signature string present in the image."""
    data = open(path, 'rb').read()
    out = set()
    for mo in PRINTABLE.finditer(data):
        s = mo.group()
        if b'::' not in s or not s.endswith(b')'):
            continue
        try:
            t = s.decode('ascii')
        except UnicodeDecodeError:
            continue
        if ICALL_RE.match(t):
            out.add(t)
    return sorted(out)


def split_args(argstr):
    """Split a parameter list on top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in argstr:
        if ch in '<[(':
            depth += 1
        elif ch in '>])':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


FLOAT_TYPES = {'System.Single'}
DOUBLE_TYPES = {'System.Double'}


def arg_kind(t):
    """'f' single, 'd' double, 'i' integer/pointer register."""
    t = t.strip()
    if t.endswith('&') or t.endswith('*'):
        return 'i'
    if t in FLOAT_TYPES:
        return 'f'
    if t in DOUBLE_TYPES:
        return 'd'
    return 'i'


class Sig(object):
    """One internal call, with its AAPCS64 register assignment."""

    __slots__ = ('raw', 'type', 'method', 'args', 'kinds', 'static', 'slots')

    def __init__(self, raw, static=None):
        head, _, rest = raw.partition('(')
        self.raw = raw
        self.type, _, self.method = head.partition('::')
        self.args = split_args(rest[:-1]) if rest.endswith(')') else []
        self.kinds = [arg_kind(a) for a in self.args]
        self.static = True if static is None else static
        self.slots = None
        self.assign()

    @property
    def short(self):
        return '%s::%s' % (self.type, self.method)

    def assign(self):
        """Map each argument to ('x', n) or ('v', n) per AAPCS64."""
        x = 0 if self.static else 1        # x0 = this for instance calls
        v = 0
        slots = []
        for k in self.kinds:
            if k == 'i':
                slots.append(('x', x)); x += 1
            else:
                slots.append((k, v)); v += 1
        self.slots = slots
        return slots

    def __repr__(self):
        return '<Sig %s static=%s slots=%r>' % (self.raw, self.static, self.slots)


# --------------------------------------------------------------------------
METHOD_IMPL_INTERNAL_CALL = 0x1000
METHOD_ATTR_STATIC = 0x0010


def metadata_index(meta):
    """(type, method, argc) -> (is_static, method_def) for every icall."""
    idx = {}
    for i in range(meta.count('images')):
        im = meta.image_def(i)
        for ti in range(im.typeStart, im.typeStart + im.typeCount):
            t = meta.type_def(ti)
            tn = meta.type_name(ti)
            for k in range(t.method_count):
                md = meta.method_def(t.methodStart + k)
                if not (md.iflags & METHOD_IMPL_INTERNAL_CALL):
                    continue
                key = (tn, meta.string(md.nameIndex), md.parameterCount)
                idx[key] = (bool(md.flags & METHOD_ATTR_STATIC), md)
    return idx


def load(apk_root, meta=None):
    """All icall signatures in the build, with static-ness from the metadata."""
    so = os.path.join(apk_root, 'lib/arm64-v8a/libil2cpp.so')
    names = scan_binary(so)
    idx = {}
    if meta is not None:
        idx = metadata_index(meta)
    sigs = []
    for n in names:
        s = Sig(n)
        hit = idx.get((s.type, s.method, len(s.args)))
        if hit is not None:
            s.static = hit[0]
            s.assign()
        elif idx:
            # An instance icall reached through its declaring type's name
            # variant (nested/generic spelling) - fall back to arity match.
            cands = [v for (tt, mm, ac), v in idx.items()
                     if mm == s.method and tt.split('.')[-1] == s.type.split('.')[-1]]
            if len(cands) == 1:
                s.static = cands[0][0]
                s.assign()
        sigs.append(s)
    return sigs


if __name__ == '__main__':
    import sys
    root = sys.argv[1] if len(sys.argv) > 1 else 'out/apk'
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    '..', 'tools'))
    import il2cpp_meta as MD
    meta = MD.load(os.path.join(
        root, 'assets/bin/Data/Managed/Metadata/global-metadata.dat'))
    sigs = load(root, meta)
    print('%d internal calls' % len(sigs))
    want = sys.argv[2] if len(sys.argv) > 2 else ''
    for s in sigs:
        if want.lower() in s.raw.lower():
            print('  %-100s %s %r' % (s.raw, 'static' if s.static else 'inst ',
                                      s.slots))
