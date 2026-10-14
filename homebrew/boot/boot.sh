#!/bin/sh
# Entry point (installed as H:\HB\boot.sh). The hook firmware (firmware/build_hb.py) runs it before the Sony UI
# starts, with cwd /contents/HB and output to /contents/HB/boot_log.txt. Delete or rename it from the PC and the
# Walkman boots exactly like stock.
# Optional: each /contents/HB/once/*.sh runs once (renamed to .done, output in .log) - for tests and diagnostics.
for f in /contents/HB/once/*.sh; do
	[ -f "$f" ] || continue
	mv "$f" "$f.done"
	sh "$f.done" > "${f%.sh}.log" 2>&1
done
# the dual boot runs from RAM with cwd / and its output in /tmp: USB mass storage in the player (hbusb) needs
# /contents free of our open files. The plain way below only if the copy fails.
if cp /contents/HB/boot_dual.sh /tmp/boot_dual.sh; then cd / && exec sh /tmp/boot_dual.sh < /dev/null > /tmp/boot_log.txt 2>&1; fi
sh ./boot_dual.sh
