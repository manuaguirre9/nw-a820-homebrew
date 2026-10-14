#!/bin/sh
# DSP experiment 2: decode MP3 on the DSP (OMX.NEC.EL.MP3D.FTB), PCM back to the ARM, measure ARM cost.
# Run by the 1.02-4x hook BEFORE the Sony UI starts (the DSP must be ours). One-shot.
# Copy to H:\HB\bin: dspmp3, test.mp3 (1 MB of the Quiroga audiobook: MPEG2 L3 22.05 kHz mono 64 kbps,
# 131 s) and test_st.mp3 (512 KB podcast: MPEG1 L3 44.1 kHz joint stereo 128 kbps, 28 s); this file
# to H:\HB\boot.sh (LF line endings!). Results in H:\HB\out\dsp2\. Each run is killed after 60 s
# (dspmp3 has its own 45 s watchdog). The Sony UI starts normally afterwards.
# A wedged DSP only breaks audio until the next reboot (gemini reloads the firmware on its own init).
O=/contents/HB/out/dsp2
B=/contents/HB/bin
FULL=0		# 1 = also decode the first real audiobook file (6.6 MB, 14 min) with a 40 s watchdog
mkdir -p $O
echo "start uptime=$(cat /proc/uptime)" > $O/events.txt
cp $B/dspmp3 /tmp/ && chmod 755 /tmp/dspmp3
cat /proc/interrupts > $O/interrupts_before.txt
cat /proc/meminfo > $O/meminfo.txt
cat /proc/cpuinfo > $O/cpuinfo.txt 2> /dev/null

# run NAME args... : runs /tmp/dspmp3 with a 60 s kill, output in $O/NAME.txt, exit code in $RC
run() {
	N=$1
	shift
	echo "== $N: $* (uptime $(cat /proc/uptime))" >> $O/events.txt
	/tmp/dspmp3 "$@" > $O/$N.txt 2>&1 &
	P=$!
	I=0
	while [ $I -lt 60 ] && kill -0 $P 2> /dev/null; do sleep 1; I=$(expr $I + 1); done
	kill -9 $P 2> /dev/null && echo "$N killed after 60 s" >> $O/events.txt
	wait $P
	RC=$?
	echo "rc=$RC" >> $O/$N.txt
	echo "   $N rc=$RC after ${I}s" >> $O/events.txt
	grep -e '^FORMAT' -e 'realtime' -e 'ARM cpu per' -e '^eos' $O/$N.txt >> $O/events.txt
	sync
}

# 1. stereo 44.1k podcast, component default parameters
run st_default -H $O/st_default_head.wav $B/test_st.mp3
ST_RC=$RC
# 2. audiobook, default parameters (+4=1), full WAV to /tmp (capped at 6 MB)
run ab_default -w /tmp/dsp_ab.wav -H $O/ab_default_head.wav $B/test.mp3
AB_RC=$RC
ls -l /tmp/dsp_ab.wav > $O/ab_default_wav.txt 2>&1
md5sum /tmp/dsp_ab.wav >> $O/ab_default_wav.txt 2>&1
rm -f /tmp/dsp_ab.wav
# 3. audiobook, gemini-like parameters (+4 = MPEG version code 2 for MPEG2, +6=4, +0x12=39)
run ab_app -P 4=2 -P 6=4 -P 0x12=39 -H $O/ab_app_head.wav $B/test.mp3
APP_RC=$RC
# 4. only if both audiobook runs failed: other input packings
if [ $AB_RC -ne 0 ] && [ $APP_RC -ne 0 ]; then
	run ab_1frame -m f -H $O/ab_1frame_head.wav $B/test.mp3
	run ab_raw -m r -H $O/ab_raw_head.wav $B/test.mp3
	run ab_app_1frame -m f -P 4=2 -P 6=4 -P 0x12=39 $B/test.mp3
fi
# 5. listen: 8 s through /dev/sound/dsp using the first run that reached EOS (format from its FORMAT line)
PLAY=""
if [ $AB_RC -eq 0 ]; then PLAY="ab_default"; PARGS="$B/test.mp3"
elif [ $APP_RC -eq 0 ]; then PLAY="ab_app"; PARGS="-P 4=2 -P 6=4 -P 0x12=39 $B/test.mp3"
elif [ $ST_RC -eq 0 ]; then PLAY="st_default"; PARGS="$B/test_st.mp3"
fi
if [ -n "$PLAY" ]; then
	set -- $(grep '^FORMAT' $O/$PLAY.txt)
	if [ -n "$4" ]; then
		run play -F $2,$3,$4 -p 8 -T 30 $PARGS
	fi
fi
# 6. optional: real audiobook file (shell glob, accented/spaced names are fine)
if [ $FULL -eq 1 ]; then
	for F in "/contents/Audiolibros/Quiroga - Cuentos de la selva/"*.mp3; do break; done
	run full -T 40 "$F"
fi
cat /proc/interrupts > $O/interrupts_after.txt
dmesg | tail -60 > $O/dmesg_tail.txt
echo "end uptime=$(cat /proc/uptime)" >> $O/events.txt
sync
