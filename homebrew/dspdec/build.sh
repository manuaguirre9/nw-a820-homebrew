#!/bin/sh
# dspdec: MP3 / ATRAC / AAC / WMA and Sony effects on the MP201 audio DSP, FTB or direct (helper process for hbplayer). Links the device's NEC OpenMAX libraries
# (on the device they live in /usr/lib/omf). By default it links against stand-ins built from omx_stub.c, so no
# Sony/NEC file is needed; OMF=<dir> links against copies of the real libraries instead (same result on the device).
# Same recipe as research/dsp/test/build.sh: no pthread symbols of our own, SONAME-less libs fixed up after linking.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
if [ -z "$OMF" ]; then
	OMF=stubs
	mkdir -p $OMF
	for l in omx_core omf_core_hw_dep omf_core_os_dep; do
		"$ZIG" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O2 -shared -nostdlib -Wl,-soname,lib$l.so -o $OMF/lib$l.so omx_stub.c
	done
fi
MSYS_NO_PATHCONV=1 "$ZIG" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O2 -s -Wall -Wno-unused-function -I../third_party -o dspdec dspdec.c -L"$OMF" \
	-Wl,--no-as-needed -lomx_core -lomf_core_hw_dep -lomf_core_os_dep -lm -Wl,-rpath,/usr/lib/omf -Wl,--allow-shlib-undefined
python ../hello/fixpthread.py dspdec || true
python - <<'PY'
import struct, re
d = bytearray(open('dspdec', 'rb').read())
for m in list(re.finditer(rb'[^\0/\\]*[\\/][^\0]*[\\/](lib(omx|omf)_[a-z_]+\.so)\0', bytes(d))):
    s, name = m.start(), m.group(1)
    d[s:m.end()] = name + b'\0' * (m.end() - s - len(name))
struct.pack_into('<I', d, 0x24, 0x04000002)
open('dspdec', 'wb').write(d)
PY
ls -la dspdec
