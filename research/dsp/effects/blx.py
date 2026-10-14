"""find BL/B callers of given addresses in an ARM ELF. usage: python blx.py elf addr..."""
import sys, struct
from elftools.elf.elffile import ELFFile
e = ELFFile(open(sys.argv[1], 'rb')); t = e.get_section_by_name('.text'); a = t['sh_addr']; d = t.data()
tg = {int(x, 16) for x in sys.argv[2:]}; res = {x: [] for x in tg}
for i in range(0, len(d) - 3, 4):
    w = struct.unpack_from('<I', d, i)[0]
    if (w >> 25) & 7 == 5:
        off = w & 0xffffff
        if off & 0x800000: off -= 0x1000000
        dst = a + i + 8 + off * 4
        if dst in tg: res[dst].append(hex(a + i))
for x in sorted(res): print(hex(x), res[x])
