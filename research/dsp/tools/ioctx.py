import sys, struct
sys.argv, files = sys.argv[:1], sys.argv[1:]
import ioctls
from elftools.elf.elffile import ELFFile
from capstone import *
for p in files:
    e = ELFFile(open(p,'rb')); data=open(p,'rb').read()
    segs = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in e.iter_segments() if s['p_type']=='PT_LOAD']
    def rd(v):
        for va,off,sz in segs:
            if va<=v<va+sz-3: return struct.unpack_from('<I',data,off+v-va)[0]
    calls = ioctls.analyze(p)
    t = e.get_section_by_name('.text'); md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    ins = list(md.disasm(t.data(), t['sh_addr'])); idx = {i.address:k for k,i in enumerate(ins)}
    print('==', p)
    for a,_,_ in calls:
        k = idx[a]; out=[]
        for i in ins[max(0,k-6):k+1]:
            s = f"{i.mnemonic} {i.op_str}"
            if i.mnemonic=='ldr' and '[pc' in i.op_str:
                off = int(i.op_str.split('#')[1].rstrip(']'),0) if '#' in i.op_str else 0
                v = rd(i.address+8+off); s += f" ;=0x{v:08x}" if v is not None else ''
            out.append(s)
        print('   ', ' | '.join(out))
