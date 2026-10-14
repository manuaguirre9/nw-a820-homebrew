#!/bin/sh
# Build dspfx (links the device's NEC OpenMAX libs, same recipe as ../test/build.sh and homebrew/dspdec)
# and ratetest (plain OSS + /dev/mem + /proc). Output binaries next to the sources.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
OMF=../rootfs_omf/omf
T="-target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O2 -s -Wall"
MSYS_NO_PATHCONV=1 "$ZIG" cc $T -o dspfx dspfx.c -L$OMF -Wl,--no-as-needed \
	-lomx_core -lomf_core_hw_dep -lomf_core_os_dep -lm -Wl,-rpath,/usr/lib/omf -Wl,--allow-shlib-undefined
"$ZIG" cc $T -o ratetest ratetest.c -lm
# device LinuxThreads libpthread lacks GLIBC_2.4 (also forces EABI4 e_flags)
python ../../../homebrew/hello/fixpthread.py dspfx ratetest || true
# SONAME-less OMF libs: strip the link directory from DT_NEEDED; EABI4 e_flags like Sony's binaries
python - <<'PY'
import struct, re
for f in ('dspfx', 'ratetest'):
    d = bytearray(open(f, 'rb').read())
    for m in list(re.finditer(rb'[^\0/\\]*[\\/][^\0]*[\\/](lib(omx|omf)_[a-z_]+\.so)\0', bytes(d))):
        s, name = m.start(), m.group(1)
        d[s:m.end()] = name + b'\0' * (m.end() - s - len(name))
    struct.pack_into('<I', d, 0x24, 0x04000002)
    open(f, 'wb').write(d)
PY
ls -la dspfx ratetest
