"""Stages the music speed test on the Walkman's drive (default H:). The next boot runs it once, before the normal
dual boot, on a throwaway copy of the player's state (/tmp), with the USB cable ignored (HB_NOUSB): restart the
Walkman with RESET while it is connected, and H: comes back when the test is over (~3 minutes).
Then: python check_music.py
  python stage_music.py [DRIVE]
The script: waits at the home screen for the background passes (library, covers), then Música > Artistas and
songs picked from lists in the order MP3 -> FLAC -> FLAC (and quick skips, a pause) -> MP3, the album lists, all
albums, Audiolibros and Podcasts; screenshots 300 ms after the picks show whether the screen and the cover are
there. The key script assumes a library whose Artistas list has an MP3 artist at 0 and FLAC artists at 3 and 4
(change the DOWN / UP counts in M for yours)."""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HB = os.path.dirname(os.path.dirname(HERE))
DRIVE = (sys.argv[1] if len(sys.argv) > 1 else 'H:').rstrip('\\/') + os.sep


def script(lines):
    out = []
    for l in lines:
        out += l if isinstance(l, list) else [l]
    return '\n'.join(out) + '\n'


BACK3 = ['BACK', 'wait 800', 'BACK', 'wait 800', 'BACK', 'wait 800']
M = script([
    'wait 40000', 'shot m01_inicio',
    'log test: musica', 'PLAY', 'wait 1500', 'shot m02_musica',
    'DOWN', 'DOWN', 'PLAY', 'wait 1500', 'shot m03_artistas',
    'PLAY', 'wait 1500', 'shot m04_albumes_ab', 'PLAY', 'wait 1500', 'shot m05_canciones',
    'log test: pick mp3', 'PLAY', 'wait 5000', 'shot m06_mp3',
    BACK3, ['DOWN'] * 3,
    'log test: pick flac after mp3', 'PLAY', 'wait 1500', 'PLAY', 'wait 1500', 'PLAY', 'wait 300', 'shot m07_flac_300ms', 'wait 4000', 'shot m08_flac',
    BACK3, 'DOWN',
    'log test: pick flac after flac', 'PLAY', 'wait 1500', 'PLAY', 'wait 1500', 'PLAY', 'wait 300', 'shot m09_flac2_300ms', 'wait 3000',
    'log test: skips', 'RIGHT', 'wait 400', 'shot m10_salto1', 'RIGHT', 'wait 400', 'RIGHT', 'wait 400', 'shot m11_salto3', 'wait 2000', 'shot m12_salto3_2s',
    'log test: pause', 'PLAY', 'wait 1000', 'PLAY', 'wait 1000',
    'BACK', 'wait 800', 'BACK', 'wait 800', 'shot m13_albumes_dt', 'BACK', 'wait 800',
    ['UP'] * 4,
    'log test: pick mp3 after flac', 'PLAY', 'wait 1500', 'PLAY', 'wait 1500', 'DOWN', 'PLAY', 'wait 300', 'shot m14_mp3_300ms', 'wait 3000',
    'BACK', 'wait 800', 'BACK', 'wait 300', 'shot m15_albumes_ab_300ms', 'BACK', 'wait 800', 'BACK', 'wait 800',
    'log test: all albums', 'DOWN', 'PLAY', 'wait 300', 'shot m16_todos_300ms', 'wait 1500', 'shot m17_todos',
    'BACK', 'wait 800', 'BACK', 'wait 1500', 'shot m18_inicio',
    'log test: books', 'UP', 'PLAY', 'wait 3000', 'shot m19_audiolibros', 'BACK', 'wait 1000',
    'log test: podcasts', 'RIGHT', 'PLAY', 'wait 3000', 'shot m20_podcasts', 'BACK', 'wait 1000',
    'quit',
])

ONCE = r'''#!/bin/sh
# music speed test staged by homebrew/tests/device/stage_music.py (runs once: boot.sh renames it first)
B=/contents/HB/bin; T=/contents/HB/test; O=/contents/HB/out/test
rm -rf $O; mkdir -p $O
echo "start $(date)" > $O/run.txt
VM=0
grep -q " /var " /proc/mounts || { mount -t ext2 /dev/mtdblock/3 /var && VM=1; }
# a copy of the real state, without "last" (the home screen then starts on Música) and without crash leftovers
rm -rf /tmp/hbM; cp -r /var/hb /tmp/hbM
rm -f /tmp/hbM/log.txt /tmp/hbM/log.1.txt /tmp/hbM/meter.txt /tmp/hbM/starting /tmp/hbM/resume.txt /tmp/hbM/boot_player
grep -v '^last ' /tmp/hbM/pos.txt > /tmp/hbM/pos.new; mv /tmp/hbM/pos.new /tmp/hbM/pos.txt
HB_NOUSB=1 HB_KEYS=$T/keys_m.txt HB_SHOTS=$O $B/hbtest run /tmp/hbM > $O/m_out.txt 2>&1
echo "m rc=$?" >> $O/run.txt
killall hbtest > /dev/null 2>&1	# its background cover pass, if still going: nothing may keep /contents busy for USB
cp /tmp/hbM/log.txt $O/m_log.txt
$B/hbplayer wdoff > /dev/null 2>&1
killall dspdec > /dev/null 2>&1
rm -rf /tmp/hbM
echo "end $(date)" >> $O/run.txt
sync
[ $VM = 1 ] && umount /var
exit 0
'''


def main():
    hb = os.path.join(DRIVE, 'HB')
    if not os.path.isdir(hb):
        sys.exit('no %s: is the Walkman connected?' % hb)
    exe = os.path.join(HB, 'hbplayer', 'hbtest')
    if not os.path.exists(exe):
        sys.exit('build it first: sh homebrew/hbplayer/build_test.sh')
    shutil.copyfile(exe, os.path.join(hb, 'bin', 'hbtest'))
    os.makedirs(os.path.join(hb, 'test'), exist_ok=True)
    open(os.path.join(hb, 'test', 'keys_m.txt'), 'w', newline='\n').write(M)
    os.makedirs(os.path.join(hb, 'once'), exist_ok=True)
    open(os.path.join(hb, 'once', 't_music.sh'), 'w', newline='\n').write(ONCE)
    print('staged: HB\\bin\\hbtest, HB\\test\\keys_m.txt, HB\\once\\t_music.sh')
    print('now: RESET with the cable in; ~3 minutes later H: comes back: python check_music.py')


if __name__ == '__main__':
    main()
