#!/bin/sh
# Build the DSP test programs with the project's zig toolchain (same target as homebrew/).
# omxprobe/dspmp3 link directly against the device's OMF libs (copies in ../rootfs_omf/omf).
# No pthread symbols in our code: libomf_core_os_dep.so pulls the device's LinuxThreads libpthread in
# itself (DT_NEEDED), and zig's stubs would version pthread_* as GLIBC_2.4, which that libpthread lacks.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
OMF=../rootfs_omf/omf
T="-target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -Os"
"$ZIG" cc $T -o dspprobe dspprobe.c
# The OMF libs have no SONAME, so link with -l (DT_NEEDED = bare name). MSYS_NO_PATHCONV keeps the rpath literal.
for p in omxprobe dspmp3; do
	MSYS_NO_PATHCONV=1 "$ZIG" cc $T -Wall -o $p $p.c -L$OMF -Wl,--no-as-needed \
		-lomx_core -lomf_core_hw_dep -lomf_core_os_dep -Wl,-rpath,/usr/lib/omf -Wl,--allow-shlib-undefined
done
# device LinuxThreads libpthread lacks GLIBC_2.4: same fix as homebrew/hello (also forces EABI4 e_flags)
python ../../../homebrew/hello/fixpthread.py dspprobe omxprobe dspmp3 || true
# lld records the link path as DT_NEEDED for SONAME-less libs: strip the directory in .dynstr,
# and force EABI4 e_flags like Sony's binaries (fixpthread only does it when it patches)
python - <<'PY'
import struct, re
for f in ('omxprobe', 'dspmp3'):
    d = bytearray(open(f, 'rb').read())
    for m in list(re.finditer(rb'[^\0/\\]*[\\/][^\0]*[\\/](lib(omx|omf)_[a-z_]+\.so)\0', bytes(d))):
        s, name = m.start(), m.group(1)
        d[s:m.end()] = name + b'\0' * (m.end() - s - len(name))
    struct.pack_into('<I', d, 0x24, 0x04000002)
    open(f, 'wb').write(d)
PY
ls -la dspprobe omxprobe dspmp3
