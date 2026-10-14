# Makes a copy of Sony/CSR's a2d_demo_app that starts with the "-a NAP:UAP:LAP" device already selected
# (the stock tool parses -a into a global it never reads). Replaces the dummy device-list fill loop at
# file offset 0x18b4 with code that copies the -a address into device_list[0] and connection slot 0 and sets
# selIdx = 0, count = 1. Found by disassembly; see the comments in homebrew/hbplayer/bt.c.
import sys
src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())
OFF = 0x18b4
orig = bytes.fromhex('0010a0e3 0200a0e3 5bc0a0e3 813184e0 282083e2 280083e5 b600c2e1 04c0c2e5 013081e2 ff1003e2 090051e3 f6ffff9a'.replace(' ', ''))
new = bytes.fromhex('24009FE5 001090E5 042090E5 281084E5 2C2084E5 001084E5 042084E5 0030A0E3 8A30C4E5 0130A0E3 8B30C4E5 48A60200'.replace(' ', ''))
assert len(orig) == len(new) == 48
if d[OFF:OFF + 48] != orig:
    sys.exit('unexpected bytes at 0x%x: %s' % (OFF, d[OFF:OFF + 48].hex()))
d[OFF:OFF + 48] = new
open(dst, 'wb').write(d)
print('patched ->', dst)
