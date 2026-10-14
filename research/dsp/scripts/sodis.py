"""Disassemble functions of an ARM ELF .so with PLT/dynsym/rodata annotation.
usage: python sodis.py lib.so [funcname|re:regex|ALL]"""
import sys, struct, re
from elftools.elf.elffile import ELFFile
from capstone import *
fn = sys.argv[1]; want = sys.argv[2] if len(sys.argv) > 2 else 'ALL'
e = ELFFile(open(fn,'rb'))
dyn = e.get_section_by_name('.dynsym')
plt = e.get_section_by_name('.plt'); rp = e.get_section_by_name('.rel.plt')
pltsyms = {}
if plt and rp:
    ents = list(rp.iter_relocations()); base = plt['sh_addr'] + 20
    for i, r in enumerate(ents): pltsyms[base + 12*i] = dyn.get_symbol(r['r_info_sym']).name + '@plt'
funcs = sorted([(s['st_value'], s['st_size'], s.name) for s in dyn.iter_symbols()
                if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF'])
names = {a: n for a, sz, n in funcs}
def secof(a):
    for s in e.iter_sections():
        if s['sh_addr'] and s['sh_addr'] <= a < s['sh_addr'] + s['sh_size'] and s['sh_type'] != 'SHT_NOBITS': return s
def cstr(a):
    s = secof(a)
    if s is None or s.name not in ('.rodata', '.data'): return None
    d = s.data()[a - s['sh_addr']:]; z = d.find(b'\0')
    t = d[:z]
    return t.decode('latin1') if z > 0 and all(32 <= c < 127 or c in (9, 10) for c in t) else None
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for a, sz, n in funcs:
    if want != 'ALL' and not (n == want or (want.startswith('re:') and re.search(want[3:], n))): continue
    s = secof(a); data = s.data()[a - s['sh_addr']: a - s['sh_addr'] + sz]
    print('\n%08x <%s> size=%d' % (a, n, sz))
    for ins in md.disasm(data, a):
        c = ''
        if ins.mnemonic in ('bl', 'b', 'blx') and ins.op_str.startswith('#'):
            t = int(ins.op_str[1:], 16); c = pltsyms.get(t) or names.get(t, '')
        if ins.mnemonic.startswith('ldr') and '[pc' in ins.op_str:
            m = re.search(r'#(-?0x[0-9a-f]+|-?\d+)\]', ins.op_str)
            off = int(m.group(1), 0) if m else 0
            la = ins.address + 8 + off; ls = secof(la)
            if ls:
                v = struct.unpack_from('<I', ls.data(), la - ls['sh_addr'])[0]
                c = '=%#x' % v
        print('  %08x: %-7s %-32s %s' % (ins.address, ins.mnemonic, ins.op_str, c))
