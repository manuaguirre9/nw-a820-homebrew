"""Parse NEC OMF SPX DSP images (omf_dsp_manager.mp201 = 'OmfSpxCore', omf_me_*.mp201 = 'ME_*').
Reverse-engineered layout:
  +0   u32 len (file size - 20)            +4 name (NUL padded)
  date string 'YYYY/MM/DD hh:mm:ss', version 'v5.6', then 'CODE'
  CODE: records {u32 dsp_addr; u32 nhalf (16-bit truncated!); u16 data[nhalf]; pad to 4}
  'SEGM' u32 1, then entries {u32 type(1=code/inst, 2=data); u32 dsp_addr; u32 size_bytes; u32 1; u32 file_off_of_record; u32 0}
  trailing tables (task names e.g. 'MIDLTASK', 'BASE') + checksum.
"""
import struct, sys, re, os
FULL = os.environ.get("FULL")
def parse(fn):
    d = open(fn,'rb').read()
    hdr_len = struct.unpack_from('<I', d, 0)[0]
    name = d[4:0x20].split(b'\0')[0].decode(errors='replace')
    date = re.search(rb'\d{4}/\d\d/\d\d \d\d:\d\d:\d\d', d[:0x40]).group().decode()
    s = d.find(b'SEGM')
    p = s + 4
    ents = []
    while p + 24 <= len(d):
        t, a, sz, one, off, z = struct.unpack_from('<6I', d, p)
        if t not in (1, 2, 3) or one > 1 or z != 0: break
        ents.append((t, a, sz, off)); p += 24
    return d, dict(len=hdr_len, filesize=len(d), name=name, date=date, segm=s, segm_end=p), ents
if __name__ == '__main__':
    for fn in sys.argv[1:]:
        d, h, ents = parse(fn)
        print('==', fn, h)
        tot = {1:0, 2:0, 3:0}
        for t,a,sz,off in ents: tot[t] += sz
        print('  entries=%d  code_bytes=%#x data_bytes=%#x' % (len(ents), tot[1], tot[2]))
        for t,a,sz,off in (ents if FULL else ents[:12]):
            print('   type=%d addr=%#010x size=%#x rec@%#x' % (t,a,sz,off))
        if not FULL and len(ents) > 12: print('   ...'); [print('   type=%d addr=%#010x size=%#x rec@%#x' % e) for e in ents[-3:]]
        tail = d[h['segm_end']:]
        print('  tail strings:', [m.group().decode() for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_ ]{3,}', tail)][:40])
        ad = [a for t,a,sz,off in ents if t==2]
        if ad: print('  data addr range %#x-%#x' % (min(ad), max(a+sz for t,a,sz,off in ents if t==2)))
