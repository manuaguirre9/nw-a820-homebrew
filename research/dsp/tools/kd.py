import sys,struct
sys.path.insert(0,'pylib')
from capstone import *
d=open('jp_file1.bin','rb').read(); B=0xC0008000
s=int(sys.argv[1],16); n=int(sys.argv[2],16)
md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
md.skipdata=True
for i in md.disasm(d[s-B:s-B+n],s):
    c=''
    if i.mnemonic.startswith('ldr') and '[pc, #' in i.op_str:
        a=i.address+8+int(i.op_str.split('#')[-1].rstrip(']'),16)
        v=struct.unpack_from('<I',d,a-B)[0]; c='=0x%x'%v
        if B<=v<B+len(d):
            e=d[v-B:v-B+60]; z=e.find(b'\0')
            if z>3 and all(32<=x<127 or x in(9,10) for x in e[:z]): c+=' '+repr(e[:z].decode())
    print('%08x %-7s %-30s %s'%(i.address,i.mnemonic,i.op_str,c))
