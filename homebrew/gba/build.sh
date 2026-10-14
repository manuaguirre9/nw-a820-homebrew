#!/bin/sh
# Build the GBA emulator (libretro gpSP core + walkman_gba.c frontend) for the Sony NW-A829.
# zig cc against the device glibc 2.3.3 (dynamic), ARM926EJ-S, ARM dynarec. Output: ./gba
# Usage: sh build.sh            (from Git Bash on Windows)
set -e
cd "$(dirname "$0")"
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
G=../third_party/gpsp
L=$G/libretro/libretro-common
O=obj
mkdir -p $O

# -DWALKMAN selects our patches in the core (OABI cacheflush, runtime ROM buffer size).
# No MMAP_JIT_CACHE: the translation caches live in the binary's own .jit section (awx, nobits), so the JIT code
# is always within ARM branch range of the asm stubs (like gpSP's miyoo / ARM926 build).
CFLAGS="-target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -marm -O2 -fomit-frame-pointer -ffast-math -fno-strict-aliasing
 -DWALKMAN -DARM_ARCH -DHAVE_DYNAREC -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=32
 -D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565 -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DINLINE=inline
 -I$G -I$G/libretro -I$L/include -I../common -w"
CXXFLAGS="-std=c++11 -fno-rtti -fno-exceptions -fno-threadsafe-statics"

SRC_C="$G/main.c $G/gba_memory.c $G/savestate.c $G/input.c $G/sound.c $G/cheats.c $G/memmap.c $G/serial.c
 $G/gbp.c $G/rfu.c $G/serial_proto.c $G/libretro/libretro.c $G/gba_cc_lut.c $G/cpu_threaded.c
 $L/compat/compat_posix_string.c $L/compat/compat_strl.c $L/compat/fopen_utf8.c $L/encodings/encoding_utf.c
 $L/file/file_path.c $L/file/file_path_io.c $L/streams/file_stream.c $L/string/stdstring.c $L/time/rtime.c
 $L/vfs/vfs_implementation.c
 walkman_gba.c ../common/gfx.c"
SRC_CC="$G/video.cc $G/cpu.cc"
SRC_S="$G/arm/arm_stub.S $G/bios_data.S"

OBJS=""
for f in $SRC_C $SRC_S; do
	o=$O/$(echo "$f" | sed 's#^\.\./##; s#[/.]#_#g').o
	"$Z" cc $CFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done
for f in $SRC_CC; do
	o=$O/$(echo "$f" | sed 's#^\.\./##; s#[/.]#_#g').o
	"$Z" cc $CFLAGS $CXXFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done
# bios_data.S does .incbin "bios/open_gba_bios.bin" relative to the assembler's cwd / include path
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O2 -s -Wl,--as-needed -o gba $OBJS
python ../hello/fixpthread.py gba
ls -l gba
