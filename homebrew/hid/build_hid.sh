#!/bin/sh
# Build the Bluetooth HID spike (hidtest) for the NW-A829: zig cc, device glibc 2.4 ABI, ARM926EJ-S
set -e
cd "$(dirname "$0")"
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -Os -s -Wl,--as-needed -Wall -Wextra -Wno-unused-parameter -I. \
	-o hidtest hidtest.c hid.c btcm.c ../common/gfx.c
python ../hello/fixpthread.py hidtest
ls -la hidtest
