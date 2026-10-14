#!/bin/sh
# Build hbplayer for the NW-A829 (zig cc against the device glibc 2.3.3; see memory/toolchain notes)
set -e
OUT=${OUT:-hbplayer}	# OUT=hbtest EXTRA=-DHB_DEV: the on-device test build (build_test.sh)
cd "$(dirname "$0")"
M=../third_party/libmad-0.15.1b
F=../third_party/faad2-2.11.1
L=../third_party/lwip-2.1.3/src
T=../third_party/mbedtls-2.28.8
N=../net
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -O2 -s -Wl,--as-needed -Wall -Wno-unused -Wno-misleading-indentation \
	-DFIXED_POINT -DFPM_ARM -DOPT_SPEED -DASO_IMDCT -DASO_INTERLEAVE1 -DASO_ZEROCHECK -DHAVE_CONFIG_H -I$M \
	-DPACKAGE_VERSION='"2.11.1"' -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DHAVE_STRING_H -DHAVE_STRINGS_H -DHAVE_MEMCPY -DHAVE_STDLIB_H -DHAVE_SYS_TYPES_H -DHAVE_MATH_H -DHAVE_FLOAT_H \
	-I$F/include -I$F/libfaad -I../third_party -I../third_party/sonic -I$N -I../third_party/zlib-1.3.1 -I$L/include -I$T/include -I$T/library \
	-DHB_VERSION="\"1.1 ($(date +%Y-%m-%d))\"" $EXTRA -o $OUT hbplayer.c audio.c bt.c focus.c screens.c engine.c music.c cover.c dec.c eq.c settings.c stretch.c lyrics.c alarm.c podsync.c podsearch.c podui.c kbd.c i18n.c batt.c chapters.c dspclient.c rsmp.c fsname.c voice.c warm.c ../third_party/sonic/sonic.c \
	$N/pan.c $N/btcm.c $N/bnep.c $N/net.c $N/rss.c ../third_party/zlib-1.3.1/inflate.c ../third_party/zlib-1.3.1/inftrees.c ../third_party/zlib-1.3.1/inffast.c ../third_party/zlib-1.3.1/adler32.c ../third_party/zlib-1.3.1/crc32.c ../third_party/zlib-1.3.1/zutil.c \
	../third_party/tjpgd/src/tjpgd.c ../common/gfx.c \
	$M/bit.c $M/decoder.c $M/fixed.c $M/frame.c $M/huffman.c $M/layer12.c $M/layer3.c $M/stream.c $M/synth.c $M/timer.c $M/version.c $M/imdct_l_arm.S \
	$F/libfaad/*.c \
	$L/core/*.c $L/core/ipv4/*.c $L/netif/ethernet.c $L/apps/altcp_tls/altcp_tls_mbedtls.c $L/apps/altcp_tls/altcp_tls_mbedtls_mem.c \
	$T/library/*.c -lm
python ../hello/fixpthread.py $OUT
# the device has only these shared libraries: anything else (librt: clock_gettime, ...) and the player won't start
python -c "import re,sys; d=open('$OUT', 'rb').read(); bad=[x.decode() for x in set(re.findall(rb'lib[a-z0-9_]+\.so[.0-9]*', d)) if x not in (b'libc.so.6', b'libm.so.6', b'libdl.so.2', b'libpthread.so.0')]; print('NEEDED OK' if not bad else 'NEEDED MISSING ON DEVICE: ' + ' '.join(bad)); sys.exit(1 if bad else 0)"
