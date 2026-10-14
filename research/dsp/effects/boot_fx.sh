#!/bin/sh
# DSP experiment 3: Sony's sound effects (DSEE / VPT / EQ / Clear Stereo / Dynamic Normalizer) on the DSP
# from our own code, through the effect page at phys 0x33EA0000 (see effects_notes.txt), for MP3
# (OMX.NEC.EL.MP3D.FTB) and for our own PCM (OMX.NEC.EL.LPCMD.FTB); then the wired-output rate test.
# Run by the 1.02-4x hook BEFORE the Sony UI starts (the DSP must be ours). One-shot.
# Copy to H:\HB\bin: dspfx, ratetest (test.mp3 / test_st.mp3 are already there); this file to
# H:\HB\boot.sh (LF line endings!). Results in H:\HB\out\fx\. Each run is killed after 60 s (dspfx has
# its own 45 s watchdog). ratetest plays 1 kHz tones at -20 dBFS: keep headphones away from the ears.
O=/contents/HB/out/fx
B=/contents/HB/bin
mkdir -p $O
echo "start uptime=$(cat /proc/uptime)" > $O/events.txt
cp $B/dspfx $B/ratetest /tmp/ && chmod 755 /tmp/dspfx /tmp/ratetest
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
	grep -e '^STATS' -e 'realtime' -e 'fx commit' -e 'stuck' -e 'MEASURED' $O/$N.txt >> $O/events.txt
	sync
}
FX="/tmp/dspfx"
S=12		# seconds of output per MP3 run

# 0. the effect page as the boot left it (read-only)
run page_boot $FX page
# 1. MP3 baselines: page untouched (= dspmp3 / dspdec behaviour), then gemini's bypass block
run st_raw $FX mp3 -s $S $O/st_raw.wav $B/test_st.mp3
RAW_RC=$RC
run st_bypass $FX mp3 -x bypass -s $S $O/st_bypass.wav $B/test_st.mp3
if [ $RAW_RC -eq 0 ] || [ $RC -eq 0 ]; then
	# 2. DSEE on the 128k stereo podcast and on the 64k 22 kHz mono audiobook (where it should matter)
	run st_dsee $FX mp3 -x dsee=1 -s $S $O/st_dsee.wav $B/test_st.mp3
	run ab_bypass $FX mp3 -x bypass -s $S $O/ab_bypass.wav $B/test.mp3
	run ab_dsee $FX mp3 -x dsee=1 -s $S $O/ab_dsee.wav $B/test.mp3
	# 3. live change while playing: bypass for 6 s, then DSEE (STATS seg0 vs seg1)
	run ab_toggle $FX mp3 -x bypass -t 6:dsee=1 -s $S $O/ab_toggle.wav $B/test.mp3
	# 4. VPT modes 0 and 3, Clear Stereo strength 1, Dynamic Normalizer
	run st_vpt0 $FX mp3 -x vpt=0 -s $S $O/st_vpt0.wav $B/test_st.mp3
	run st_vpt3 $FX mp3 -x vpt=3 -s $S $O/st_vpt3.wav $B/test_st.mp3
	run st_cs1 $FX mp3 -x cs=1 -s $S $O/st_cs1.wav $B/test_st.mp3
	run st_dn $FX mp3 -x dn=1 -s $S $O/st_dn.wav $B/test_st.mp3
	# 5. EQ: preset Heavy, and a custom curve that tells the band order / sign (band1 = 0, band6 = 6)
	run st_eq_heavy $FX mp3 -x eq=heavy -s $S $O/st_eq_heavy.wav $B/test_st.mp3
	run st_eq_0to6 $FX mp3 -x eq=0:3:3:3:3:6 -s $S $O/st_eq_0to6.wav $B/test_st.mp3
	# 6. our own PCM through the LPCM "decoder": generated log sweeps (frequency response = EQ curve)
	run pcm_sweep_bypass $FX pcm -x bypass -s 10 $O/pcm_sweep_bypass.wav gen:44100:2:10
	run pcm_sweep_eq $FX pcm -x eq=0:3:3:3:3:6 -s 10 $O/pcm_sweep_eq.wav gen:44100:2:10
	run pcm_sweep_dsee $FX pcm -x dsee=1 -s 10 $O/pcm_sweep_dsee.wav gen:44100:2:10
	run pcm_lr_vpt0 $FX pcm -x vpt=0 -s 10 $O/pcm_lr_vpt0.wav gen:44100:2:10:lr
	run pcm_lr_cs1 $FX pcm -x cs=1 -s 10 $O/pcm_lr_cs1.wav gen:44100:2:10:lr
	# resampler: 22.05 kHz mono in, 44.1 kHz stereo out
	run pcm_2205_mono $FX pcm -x bypass -s 8 $O/pcm_2205_mono.wav gen:22050:1:8
	# 48 kHz in, 44.1 kHz out (DSP as sample-rate converter for 48k material)
	run pcm_48k $FX pcm -x bypass -s 8 $O/pcm_48k.wav gen:48000:2:8
	# real music as PCM: the DSP-decoded bypass WAV fed back through LPCMD with and without DSEE / VPT
	if [ -s $O/st_bypass.wav ]; then
		run pcm_st_bypass $FX pcm -x bypass -s $S $O/pcm_st_bypass.wav $O/st_bypass.wav
		run pcm_st_dsee $FX pcm -x dsee=1 -s $S $O/pcm_st_dsee.wav $O/st_bypass.wav
		run pcm_st_vpt0 $FX pcm -x vpt=0 -s $S $O/pcm_st_vpt0.wav $O/st_bypass.wav
	fi
	run page_end $FX page
fi
# 7. wired output: rates / formats the OSS driver accepts, measured playback rate, 88.2k codec experiment
run ratetest /tmp/ratetest -H -d 3
ls /proc/icx1087audio > $O/proc_icx1087audio.txt 2>&1
cat /proc/interrupts > $O/interrupts_after.txt
dmesg | tail -80 > $O/dmesg_tail.txt
ls -l $O > $O/files.txt
echo "end uptime=$(cat /proc/uptime)" >> $O/events.txt
sync
