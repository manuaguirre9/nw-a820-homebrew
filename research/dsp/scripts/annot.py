"""annotate =0x... literals in a .dis file with rodata strings. usage: python annot.py elf file.dis"""
import re, sys
from elftools.elf.elffile import ELFFile
e = ELFFile(open(sys.argv[1], 'rb')); ro = e.get_section_by_name('.rodata'); a = ro['sh_addr']; d = ro.data()
out = []
for l in open(sys.argv[2], encoding='utf-8'):
    m = re.search(r'=0x([0-9a-f]+)\s*$', l)
    if m:
        v = int(m.group(1), 16)
        if a <= v < a + len(d):
            s = d[v-a:v-a+80].split(b'\0')[0]
            if s and all(32 <= c < 127 for c in s): l = l.rstrip() + '  "' + s.decode() + '"\n'
    out.append(l)
open(sys.argv[2], 'w', encoding='utf-8').writelines(out)
