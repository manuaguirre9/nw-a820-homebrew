"""macOS installer for the NW-A91x (NW-A916/A918/A919, Japanese 2008 models with 1seg). EXPERIMENTAL: the homebrew
was written for the NW-A82x; the A91x is the same MP201 platform and the same Linux userland (identical libc, same
/etc/init.d/gemini), but it has no Bluetooth and its DSP/kernel builds differ.

The hook is the same as build_hb.py's: Sony's official A91x 1.03 rootfs with the /etc/init.d/gemini hook and
sub-revision 41 (the Walkman reports 1.03-41). Kernel, bootloader and Sony's UI are untouched; without H:/HB/boot.sh
it boots exactly like stock, and Sony's official 1.03 can be flashed back at any time (--uninstall).

Differences from the A82x: the update package is NW_A91X.UPG, encrypted with key b8442b44 / signature 5dfeff3c
(found with a known-plaintext search over the 9^8 DES-equivalent hex keys), from Sony's NW-A910_V1_03.exe.

  sudo VENV/bin/python firmware/install_mac_a910.py --check      read-only: device info, download, build, MD5 checks
  sudo VENV/bin/python firmware/install_mac_a910.py --firmware   older firmware: Sony's official 1.03 first
  sudo VENV/bin/python firmware/install_mac_a910.py              stage 1: hook firmware + diagnostic boot.sh only
                                                                 (logs hardware info to HB/out/diag.txt, then Sony UI)
  sudo VENV/bin/python firmware/install_mac_a910.py --full       stage 2: the whole homebrew (player, emulators)
  sudo VENV/bin/python firmware/install_mac_a910.py --uninstall  Sony's official 1.03 back, HB/boot.sh disabled"""
import argparse, gzip, io, os, shutil, struct, subprocess, sys, tarfile, tempfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import install_mac as M
import build_hb
from install import InstallError, say, md5
from upgpack import unpack

M.UPG_NAME = 'NW_A91X.UPG'
UPDATER_URL = 'http://walkman.update.sony.net/fw/pc/A910/NW-A910_V1_03.exe'
UPDATER_MD5 = '49d83ca262b6f4136455e5371be7c385'
OFFICIAL_MD5 = '69074bfe5397c70d8d9a10da43e02fa9'	# NW_A91X.UPG inside it: 1.03
KEY, SIG = b'b8442b44', b'5dfeff3c'
REV, SUBREV = '1.03', '41'
MODELS = ('NW-A916', 'NW-A918', 'NW-A919')
CACHE = os.path.join(M.DEFAULT_CACHE, 'a910')

DIAG_BOOT = '''#!/bin/sh
# NW-A91x stage 1 (install_mac_a910.py): read-only hardware info for porting, then the Sony UI starts as usual.
O=/contents/HB/out; mkdir -p $O; F=$O/diag.txt
for c in "cat /proc/version" "cat /etc/firmversion" "cat /proc/cpuinfo" "cat /proc/meminfo" "cat /proc/mounts" \\
	"cat /proc/partitions" "cat /proc/mtd" "cat /proc/fb" "cat /proc/bus/input/devices" "cat /proc/devices" \\
	"cat /proc/modules" "cat /proc/interrupts" "ls -l /dev /dev/fb /dev/input /dev/misc" "ls -l /proc" \\
	"ls -l /usr/local/bin /usr/local/test /lib/modules /usr/lib/omf" "ps" "uptime"; do
	echo "===== $c"; $c
done > $F 2>&1
sync
exit 0
'''


def official_firmware():
    os.makedirs(CACHE, exist_ok=True)
    exe = os.path.join(CACHE, os.path.basename(UPDATER_URL))
    if not os.path.exists(exe) or md5(open(exe, 'rb').read()) != UPDATER_MD5:
        say('downloading Sony\'s updater (13 MB) from %s' % UPDATER_URL)
        urllib.request.urlretrieve(UPDATER_URL, exe + '.part')
        os.replace(exe + '.part', exe)
    d = open(exe, 'rb').read()
    if md5(d) != UPDATER_MD5:
        raise InstallError('Sony\'s updater did not download correctly (MD5 %s)' % md5(d))
    i = d.find(b'MSCF\0\0\0\0')
    n = struct.unpack_from('<I', d, i + 8)[0]
    with tempfile.TemporaryDirectory() as t:
        cab = os.path.join(t, 'fw.cab')
        open(cab, 'wb').write(d[i:i + n])
        subprocess.run(['tar', '-xf', cab, '-C', t], capture_output=True)	# entries are named "/NW_A91X.UPG"
        upg = os.path.join(t, 'NW_A91X.UPG')
        if not os.path.exists(upg):
            raise InstallError('could not extract the firmware from Sony\'s updater (tar)')
        b = open(upg, 'rb').read()
    if md5(b) != OFFICIAL_MD5:
        raise InstallError('the official firmware does not have the expected MD5 (%s)' % md5(b))
    say('official firmware %s: OK (%s)' % (REV, OFFICIAL_MD5))
    return b


def hook_firmware(official):
    """build_hb's hook on the A91x rootfs, then checked: kernel and loader files identical, and the only rootfs
    changes are /etc/init.d/gemini (the hook) and /etc/firmversion (sub-revision 41)"""
    sig, files = unpack(official, KEY)
    if sig != SIG:
        raise InstallError('unexpected firmware signature %r' % sig)
    hook = build_hb._build(gzip.decompress(files[3]), files, KEY, SIG, {})
    s2, f2 = unpack(hook, KEY)
    assert s2 == SIG and f2[:3] == files[:3]
    a = tarfile.open(fileobj=io.BytesIO(gzip.decompress(files[3])))
    b = tarfile.open(fileobj=io.BytesIO(gzip.decompress(f2[3])))
    ma, mb = a.getmembers(), b.getmembers()
    assert [m.name for m in ma] == [m.name for m in mb]
    changed = [x.name for x, y in zip(ma, mb) if x.isfile() and a.extractfile(x).read() != b.extractfile(y).read()]
    assert changed == ['./etc/firmversion', './etc/init.d/gemini'] or sorted(changed) == ['./etc/firmversion', './etc/init.d/gemini'], changed
    fv = b.extractfile('./etc/firmversion').read().decode('latin1')
    assert fv.rstrip().endswith('DOB_MINOR=03\nDOB_SUBREV="41  "\t\t# two following space (0x20)'), fv[-80:]
    say('hook firmware %s-%s: OK (%s; only /etc/init.d/gemini and /etc/firmversion differ)' % (REV, SUBREV, md5(hook)))
    return hook


def write_boot(vol, full, hb):
    dst = os.path.join(vol, 'HB')
    os.makedirs(os.path.join(dst, 'out'), exist_ok=True)
    if full:
        if not os.path.exists(os.path.join(hb, 'bin', 'hbplayer')):
            raise InstallError('no homebrew build at %s (use --hb)' % hb)
        n = 0
        for root, dirs, files in os.walk(hb):
            rel = os.path.relpath(root, hb)
            os.makedirs(os.path.join(dst, rel), exist_ok=True)
            for f in files:
                if not f.startswith('.'):
                    shutil.copyfile(os.path.join(root, f), os.path.join(dst, rel, f)); n += 1
        if not os.path.exists(os.path.join(dst, 'cacert.pem')):
            subprocess.run(['curl', '-sfL', '-o', os.path.join(dst, 'cacert.pem'), 'https://curl.se/ca/cacert.pem'])
        for f in ('Audiolibros', 'Podcasts', 'Juegos'):
            os.makedirs(os.path.join(vol, f), exist_ok=True)
        open(os.path.join(dst, 'version.txt'), 'w').write(M.I.VERSION + '\n')
        say('%d homebrew files copied to %s (no Bluetooth helper: the A91x has no Bluetooth)' % (n, dst))
    else:
        with open(os.path.join(dst, 'boot.sh'), 'w', newline='\n') as f:
            f.write(DIAG_BOOT)
        say('diagnostic boot.sh written to %s' % dst)
    subprocess.run(['sync'])


def main():
    ap = argparse.ArgumentParser(description='Walkman NW-A91x homebrew installer (macOS, experimental)')
    ap.add_argument('--check', action='store_true', help='everything but writing to the Walkman')
    ap.add_argument('--firmware', action='store_true', help='update an older firmware to Sony\'s official 1.03')
    ap.add_argument('--full', action='store_true', help='stage 2: install the whole homebrew instead of diagnostics')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--reflash', action='store_true')
    ap.add_argument('--hb', default=M.RELEASE_HB)
    ap.add_argument('--yes', action='store_true')
    a = ap.parse_args()
    if os.geteuid() != 0 and not a.check:
        sys.exit('run it with sudo: macOS lets only root talk to a USB disk directly')
    ask = lambda q: a.yes or input(q + ' Type "yes": ').strip().lower() == 'yes'
    try:
        official = official_firmware()
        hook = hook_firmware(official)
        if os.geteuid() != 0:
            say('(not root: firmware checks only, the Walkman was not queried)'); return
        vol = M.find_volume()
        if not vol:
            raise InstallError('no Walkman volume mounted. Connect it with USB.')
        info, vol = M.devinfo(vol)
        if not info:
            raise InstallError('the device did not answer Sony\'s DEVINFO')
        if not vol:
            raise InstallError('macOS did not mount the Walkman again. Unplug it, plug it in again and retry.')
        say('Walkman: %s firmware %s-%s on %s' % (info['product'], info['rev'], info['subrev'], vol))
        if info['product'] not in MODELS:
            raise InstallError('%s is not an NW-A91x' % info['product'])
        if a.check:
            say('(check only: nothing written to the Walkman)'); return
        if a.firmware:
            if info['rev'] == REV:
                say('already %s: nothing to do' % REV); return
            if ask('Flash Sony\'s official firmware %s?' % REV):
                M.flash(vol, official, OFFICIAL_MD5, REV, '00')
                say('\nDone: official firmware %s.' % REV)
            return
        if info['rev'] != REV:
            raise InstallError('firmware %s: update it to %s first (--firmware)' % (info['rev'], REV))
        if a.uninstall:
            if not ask('Flash Sony\'s official firmware %s back?' % REV):
                return
            M.disable_homebrew(vol)
            if info['subrev'] != '00':
                M.flash(vol, official, OFFICIAL_MD5, REV, '00')
            say('\nDone: stock firmware. Delete %s/HB whenever you like.' % vol)
            return
        write_boot(vol, a.full, a.hb)	# first: the boot after the flash then finds it
        if info['subrev'] != SUBREV or a.reflash:
            say('\nFlashing the hook firmware %s-%s (about 2 minutes). Battery charged, cable stays connected.' % (REV, SUBREV))
            if not ask('Flash it now?'):
                return
            M.flash(vol, hook, md5(hook), REV, SUBREV)
            say('firmware %s-%s installed' % (REV, SUBREV))
        say('\nDone. Eject and unplug the Walkman, let it boot, then connect it again: %s'
            % ('HOME held 3 s switches to the player.' if a.full else 'HB/out/diag.txt should be there.'))
    except InstallError as e:
        sys.exit('\nERROR: %s' % e)


if __name__ == '__main__':
    main()
