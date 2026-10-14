#!/bin/sh
# boot_hidtest.sh: one-shot Bluetooth HID test at boot, before the Sony UI (gemini) starts.
# Paste into H:\HB\boot.sh (or call it from there) like the pantest block. Copy hidtest to H:\HB\hidtest.
# Guard: runs once; delete /contents/HB/out/hid_done to run it again. Output in /contents/HB/out/hid/.
HB=/contents/HB
OUT=$HB/out/hid
if [ -x $HB/hidtest ] && [ ! -f $HB/out/hid_done ]; then
	mkdir -p $OUT
	touch $HB/out/hid_done
	sync
	date > $OUT/start.txt
	/usr/local/test/bt/bt_test 3 > $OUT/bt_test3.txt 2>&1
	/usr/local/test/bt/bc_attach -b 2592000 -d /dev/tts/0 > $OUT/bc_attach.txt 2>&1 &
	BCA=$!
	sleep 3
	# hidtest itself puts /dev/tts/0 in low-latency mode, then runs up to 290 s (BT key held 2 s = quit)
	$HB/hidtest -t 290 > $OUT/hidtest.log 2>&1 &
	HT=$!
	( sleep 300; kill $HT 2>/dev/null ) > /dev/null 2>&1 &
	KILLER=$!
	wait $HT
	echo "hidtest exit $?" >> $OUT/hidtest.log
	kill $KILLER 2>/dev/null
	kill $BCA 2>/dev/null
	sleep 1
	/usr/local/test/bt/bt_test 4 > $OUT/bt_test4.txt 2>&1
	cp /var/hb/bt/hid_host $OUT/hid_host.txt 2>/dev/null
	ls -la /var/hb/bt > $OUT/var_hb_bt.txt 2>&1
	date >> $OUT/start.txt
	sync
fi
