"""Disassemble an address range of an ARM ELF .so (annotates PLT calls, named funcs, pc-rel literals).
usage: python rdis.py lib.so start end"""
import sys, struct, re
from elftools.elf.elffile import ELFFile
from capstone import *
e = ELFFile(open(sys.argv[1], 'rb')); a0 = int(sys.argv[2], 16); a1 = int(sys.argv[3], 16)
dyn = e.get_section_by_name('.dynsym'); plt = e.get_section_by_name('.plt'); rp = e.get_section_by_name('.rel.plt')
pltsyms = {}
if plt and rp:
    base = plt['sh_addr'] + 20
    for i, r in enumerate(rp.iter_relocations()): pltsyms[base + 12*i] = dyn.get_symbol(r['r_info_sym']).name + '@plt'
names = {s['st_value']: s.name for s in dyn.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF' and s.name}
def secof(a):
    for s in e.iter_sections():
        if s['sh_addr'] and s['sh_addr'] <= a < s['sh_addr'] + s['sh_size'] and s['sh_type'] != 'SHT_NOBITS': return s
s = secof(a0); data = s.data()[a0 - s['sh_addr']: a1 - s['sh_addr']]
md = Cs(CS_ARCH_ARM, CS_MODE_ARM); md.skipdata = True
for ins in md.disasm(data, a0):
    if ins.address in names: print('<%s>:' % names[ins.address])
    c = ''
    if ins.mnemonic[:2] in ('bl', 'b') and ins.op_str.startswith('#'):
        t = int(ins.op_str[1:], 16); c = pltsyms.get(t) or names.get(t, '')
    if ins.mnemonic.startswith('ldr') and '[pc' in ins.op_str:
        m = re.search(r'#(-?0x[0-9a-f]+|-?\d+)\]', ins.op_str); off = int(m.group(1), 0) if m else 0
        la = ins.address + 8 + off; ls = secof(la)
        if ls: c = '=%#x' % struct.unpack_from('<I', ls.data(), la - ls['sh_addr'])[0]
    print('  %08x: %-7s %-32s %s' % (ins.address, ins.mnemonic, ins.op_str, c))
