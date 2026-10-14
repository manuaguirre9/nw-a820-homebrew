import sys,glob,os,re
from elftools.elf.elffile import ELFFile
for f in sorted(glob.glob(sys.argv[1]+'/*.ko')):
  e=ELFFile(open(f,'rb'))
  s=e.get_section_by_name('.modinfo')
  info=[x.decode('latin1') for x in s.data().split(b'\0') if x] if s else []
  # params from __param? just modinfo parm
  print('=====',os.path.basename(f),os.path.getsize(f))
  for i in info: print('  ',i)
  sy=e.get_section_by_name('.symtab')
  if sy:
    und=[x.name for x in sy.iter_symbols() if x['st_shndx']=='SHN_UNDEF' and x.name]
    print('   imports:',' '.join(sorted(set(und)))[:1500])
