#!/bin/sh
# Build gnuboy for the Sony NW-A829 (zig cc against the device glibc 2.3.3)
set -e
cd "$(dirname "$0")"
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
SRC="lcd.c refresh.c lcdc.c palette.c cpu.c mem.c rtc.c hw.c sound.c events.c keytable.c menu.c loader.c save.c debug.c emu.c main.c rccmds.c rckeys.c rcvars.c rcfile.c exports.c split.c path.c inflate.c miniz_tinfl.c xz/xz_crc32.c xz/xz_crc64.c xz/xz_dec_lzma2.c xz/xz_dec_stream.c xz/xz_dec_bcj.c sys/walkman/walkman.c ../../common/gfx.c"
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O3 -fomit-frame-pointer -s -Wl,--as-needed -w -DIS_LITTLE_ENDIAN -DHAVE_USLEEP -I. -I../../common -o gnuboy $SRC
python ../../hello/fixpthread.py gnuboy
