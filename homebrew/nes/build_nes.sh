#!/bin/sh
# Build the NES emulator (agnes core) for the NW-A829
set -e
cd "$(dirname "$0")"
A=../third_party/agnes-master
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O3 -fomit-frame-pointer -s -Wl,--as-needed -w -I$A -o nes nes.c $A/agnes.c
python ../hello/fixpthread.py nes
ls -la nes
