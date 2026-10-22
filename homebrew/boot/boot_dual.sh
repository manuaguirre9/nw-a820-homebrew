#!/bin/sh
# Dual boot (installed as H:\HB\boot_dual.sh; boot.sh runs it after the one-shot tests).
#  - Boots into the UI used last: /var/hb/mode = player | sony (written by the player), no prompt. The old 3 s prompt
#    ("HOME: Audiolibros") only appears while there is no mode file yet.
#  - Sony UI -> player: HOME held 3 s (hbwatch writes /var/hb/boot_player and reboots).
#  - Player -> Sony UI: "Volver al Walkman" exits the player with 0 and this script goes on into the Sony UI (no reboot).
#  - A player crash after more than a minute of use: started once more (it resumes where it was); a crash right away
#    goes to the Sony UI and makes it the next boot's choice too.
#  - Our programs run straight from /contents/HB/bin (FAT, fmask 0022 = executable): copies in /tmp (ramfs) could never
#    be evicted. Only the 16 KB watcher is copied, because USB mass storage can take /contents away from the Sony UI.
#  - USB in the player: it exits with 3 when a computer connects; hbusb (copied to /tmp) hands /contents to the
#    computer and takes it back when the cable comes out, then the player starts again. This script runs from /tmp
#    with cwd / (boot.sh copies it there) so that nothing of ours keeps /contents busy.
# Player state (positions, log, flags) lives on /var (internal ext2), never on the FAT partition.
B=/contents/HB/bin
O=/contents/HB/out
mkdir -p $O
VM=0
if ! grep -q " /var " /proc/mounts; then mount -t ext2 /dev/mtdblock/3 /var && VM=1; fi
mkdir -p /var/hb
[ -f /var/hb/log.txt ] && cp /var/hb/log.txt $O/player_log.txt
[ -f /var/hb/log.1.txt ] && cp /var/hb/log.1.txt $O/player_log.1.txt
[ -f /var/hb/dspdec.log ] && cp /var/hb/dspdec.log $O/dspdec_player_log.txt
[ -f /var/hb/gnuboy.log ] && cp /var/hb/gnuboy.log $O/gnuboy_log.txt
[ -f /var/hb/hbwatch.log ] && cp /var/hb/hbwatch.log $O/hbwatch_log.txt
[ -f /var/hb/usb.log ] && cp /var/hb/usb.log $O/usb_log.txt
ls -la /var/hb /var/hb/bt > $O/var_hb.txt 2>&1
# debug only (create an empty H:\HB\debug to enable): CPU / clock / interrupts / processes every 10 s to
# /var/hb/meter.txt in both UIs; the log is moved to H:\HB\out at every boot so it can't grow without bound
[ -f /var/hb/meter.txt ] && mv /var/hb/meter.txt $O/meter_$(date +%H%M%S).txt
[ -f /contents/HB/debug ] && [ -f $B/meter ] && cp $B/meter /tmp/meter && chmod 755 /tmp/meter && ( cd / && exec /tmp/meter 10 < /dev/null > /dev/null 2>&1 ) &
PLAYER=0
MODE=$(cat /var/hb/mode 2> /dev/null)
# the last player start never got healthy (it removes this flag after a minute): a hang the watchdog reset. Boot
# the Sony UI this time (and next), instead of looping into the same hang
if [ -f /var/hb/starting ]; then
	rm -f /var/hb/starting /var/hb/boot_player; echo sony > /var/hb/mode; sync; MODE=sony
	echo "last player start never got healthy: Sony UI" >> $O/boot_events.txt
fi
if [ -f /var/hb/boot_player ]; then
	rm -f /var/hb/boot_player; sync; PLAYER=1
elif [ "$MODE" = sony ]; then
	PLAYER=0
elif [ "$MODE" = player ]; then
	PLAYER=1
elif $B/hbplayer ask 3; then
	PLAYER=1
fi
[ -f $B/hbusb ] && cp $B/hbusb /tmp/hbusb && chmod 755 /tmp/hbusb
TRIES=0
while [ $PLAYER = 1 ]; do
	PLAYER=0
	T0=$(cut -d. -f1 /proc/uptime)
	echo "player start uptime=$(cat /proc/uptime)" >> $O/boot_events.txt
	touch /var/hb/starting
	sync
	$B/hbplayer run /var/hb > /tmp/hbplayer.out 2>&1
	RC=$?
	rm -f /var/hb/starting	# it ended by itself (crash or exit): the checks below decide
	# clean up its helpers, watchdog and clock (the player exits cleanly itself; this covers a crash)
	killall a2d_demo_app a2d_hb bc_attach gnuboy nes doom gba dspdec hidtest > /dev/null 2>&1
	/usr/local/test/bt/bt_test 4 > /dev/null 2>&1
	$B/hbplayer wdoff > /dev/null 2>&1
	echo -n M248 > /proc/pm
	T1=$(cut -d. -f1 /proc/uptime)
	RAN=$(expr $T1 - $T0)
	echo "player returned rc=$RC after $RAN s" >> $O/boot_events.txt
	# this session's logs now, not only at the next boot
	[ -f /var/hb/log.txt ] && cp /var/hb/log.txt $O/player_log.txt
	[ -f /var/hb/dspdec.log ] && cp /var/hb/dspdec.log $O/dspdec_player_log.txt
	if [ $RC = 3 ] && [ -x /tmp/hbusb ]; then
		echo "USB: hand-over" >> $O/boot_events.txt
		sync
		/tmp/hbusb /var/hb
		echo "USB: hbusb rc=$? at uptime $(cat /proc/uptime)" >> $O/boot_events.txt
		cp /var/hb/usb.log $O/usb_log.txt > /dev/null 2>&1
		PLAYER=1
	elif [ $RC != 0 ]; then
		cp /tmp/hbplayer.out $O/player_crash.txt > /dev/null 2>&1
		if [ $TRIES = 0 ] && [ $RAN -gt 60 ]; then
			TRIES=1; PLAYER=1
			echo "restarting the player after a crash" >> $O/boot_events.txt
		else
			echo sony > /var/hb/mode
		fi
	fi
done
sync
[ $VM = 1 ] && umount /var
# the Sony UI runs at whatever clock it finds (it never changes it): 248 MHz, unless the player's setting "Walkman de
# Sony: ahorro" (experimental) left /contents/HB/sony124
[ -f /contents/HB/sony124 ] && echo -n M124 > /proc/pm
# Sony UI: the HOME-held watcher (16 KB, copied to RAM: USB mass storage can take /contents away from the Sony UI).
# It never ends by itself (HOME held reboots the Walkman), so an exit is logged and it is started again: on 2026-10-20
# a Sony session left no hbwatch log at all and HOME did nothing
echo "sony UI uptime=$(cat /proc/uptime)" >> $O/boot_events.txt
if [ -f $B/hbwatch ] && cp $B/hbwatch /tmp/hbwatch && chmod 755 /tmp/hbwatch; then
	( cd / && N=0; while [ $N -lt 20 ]; do
		/tmp/hbwatch /var/hb < /dev/null > /dev/null 2>&1
		RC=$?
		[ -w /var/hb ] && echo "[$(date +%s)] hbwatch exited rc=$RC, starting it again" >> /var/hb/hbwatch.log
		N=$(expr $N + 1); sleep 2
	done ) &
else
	echo "hbwatch could not be started" >> $O/boot_events.txt
fi
exit 0
