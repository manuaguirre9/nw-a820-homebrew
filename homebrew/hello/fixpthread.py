# Drop the (unused) libpthread dependency: the device's LinuxThreads libpthread has no GLIBC_2.4 version,
# so rename the DT_NEEDED/verneed file string "libpthread.so.0" to "libc.so.6" (which does define GLIBC_2.4).
import sys, struct
for f in sys.argv[1:]:
    d = bytearray(open(f, 'rb').read())
    i = d.find(b'\0libpthread.so.0\0')
    if i < 0: print(f, 'no libpthread'); continue
    d[i+1:i+16] = b'libc.so.6' + b'\0' * 6
    struct.pack_into('<I', d, 0x24, 0x04000002)
    open(f, 'wb').write(d); print(f, 'patched')
