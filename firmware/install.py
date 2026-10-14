"""One-step installer for the NW-A82x (Japanese 1.02 firmware) homebrew. Windows, Python 3 + cryptography.

  python install.py --check       finds the Walkman, downloads and checks Sony's updater, builds the hook firmware;
                                  writes nothing to the Walkman
  python install.py               the above, then flashes the hook firmware if the Walkman does not have it yet
                                  (1.02-41), and copies the homebrew (build/HB from build_all.sh) to it
  python install.py --reflash     the same, flashing 1.02-41 again even if it is there (to test the flashing)
  python install.py --uninstall   flashes Sony's official firmware back and removes H:\\HB\\boot.sh (the homebrew
                                  files stay until you delete H:\\HB)
Options: --drive H:   --hb path\\to\\build\\HB   --cache folder (default: %USERPROFILE%\\.walkman-hb)

Every file is checked against a known MD5: Sony's updater, the official firmware inside it, and the hook firmware
built from it (the same bytes as the developer's tested unit). Nothing from Sony is shipped with this project."""
import argparse, glob, gzip, hashlib, io, os, shutil, struct, subprocess, sys, tarfile, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_hb, fwtrig
from upgpack import unpack

UPDATER_URL = 'http://walkman.update.sony.net/fw/pc/A820/NW-A820_V1_02.exe'
UPDATER_MD5 = '7ed36a5255b90f8ced476713a5cde81b'
OFFICIAL_MD5 = 'fa3c6ae0152eb26c9ce94570735f0318'	# NW_WM_FW.UPG inside it: JP 1.02
HOOK_MD5 = 'ee63d042897953614389f6224051275a'	# build_hb.build_hook(official): 1.02-41
A2D_MD5 = '106ac24f1f4295a152788165fcb30fc4'	# patch_a2d.py on the official a2d_demo_app (the tested unit's)
CACERT_URL = 'https://curl.se/ca/cacert.pem'
MODELS = ('NW-A826', 'NW-A828', 'NW-A829')


def md5(b): return hashlib.md5(b).hexdigest()


def say(*a): print(*a, flush=True)


def fail(msg): sys.exit('\nERROR: ' + msg)


def find_walkman(drive):
    letters = [drive] if drive else ['%s:' % c for c in 'DEFGHIJKLMNOPQRSTUVWXYZ']
    for d in letters:
        if not os.path.exists(d + '\\'):
            continue
        info = fwtrig.devinfo(d)
        if info:
            return d, info
    fail('no Walkman found%s. Connect it with USB (Sony UI, not the homebrew player) and try again.' % (' on ' + drive if drive else ''))


def official_firmware(cache):
    """Sony's JP 1.02 NW_WM_FW.UPG (bytes), from the updater downloaded once into cache"""
    os.makedirs(cache, exist_ok=True)
    exe = os.path.join(cache, 'NW-A820_V1_02.exe')
    if not os.path.exists(exe) or md5(open(exe, 'rb').read()) != UPDATER_MD5:
        say('downloading Sony\'s updater (12 MB) ...')
        urllib.request.urlretrieve(UPDATER_URL, exe + '.part')
        os.replace(exe + '.part', exe)
    d = open(exe, 'rb').read()
    if md5(d) != UPDATER_MD5:
        fail('the updater does not have the expected MD5 (%s). Delete %s and try again.' % (md5(d), exe))
    i = d.find(b'MSCF\0\0\0\0')
    if i < 0:
        fail('no cabinet inside the updater')
    n = struct.unpack_from('<I', d, i + 8)[0]
    cab = os.path.join(cache, 'fw.cab')
    open(cab, 'wb').write(d[i:i + n])
    out = os.path.join(cache, 'cab')
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'expand.exe'), cab, '-F:NW_WM_FW.UPG', out],
                       capture_output=True)
    upg = os.path.join(out, 'NW_WM_FW.UPG')
    if r.returncode or not os.path.exists(upg):
        fail('could not extract the firmware from the updater (expand.exe)')
    b = open(upg, 'rb').read()
    if md5(b) != OFFICIAL_MD5:
        fail('the official firmware does not have the expected MD5 (%s)' % md5(b))
    return b


def rootfs_file(official, name):
    """one file of the official firmware's root filesystem"""
    sig, files = unpack(official, build_hb.JPKEY)
    t = tarfile.open(fileobj=io.BytesIO(gzip.decompress(files[3])))
    return t.extractfile(name).read()


def a2d_hb(official):
    """Sony/CSR's Bluetooth tool, patched to start with the remembered device (homebrew/bt/patch_a2d.py)"""
    src = rootfs_file(official, './usr/local/test/bt/a2d_demo_app')
    tmp_in, tmp_out = os.path.join(CACHE, 'a2d_demo_app'), os.path.join(CACHE, 'a2d_hb')
    open(tmp_in, 'wb').write(src)
    r = subprocess.run([sys.executable, os.path.join(HERE, '..', 'homebrew', 'bt', 'patch_a2d.py'), tmp_in, tmp_out], capture_output=True, text=True)
    if r.returncode:
        fail('patching a2d_demo_app: ' + r.stdout + r.stderr)
    b = open(tmp_out, 'rb').read()
    if md5(b) != A2D_MD5:
        fail('the patched Bluetooth helper came out different (%s)' % md5(b))
    return b


def wait_for(drive, subrev, minutes=8):
    """after a flash: the Walkman disconnects, updates and comes back; True once it reports subrev"""
    say('waiting for the Walkman to update and come back (about a minute; do not unplug it) ...')
    gone, t0 = False, time.time()
    while time.time() - t0 < minutes * 60:
        time.sleep(5)
        info = fwtrig.devinfo(drive) if os.path.exists(drive + '\\') else None
        if not info:
            gone = True
            continue
        if gone and info['subrev'] == subrev:
            return True
    return False


def flash(drive, upg, want_md5, subrev):
    p = os.path.join(drive + '\\', 'NW_WM_FW.UPG')
    open(p, 'wb').write(upg)
    if md5(open(p, 'rb').read()) != want_md5:
        fail('the copy on the Walkman is not intact (full drive?)')
    if not fwtrig.trigger(drive, want_md5):
        fail('the Walkman did not accept the update command (run this as administrator?)')
    if not wait_for(drive, subrev):
        fail('the Walkman did not come back with firmware 1.02-%s in time. If its screen is still updating, wait;'
             ' otherwise unplug and plug it again and run this once more.' % subrev)
    try:
        os.remove(p)
    except OSError:
        pass


def copy_tree(src, dst):
    n = 0
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            shutil.copyfile(os.path.join(root, f), os.path.join(dst, rel, f)); n += 1
    return n


def main():
    global CACHE
    ap = argparse.ArgumentParser(description='Walkman NW-A82x homebrew installer')
    ap.add_argument('--check', action='store_true', help='everything but writing to the Walkman')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--drive')
    ap.add_argument('--hb', default=os.path.join(HERE, '..', 'build', 'HB'))
    ap.add_argument('--cache', default=os.path.join(os.path.expanduser('~'), '.walkman-hb'))
    ap.add_argument('--yes', action='store_true', help='do not ask before flashing')
    ap.add_argument('--reflash', action='store_true', help='flash 1.02-41 even if the Walkman has it (to test the flashing)')
    a = ap.parse_args()
    CACHE = a.cache
    if sys.platform != 'win32':
        fail('this installer runs on Windows (it talks to the Walkman with SCSI pass-through)')

    drive, info = find_walkman(a.drive)
    say('Walkman: %s firmware %s-%s on %s' % (info['product'], info['rev'], info['subrev'], drive))
    if info['product'] not in MODELS:
        fail('%s is not a Japanese NW-A82x. International NWZ-A82x units are not supported by this installer.' % info['product'])
    if info['rev'] != '1.02':
        fail('firmware %s: update it to 1.02 with Sony\'s own updater first (%s)' % (info['rev'], UPDATER_URL))

    official = official_firmware(CACHE)
    say('official firmware 1.02: OK (%s)' % OFFICIAL_MD5)
    hook = build_hb.build_hook(official)
    if md5(hook) != HOOK_MD5:
        fail('the hook firmware came out different (%s): not flashing it' % md5(hook))
    say('hook firmware 1.02-41: OK (%s)' % HOOK_MD5)
    a2d = a2d_hb(official)
    say('Bluetooth helper a2d_hb: %s' % md5(a2d))

    if a.uninstall:
        if a.check:
            say('(check only: would flash the official firmware and remove HB\\boot.sh)'); return
        if not a.yes and input('Flash Sony\'s official firmware back? Type "yes": ').strip().lower() != 'yes':
            return
        boot = os.path.join(drive + '\\', 'HB', 'boot.sh')
        if os.path.exists(boot):
            os.replace(boot, boot + '.off')
        if info['subrev'] != '00':
            flash(drive, official, OFFICIAL_MD5, '00')
        say('\nDone: stock firmware. Delete %s\\HB whenever you like.' % drive)
        return

    if not os.path.isdir(a.hb) or not os.path.exists(os.path.join(a.hb, 'bin', 'hbplayer')):
        fail('no homebrew build at %s: run "sh build_all.sh" first (or use the release zip\'s HB folder with --hb)' % a.hb)
    if a.check:
        say('(check only: nothing written to the Walkman)')
        say('would %s, then copy %s to %s\\HB' % ('flash 1.02-41' if info['subrev'] != '41' or a.reflash else 'skip the flash (already 1.02-41)', a.hb, drive))
        return

    if info['subrev'] != '41' or a.reflash:
        say('\nThe Walkman needs the hook firmware (1.02-41). It flashes in about a minute; the battery must be charged'
            ' and the cable must stay connected. Sony\'s updater restores the stock firmware at any time.')
        if not a.yes and input('Flash it now? Type "yes": ').strip().lower() != 'yes':
            return
        flash(drive, hook, HOOK_MD5, '41')
        say('firmware 1.02-41 installed')

    dst = os.path.join(drive + '\\', 'HB')
    n = copy_tree(a.hb, dst)
    open(os.path.join(dst, 'bin', 'a2d_hb'), 'wb').write(a2d)
    if not os.path.exists(os.path.join(dst, 'cacert.pem')):
        try:
            urllib.request.urlretrieve(CACERT_URL, os.path.join(dst, 'cacert.pem'))
            say('certificates for HTTPS: cacert.pem')
        except OSError as e:
            say('(no cacert.pem: %s; podcasts will use HTTPS without checking certificates)' % e)
    for f in ('Music', 'Audiolibros', 'Podcasts', 'Juegos'):
        os.makedirs(os.path.join(drive + '\\', f), exist_ok=True)
    say('\nDone: %d files in %s. Eject the Walkman and restart it (or wait for it to sleep and wake it):'
        ' the first start asks for the language. HOME for 3 s switches between Sony\'s UI and the player.' % (n + 1, dst))


if __name__ == '__main__':
    main()
