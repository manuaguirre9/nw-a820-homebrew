import sys; sys.argv=['probe.py',r'\\.\H:']
exec(open('probe.py').read().split("ok,st,sense,data=scsi([0x12")[0])
print('=== DNK 0x23 sub enumeration (read-only)')
for sub in range(0,0x26):
    ok,st,sense,data=dnk(0x23,sub,64)
    if st==0: print(f'sub 0x{sub:02x}: {data.rstrip(b"\\x00").hex()}  {data.rstrip(b"\\x00").decode("latin1",errors="replace")!r}')
    else: print(f'sub 0x{sub:02x}: status {st} sense {sense[:3].hex()}')
print('=== raw NVP nodes via sub 10 (arg = node index)')
for node in (16,23,24,29,32,5,6,9,10,17,18,21,22,25,27):
    ok,st,sense,data=dnk(0x23,10,68,node)
    print(f'node {node:2}: status {st} sense {sense[:3].hex()} data {data.hex()}')
ok,st,sense,data=dnk(0x12,0,64); print('devinfo 0x12:',st,data.hex())
k.CloseHandle(h)
