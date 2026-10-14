#!/bin/sh
# Build hbwatch (HOME-hold watcher that runs next to the Sony UI) for the NW-A829
set -e
cd "$(dirname "$0")"
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -Os -s -Wl,--as-needed -Wall -Wno-unused -o hbwatch hbwatch.c ../common/gfx.c
python ../hello/fixpthread.py hbwatch
ls -la hbwatch
