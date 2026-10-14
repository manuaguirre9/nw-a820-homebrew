import struct, sys
p = sys.argv[1]
d = open(p, 'rb').read()
tot, cnt = struct.unpack_from('<II', d, 0)
print(f"file={len(d)} hdr_total={tot} count={cnt}")
ents = []
for i in range(cnt):
    h, t, off, z = struct.unpack_from('<IIII', d, 16 + i * 16)
    ents.append((h, t, off))
for i, (h, t, off) in enumerate(ents):
    end = ents[i + 1][2] if i + 1 < cnt else len(d)
    blk = d[off:end]
    head = blk[:48]
    print(f"[{i:3}] hash={h:08x} t={t:08x} off=0x{off:x} size={end-off} head={head[:32].hex()} ascii={''.join(chr(c) if 32<=c<127 else '.' for c in head)}")
    if len(sys.argv) > 2 and i >= int(sys.argv[2]): break
