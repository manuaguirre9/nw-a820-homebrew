"""Find ioctl() call sites and the request code (r1) in ARM ELF binaries."""
import sys, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from capstone import *

def imm(ops):
    parts = ops.split('#')[1:]
    v = int(parts[0].rstrip(', '), 0)
    if len(parts) > 1:
        r = int(parts[1], 0); v = ((v >> r) | (v << (32 - r))) & 0xffffffff
    return v

def analyze(path, want=('ioctl',)):
    f = open(path, 'rb'); e = ELFFile(f); data = open(path, 'rb').read()
    segs = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    def rd(v):
        for va, off, sz in segs:
            if va <= v < va + sz - 3: return struct.unpack_from('<I', data, off + v - va)[0]
        return None
    # PLT resolution: .rel.plt GOT addr -> name; plt stubs load from GOT
    got2name = {}
    dynsym = e.get_section_by_name('.dynsym')
    for sec in e.iter_sections():
        if isinstance(sec, RelocationSection) and sec.name in ('.rel.plt',):
            for r in sec.iter_relocations():
                got2name[r['r_offset']] = dynsym.get_symbol(r['r_info_sym']).name
    plt = e.get_section_by_name('.plt')
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    plt2name = {}
    if plt:
        code = plt.data(); base = plt['sh_addr']
        ins = list(md.disasm(code, base))
        # stub: add ip, pc, #X ; add ip, ip, #Y ; ldr pc, [ip, #Z]!
        for i in range(len(ins) - 2):
            a, b, c = ins[i], ins[i + 1], ins[i + 2]
            if a.mnemonic == 'add' and a.op_str.startswith('ip, pc') and c.mnemonic == 'ldr' and 'pc, [ip' in c.op_str:
                try:
                    x = imm(a.op_str); y = imm(b.op_str)
                    z = int(c.op_str.split('#')[1].rstrip(']!'), 0)
                    got = a.address + 8 + x + y + z
                    if got in got2name: plt2name[a.address] = got2name[got]
                except Exception:
                    pass
    text = e.get_section_by_name('.text')
    code = text.data(); base = text['sh_addr']
    regs = {}
    res = []
    for ins in md.disasm(code, base):
        m, ops = ins.mnemonic, ins.op_str
        if m in ('mov', 'mvn') and '#' in ops and ops.split(',')[0] in ('r0','r1','r2'):
            r = ops.split(',')[0]
            try:
                v = imm(ops)
                regs[r] = (~v & 0xffffffff) if m == 'mvn' else v & 0xffffffff
            except Exception: pass
        elif m == 'ldr' and '[pc' in ops and ops.split(',')[0] in ('r0','r1','r2'):
            r = ops.split(',')[0]
            try:
                off = int(ops.split('#')[1].rstrip(']'), 0) if '#' in ops else 0
                regs[r] = rd(ins.address + 8 + off)
            except Exception: pass
        elif m == 'bl':
            try: tgt = int(ops.lstrip('#'), 0)
            except Exception: continue
            name = plt2name.get(tgt)
            if name in want:
                res.append((ins.address, name, regs.get('r1')))
            regs = {}
        elif m in ('orr','add','sub') and ops.split(',')[0] in ('r0','r1','r2'):
            p = [x.strip() for x in ops.split(',')]
            r = p[0]
            if len(p) >= 3 and p[1] == r and p[2].startswith('#') and r in regs and regs[r] is not None:
                v = imm(', '.join(p[2:]))
                regs[r] = {'orr': regs[r] | v, 'add': (regs[r] + v) & 0xffffffff, 'sub': (regs[r] - v) & 0xffffffff}[m]
            else:
                regs.pop(r, None)
    return res

def dec(v):
    if v is None: return '?'
    d = (v >> 30) & 3; size = (v >> 16) & 0x3fff; t = (v >> 8) & 0xff; nr = v & 0xff
    dirs = {0: '_IO', 1: '_IOW', 2: '_IOR', 3: '_IOWR'}
    tc = chr(t) if 32 < t < 127 else hex(t)
    return f"0x{v:08x} {dirs[d]}('{tc}',{nr},size={size})"

for p in sys.argv[1:]:
    r = analyze(p)
    codes = sorted(set(x[2] for x in r if x[2] is not None))
    print(f"== {p}: {len(r)} ioctl calls; codes: " + '; '.join(dec(c) for c in codes))
