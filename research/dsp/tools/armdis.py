import sys; sys.path.insert(0, sys.argv[0].rsplit('/',1)[0]+'/pylib')
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from capstone import *
f=sys.argv[1]; e=ELFFile(open(f,'rb'))
# plt symbol map via rel.plt
syms={}
dyn=e.get_section_by_name('.dynsym')
rp=e.get_section_by_name('.rel.plt')
plt=e.get_section_by_name('.plt')
if rp and plt:
  ents=list(rp.iter_relocations())
  base=plt['sh_addr']+20
  for i,r in enumerate(ents):
    syms[base+12*i]=dyn.get_symbol(r['r_info_sym']).name
ro=e.get_section_by_name('.rodata')
def rostr(a):
  if ro and ro['sh_addr']<=a<ro['sh_addr']+ro['sh_size']:
    d=ro.data()[a-ro['sh_addr']:]; return d[:d.find(b'\0')].decode('latin1')
t=e.get_section_by_name('.text')
md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
data=t.data(); addr=t['sh_addr']
import struct
for ins in md.disasm(data,addr):
  c=''
  if ins.mnemonic=='bl':
    tgt=int(ins.op_str.lstrip('#'),16); c=syms.get(tgt,'')
  if ins.mnemonic=='ldr' and 'pc' in ins.op_str:
    off=int(ins.op_str.split('#')[-1].rstrip(']'),16) if '#' in ins.op_str else 0
    a=ins.address+8+off
    if t['sh_addr']<=a<t['sh_addr']+t['sh_size']:
      v=struct.unpack_from('<I',data,a-addr)[0]; s=rostr(v); c='=0x%x %s'%(v,repr(s) if s else '')
  print('%08x %-6s %-30s %s'%(ins.address,ins.mnemonic,ins.op_str,c))
