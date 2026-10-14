import sys,glob,os,re
from elftools.elf.elffile import ELFFile
for f in sys.argv[1:]:
  e=ELFFile(open(f,'rb'))
  out=set()
  for sec in e.iter_sections():
    if sec.name.startswith('.rodata') or sec.name in ('.data','.modinfo'):
      for m in re.finditer(rb'[\x20-\x7e\t]{4,}',sec.data()): out.add(m.group().decode())
  print('=====',os.path.basename(f))
  for s in sorted(out):
    if s.startswith(('license','vermagic','depends','parm','author','description')): continue
    print('  ',s[:200])
