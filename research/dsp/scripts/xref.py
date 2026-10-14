"""Find literal-pool words equal to given values in an ARM ELF (exec or .so) and the ldr instructions using them.
usage: python xref.py file val1 [val2...]  (hex)"""
import sys, struct, re
from elftools.elf.elffile import ELFFile
from capstone import *
f = sys.argv[1]; e = ELFFile(open(f, 'rb')); raw = open(f, 'rb').read()
tx = e.get_section_by_name('.text'); ta = tx['sh_addr']; td = tx.data()
for v in [int(x, 16) for x in sys.argv[2:]]:
    pools = [ta + m.start() for m in re.finditer(re.escape(struct.pack('<I', v)), td) if m.start() % 4 == 0]
    users = []
    for p in pools:
        # ldr rX, [pc, #off] : pc = ins+8 ; scan back up to 4 KB
        for ia in range(max(ta, p - 4096), p, 4):
            w = struct.unpack_from('<I', td, ia - ta)[0]
            if (w & 0x0f7f0000) == 0x051f0000:  # LDR Rd,[PC,#+-imm12]
                off = w & 0xfff; off = off if (w >> 23) & 1 else -off
                if ia + 8 + off == p: users.append(ia)
    print(hex(v), 'pools', [hex(p) for p in pools], 'users', [hex(u) for u in users])
