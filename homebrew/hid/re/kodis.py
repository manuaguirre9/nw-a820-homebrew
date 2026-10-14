"""Disassemble functions of a relocatable ARM .ko with relocation targets shown.
usage: kodis.py module.ko func [func...]   (func = symbol name or regex prefix 're:')"""
import sys, re, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

e = ELFFile(open(sys.argv[1], 'rb'))
st = e.get_section_by_name('.symtab')
syms = list(st.iter_symbols())
text_idx = [i for i, s in enumerate(e.iter_sections()) if s.name == '.text'][0]
text = e.get_section(text_idx)
data = text.data()
# relocations for .text: offset -> symbol name (+addend in place)
rel = {}
for s in e.iter_sections():
    if isinstance(s, RelocationSection) and s['sh_info'] == text_idx:
        for r in s.iter_relocations():
            sym = syms[r['r_info_sym']]
            name = sym.name
            if not name:
                sec = e.get_section(sym['st_shndx'])
                name = sec.name
            rel[r['r_offset']] = name
# symbols in .text by address
funcs = sorted([(s['st_value'], s['st_size'], s.name) for s in syms
                if s['st_shndx'] == text_idx and s.name and not s.name.startswith('$')])
byname = {n: (a, z) for a, z, n in funcs}

def name_at(a):
    for fa, fz, fn in funcs:
        if fa == a:
            return fn
    return ''

md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for want in sys.argv[2:]:
    if want.startswith('re:'):
        targets = [n for n in byname if re.search(want[3:], n)]
    else:
        targets = [want]
    for fn in targets:
        a, z = byname[fn]
        print(f'==== {fn} @ {a:#x} size {z}')
        for ins in md.disasm(data[a:a + z], a):
            c = ''
            if ins.address in rel:
                c = ' ; -> ' + rel[ins.address]
                if rel[ins.address] == '.text' and ins.mnemonic.startswith('b'):
                    m = re.match(r'#(0x[0-9a-f]+)', ins.op_str)
                    if m:
                        t = int(m.group(1), 16) - ins.address
                        c = ' ; => %#x %s' % (t, name_at(t))
            elif ins.mnemonic in ('b', 'bl') or ins.mnemonic.startswith('b'):
                m = re.match(r'#(0x[0-9a-f]+)', ins.op_str)
                if m:
                    n = name_at(int(m.group(1), 16))
                    if n:
                        c = ' ; ' + n
            if ins.mnemonic == 'ldr' and '[pc' in ins.op_str:
                m = re.search(r'#(-?0x[0-9a-f]+)', ins.op_str)
                off = int(m.group(1), 16) if m else 0
                la = ins.address + 8 + off
                if la in rel:
                    c += ' ; =' + rel[la]
                elif 0 <= la < len(data):
                    c += ' ; =%#x' % struct.unpack_from('<I', data, la)[0]
            print(f'  {ins.address:6x}: {ins.mnemonic:8s} {ins.op_str}{c}')
