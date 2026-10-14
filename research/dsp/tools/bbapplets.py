import struct, sys
from elftools.elf.elffile import ELFFile
p = sys.argv[1]
f = open(p, 'rb'); e = ELFFile(f); data = open(p, 'rb').read()
segs = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
def v2o(v):
    for va, off, sz in segs:
        if va <= v < va + sz: return off + (v - va)
    return None
def cstr(v):
    o = v2o(v)
    if o is None: return None
    end = data.find(b'\0', o)
    if end < 0 or end - o > 40: return None
    s = data[o:end]
    try: s = s.decode('ascii')
    except: return None
    if not s or not all(c.isalnum() or c in '[]._-' for c in s): return None
    return s
# find the "[" string address(es)
best = []
for stride in (12, 16, 8):
    for sec in e.iter_sections():
        if sec.name not in ('.data', '.data.rel.ro', '.rodata'): continue
        base = sec['sh_addr']; off = sec['sh_offset']; sz = sec['sh_size']
        i = 0
        while i + 4 <= sz:
            v = struct.unpack_from('<I', data, off + i)[0]
            s = cstr(v)
            if s == '[':
                names = []; j = i
                while j + 4 <= sz:
                    s2 = cstr(struct.unpack_from('<I', data, off + j)[0])
                    if not s2: break
                    names.append(s2); j += stride
                if len(names) > len(best): best = names; info = (sec.name, stride, hex(base + i))
            i += 4
print(info, len(best))
print(' '.join(best))
