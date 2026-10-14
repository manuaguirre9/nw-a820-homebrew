import sys,struct
sys.path.insert(0,'pylib')
from elftools.elf.elffile import ELFFile
import capstone
p=sys.argv[1]; e=ELFFile(open(p,'rb')); raw=open(p,'rb').read()
segs=[(s['p_vaddr'],s['p_offset'],s['p_filesz']) for s in e.iter_segments() if s['p_type']=='PT_LOAD']
def rd(v,n=4):
    for a,o,sz in segs:
        if a<=v<a+sz: return raw[o+v-a:o+v-a+n]
def cstr(v):
    b=rd(v,80)
    if not b: return None
    z=b.find(b'\0')
    if z>=3 and all(32<=x<127 or x in (9,10) for x in b[:z]): return b[:z].decode()
t=e.get_section_by_name('.text'); a=t['sh_addr']; d=t.data()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM); md.skipdata=True
plt=e.get_section_by_name('.plt'); relplt=e.get_section_by_name('.rel.plt'); ds=e.get_section_by_name('.dynsym')
names={}
for i,r in enumerate(relplt.iter_relocations()):
    names[plt['sh_addr']+20+12*i]=ds.get_symbol(r['r_info_sym']).name
lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
for i in md.disasm(d[lo-a:hi-a],lo):
    c=''
    if i.mnemonic in('bl','b','blx') and i.op_str.startswith('#'):
        c=names.get(int(i.op_str[1:],16),'')
    if i.mnemonic.startswith('ldr') and '[pc, #' in i.op_str:
        ad=i.address+8+int(i.op_str.split('#')[1].rstrip(']'),16)
        b=rd(ad)
        if b:
            v=struct.unpack('<I',b)[0]; s=cstr(v); c='=0x%x %s'%(v,repr(s) if s else '')
    print('%x: %-7s %-28s %s'%(i.address,i.mnemonic,i.op_str,c))
