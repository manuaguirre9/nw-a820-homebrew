import struct,sys
d=open('rootfs_jp/usr/local/share/gemini/res/wording_res/ICX-COMMON_wording.dpk','rb').read()
cnt=struct.unpack_from('<I',d,4)[0]
ents=[struct.unpack_from('<IIII',d,16+i*16) for i in range(cnt)]
def get(bi,idx):
    off=ents[bi][2]
    n,lo,so=struct.unpack_from('<III',d,off+4)
    a,i,co,fl=struct.unpack_from('<HHHH',d,off+16+idx*8)
    s=off+so+co*2; e=d.find(b'\0\0',s)
    while (e-s)%2: e=d.find(b'\0\0',e+1)
    return d[s:e].decode('utf-16le','replace')
def w(id,bi=1):
    if id>=0x3ba: return get(0,id-0x3ba)
    return get(bi,id)
for a in sys.argv[1:]:
    x=int(a,16); print(hex(x), repr(w(x,1)), repr(w(x,2)) if x<0x3ba else '')
