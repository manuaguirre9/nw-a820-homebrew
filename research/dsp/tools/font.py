import struct, sys, collections
p = sys.argv[1]
d = open(p, 'rb').read()
tot, cnt = struct.unpack_from('<II', d, 0)
ents = [struct.unpack_from('<IIII', d, 16 + i * 16) for i in range(cnt)]
blocks = {
    'Latin/Cyrillic?': None,
}
def ranges(cps):
    out = []; s = None; prev = None
    for c in cps:
        if s is None: s = prev = c; continue
        if c == prev + 1: prev = c; continue
        out.append((s, prev)); s = prev = c
    if s is not None: out.append((s, prev))
    return out
UB = [(0x20,0x7f,'ASCII'),(0x80,0x24f,'Latin-1/Ext'),(0x370,0x3ff,'Greek'),(0x400,0x4ff,'Cyrillic'),(0x2000,0x2bff,'Punct/Symbols'),(0x3000,0x30ff,'CJK punct/Kana'),(0x3100,0x31ff,'Bopomofo'),(0x3400,0x4dbf,'CJK ExtA'),(0x4e00,0x9fff,'CJK Unified'),(0xac00,0xd7af,'Hangul'),(0xe000,0xf8ff,'PUA'),(0xf900,0xfaff,'CJK Compat'),(0xff00,0xffef,'Halfwidth/Fullwidth')]
for i, (h, t, off, z) in enumerate(ents):
    b = d[off:]
    first, last = struct.unpack_from('<HH', b, 12)
    mtab, bmp = struct.unpack_from('<II', b, 0x18)
    n0 = struct.unpack_from("<I", b, mtab - 4)[0]; n = min(n0, (bmp - mtab) // 8)
    cmap = struct.unpack_from('<%dH' % (last - first + 1), b, 0x60)
    n = max(cmap) + 1
    cps = [first + k for k, g in enumerate(cmap) if g]
    heights = collections.Counter(); widths = collections.Counter()
    for g in range(n):
        adv, bw, ht, bo = struct.unpack_from('<HHHH', b, mtab + g * 8)
        heights[ht] += 1; widths[adv] += 1
    cov = collections.Counter()
    for c in cps:
        for lo, hi, name in UB:
            if lo <= c <= hi: cov[name] += 1; break
        else: cov['other'] += 1
    print(f"sub{i}: U+{first:04X}..U+{last:04X} glyphs={n} mapped={len(cps)} heights={dict(heights)} adv_range={min(widths or [0])}-{max(widths or [0])} mtab=0x{mtab:x} bmp=0x{bmp:x}")
    print('   coverage:', dict(cov))
