#!/bin/sh
# DSP experiment 5 (one-shot): DIRECT playback ("maximum battery": the DSP decodes and plays to the codec
# itself), AAC and WMA on the DSP, CPU of FAAD2 vs the DSP vs direct. See research/dsp/notes/direct_notes.txt
# and aac_wma_notes.txt. Runs from /contents/HB/once/ before the Sony UI (boot.sh). Results: H:\HB\out\dsp3\.
#
# COPY to H:\HB\bin\ (from homebrew\dspdec, homebrew\hbplayer, research\dsp\test):
#   dspdec  hbplayer  test_aac.m4a  test_aac.aac  test_aac22m.m4a  test_aac_long.m4a  test_wma.wma  test_wma22m.wma
#   (test.mp3 / test_st.mp3 / test_at3.oma are already there; an HE-AAC file of your own as test_he.m4a is used
#   if present). This file to H:\HB\once\once_dsp3.sh (LF line endings).
#
# LISTEN (headphones, codec volume 60 = about -10 dB). The part-1 runs play by themselves, in this order:
#   d_st     10 s  the podcast (test_st.mp3), plain                 -> music?  (events.txt has the start times)
#   d_dsee   16 s  the audiobook (64 kbps / 22 kHz), DSEE switched on at 4 s, off at 8, on at 12
#                  (brighter "s" / "t" in the on periods; the log's page +1c = 1 while DSEE works)
#   d_pause  14 s  the podcast: 2 s of silence at 4 s (pause), then at 9 s a jump to the middle of the file
#   d_at3    10 s  ATRAC3 test tone: left, right, both (and a quiet end)
#   d_aac    12 s  AAC test tone 440 Hz LEFT 4 s, 660 Hz RIGHT 4 s, both 4 s
#   d_wma    12 s  WMA test tone, same pattern
#   st_*_direct (part 4, 30 s each): the audiobook, then the long AAC file, through hbplayer in direct mode
# Every direct run also proves itself without a listener: see "VERDICT" (the PCM DMA channel's source pointer
# moving through DSP memory) and "pace" (seconds consumed by the DSP per wall second ~ 1.00 = paced by the DAC).
O=/contents/HB/out/dsp3
B=/contents/HB/bin
mkdir -p $O
echo "start uptime=$(cat /proc/uptime)" > $O/events.txt
cp $B/dspdec /tmp/dspdec && chmod 755 /tmp/dspdec
cp $B/hbplayer /tmp/hbplayer && chmod 755 /tmp/hbplayer
D=/tmp/dspdec
cat /proc/interrupts > $O/interrupts_before.txt

# stop P: SIGTERM first (dspdec tears the OMX session down: the PCM interrupt goes back to the CPU), SIGKILL later
stopit() {
	kill $1 2> /dev/null
	J=0
	while [ $J -lt 4 ] && kill -0 $1 2> /dev/null; do sleep 1; J=$(expr $J + 1); done
	kill -9 $1 2> /dev/null && echo "   (SIGKILL needed)" >> $O/events.txt
}
# run NAME prog args... : 60 s limit, output in $O/NAME.txt
run() {
	N=$1
	shift
	echo "== $N: $* (uptime $(cat /proc/uptime))" >> $O/events.txt
	"$@" > $O/$N.txt 2>&1 &
	P=$!
	I=0
	while [ $I -lt 60 ] && kill -0 $P 2> /dev/null; do sleep 1; I=$(expr $I + 1); done
	if kill -0 $P 2> /dev/null; then echo "   $N stopped after 60 s" >> $O/events.txt; stopit $P; fi
	wait $P
	RC=$?
	echo "rc=$RC" >> $O/$N.txt
	echo "   $N rc=$RC after ${I}s" >> $O/events.txt
	grep -e '^RESULT' -e '^VERDICT' -e '^STATS' -e '^FORMAT' -e 'SetParameter' -e 'error' -e '^EOS' -e '^ERROR' -e '^PAUSE' -e '^RESUME' -e '^SEEK' -e 'buffer flag' -e 'stalled' -e 'failed' $O/$N.txt >> $O/events.txt
	sync
}

# ---- 1. DIRECT playback (priority): the DSP plays to the headphones ----
G="-V 60 -G 0,fd29,8001"	# codec volume 60; page: DSEE off, level -727 (gemini's headphone value), Clear Stereo off
run d_st $D - -r mp3 $B/test_st.mp3 -s 10 $G
run d_dsee $D - -r mp3 $B/test.mp3 -s 16 -a 4 $G
run d_pause $D - -r mp3 $B/test_st.mp3 -s 14 -p 4 -j 9 $G
[ -f $B/test_at3.oma ] && run d_at3 $D - -r at3 $B/test_at3.oma -s 10 $G
[ -f $B/test_aac.m4a ] && run d_aac $D - -r aac $B/test_aac.m4a -s 12 $G
[ -f $B/test_wma.wma ] && run d_wma $D - -r wma $B/test_wma.wma -s 12 $G
cat /proc/interrupts > $O/interrupts_direct.txt

# ---- 2. AAC decoded on the DSP (FTB: PCM back to the ARM) -> raw 44.1k stereo S16 in $O/*.raw ----
# expected (test_aac*): 12 s, L then R then both at about -15 dBFS (STATS averages over the 12 s: ~ -16.8 dB)
if [ -f $B/test_aac.m4a ]; then
	run aac_m4a $D - -d aac $B/test_aac.m4a $O/aac_m4a.raw			# raw access units, +8 = 0 (the player's way)
	run aac_m4a_p8 $D - -d aac $B/test_aac.m4a $O/aac_m4a_p8.raw -P 8=2		# raw AUs but +8 = 2 (in case 2 means raw)
	run aac_adts_raw $D - -d aac $B/test_aac.aac $O/aac_adts_raw.raw -A 0		# ADTS file, headers stripped, +8 = 0
	run aac_adts_whole $D - -d aac $B/test_aac.aac $O/aac_adts_whole.raw -A 2	# ADTS frames whole, +8 = 2 (gemini's DTV way)
	run aac22m $D - -d aac $B/test_aac22m.m4a $O/aac22m.raw			# 22.05 kHz mono (rate code 7, 1 channel)
fi
F="/contents/Music/Pruebas FLAC-AAC/prueba_aac.m4a"
[ -f "$F" ] && run aac_prueba $D - -d aac "$F" $O/aac_prueba.raw
[ -f $B/test_he.m4a ] && run aac_he $D - -d aac $B/test_he.m4a $O/aac_he.raw
[ -f $B/test_he.m4a ] && run aac_he_c4 $D - -d aac $B/test_he.m4a $O/aac_he_c4.raw -P 0xc=4	# rate code of the SBR output rate

# ---- 3. WMA on the DSP (FTB) ----
if [ -f $B/test_wma.wma ]; then
	run wma $D - -d wma $B/test_wma.wma $O/wma.raw
	run wma_spb $D - -d wma $B/test_wma.wma $O/wma_spb.raw -P 0x20=0x800	# samples-per-block 2048 instead of ffmpeg's 0
	run wma22m $D - -d wma $B/test_wma22m.wma $O/wma22m.raw
fi

# ---- 4. CPU: FAAD2 vs AAC on the DSP vs direct; MP3 on the DSP vs direct (hbplayer selftest, 30 s each) ----
mount -t ext2 /dev/mtdblock/3 /var > /dev/null 2>&1
mkdir -p /tmp/stA
st() {	# name mode(faad|dsp|direct) file
	echo "== $1 $(cat /proc/uptime)" >> $O/events.txt
	case $2 in
	faad) HB_NODSP=1 HB_MHZ=124 /tmp/hbplayer selftest /tmp/stA "$3" 30 > /dev/null 2>&1 & ;;
	direct) HB_DIRECT=1 HB_MHZ=124 /tmp/hbplayer selftest /tmp/stA "$3" 30 > /dev/null 2>&1 & ;;
	*) HB_MHZ=124 /tmp/hbplayer selftest /tmp/stA "$3" 30 > /dev/null 2>&1 & ;;
	esac
	P=$!; I=0
	while [ $I -lt 50 ] && kill -0 $P 2> /dev/null; do sleep 1; I=$(expr $I + 1); done
	kill -0 $P 2> /dev/null && { echo "   $1 stopped" >> $O/events.txt; stopit $P; }
	killall dspdec > /dev/null 2>&1; sleep 1; killall -9 dspdec > /dev/null 2>&1
	cat /tmp/stA/log.txt >> $O/$1.txt; rm -f /tmp/stA/log.txt
	[ -f /tmp/stA/dspdec.log ] && { echo "---- dspdec.log" >> $O/$1.txt; cat /tmp/stA/dspdec.log >> $O/$1.txt; rm -f /tmp/stA/dspdec.log; }
	grep -e 'selftest:' -e 'direct' -e 'dsp:' $O/$1.txt | tail -8 >> $O/events.txt
	sync
}
MP3=$(ls /contents/Audiolibros/*/*.mp3 | head -n 1)
if [ -f $B/test_aac_long.m4a ]; then
	st st_aac_faad faad $B/test_aac_long.m4a
	st st_aac_dsp dsp $B/test_aac_long.m4a
	st st_aac_direct direct $B/test_aac_long.m4a
fi
st st_mp3_dsp dsp "$MP3"
st st_mp3_direct direct "$MP3"
echo M124 > /proc/pm
umount /var > /dev/null 2>&1

cat /proc/interrupts > $O/interrupts_after.txt
dmesg | tail -40 > $O/dmesg_tail.txt
ls -l $O > $O/files.txt
echo "end uptime=$(cat /proc/uptime)" >> $O/events.txt
sync
