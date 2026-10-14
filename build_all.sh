#!/bin/sh
# Build every program the Walkman needs and stage them as build/HB, ready to copy to H:\HB.
#   ZIG=/path/to/zig sh build_all.sh
set -e
cd "$(dirname "$0")"
H=homebrew
sh $H/hbplayer/build.sh
sh $H/hbwatch/build.sh
sh $H/hbusb/build.sh
sh $H/dspdec/build.sh
sh $H/third_party/gnuboy-master/build_walkman.sh
sh $H/nes/build_nes.sh
sh $H/gba/build.sh
sh $H/doom/build_doom.sh
rm -rf build/HB
mkdir -p build/HB/bin
cp $H/hbplayer/hbplayer $H/hbwatch/hbwatch $H/hbusb/hbusb $H/dspdec/dspdec $H/third_party/gnuboy-master/gnuboy \
	$H/nes/nes $H/gba/gba $H/doom/doom build/HB/bin/
cp $H/boot/boot.sh $H/boot/boot_dual.sh build/HB/
echo "staged build/HB:"
ls -la build/HB build/HB/bin
