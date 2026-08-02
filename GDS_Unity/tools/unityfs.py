"""Reader for Unity SerializedFiles (the format under assets/bin/Data).

The port replaces libunity.so, so it also has to replace Unity's asset
loading: `Resources.Load()` has to find the real objects the game shipped.
This module parses the serialized files (version 22, Unity 2022.3) well
enough to resolve the ResourceManager container and read the object types
the game actually asks for - TextAsset, Texture2D, AudioClip.

No type trees are present in a player build, so the field layouts of those
few classes are spelled out below, which is exactly what Unity's own
generated readers do.
"""
import os
import struct
import sys


class Reader(object):
    def __init__(self, data, pos=0, little=True):
        self.d = data
        self.p = pos
        self.e = '<' if little else '>'

    def align(self, n=4):
        self.p = (self.p + n - 1) & ~(n - 1)

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def i16(self):
        v = struct.unpack_from(self.e + 'h', self.d, self.p)[0]
        self.p += 2
        return v

    def u16(self):
        v = struct.unpack_from(self.e + 'H', self.d, self.p)[0]
        self.p += 2
        return v

    def i32(self):
        v = struct.unpack_from(self.e + 'i', self.d, self.p)[0]
        self.p += 4
        return v

    def u32(self):
        v = struct.unpack_from(self.e + 'I', self.d, self.p)[0]
        self.p += 4
        return v

    def i64(self):
        v = struct.unpack_from(self.e + 'q', self.d, self.p)[0]
        self.p += 8
        return v

    def u64(self):
        v = struct.unpack_from(self.e + 'Q', self.d, self.p)[0]
        self.p += 8
        return v

    def f32(self):
        v = struct.unpack_from(self.e + 'f', self.d, self.p)[0]
        self.p += 4
        return v

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def cstr(self):
        end = self.d.index(b'\0', self.p)
        v = self.d[self.p:end]
        self.p = end + 1
        return v.decode('utf-8', 'replace')

    def astr(self):
        """Aligned length-prefixed string."""
        n = self.i32()
        v = self.d[self.p:self.p + n]
        self.p += n
        self.align(4)
        return v.decode('utf-8', 'replace')

    def abytes(self):
        n = self.i32()
        v = self.d[self.p:self.p + n]
        self.p += n
        self.align(4)
        return v


class ObjectInfo(object):
    __slots__ = ('path_id', 'start', 'size', 'type_id', 'class_id')

    def __init__(self, path_id, start, size, type_id, class_id):
        self.path_id = path_id
        self.start = start
        self.size = size
        self.type_id = type_id
        self.class_id = class_id

    def __repr__(self):
        return '<obj %d class=%d @%d+%d>' % (self.path_id, self.class_id,
                                             self.start, self.size)


class SerializedFile(object):
    def __init__(self, data, name=''):
        self.name = name
        self.d = data
        r = Reader(data, 0, little=False)
        r.u32(); r.u32()
        self.version = r.u32()
        r.u32()
        self.big_endian = r.u8() != 0
        r.raw(3)
        if self.version >= 22:
            self.metadata_size = r.u32()
            self.file_size = r.i64()
            self.data_offset = r.i64()
            r.i64()
        else:
            raise ValueError('unsupported serialized file version %d' % self.version)

        m = Reader(data, r.p, little=not self.big_endian)
        self.unity_version = m.cstr()
        self.target_platform = m.i32()
        self.has_type_tree = m.u8() != 0
        ntypes = m.i32()
        self.types = []
        for _ in range(ntypes):
            self.types.append(self._read_type(m))
        self.objects = []
        nobj = m.i32()
        for _ in range(nobj):
            m.align(4)
            path_id = m.i64()
            start = m.i64()
            size = m.u32()
            tid = m.i32()
            cls = self.types[tid][0] if 0 <= tid < len(self.types) else -1
            self.objects.append(ObjectInfo(path_id, start, size, tid, cls))
        nscript = m.i32()
        for _ in range(nscript):
            m.i32()
            m.align(4)
            m.i64()
        next_ = m.i32()
        self.externals = []
        for _ in range(next_):
            m.cstr()
            guid = m.raw(16)
            typ = m.i32()
            path = m.cstr()
            self.externals.append((guid, typ, path))
        self.by_path = {o.path_id: o for o in self.objects}

    def _read_type(self, m):
        class_id = m.i32()
        m.u8()                        # is stripped
        script_index = m.i16()
        if class_id == 114:
            m.raw(16)
        m.raw(16)                     # old type hash
        if self.has_type_tree:
            raise ValueError('type trees not supported (player build has none)')
        return (class_id, script_index)

    def reader(self, obj):
        return Reader(self.d, self.data_offset + obj.start,
                      little=not self.big_endian)

    def objects_of(self, class_id):
        return [o for o in self.objects if o.class_id == class_id]


# --------------------------------------------------------------- class 147
def read_resource_manager(sf, obj):
    """ResourceManager: m_Container = vector<pair<string, PPtr<Object>>>."""
    r = sf.reader(obj)
    n = r.i32()
    out = []
    for _ in range(n):
        path = r.astr()
        file_id = r.i32()
        path_id = r.i64()
        out.append((path, file_id, path_id))
    return out


# ---------------------------------------------------------------- class 49
def read_text_asset(sf, obj):
    r = sf.reader(obj)
    name = r.astr()
    data = r.abytes()
    return {'name': name, 'bytes': data}


# ---------------------------------------------------------------- class 83
def read_audio_clip(sf, obj):
    r = sf.reader(obj)
    name = r.astr()
    load_type = r.i32()
    channels = r.i32()
    freq = r.i32()
    bits = r.i32()
    length = r.f32()
    is_tracker = r.u8()
    r.align(4)
    ambisonic = r.u8()
    r.align(4)
    subsound_index = r.i32()
    preload = r.u8(); r.align(4)
    background = r.u8(); r.align(4)
    legacy3d = r.u8(); r.align(4)
    # StreamedResource
    src = r.astr()
    offset = r.i64()
    size = r.i64()
    fmt = r.i32()
    return {'name': name, 'channels': channels, 'freq': freq, 'bits': bits,
            'length': length, 'source': src, 'offset': offset, 'size': size,
            'format': fmt}


# ---------------------------------------------------------------- class 28
def read_texture2d(sf, obj):
    r = sf.reader(obj)
    name = r.astr()
    forced_fallback = r.i32()
    downscale = r.u8()
    alpha_optional = r.u8()
    r.align(4)
    width = r.i32()
    height = r.i32()
    complete_size = r.i32()
    mips_stripped = r.i32()
    fmt = r.i32()
    mip_count = r.i32()
    is_readable = r.u8()
    is_pre = r.u8()
    ignore_limit = r.u8()
    r.align(4)
    group = r.astr()
    streaming = r.u8()
    r.align(4)
    priority = r.i32()
    image_count = r.i32()
    dimension = r.i32()
    # GLTextureSettings
    filt = r.i32(); aniso = r.i32(); bias = r.f32()
    wrap_u = r.i32(); wrap_v = r.i32(); wrap_w = r.i32()
    lightmap = r.i32()
    color_space = r.i32()
    blob = r.abytes()
    size = r.i32()
    data = r.raw(size) if size else b''
    stream_path, stream_off, stream_size = '', 0, 0
    if not size:
        stream_off = r.u32()
        stream_size = r.u32()
        stream_path = r.astr()
    return {'name': name, 'width': width, 'height': height, 'format': fmt,
            'mips': mip_count, 'data': data, 'stream': (stream_path,
                                                        stream_off, stream_size)}


READERS = {49: read_text_asset, 83: read_audio_clip, 28: read_texture2d,
           147: read_resource_manager}

CLASS_NAMES = {1: 'GameObject', 4: 'Transform', 21: 'Material', 23: 'MeshRenderer',
               28: 'Texture2D', 33: 'MeshFilter', 43: 'Mesh', 48: 'Shader',
               49: 'TextAsset', 83: 'AudioClip', 89: 'Cubemap', 114: 'MonoBehaviour',
               115: 'MonoScript', 128: 'Font', 142: 'AssetBundle',
               147: 'ResourceManager', 150: 'BuildSettings', 213: 'Sprite',
               687078895: 'SpriteAtlas'}


class DataDir(object):
    """assets/bin/Data as a whole: name -> serialized file, plus Resources."""

    def __init__(self, root):
        self.root = root
        self.cache = {}
        self.container = {}          # 'path' -> (file, path_id)
        self._load_container()

    # -------------------------------------------------------------- files
    def read_file(self, name):
        """Read a data file, transparently joining Android split parts."""
        p = os.path.join(self.root, name)
        if os.path.exists(p):
            return open(p, 'rb').read()
        parts = []
        i = 0
        while True:
            sp = p + '.split%d' % i
            if not os.path.exists(sp):
                break
            parts.append(open(sp, 'rb').read())
            i += 1
        if parts:
            return b''.join(parts)
        raise IOError('no such data file: %s' % name)

    def serialized(self, name):
        sf = self.cache.get(name)
        if sf is None:
            sf = SerializedFile(self.read_file(name), name)
            self.cache[name] = sf
        return sf

    # ---------------------------------------------------------- container
    def _load_container(self):
        ggm = self.serialized('globalgamemanagers')
        for o in ggm.objects_of(147):
            for path, file_id, path_id in read_resource_manager(ggm, o):
                fname = 'globalgamemanagers'
                if file_id:
                    ext = ggm.externals[file_id - 1][2]
                    fname = ext.split('/')[-1]
                self.container[path.lower()] = (fname, path_id)

    def load(self, path):
        """Resources.Load: returns (class_id, parsed object) or None."""
        ent = self.container.get(path.lower())
        if ent is None:
            return None
        fname, path_id = ent
        try:
            sf = self.serialized(fname)
        except IOError:
            return None
        obj = sf.by_path.get(path_id)
        if obj is None:
            return None
        fn = READERS.get(obj.class_id)
        if fn is None:
            return (obj.class_id, None)
        return (obj.class_id, fn(sf, obj))

    def resource_blob(self, name):
        return self.read_file(name.split('/')[-1])


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'out/apk/assets/bin/Data'
    dd = DataDir(root)
    print('container entries: %d' % len(dd.container))
    kinds = {}
    for p, (f, pid) in sorted(dd.container.items()):
        try:
            sf = dd.serialized(f)
            o = sf.by_path.get(pid)
            cid = o.class_id if o else -1
        except Exception:
            cid = -2
        kinds.setdefault(cid, []).append(p)
    for cid, paths in sorted(kinds.items()):
        print('  %-18s %4d   %s' % (CLASS_NAMES.get(cid, 'class%d' % cid),
                                    len(paths), ', '.join(sorted(paths)[:6])))
    if len(sys.argv) > 2:
        r = dd.load(sys.argv[2])
        if r is None:
            print('not found: %s' % sys.argv[2])
        else:
            cid, o = r
            print('%s -> %s' % (CLASS_NAMES.get(cid, cid),
                                {k: (v[:64] if isinstance(v, bytes) else v)
                                 for k, v in (o or {}).items()}))


if __name__ == '__main__':
    main()
