# Build the adapted international package (user-authorized): intl 1.02 rootfs with 3 boot scripts patched so JP model ID 0x0301 behaves like 0x0300, repacked with the device's own (JP) key.
import tarfile, io, gzip
from upgpack import pack, unpack
JPKEY,JPSIG=b'5ef17493',b'07fa0b6e'; INTLKEY=b'4abf02d3'
src=open('intl_file3.dec','rb').read()
tin=tarfile.open(fileobj=io.BytesIO(src)); out=io.BytesIO(); tout=tarfile.open(fileobj=out,mode='w',format=tin.format)
patches={
 './etc/init.d/rc.sysinfo': [('${MID_ICX_1113}*|${MID_ICX_1113U2}*|${MID_ICX_1113W})', '${MID_ICX_1113}*|${MID_ICX_1115}*|${MID_ICX_1113U2}*|${MID_ICX_1113W})')],
 './usr/local/bin/load_usb_epson': [('\t"${MID_ICX_1115}"00??)\n\t\t_PID=$PID_ICX_1115\n\t\t_MODE=$MODE_MSC\n\t\t_MODE_SYM=$MODE_MSC_SYM\n', '\t"${MID_ICX_1115}"00??)\n\t\t_PID=$PID_ICX_1113\n\t\t_MODE=$MODE_DUAL\n\t\t_MODE_SYM=$MODE_DUAL_SYM\n')],
 './usr/local/bin/load_mg': [('\t"${MID_ICX_1115}"00??)\n\t\t_MODE=$MODE_AAD\n\t\t_MESG=$MESG_AAD\n', '\t"${MID_ICX_1115}"00??)\n\t\t_MODE=$MODE_WMT\n\t\t_MESG=$MESG_WMT\n')],
}
done=[]
for m in tin.getmembers():
    if m.isfile() and m.name in patches:
        s=tin.extractfile(m).read().decode('latin1')
        for a,b in patches[m.name]:
            assert s.count(a)==1,(m.name,s.count(a)); s=s.replace(a,b)
        data=s.encode('latin1'); m.size=len(data); tout.addfile(m,io.BytesIO(data)); done.append(m.name)
    elif m.isfile(): tout.addfile(m,tin.extractfile(m))
    elif m.issym() or m.islnk() or m.isdir(): tout.addfile(m)
    else: tout.addfile(m)
tout.close(); patched=out.getvalue(); print('patched:',done,'tar',len(patched),'orig',len(src))
t2=tarfile.open(fileobj=io.BytesIO(patched))
for n in patches:
    print('---',n); print('\n'.join(l for l in t2.extractfile(n).read().decode('latin1').splitlines() if 'MID_ICX_1115' in l or ('1115' in l and '_' in l and 'PID_ICX_1113' in l) or 'MODE_DUAL\n' in l))
gz=gzip.compress(patched,compresslevel=9,mtime=0)
sig,files=unpack(open('out/NW_WM_FW.UPG','rb').read(),INTLKEY)   # official intl: script, kernel, initrd, rootfs
new=pack([files[0],files[1],files[2],gz],JPKEY,JPSIG)
open('NW_WM_FW_intl_adapted.UPG','wb').write(new)
s2,f2=unpack(new,JPKEY); assert s2==JPSIG and f2[0]==files[0] and f2[1]==files[1] and f2[2]==files[2] and f2[3]==gz
print('built NW_WM_FW_intl_adapted.UPG',len(new),'bytes; verify-unpack OK; files',[len(x) for x in f2])
