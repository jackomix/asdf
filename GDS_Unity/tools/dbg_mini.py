import struct
import unicorn
import capstone
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import *

data = open('loader/mini', 'rb').read()
e_entry = struct.unpack_from('<Q', data, 0x18)[0]
e_phoff = struct.unpack_from('<Q', data, 0x20)[0]
e_phnum = struct.unpack_from('<H', data, 0x38)[0]
PAGE = 0x1000
minv = 1 << 64
maxv = 0
segs = []
for i in range(e_phnum):
    o = e_phoff + i * 56
    if struct.unpack_from('<I', data, o)[0] != 1:
        continue
    p_offset = struct.unpack_from('<Q', data, o + 8)[0]
    p_vaddr = struct.unpack_from('<Q', data, o + 16)[0]
    p_filesz = struct.unpack_from('<Q', data, o + 32)[0]
    p_memsz = struct.unpack_from('<Q', data, o + 40)[0]
    minv = min(minv, p_vaddr)
    maxv = max(maxv, p_vaddr + p_memsz)
    segs.append((p_offset, p_vaddr, p_filesz, p_memsz))
minv &= ~(PAGE - 1)
maxv = (maxv + PAGE - 1) & ~(PAGE - 1)
span = maxv - minv
BASE = 0x400000
uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
uc.mem_map(BASE, span + 2 * PAGE, 7)
for p_offset, p_vaddr, p_filesz, p_memsz in segs:
    if p_filesz:
        uc.mem_write(BASE + (p_vaddr - minv), data[p_offset:p_offset + p_filesz])
uc.mem_map(0x7fff0000 - 0x100000, 0x100000 + 0x1000, 7)
uc.reg_write(UC_ARM64_REG_SP, 0x7fff0000 - 0x100)

PC = 0x411514
raw = uc.mem_read(PC, 64)
print('guest bytes @%#x:' % PC, bytes(raw).hex())
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for ins in md.disasm(bytes(raw), PC):
    print(hex(ins.address), ins.mnemonic, ins.op_str)
