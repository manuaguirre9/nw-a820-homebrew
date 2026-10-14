#!/bin/sh
# DSP experiment 4: why DSEE (+0x0c = 0x0f on the effect page) did nothing in experiment 3, plus the first
# on-device test of the ATRAC path of dspdec. See effects/dsee_notes.txt for the hypotheses behind each run.
# Run by the 1.02-4x hook BEFORE the Sony UI starts (the DSP must be ours). One-shot.
# Copy to H:\HB\bin: dspfx (new build: -a / mp3r / >11k stats), dspdec (new build), test_at3.oma
# (research/dsp/test); test.mp3 / test_st.mp3 are already there. This file to H:\HB\boot.sh (LF line endings!).
# Results in H:\HB\out\dsee\. Each run is killed after 60 s (dspfx has its own 50 s watchdog).
# The LAST part plays sound on the headphones by itself (render mode, codec volume 60 = about -10 dB):
#   r_vpt  (16 s): VPT 3 switched on/off every 4 s - the positive control, clearly audible
#   r_dsee (32 s): DSEE switched on/off every 4 s on the 64 kbps / 22 kHz audiobook - listen for "s" / "t"
#                  sounds getting brighter / sharper in the B periods
#   r_cs   (16 s): Clear Stereo on/off every 4 s on the podcast
# The starts are in events.txt (uptime) and each r_*.txt has "AB A/B at ..." lines.
O=/contents/HB/out/dsee
B=/contents/HB/bin
FXO=/contents/HB/out/fx
mkdir -p $O
echo "start uptime=$(cat /proc/uptime)" > $O/events.txt
cp $B/dspfx /tmp/dspfx && chmod 755 /tmp/dspfx
cp $B/dspdec /tmp/dspdec_t && chmod 755 /tmp/dspdec_t
cat /proc/interrupts > $O/interrupts_before.txt

# run NAME prog args... : 60 s kill, output in $O/NAME.txt, exit code in $RC
run() {
	N=$1
	shift
	echo "== $N: $* (uptime $(cat /proc/uptime))" >> $O/events.txt
	"$@" > $O/$N.txt 2>&1 &
	P=$!
	I=0
	while [ $I -lt 60 ] && kill -0 $P 2> /dev/null; do sleep 1; I=$(expr $I + 1); done
	kill -9 $P 2> /dev/null && echo "$N killed after 60 s" >> $O/events.txt
	wait $P
	RC=$?
	echo "rc=$RC" >> $O/$N.txt
	echo "   $N rc=$RC after ${I}s" >> $O/events.txt
	grep -e '^STATS' -e 'realtime' -e 'RESULT' -e 'stuck' -e 'SetParameter' -e 'PARAM set' -e 'error' $O/$N.txt >> $O/events.txt
	sync
}
FX="/tmp/dspfx"
S=12		# seconds of output per FTB run; A/B every 3 s -> seg0 = A (0-3, 6-9 s), seg1 = B (3-6, 9-12 s)
AB="-s $S -x bypass -a 3:dsee=1"

run page_boot $FX page
# 1. FTB MP3, audiobook (64 kbps MPEG-2, 22.05 kHz mono: nothing above 11 kHz, DSEE's best case)
#    a. as in experiment 3 (+0x12 = 0x27, gemini's "no bitrate" value), now with A/B and >11k stats
run ab_base $FX mp3 $AB $O/ab_base.wav $B/test.mp3
#    b. +0x12 = the real bitrate in kbps (gemini sends stream bitrate there when it knows it)
run ab_br64 $FX mp3 -P 0x12=64 $AB $O/ab_br64.wav $B/test.mp3
#    c. + the +0x14 flag gemini sets from its stream info
run ab_br64_f14 $FX mp3 -P 0x12=64 -P 0x14=1 $AB $O/ab_br64_f14.wav $B/test.mp3
#    d. other bitrates: very low (DSEE should work hardest) and 0
run ab_br32 $FX mp3 -P 0x12=32 $AB $O/ab_br32.wav $B/test.mp3
run ab_br0 $FX mp3 -P 0x12=0 $AB $O/ab_br0.wav $B/test.mp3
#    e. +0 = 7 (accepted 6..7, meaning unknown)
run ab_p0_7 $FX mp3 -P 0=7 -P 0x12=64 $AB $O/ab_p0_7.wav $B/test.mp3
# 2. the page value: gemini stores 0x0f; try 1, 0xff, 0xffff
run ab_dsx1 $FX mp3 -P 0x12=64 -s $S -x bypass -a 3:dsx=1 $O/ab_dsx1.wav $B/test.mp3
run ab_dsxff $FX mp3 -P 0x12=64 -s $S -x bypass -a 3:dsx=0xff $O/ab_dsxff.wav $B/test.mp3
run ab_dsxffff $FX mp3 -P 0x12=64 -s $S -x bypass -a 3:dsx=0xffff $O/ab_dsxffff.wav $B/test.mp3
# 3. the rest of gemini's normal (headphone) block: volume level -727 at +0x12 of the page (0 is gemini's
#    BYPASS value), the +0x1e output flag, and DSEE together with another effect already running
run ab_gain $FX mp3 -P 0x12=64 -s $S -x gain=-727 -a 3:dsee=1,gain=-727 $O/ab_gain.wav $B/test.mp3
run ab_flag $FX mp3 -P 0x12=64 -s $S -x gain=-727,flag=1 -a 3:dsee=1,gain=-727,flag=1 $O/ab_flag.wav $B/test.mp3
run ab_eqflat $FX mp3 -P 0x12=64 -s $S -x eq=flat,gain=-727 -a 3:eq=flat,gain=-727,dsee=1 $O/ab_eqflat.wav $B/test.mp3
# 4. the 128 kbps / 44.1 kHz podcast (content to ~16 kHz: look at >16k)
run st_br128 $FX mp3 -P 0x12=128 -s $S -x gain=-727 -a 3:dsee=1,gain=-727 $O/st_br128.wav $B/test_st.mp3
# 5. LPCM: the band-limited audiobook PCM from experiment 3, with a "lossy bitrate" in +0x12
if [ -s $FXO/ab_bypass.wav ]; then
	run pcm_br64 $FX pcm -P 0x12=64 -P 0x14=1 -s $S -x gain=-727 -a 3:dsee=1,gain=-727 $O/pcm_br64.wav $FXO/ab_bypass.wav
fi
run page_mid $FX page
# 6. ATRAC on the DSP through dspdec's test mode (decode to raw PCM, STATS / realtime)
if [ -f $B/test_at3.oma ]; then
	run at3_test /tmp/dspdec_t - -d at3 $B/test_at3.oma $O/at3_test.raw
	run at3_noflag /tmp/dspdec_t - -d at3 $B/test_at3.oma $O/at3_noflag.raw -F 0
fi
for F in $B/test_atx.oma $B/test_aal.oma; do
	if [ -f $F ]; then
		C=$(basename $F .oma | sed 's/test_//')
		run ${C}_test /tmp/dspdec_t - -d $C $F $O/${C}_test.raw -s 20
	fi
done
# 7. RENDER mode (the non-FTB firmware, which has ~170 KB more resident DSP code and 4 more references to the
#    effect page than the FTB one): the DSP plays to the headphones. Listening test, see the header.
run r_vpt $FX mp3r -V 60 -s 16 -x bypass -a 4:vpt=3 - $B/test_st.mp3
run r_dsee $FX mp3r -V 60 -s 32 -P 0x12=64 -x gain=-727 -a 4:dsee=1,gain=-727 - $B/test.mp3
run r_cs $FX mp3r -V 60 -s 16 -x gain=-727 -a 4:cs=1,gain=-727 - $B/test_st.mp3
run page_end $FX page
cat /proc/interrupts > $O/interrupts_after.txt
dmesg | tail -60 > $O/dmesg_tail.txt
ls -l $O > $O/files.txt
echo "end uptime=$(cat /proc/uptime)" >> $O/events.txt
sync
