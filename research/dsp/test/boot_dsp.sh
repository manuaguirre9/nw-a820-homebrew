#!/bin/sh
# DSP experiment 1 (run by the 1.02-4x hook BEFORE the Sony UI starts, so the DSP is idle/ours).
# Copy dspprobe + omxprobe to H:\HB\bin and this file to H:\HB\boot.sh (LF line endings!).
# Results in H:\HB\out\dsp\. The Sony UI starts normally afterwards (no hb_skip_gemini).
O=/contents/HB/out/dsp
mkdir -p $O
echo "start uptime=$(cat /proc/uptime)" > $O/events.txt
cp /contents/HB/bin/dspprobe /contents/HB/bin/omxprobe /tmp/ && chmod 755 /tmp/dspprobe /tmp/omxprobe
ls -l /dev/InterDSP > $O/devnodes.txt 2>&1
cat /proc/devices > $O/proc_devices.txt
cat /proc/interrupts > $O/interrupts_before.txt
# A. read-only state before anyone touched the DSP
/tmp/dspprobe -s > $O/probe_before.txt 2>&1
# B. boot the DSP with Sony's firmware through the NEC OpenMAX core, open the MP3 FTB decoder, close
( /tmp/omxprobe /usr/lib/omf/omx_nw_walkman_audio_ftb.cfg OMX.NEC.EL.MP3D.FTB > $O/omxprobe.txt 2>&1; echo "rc=$?" >> $O/omxprobe.txt ) &
P=$!
N=0; while [ $N -lt 30 ] && kill -0 $P 2> /dev/null; do sleep 1; N=$(expr $N + 1); done
kill -9 $P 2> /dev/null && echo "omxprobe killed after 30 s" >> $O/events.txt
# C. state after (firmware should now be resident at 0x33B00200 and the shmem magic set)
/tmp/dspprobe -s > $O/probe_after.txt 2>&1
cat /proc/interrupts > $O/interrupts_after.txt
dmesg | tail -40 > $O/dmesg_tail.txt
echo "end uptime=$(cat /proc/uptime)" >> $O/events.txt
sync
