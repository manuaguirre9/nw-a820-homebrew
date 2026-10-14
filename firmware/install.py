"""One-step installer for the NW-A82x (Japanese 1.02 firmware) homebrew. Windows, Python 3 + cryptography.
The same steps run behind the installer window (installer_gui.py, built into Walkman-Homebrew-Installer.exe).

  python install.py --check       finds the Walkman, downloads and checks Sony's updater, builds the hook firmware;
                                  writes nothing to the Walkman
  python install.py               the above, then copies the homebrew (build/HB from build_all.sh) and flashes the
                                  hook firmware if the Walkman does not have it yet (1.02-41)
  python install.py --reflash     the same, flashing 1.02-41 again even if it is there (to test the flashing)
  python install.py --firmware    a Walkman with an older firmware (1.00, 1.01): flashes Sony's official 1.02 first
  python install.py --uninstall   flashes Sony's official firmware back and removes H:\\HB\\boot.sh (the homebrew
                                  files stay until you delete H:\\HB)
Options: --drive H:   --hb path\\to\\build\\HB   --cache folder (default: %USERPROFILE%\\.walkman-hb)

Every file is checked against a known MD5: Sony's updater, the official firmware inside it, and the hook firmware
built from it (the same bytes as the developer's tested unit). Nothing from Sony is shipped with this project."""
import argparse, ctypes, gzip, hashlib, io, os, shutil, struct, subprocess, sys, tarfile, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, 'frozen', False)	# inside the PyInstaller .exe: the homebrew is bundled next to the code
BUNDLE = getattr(sys, '_MEIPASS', HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'homebrew', 'bt'))
import build_hb, fwtrig, patch_a2d
from upgpack import unpack

UPDATER_URL = 'http://walkman.update.sony.net/fw/pc/A820/NW-A820_V1_02.exe'
UPDATER_MD5 = '7ed36a5255b90f8ced476713a5cde81b'
OFFICIAL_MD5 = 'fa3c6ae0152eb26c9ce94570735f0318'	# NW_WM_FW.UPG inside it: JP 1.02
HOOK_MD5 = 'ee63d042897953614389f6224051275a'	# build_hb.build_hook(official): 1.02-41
A2D_MD5 = '106ac24f1f4295a152788165fcb30fc4'	# patch_a2d.py on the official a2d_demo_app (the tested unit's)
CACERT_URL = 'https://curl.se/ca/cacert.pem'
MODELS = ('NW-A826', 'NW-A828', 'NW-A829')
HOOK_SUBREV = '41'
DEFAULT_HB = os.path.join(BUNDLE, 'HB') if FROZEN else os.path.join(HERE, '..', 'build', 'HB')
DEFAULT_CACHE = os.path.join(os.path.expanduser('~'), '.walkman-hb')
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


class InstallError(Exception):
    """a step failed; the message is for the user (es: the same in Spanish, for the installer window)"""
    def __init__(self, en, es=None):
        super().__init__(en)
        self.es = es or en


LOG = print	# the installer window replaces it


def say(*a):
    LOG(' '.join(str(x) for x in a))


def md5(b): return hashlib.md5(b).hexdigest()


# ---------- finding the Walkman ----------

def drives():
    """drive letters that could be the Walkman (removable or fixed, not A:-C:)"""
    out = []
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    for i in range(3, 26):
        if mask & (1 << i):
            d = '%s:' % chr(65 + i)
            if ctypes.windll.kernel32.GetDriveTypeW(d + '\\') in (2, 3):	# DRIVE_REMOVABLE, DRIVE_FIXED
                out.append(d)
    return out


def scan():
    """(drive, info) of the first Walkman that answers Sony's DEVINFO; (drive, None) for a Walkman that only answers
    INQUIRY (the homebrew player's USB mode); (None, None) if there is none"""
    quiet = None
    for d in drives():
        info = fwtrig.devinfo(d)
        if info:
            return d, info
        if quiet is None and fwtrig.is_walkman(d):
            quiet = d
    return quiet, None


def find_walkman(drive):
    if drive:
        info = fwtrig.devinfo(drive)
        if info:
            return drive, info
    else:
        d, info = scan()
        if info:
            return d, info
    raise InstallError('no Walkman found%s. Connect it with USB (Sony UI, not the homebrew player) and try again.'
                       % (' on ' + drive if drive else ''),
                       'No encontré el Walkman. Conectalo por USB (en el modo de Sony, no en el reproductor homebrew) y probá de nuevo.')


def check_model(info):
    if info['product'] not in MODELS:
        raise InstallError('%s is not a Japanese NW-A82x. International NWZ-A82x units are not supported yet.' % info['product'],
                           '%s no es un NW-A82x japonés. Los modelos internacionales NWZ-A82x todavía no están soportados.' % info['product'])


# ---------- Sony's firmware and what is built from it ----------

def official_firmware(cache, progress=None):
    """Sony's JP 1.02 NW_WM_FW.UPG (bytes), from the updater downloaded once into cache. progress(done, total)"""
    os.makedirs(cache, exist_ok=True)
    exe = os.path.join(cache, 'NW-A820_V1_02.exe')
    if not os.path.exists(exe) or md5(open(exe, 'rb').read()) != UPDATER_MD5:
        say('downloading Sony\'s updater (12 MB) from %s' % UPDATER_URL)
        hook = (lambda n, bs, total: progress(min(n * bs, total), total)) if progress else None
        try:
            urllib.request.urlretrieve(UPDATER_URL, exe + '.part', hook)
        except OSError as e:
            raise InstallError('could not download Sony\'s firmware (%s). Check the internet connection and try again.' % e,
                               'No se pudo descargar el firmware de Sony (%s). Revisá la conexión a internet y probá de nuevo.' % e)
        os.replace(exe + '.part', exe)
    d = open(exe, 'rb').read()
    if md5(d) != UPDATER_MD5:
        os.remove(exe)
        raise InstallError('Sony\'s updater did not download correctly (MD5 %s). Try again.' % md5(d),
                           'El firmware de Sony no se descargó bien (MD5 %s). Probá de nuevo.' % md5(d))
    i = d.find(b'MSCF\0\0\0\0')
    if i < 0:
        raise InstallError('no cabinet inside Sony\'s updater', 'El archivo de Sony no tiene el formato esperado.')
    n = struct.unpack_from('<I', d, i + 8)[0]
    cab = os.path.join(cache, 'fw.cab')
    open(cab, 'wb').write(d[i:i + n])
    out = os.path.join(cache, 'cab')
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'expand.exe'), cab, '-F:NW_WM_FW.UPG', out],
                       capture_output=True, creationflags=NO_WINDOW)
    upg = os.path.join(out, 'NW_WM_FW.UPG')
    if r.returncode or not os.path.exists(upg):
        raise InstallError('could not extract the firmware from Sony\'s updater (expand.exe)',
                           'No se pudo extraer el firmware del archivo de Sony (expand.exe).')
    b = open(upg, 'rb').read()
    if md5(b) != OFFICIAL_MD5:
        raise InstallError('the official firmware does not have the expected MD5 (%s)' % md5(b),
                           'El firmware oficial no es el esperado (MD5 %s). No se instaló nada.' % md5(b))
    say('official firmware 1.02: OK (%s)' % OFFICIAL_MD5)
    return b


def hook_firmware(official):
    hook = build_hb.build_hook(official)
    if md5(hook) != HOOK_MD5:
        raise InstallError('the hook firmware came out different (%s): not flashing it' % md5(hook),
                           'El firmware modificado no salió idéntico al probado (%s). No se instaló nada.' % md5(hook))
    say('hook firmware 1.02-%s: OK (%s)' % (HOOK_SUBREV, HOOK_MD5))
    return hook


def rootfs_file(official, name):
    """one file of the official firmware's root filesystem"""
    sig, files = unpack(official, build_hb.JPKEY)
    t = tarfile.open(fileobj=io.BytesIO(gzip.decompress(files[3])))
    return t.extractfile(name).read()


def a2d_hb(official):
    """Sony/CSR's Bluetooth tool, patched to start with the remembered device (homebrew/bt/patch_a2d.py)"""
    try:
        b = patch_a2d.patch(rootfs_file(official, './usr/local/test/bt/a2d_demo_app'))
    except ValueError as e:
        raise InstallError('patching the Bluetooth helper: %s' % e, 'Falló la preparación del ayudante de Bluetooth: %s' % e)
    if md5(b) != A2D_MD5:
        raise InstallError('the patched Bluetooth helper came out different (%s)' % md5(b),
                           'El ayudante de Bluetooth no salió idéntico al probado (%s). No se instaló nada.' % md5(b))
    say('Bluetooth helper a2d_hb: OK (%s)' % A2D_MD5)
    return b


# ---------- writing to the Walkman ----------

def wait_for(rev, subrev, minutes=8, tick=None):
    """after a flash: the Walkman disconnects, updates and comes back (maybe as another drive letter); its drive once
    it reports rev-subrev, else None. tick(seconds, gone) is called every few seconds"""
    say('waiting for the Walkman to update and come back')
    gone, t0 = False, time.time()
    while time.time() - t0 < minutes * 60:
        time.sleep(3)
        d, info = scan()
        if tick:
            tick(time.time() - t0, gone)
        if not info:
            gone = True
            continue
        if gone and info['rev'] == rev and info['subrev'] == subrev:
            say('back as %s: %s %s-%s' % (d, info['product'], info['rev'], info['subrev']))
            return d
    return None


def flash(drive, upg, want_md5, rev, subrev, tick=None):
    """copies upg to the Walkman, makes it flash it and waits until it is back; returns its drive"""
    p = os.path.join(drive + '\\', 'NW_WM_FW.UPG')
    if shutil.disk_usage(drive + '\\').free < len(upg) + (4 << 20):
        raise InstallError('the Walkman needs %d MB free for the update. Delete something from it and try again.' % ((len(upg) >> 20) + 5),
                           'El Walkman necesita %d MB libres para la actualización. Borrá algo y probá de nuevo.' % ((len(upg) >> 20) + 5))
    open(p, 'wb').write(upg)
    if md5(open(p, 'rb').read()) != want_md5:
        raise InstallError('the copy of the firmware on the Walkman is not intact. Try again.',
                           'La copia del firmware en el Walkman no quedó bien. Probá de nuevo.')
    if not fwtrig.trigger(drive, want_md5):
        raise InstallError('the Walkman did not accept the update command. Unplug it, plug it in again and retry.',
                           'El Walkman no aceptó la orden de actualizar. Desconectalo, volvé a conectarlo y probá de nuevo.')
    back = wait_for(rev, subrev, tick=tick)
    if not back:
        raise InstallError('the Walkman did not come back with firmware %s-%s in time. If its screen still shows the'
                           ' update, wait for it to finish; then unplug it, plug it in again and run the installer once more.' % (rev, subrev),
                           'El Walkman no volvió con el firmware %s-%s a tiempo. Si su pantalla sigue actualizando, esperá a que termine;'
                           ' después desconectalo, volvé a conectarlo y abrí el instalador otra vez.' % (rev, subrev))
    try:
        os.remove(os.path.join(back + '\\', 'NW_WM_FW.UPG'))
    except OSError:
        pass
    return back


def copy_homebrew(drive, hb, a2d):
    """H:\\HB from the build, the Bluetooth helper, certificates for HTTPS and the content folders; files copied"""
    if not os.path.isdir(hb) or not os.path.exists(os.path.join(hb, 'bin', 'hbplayer')):
        raise InstallError('no homebrew build at %s: run "sh build_all.sh" first (or use --hb)' % hb,
                           'Faltan los archivos del homebrew en %s.' % hb)
    dst = os.path.join(drive + '\\', 'HB')
    n = 0
    for root, dirs, files in os.walk(hb):
        rel = os.path.relpath(root, hb)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            shutil.copyfile(os.path.join(root, f), os.path.join(dst, rel, f)); n += 1
    open(os.path.join(dst, 'bin', 'a2d_hb'), 'wb').write(a2d)
    if not os.path.exists(os.path.join(dst, 'cacert.pem')):
        try:
            urllib.request.urlretrieve(CACERT_URL, os.path.join(dst, 'cacert.pem'))
            say('certificates for HTTPS: cacert.pem')
        except OSError as e:
            say('(no cacert.pem: %s; podcasts will use HTTPS without checking certificates)' % e)
    for f in ('Music', 'Audiolibros', 'Podcasts', 'Juegos'):
        os.makedirs(os.path.join(drive + '\\', f), exist_ok=True)
    fwtrig.flush(drive)
    say('%d files copied to %s' % (n + 1, dst))
    return n + 1


def disable_homebrew(drive):
    boot = os.path.join(drive + '\\', 'HB', 'boot.sh')
    if os.path.exists(boot):
        os.replace(boot, boot + '.off')


# ---------- command line ----------

def main():
    ap = argparse.ArgumentParser(description='Walkman NW-A82x homebrew installer')
    ap.add_argument('--check', action='store_true', help='everything but writing to the Walkman')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--firmware', action='store_true', help='update an older firmware to Sony\'s official 1.02')
    ap.add_argument('--drive')
    ap.add_argument('--hb', default=DEFAULT_HB)
    ap.add_argument('--cache', default=DEFAULT_CACHE)
    ap.add_argument('--yes', action='store_true', help='do not ask before flashing')
    ap.add_argument('--reflash', action='store_true', help='flash 1.02-41 even if the Walkman has it (to test the flashing)')
    a = ap.parse_args()
    if sys.platform != 'win32':
        sys.exit('this installer runs on Windows (it talks to the Walkman with SCSI pass-through)')
    ask = lambda q: a.yes or input(q + ' Type "yes": ').strip().lower() == 'yes'
    try:
        drive, info = find_walkman(a.drive)
        say('Walkman: %s firmware %s-%s on %s' % (info['product'], info['rev'], info['subrev'], drive))
        check_model(info)
        official = official_firmware(a.cache)
        if a.firmware:
            if info['rev'] == '1.02':
                say('already 1.02: nothing to do'); return
            if not a.check and ask('Flash Sony\'s official firmware 1.02?'):
                flash(drive, official, OFFICIAL_MD5, '1.02', '00')
                say('\nDone: official firmware 1.02. Now run the installer again to install the homebrew.')
            return
        if info['rev'] != '1.02':
            raise InstallError('firmware %s: update it to 1.02 first (python install.py --firmware)' % info['rev'])
        hook = hook_firmware(official)
        a2d = a2d_hb(official)
        if a.uninstall:
            if a.check:
                say('(check only: would flash the official firmware and remove HB\\boot.sh)'); return
            if not ask('Flash Sony\'s official firmware back?'):
                return
            disable_homebrew(drive)
            if info['subrev'] != '00':
                flash(drive, official, OFFICIAL_MD5, '1.02', '00')
            say('\nDone: stock firmware. Delete %s\\HB whenever you like.' % drive)
            return
        need_flash = info['subrev'] != HOOK_SUBREV or a.reflash
        if a.check:
            say('(check only: nothing written to the Walkman)')
            say('would copy %s to %s\\HB, then %s' % (a.hb, drive, 'flash 1.02-41' if need_flash else 'skip the flash (already 1.02-41)'))
            return
        copy_homebrew(drive, a.hb, a2d)	# first: the boot after the flash then finds it
        if need_flash:
            say('\nThe Walkman needs the hook firmware (1.02-41). It flashes in about a minute; the battery must be charged'
                ' and the cable must stay connected. Sony\'s updater restores the stock firmware at any time.')
            if not ask('Flash it now?'):
                return
            flash(drive, hook, HOOK_MD5, '1.02', HOOK_SUBREV)
            say('firmware 1.02-41 installed')
        say('\nDone. Unplug the Walkman; HOME for 3 s switches between Sony\'s UI and the player'
            ' (the first start asks for the language).')
    except InstallError as e:
        sys.exit('\nERROR: %s' % e)


if __name__ == '__main__':
    main()
