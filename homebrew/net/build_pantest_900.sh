#!/bin/sh
# Build the PAN feasibility test (lwIP + mbedTLS + our BNEP/L2CAP client) for the NW-A829
set -e
cd "$(dirname "$0")"
L=../third_party/lwip-2.1.3/src
T=../third_party/mbedtls-2.28.8
Z="${ZIG:-zig}"	# zig 0.15.2 (set ZIG if it is not on PATH)
"$Z" cc -target arm-linux-gnueabi.2.4 -mcpu=arm926ej_s -Os -s -DTCP_MSS=900 -Wl,--as-needed -w -I. -I$L/include -I$T/include -I$T/library \
	-o pantest_900 pantest.c btcm.c bnep.c net.c rss.c \
	$L/core/*.c $L/core/ipv4/*.c $L/netif/ethernet.c $L/apps/altcp_tls/altcp_tls_mbedtls.c $L/apps/altcp_tls/altcp_tls_mbedtls_mem.c \
	$T/library/*.c
python ../hello/fixpthread.py pantest_900
ls -la pantest
