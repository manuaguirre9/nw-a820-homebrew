import struct,sys,re
d=open('jp_file1.bin','rb').read(); B=0xC0008000
def ptrs(name):
    out=[]
    for m in re.finditer(re.escape(name.encode())+b'\0',d):
        a=m.start()+B
        if d[m.start()-1]!=0: continue
        p=struct.pack('<I',a)
        for n in re.finditer(re.escape(p),d):
            if n.start()%4==0: out.append((hex(n.start()+B), hex(struct.unpack_from('<I',d,n.start()-4)[0])))
    return out
for n in sys.argv[1:]: print(n,ptrs(n))
