#!/bin/sh
# PC smoke test: same gpSP core + options as the Walkman build, interpreter (no ARM dynarec), Windows x86_64.
#   sh tools/build_hosttest.sh && cd tools && ./hosttest.exe ../roms/ucity-advance.gba 900 start
# Writes tools/out/<rom>.ppm (last frame) and <rom>_rot.ppm (mode-0 rotated framebuffer as on the Walkman).
set -e
cd "$(dirname "$0")"
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
G=../../third_party/gpsp
L=$G/libretro/libretro-common
mkdir -p hostobj out
F="-target x86_64-windows-gnu -O2 -w -D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565 -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DINLINE=inline -I$G -I$G/libretro -I$L/include"
OBJS=""
for f in $G/main.c $G/gba_memory.c $G/savestate.c $G/input.c $G/sound.c $G/cheats.c $G/memmap.c $G/serial.c $G/gbp.c \
	$G/rfu.c $G/serial_proto.c $G/libretro/libretro.c $G/gba_cc_lut.c $L/compat/compat_posix_string.c $L/compat/compat_strl.c \
	$L/compat/fopen_utf8.c $L/encodings/encoding_utf.c $L/file/file_path.c $L/file/file_path_io.c $L/streams/file_stream.c \
	$L/string/stdstring.c $L/time/rtime.c $L/vfs/vfs_implementation.c $G/bios_data.S hosttest.c $G/video.cc $G/cpu.cc; do
	o=hostobj/$(basename $f).o
	X=""
	case $f in *.cc) X="-std=c++11 -fno-rtti -fno-exceptions -fno-threadsafe-statics";; esac
	"$Z" cc $F $X -c $f -o $o
	OBJS="$OBJS $o"
done
"$Z" cc -target x86_64-windows-gnu -O2 -o hosttest.exe $OBJS
ls -l hosttest.exe
