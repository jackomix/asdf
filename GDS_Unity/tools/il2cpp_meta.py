"""Dependency-free reader for IL2CPP global-metadata.dat (versions 24..31).

Layout is recovered from the header, which is a contiguous run of
(offset, size-in-bytes) int32 pairs; every table's element count is derived by
dividing the byte size by the struct stride, which also validates the guess.
"""
import struct
from collections import namedtuple

# header pair index -> table name (v29..v31 ordering)
TABLES = [
    'stringLiteral', 'stringLiteralData', 'string', 'events', 'properties',
    'methods', 'parameterDefaultValues', 'fieldDefaultValues',
    'fieldAndParameterDefaultValueData', 'fieldMarshaledSizes', 'parameters',
    'fields', 'genericParameters', 'genericParameterConstraints',
    'genericContainers', 'nestedTypes', 'interfaces', 'vtableMethods',
    'interfaceOffsets', 'typeDefinitions', 'images', 'assemblies',
    'fieldRefs', 'referencedAssemblies', 'attributeData', 'attributeDataRange',
    'unresolvedIndirectCallParameterTypes',
    'unresolvedIndirectCallParameterRanges',
    'windowsRuntimeTypeNames', 'windowsRuntimeStrings',
    'exportedTypeDefinitions',
]

STRIDE = {
    'stringLiteral': 8, 'events': 24, 'properties': 24, 'methods': 36,
    'parameterDefaultValues': 12, 'fieldDefaultValues': 12,
    'fieldMarshaledSizes': 12, 'parameters': 12, 'fields': 12,
    'genericParameters': 16, 'genericParameterConstraints': 4,
    'genericContainers': 12, 'nestedTypes': 4, 'interfaces': 4,
    'vtableMethods': 4, 'interfaceOffsets': 8, 'typeDefinitions': 88,
    'images': 40, 'assemblies': 64, 'fieldRefs': 8,
    'referencedAssemblies': 4, 'exportedTypeDefinitions': 4,
}

TypeDef = namedtuple('TypeDef', (
    'nameIndex namespaceIndex byvalTypeIndex declaringTypeIndex parentIndex '
    'elementTypeIndex genericContainerIndex flags fieldStart methodStart '
    'eventStart propertyStart nestedTypesStart interfacesStart vtableStart '
    'interfaceOffsetsStart method_count property_count field_count '
    'event_count nested_type_count vtable_count interfaces_count '
    'interface_offsets_count bitfield token'))

MethodDef = namedtuple('MethodDef', (
    'nameIndex declaringType returnType returnParameterToken parameterStart '
    'genericContainerIndex token flags iflags slot parameterCount'))

ImageDef = namedtuple('ImageDef', (
    'nameIndex assemblyIndex typeStart typeCount exportedTypeStart '
    'exportedTypeCount entryPointIndex token customAttributeStart '
    'customAttributeCount'))

FieldDef = namedtuple('FieldDef', 'nameIndex typeIndex token')
ParamDef = namedtuple('ParamDef', 'nameIndex token typeIndex')


class Metadata(object):
    def __init__(self, data):
        self.d = data
        sanity, self.version = struct.unpack_from('<Ii', data, 0)
        if sanity != 0xFAB11BAF:
            raise ValueError('bad metadata magic 0x%08X' % sanity)
        self.tab = {}
        raw = struct.unpack_from('<%di' % (len(TABLES) * 2), data, 8)
        for i, name in enumerate(TABLES):
            self.tab[name] = (raw[i * 2], raw[i * 2 + 1])
        self.header_size = 8 + len(TABLES) * 8
        self._strcache = {}

    # ---------------------------------------------------------------- utils
    def count(self, table):
        off, size = self.tab[table]
        return size // STRIDE[table]

    def _slice(self, table, i, stride):
        off, size = self.tab[table]
        assert 0 <= i * stride < size, '%s index %d out of range' % (table, i)
        return off + i * stride

    def string(self, idx):
        """NUL-terminated C string out of the `string` blob."""
        if idx < 0:
            return ''
        s = self._strcache.get(idx)
        if s is None:
            base = self.tab['string'][0] + idx
            end = self.d.index(b'\0', base)
            s = self.d[base:end].decode('utf-8', 'replace')
            self._strcache[idx] = s
        return s

    def string_literal(self, i):
        off = self._slice('stringLiteral', i, 8)
        length, dataIndex = struct.unpack_from('<Ii', self.d, off)
        base = self.tab['stringLiteralData'][0] + dataIndex
        return self.d[base:base + length].decode('utf-8', 'replace')

    # --------------------------------------------------------------- tables
    def type_def(self, i):
        off = self._slice('typeDefinitions', i, 88)
        return TypeDef(*struct.unpack_from('<16i8HII', self.d, off))

    def method_def(self, i):
        off = self._slice('methods', i, 36)
        return MethodDef(*struct.unpack_from('<7i4H', self.d, off))

    def field_def(self, i):
        return FieldDef(*struct.unpack_from(
            '<3i', self.d, self._slice('fields', i, 12)))

    def param_def(self, i):
        return ParamDef(*struct.unpack_from(
            '<3i', self.d, self._slice('parameters', i, 12)))

    def image_def(self, i):
        return ImageDef(*struct.unpack_from(
            '<10i', self.d, self._slice('images', i, 40)))

    def nested_type(self, i):
        return struct.unpack_from('<i', self.d,
                                  self._slice('nestedTypes', i, 4))[0]

    def interface(self, i):
        return struct.unpack_from('<i', self.d,
                                  self._slice('interfaces', i, 4))[0]

    def vtable_method(self, i):
        return struct.unpack_from('<I', self.d,
                                  self._slice('vtableMethods', i, 4))[0]

    # ------------------------------------------------------------- helpers
    def type_name(self, i, with_ns=True):
        t = self.type_def(i)
        n = self.string(t.nameIndex)
        ns = self.string(t.namespaceIndex)
        if t.declaringTypeIndex >= 0:
            # nested: qualify with the outer type
            outer = self.enclosing.get(i)
            if outer is not None:
                return self.type_name(outer, with_ns) + '/' + n
        return (ns + '.' + n) if (with_ns and ns) else n

    def build_indexes(self):
        """nested-type -> declaring-type map (declaringTypeIndex is a TypeIndex
        into the Il2CppType array, not a TypeDefinitionIndex, so walk
        nestedTypes instead)."""
        self.enclosing = {}
        for ti in range(self.count('typeDefinitions')):
            t = self.type_def(ti)
            for k in range(t.nested_type_count):
                self.enclosing[self.nested_type(t.nestedTypesStart + k)] = ti
        return self


def load(path):
    return Metadata(open(path, 'rb').read()).build_indexes()
