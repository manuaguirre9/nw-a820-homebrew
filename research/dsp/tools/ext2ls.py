import struct,sys,os
img=open(sys.argv[1],'rb').read()
out=sys.argv[2] if len(sys.argv)>2 else None
sb=img[1024:2048]
(ninodes,nblocks,rblocks,freeb,freei,first,logbs)=struct.unpack_from('<7I',sb,0)
bs=1024<<logbs
bpg,fpg,ipg=struct.unpack_from('<III',sb,32)
magic,=struct.unpack_from('<H',sb,56)
rev,=struct.unpack_from('<I',sb,76)
isz=struct.unpack_from('<H',sb,88)[0] if rev>=1 else 128
vol=sb[120:136]
print('magic',hex(magic),'bs',bs,'blocks',nblocks,'free',freeb,'inodes',ninodes,'freei',freei,'vol',vol,'lastmnt',sb[136:200].split(b'\0')[0])
gdt=(first+1)*bs
def inode(n):
  g=(n-1)//ipg; i=(n-1)%ipg
  itab,=struct.unpack_from('<I',img,gdt+g*32+8)
  off=itab*bs+i*isz
  return img[off:off+isz]
def blocks(ino):
  bl=list(struct.unpack_from('<15I',ino,40))
  res=[b for b in bl[:12] if b]
  def ind(b,lvl):
    if not b: return []
    ptrs=struct.unpack_from('<%dI'%(bs//4),img,b*bs)
    if lvl==1: return [p for p in ptrs if p]
    r=[]
    for p in ptrs: r+=ind(p,lvl-1)
    return r
  res+=ind(bl[12],1)+ind(bl[13],2)+ind(bl[14],3)
  return res
def data(ino):
  size,=struct.unpack_from('<I',ino,4)
  mode,=struct.unpack_from('<H',ino,0)
  if (mode&0xF000)==0xA000 and size<60: return ino[40:40+size]
  return b''.join(img[b*bs:(b+1)*bs] for b in blocks(ino))[:size]
def walk(n,path):
  ino=inode(n); d=data(ino); p=0
  while p<len(d):
    inum,rl,nl,ft=struct.unpack_from('<IHBB',d,p)
    name=d[p+8:p+8+nl].decode('latin1'); p+=rl
    if rl==0: break
    if not inum or name in('.','..'): continue
    ci=inode(inum); mode,=struct.unpack_from('<H',ci,0); size,=struct.unpack_from('<I',ci,4)
    t=mode&0xF000; fp=path+'/'+name
    extra=''
    if t==0xA000: extra=' -> '+data(ci).decode('latin1')
    if t in (0x2000,0x6000):
      dv=struct.unpack_from('<I',ci,40)[0]; extra=' dev %d,%d'%((dv>>8)&0xff,dv&0xff)
    print('%06o %8d %s%s'%(mode,size,fp,extra))
    if t==0x4000: 
      if out: os.makedirs(out+fp,exist_ok=True)
      walk(inum,fp)
    elif t==0x8000 and out:
      open(out+fp,'wb').write(data(ci))
walk(2,'')
