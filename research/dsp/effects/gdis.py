"""Disassemble an address range of an ARM ELF executable (gemini/mptapp) with PLT names,
literal values and rodata string annotation.
usage: python gdis.py elf start end"""
import sys, struct, re
from elftools.elf.elffile import ELFFile
from capstone import *
f = sys.argv[1]; e = ELFFile(open(f, 'rb'))
a0 = int(sys.argv[2], 16); a1 = int(sys.argv[3], 16)
dyn = e.get_section_by_name('.dynsym'); plt = e.get_section_by_name('.plt'); rp = e.get_section_by_name('.rel.plt')
pltsyms = {}
if plt and rp:
    base = plt['sh_addr'] + 20
    for i, r in enumerate(rp.iter_relocations()): pltsyms[base + 12*i] = dyn.get_symbol(r['r_info_sym']).name + '@plt'
secs = [s for s in e.iter_sections() if s['sh_addr'] and s['sh_type'] != 'SHT_NOBITS']
def secof(a):
    for s in secs:
        if s['sh_addr'] <= a < s['sh_addr'] + s['sh_size']: return s
def cstr(a):
    s = secof(a)
    if s is None or s.name not in ('.rodata', '.data'): return None
    d = s.data()[a - s['sh_addr']:a - s['sh_addr'] + 120]; z = d.find(b'\0')
    t = d[:z]
    return t.decode('latin1') if z > 1 and all(32 <= c < 127 or c in (9, 10) for c in t) else None
s = secof(a0); data = s.data()[a0 - s['sh_addr']: a1 - s['sh_addr']]
md = Cs(CS_ARCH_ARM, CS_MODE_ARM); md.skipdata = True
for ins in md.disasm(data, a0):
    c = ''
    if ins.mnemonic[:2] in ('bl', 'b') and ins.op_str.startswith('#'):
        t = int(ins.op_str[1:], 16); c = pltsyms.get(t, '')
    if ins.mnemonic.startswith('ldr') and '[pc' in ins.op_str:
        m = re.search(r'#(-?0x[0-9a-f]+|-?\d+)\]', ins.op_str); off = int(m.group(1), 0) if m else 0
        la = ins.address + 8 + off; ls = secof(la)
        if ls:
            v = struct.unpack_from('<I', ls.data(), la - ls['sh_addr'])[0]
            c = '=%#x' % v
            t = cstr(v)
            if t: c += ' "%s"' % t[:70]
    print('  %08x: %-7s %-32s %s' % (ins.address, ins.mnemonic, ins.op_str, c))
