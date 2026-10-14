"""Stages the on-device tests on the Walkman's drive (default H:). The next boot runs them once, before the normal
dual boot, on throwaway copies of the player's state (/tmp), and writes logs + screenshots to H:\\HB\\out\\test.
Then: python check.py (reads the results, checks the logs, makes PNG contact sheets).
  python stage.py [DRIVE]        DRIVE defaults to H:
Scenarios:
  selftest  an AAC (normal path) then MP3s: the track change must stay on the DSP's direct path
  a         first run with an empty state: language (English), welcome, home, settings, about, language back and
            forth, podcasts, the keyboard, the podcast options, games, focus mode (no network)
  b         a copy of your state, in Spanish: "Agregar podcast" -> "corridor" -> results -> card -> episodes, over
            the phone's Bluetooth tethering (turn it on before booting; without it the screens show the error)"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HB = os.path.dirname(os.path.dirname(HERE))
DRIVE = (sys.argv[1] if len(sys.argv) > 1 else 'H:').rstrip('\\/') + os.sep

# the keyboard (screens.c kb_chars, letters, lower case): row, column of each character; space = row 4, column 4
ROWS = ['abcdefg', 'hijklmn', '\u00f1opqrst', "uvwxyz'"]


# the special rows: (kind, first column, span), as kbd_row in screens.c
SPECIAL = {4: [('shift', 0, 2), ('mode', 2, 2), ('space', 4, 3)], 5: [('del', 0, 3), ('ok', 3, 4)]}


def row_keys(r):
    return [('ch', c, 1) for c in range(7)] if r < 4 else SPECIAL[r]


def move(state, k):
    """kbd.c: left/right go key by key (wrapping) and set the column; up/down keep the column"""
    r, key, col = state
    keys = row_keys(r)
    if k in ('LEFT', 'RIGHT'):
        key = (key + (1 if k == 'RIGHT' else -1)) % len(keys)
        return (r, key, keys[key][1])
    r = (r + (1 if k == 'DOWN' else -1)) % 6
    keys = row_keys(r)
    key = next((i for i, (_, c0, sp) in enumerate(keys) if c0 <= col < c0 + sp), len(keys) - 1)
    return (r, key, col)


def type_keys(text):
    """keys that type text on the keyboard (letters, lower case, and spaces) from its start (row 0, key 0)"""
    from collections import deque
    keys, state = [], (0, 0, 0)
    for ch in text:
        if ch == ' ':
            goal = (4, 2)
        else:
            tr = next(i for i, row in enumerate(ROWS) if ch in row)
            goal = (tr, ROWS[tr].index(ch))
        prev = {state: None}
        q = deque([state])
        while q:	# shortest way there
            st = q.popleft()
            if st[:2] == goal:
                break
            for k in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
                n = move(st, k)
                if n not in prev:
                    prev[n] = (st, k); q.append(n)
        found, path = st, []
        while prev[st]:
            st0, k = prev[st]; path.append(k); st = st0
        keys += path[::-1] + ['PLAY']
        state = found
    return keys


def script(lines):
    out = []
    for l in lines:
        out += l if isinstance(l, list) else [l]
    return '\n'.join(out) + '\n'


A = script([
    'wait 5000', 'shot a01_idioma', 'DOWN', 'PLAY', 'wait 1500', 'shot a02_bienvenida', 'PLAY', 'wait 3000', 'shot a03_inicio',
    'log settings', 'DOWN', 'RIGHT', 'PLAY', 'wait 1000', 'shot a04_ajustes',
    ['DOWN'] * 7, 'PLAY', 'wait 1500', 'shot a05_acerca', 'BACK', 'wait 500',	# About is the 8th row
    'UP', 'UP', 'UP', 'PLAY',	# Language, the 5th 'wait 800', 'shot a06_ajustes_espanol', 'PLAY', 'wait 800', 'shot a07_ajustes_ingles', 'BACK', 'wait 800',
    'log podcasts', 'UP', 'UP', 'PLAY', 'wait 2000', 'shot a08_podcasts',
    'LEFT', 'LEFT', 'DOWN', 'PLAY', 'wait 1000', 'shot a09_teclado', type_keys('radio nacional'), 'wait 500', 'shot a10_escrito',
    ['BACK'] * 15, 'wait 1500', 'shot a11_podcasts_otra_vez',
    'OPTION', 'wait 1000', 'shot a12_opciones_podcasts', 'BACK', 'wait 800', 'BACK', 'wait 1000',
    'log games', 'DOWN', 'PLAY', 'wait 1500', 'shot a13_juegos', 'BACK', 'wait 800',
    'log focus', 'LEFT', 'LEFT', 'LEFT', 'LEFT', 'LEFT', 'PLAY', 'wait 1000', 'shot a14_foco', 'BACK', 'wait 800', 'shot a15_inicio_final',
    'quit',
])

B = script([
    'wait 6000', 'shot b01_inicio', 'UP', 'RIGHT', 'RIGHT', 'PLAY', 'wait 3000', 'shot b02_podcasts',
    'LEFT', 'LEFT', 'DOWN', 'PLAY', 'wait 1000', type_keys('corridor'), 'wait 300', 'shot b03_escrito', 'OPTION',
    'wait 70000', 'shot b04_resultados', 'PLAY', 'wait 10000', 'shot b05_ficha', 'DOWN', 'PLAY', 'wait 40000', 'shot b06_episodios',
    'DOWN', 'wait 500', 'shot b07_episodios_2', 'BACK', 'wait 1000', 'shot b08_ficha_otra_vez', 'BACK', 'wait 1000', 'BACK', 'wait 1000',
    ['BACK'] * 9, 'wait 5000', 'shot b09_podcasts', 'DOWN', 'OPTION', 'wait 1000', 'shot b10_opciones_programa', 'BACK', 'wait 500',
    'quit',
])


def test_files():
    """an AAC (.m4a) and two MP3s from the drive's Music folder, as the Walkman sees them (/contents/...)"""
    m4a, mp3 = [], []
    for root, _, files in os.walk(os.path.join(DRIVE, 'Music')):
        for f in sorted(files):
            p = '/contents/' + os.path.relpath(os.path.join(root, f), DRIVE).replace(os.sep, '/')
            if f.lower().endswith('.m4a') and not m4a: m4a.append(p)
            if f.lower().endswith('.mp3') and len(mp3) < 2: mp3.append(p)
        if m4a and len(mp3) == 2: break
    if not m4a or len(mp3) < 2:
        sys.exit('the selftest needs one .m4a and two .mp3 under %sMusic' % DRIVE)
    return m4a + mp3

ONCE = r'''#!/bin/sh
# on-device tests staged by homebrew/tests/device/stage.py (runs once: boot.sh renames it first)
B=/contents/HB/bin; T=/contents/HB/test; O=/contents/HB/out/test
rm -rf $O; mkdir -p $O
echo "start $(date)" > $O/run.txt
VM=0
grep -q " /var " /proc/mounts || { mount -t ext2 /dev/mtdblock/3 /var && VM=1; }
# selftest: AAC (normal path) -> MP3 -> MP3, the DSP's direct path must survive the change
rm -rf /tmp/st; mkdir -p /tmp/st; cp /var/hb/eq.txt /tmp/st/ 2> /dev/null
HB_DIRECT=1 $B/hbtest selftest /tmp/st "%(files)s" 75 > $O/selftest_out.txt 2>&1
echo "selftest rc=$?" >> $O/run.txt
cp /tmp/st/log.txt $O/selftest_log.txt; cp /tmp/st/dspdec.log $O/selftest_dspdec.txt 2> /dev/null
$B/hbplayer wdoff > /dev/null 2>&1
# a: first run, empty state
rm -rf /tmp/hbA; mkdir -p /tmp/hbA
HB_KEYS=$T/keys_a.txt HB_SHOTS=$O $B/hbtest run /tmp/hbA > $O/a_out.txt 2>&1
echo "a rc=$?" >> $O/run.txt
cp /tmp/hbA/log.txt $O/a_log.txt; cp /tmp/hbA/pos.txt $O/a_pos.txt 2> /dev/null
$B/hbplayer wdoff > /dev/null 2>&1
killall dspdec a2d_demo_app a2d_hb bc_attach > /dev/null 2>&1
# b: a copy of the real state (positions, Bluetooth pairing), podcasts over the phone
rm -rf /tmp/hbB; cp -r /var/hb /tmp/hbB; rm -f /tmp/hbB/log.txt /tmp/hbB/log.1.txt /tmp/hbB/meter.txt /tmp/hbB/starting
HB_KEYS=$T/keys_b.txt HB_SHOTS=$O $B/hbtest run /tmp/hbB > $O/b_out.txt 2>&1
echo "b rc=$?" >> $O/run.txt
cp /tmp/hbB/log.txt $O/b_log.txt
$B/hbplayer wdoff > /dev/null 2>&1
killall dspdec a2d_demo_app a2d_hb bc_attach > /dev/null 2>&1
/usr/local/test/bt/bt_test 4 > /dev/null 2>&1
rm -rf /tmp/st /tmp/hbA /tmp/hbB
echo "end $(date)" >> $O/run.txt
sync
[ $VM = 1 ] && umount /var
exit 0
'''


def main():
    hb = os.path.join(DRIVE, 'HB')
    if not os.path.isdir(hb):
        sys.exit('no %s: is the Walkman connected (Sony UI, USB)?' % hb)
    exe = os.path.join(HB, 'hbplayer', 'hbtest')
    if not os.path.exists(exe):
        sys.exit('build it first: sh homebrew/hbplayer/build_test.sh')
    shutil.copyfile(exe, os.path.join(hb, 'bin', 'hbtest'))
    os.makedirs(os.path.join(hb, 'test'), exist_ok=True)
    for name, text in (('keys_a.txt', A), ('keys_b.txt', B)):
        open(os.path.join(hb, 'test', name), 'w', newline='\n').write(text)
    os.makedirs(os.path.join(hb, 'once'), exist_ok=True)
    files = test_files()
    print('selftest files:', ', '.join(f.rsplit('/', 1)[-1] for f in files))
    open(os.path.join(hb, 'once', 't_hbtest.sh'), 'w', newline='\n').write(ONCE % {'files': '|'.join(files)})
    print('staged: HB\\bin\\hbtest, HB\\test\\keys_a.txt, keys_b.txt, HB\\once\\t_hbtest.sh')
    print('now: (phone: Bluetooth tethering on), eject, restart the Walkman; ~6 minutes later connect it and run check.py')


if __name__ == '__main__':
    main()
