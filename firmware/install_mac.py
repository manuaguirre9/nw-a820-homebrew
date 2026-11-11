"""install.py for macOS: the same steps and the same MD5 checks, with the three Windows-only parts replaced.

  - SCSI pass-through (DeviceIoControl)  -> the same CDBs over USB Bulk-Only Transport with libusb (pyusb). macOS lets
    a program take a USB disk from its driver only as root, so this runs with sudo; the volume is unmounted first and
    mounted again afterwards.
  - expand.exe (CAB inside Sony's updater) -> macOS's tar (libarchive reads CAB).
  - drive letters                          -> the mounted volume (/Volumes/WALKMAN).

Downloading Sony's updater, building the hook firmware and the Bluetooth helper are install.py's own functions, so
the bytes flashed are checked against the same MD5s as on Windows.

  sudo VENV/bin/python firmware/install_mac.py --check      read-only: device info, downloads, builds, MD5 checks
  sudo VENV/bin/python firmware/install_mac.py              copies HB, flashes 1.02-41 if needed
  sudo VENV/bin/python firmware/install_mac.py --firmware   older firmware: Sony's official 1.02 first
  sudo VENV/bin/python firmware/install_mac.py --uninstall  Sony's official firmware back, HB/boot.sh disabled
VENV needs pyusb, libusb-package and cryptography."""
import argparse, glob, hashlib, os, plistlib, shutil, struct, subprocess, sys, tempfile, time, types, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.modules['fwtrig'] = types.ModuleType('fwtrig')	# the Windows SCSI module; install.py only needs it at run time
import install as I
from install import InstallError, say, md5

import usb.core, usb.util, libusb_package

SONY = 0x054c
UPG_NAME = 'NW_WM_FW.UPG'	# the file name the Walkman's updater looks for at the root of the drive (NW-A91x: NW_A91X.UPG)
HOME = os.path.expanduser('~' + os.environ.get('SUDO_USER', ''))
DEFAULT_CACHE = os.path.join(HOME, '.walkman-hb')
REPO_HB = os.path.join(HERE, '..', 'build', 'HB')
RELEASE_HB = os.path.join(DEFAULT_CACHE, 'release', 'nw-a820-homebrew-v%s' % I.VERSION, 'build', 'HB')


# ---------- the volume ----------

def diskinfo(path):
    r = subprocess.run(['diskutil', 'info', '-plist', path], capture_output=True)
    return plistlib.loads(r.stdout) if r.returncode == 0 else None


def find_volume():
    """mount point of the Walkman's USB volume, or None"""
    for v in sorted(glob.glob('/Volumes/*')):
        d = diskinfo(v)
        if d and d.get('BusProtocol') == 'USB' and 'WALKMAN' in (d.get('MediaName') or '') + (d.get('VolumeName') or ''):
            return v
    return None


def wait_volume(seconds=45):
    t0 = time.time()
    while time.time() - t0 < seconds:
        v = find_volume()
        if v:
            return v
        time.sleep(1)
    return None


def unmount(vol):
    d = diskinfo(vol)
    disk = d['ParentWholeDisk'] if d else None
    subprocess.run(['sync'])
    r = subprocess.run(['diskutil', 'unmountDisk', disk or vol], capture_output=True, text=True)
    if r.returncode:
        raise InstallError('macOS could not unmount the Walkman (%s). Close Finder windows or apps using it and retry.'
                           % (r.stderr or r.stdout).strip())


# ---------- SCSI over USB Bulk-Only Transport ----------

def find_device():
    be = libusb_package.get_libusb1_backend()
    for dev in usb.core.find(find_all=True, idVendor=SONY, backend=be) or []:
        try:
            for intf in dev.get_active_configuration():
                if intf.bInterfaceClass == 8 and intf.bInterfaceProtocol == 0x50:	# mass storage, bulk-only
                    return dev, intf
        except usb.core.USBError:
            continue
    return None, None


class Scsi:
    """takes the Walkman from macOS's driver (volume unmounted) and gives it back on exit (macOS mounts it again)"""

    def __init__(self, vol):
        self.vol, self.tag = vol, 0

    def __enter__(self):
        if self.vol:
            unmount(self.vol)
        self.dev, intf = find_device()
        if not self.dev:
            raise InstallError('no Sony USB device found')
        self.n = intf.bInterfaceNumber
        if self.dev.is_kernel_driver_active(self.n):
            self.dev.detach_kernel_driver(self.n)
        usb.util.claim_interface(self.dev, self.n)
        eps = list(intf)
        self.ep_in = next(e.bEndpointAddress for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
                          and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
        self.ep_out = next(e.bEndpointAddress for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
                           and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
        return self

    def cmd(self, cdb, n, timeout=30000):
        """(status, data): status 0 = good. Raises usb.core.USBError if the device goes away"""
        self.tag += 1
        cbw = struct.pack('<IIIBBB', 0x43425355, self.tag, n, 0x80, 0, len(cdb)) + bytes(cdb).ljust(16, b'\0')
        self.dev.write(self.ep_out, cbw, timeout=5000)
        data = b''
        if n:
            try:
                data = bytes(self.dev.read(self.ep_in, n, timeout=timeout))
            except usb.core.USBError as e:
                if e.errno != 32:	# a stall: no data, the status follows
                    raise
                self.dev.clear_halt(self.ep_in)
        try:
            csw = bytes(self.dev.read(self.ep_in, 13, timeout=timeout))
        except usb.core.USBError as e:
            if e.errno != 32:
                raise
            self.dev.clear_halt(self.ep_in)
            csw = bytes(self.dev.read(self.ep_in, 13, timeout=timeout))
        sig, tag, residue, status = struct.unpack('<IIIB', csw[:13])
        if sig != 0x53425355 or tag != self.tag:
            raise InstallError('unexpected USB status from the Walkman')
        return status, data.ljust(n, b'\0')

    def __exit__(self, *exc):
        try:
            usb.util.release_interface(self.dev, self.n)
            self.dev.attach_kernel_driver(self.n)
        except (usb.core.USBError, NotImplementedError):
            pass
        usb.util.dispose_resources(self.dev)
        return False


INQUIRY = [0x12, 0, 0, 0, 36, 0]
DEVINFO = [0xFC, 0, 0x20, ord('d'), ord('b'), ord('m'), ord('n'), 0, 0x80, 0, 0, 0]


def devinfo(vol):
    """{'vendor', 'product', 'rev', 'subrev'} like fwtrig.devinfo, or None. Leaves the volume mounted again;
    returns (info, mount point)"""
    with Scsi(vol) as s:
        st, d = s.cmd(INQUIRY, 36)
        if st or not d[8:36].startswith(b'SONY    WALKMAN'):
            info = None
        else:
            st, d = s.cmd(DEVINFO, 0x80)
            t = lambda b: b.decode('latin1').strip('\0 ')
            info = None if st or not d.startswith(b'DEVINFO') else \
                {'vendor': t(d[16:24]), 'product': t(d[24:40]), 'rev': t(d[40:44]), 'subrev': t(d[44:48])}
    return info, wait_volume()


def trigger(vol, want_md5):
    """fwtrig.trigger: the Walkman flashes VOL/UPG_NAME. True if it accepted or dropped off the bus right away
    (wait_for then checks the version it comes back with)"""
    p = os.path.join(vol, UPG_NAME)
    m = md5(open(p, 'rb').read())
    say('%s md5 %s size %d' % (p, m, os.path.getsize(p)))
    if m != want_md5:
        say('md5 mismatch, aborting'); return False
    with Scsi(vol) as s:
        for flags in (0x80, 0x00):
            try:
                st, d = s.cmd([0xFC, 0, 0x04, ord('d'), ord('b'), ord('m'), ord('n'), 0, flags, 0, 0, 0], 0x80)
            except usb.core.USBError as e:
                say('FC04 flags=0x%02x: the Walkman left the bus (%s); treating as accepted' % (flags, e))
                return True
            say('FC04 upgrade flags=0x%02x status=%d' % (flags, st))
            if st == 0:
                say('accepted'); return True
    return False


# ---------- install.py's steps with paths instead of drive letters ----------

def official_firmware(cache):
    """install.official_firmware with tar instead of expand.exe"""
    os.makedirs(cache, exist_ok=True)
    exe = os.path.join(cache, 'NW-A820_V1_02.exe')
    if not os.path.exists(exe) or md5(open(exe, 'rb').read()) != I.UPDATER_MD5:
        say('downloading Sony\'s updater (12 MB) from %s' % I.UPDATER_URL)
        urllib.request.urlretrieve(I.UPDATER_URL, exe + '.part')
        os.replace(exe + '.part', exe)
    d = open(exe, 'rb').read()
    if md5(d) != I.UPDATER_MD5:
        raise InstallError('Sony\'s updater did not download correctly (MD5 %s)' % md5(d))
    i = d.find(b'MSCF\0\0\0\0')
    if i < 0:
        raise InstallError('no cabinet inside Sony\'s updater')
    n = struct.unpack_from('<I', d, i + 8)[0]
    with tempfile.TemporaryDirectory() as t:
        cab = os.path.join(t, 'fw.cab')
        open(cab, 'wb').write(d[i:i + n])
        r = subprocess.run(['tar', '-xf', cab, '-C', t, 'NW_WM_FW.UPG'], capture_output=True)
        upg = os.path.join(t, 'NW_WM_FW.UPG')
        if not os.path.exists(upg):
            subprocess.run(['tar', '-xf', cab, '-C', t], capture_output=True)
        if not os.path.exists(upg):
            raise InstallError('could not extract the firmware from Sony\'s updater (tar)')
        b = open(upg, 'rb').read()
    if md5(b) != I.OFFICIAL_MD5:
        raise InstallError('the official firmware does not have the expected MD5 (%s)' % md5(b))
    say('official firmware 1.02: OK (%s)' % I.OFFICIAL_MD5)
    return b


def hook_firmware(official):
    """install.hook_firmware, made identical to the Windows build: Python's gzip leaves the header's OS byte to zlib,
    which writes 255 (unknown) here and 10 on Windows, where HOOK_MD5 was taken. Same compressed data either way."""
    import build_hb, upgpack
    hook = build_hb.build_hook(official)
    if md5(hook) != I.HOOK_MD5:
        sig, files = upgpack.unpack(hook, build_hb.JPKEY)
        gz = bytearray(files[3]); gz[9] = 10
        hook = upgpack.pack(files[:3] + [bytes(gz)], build_hb.JPKEY, sig)
    if md5(hook) != I.HOOK_MD5:
        raise InstallError('the hook firmware came out different (%s): not flashing it' % md5(hook))
    say('hook firmware 1.02-%s: OK (%s)' % (I.HOOK_SUBREV, I.HOOK_MD5))
    return hook


def wait_for(rev, subrev, minutes=8):
    say('waiting for the Walkman to update and come back')
    gone, t0 = False, time.time()
    while time.time() - t0 < minutes * 60:
        time.sleep(3)
        dev, _ = find_device()
        if not dev:
            if not gone:
                say('  the Walkman left USB (updating)')
            gone = True
            continue
        if not gone:
            continue
        vol = wait_volume(60)
        try:
            info, vol = devinfo(vol)
        except (usb.core.USBError, InstallError):
            continue
        if info and info['rev'] == rev and info['subrev'] == subrev:
            say('back as %s: %s %s-%s' % (vol, info['product'], info['rev'], info['subrev']))
            return vol
    return None


def flash(vol, upg, want_md5, rev, subrev):
    p = os.path.join(vol, UPG_NAME)
    if shutil.disk_usage(vol).free < len(upg) + (4 << 20):
        raise InstallError('the Walkman needs %d MB free for the update' % ((len(upg) >> 20) + 5))
    open(p, 'wb').write(upg)
    subprocess.run(['sync'])
    if md5(open(p, 'rb').read()) != want_md5:
        raise InstallError('the copy of the firmware on the Walkman is not intact. Try again.')
    if not trigger(vol, want_md5):
        raise InstallError('the Walkman did not accept the update command. Unplug it, plug it in again and retry.')
    back = wait_for(rev, subrev)
    if not back:
        raise InstallError('the Walkman did not come back with firmware %s-%s in time. If its screen still shows the'
                           ' update, wait for it to finish; then unplug it, plug it in again and run this again.' % (rev, subrev))
    try:
        os.remove(os.path.join(back, UPG_NAME))
    except OSError:
        pass
    return back


def copy_homebrew(vol, hb, a2d):
    if not os.path.exists(os.path.join(hb, 'bin', 'hbplayer')):
        raise InstallError('no homebrew build at %s (use --hb)' % hb)
    dst = os.path.join(vol, 'HB')
    n = 0
    for root, dirs, files in os.walk(hb):
        rel = os.path.relpath(root, hb)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            if not f.startswith('.'):
                shutil.copyfile(os.path.join(root, f), os.path.join(dst, rel, f)); n += 1
    open(os.path.join(dst, 'bin', 'a2d_hb'), 'wb').write(a2d)
    if not os.path.exists(os.path.join(dst, 'cacert.pem')):
        # curl, not urllib: python.org's Python on macOS has no CA store until "Install Certificates" is run
        r = subprocess.run(['curl', '-sfL', '-o', os.path.join(dst, 'cacert.pem'), I.CACERT_URL], capture_output=True, text=True)
        say('certificates for HTTPS: cacert.pem' if r.returncode == 0 else '(no cacert.pem: curl %s)' % r.returncode)
    for f in ('Music', 'Audiolibros', 'Podcasts', 'Juegos'):
        os.makedirs(os.path.join(vol, f), exist_ok=True)
    open(os.path.join(dst, 'version.txt'), 'w').write(I.VERSION + '\n')
    subprocess.run(['sync'])
    say('%d files copied to %s' % (n + 1, dst))


def disable_homebrew(vol):
    boot = os.path.join(vol, 'HB', 'boot.sh')
    if os.path.exists(boot):
        os.replace(boot, boot + '.off')


def main():
    ap = argparse.ArgumentParser(description='Walkman NW-A82x homebrew installer (macOS)')
    ap.add_argument('--check', action='store_true', help='everything but writing to the Walkman')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--firmware', action='store_true', help='update an older firmware to Sony\'s official 1.02')
    ap.add_argument('--hb', default=REPO_HB if os.path.exists(os.path.join(REPO_HB, 'bin', 'hbplayer')) else RELEASE_HB)
    ap.add_argument('--cache', default=DEFAULT_CACHE)
    ap.add_argument('--yes', action='store_true', help='do not ask before flashing')
    ap.add_argument('--reflash', action='store_true')
    a = ap.parse_args()
    if sys.platform != 'darwin':
        sys.exit('this is the macOS installer; on Windows use install.py')
    if os.geteuid() != 0:
        sys.exit('run it with sudo: macOS lets only root talk to a USB disk directly')
    ask = lambda q: a.yes or input(q + ' Type "yes": ').strip().lower() == 'yes'
    try:
        vol = find_volume()
        if not vol:
            raise InstallError('no Walkman volume mounted. Connect it with USB (Sony UI, not the homebrew player).')
        info, vol = devinfo(vol)
        if not info:
            raise InstallError('the device did not answer Sony\'s DEVINFO (is it in the homebrew player\'s USB mode?)')
        if not vol:
            raise InstallError('macOS did not mount the Walkman again. Unplug it, plug it in again and retry.')
        say('Walkman: %s firmware %s-%s on %s' % (info['product'], info['rev'], info['subrev'], vol))
        I.check_model(info)
        official = official_firmware(a.cache)
        if a.firmware:
            if info['rev'] == '1.02':
                say('already 1.02: nothing to do'); return
            if not a.check and ask('Flash Sony\'s official firmware 1.02?'):
                flash(vol, official, I.OFFICIAL_MD5, '1.02', '00')
                say('\nDone: official firmware 1.02. Run this again to install the homebrew.')
            return
        if info['rev'] != '1.02':
            raise InstallError('firmware %s: update it to 1.02 first (--firmware)' % info['rev'])
        hook = hook_firmware(official)
        a2d = I.a2d_hb(official)
        if a.uninstall:
            if a.check:
                say('(check only: would flash the official firmware and disable HB/boot.sh)'); return
            if not ask('Flash Sony\'s official firmware back?'):
                return
            disable_homebrew(vol)
            if info['subrev'] != '00':
                flash(vol, official, I.OFFICIAL_MD5, '1.02', '00')
            say('\nDone: stock firmware. Delete %s/HB whenever you like.' % vol)
            return
        need_flash = info['subrev'] != I.HOOK_SUBREV or a.reflash
        if a.check:
            say('(check only: nothing written to the Walkman)')
            say('would copy %s to %s/HB, then %s' % (os.path.abspath(a.hb), vol,
                'flash 1.02-41' if need_flash else 'skip the flash (already 1.02-41)'))
            return
        copy_homebrew(vol, a.hb, a2d)
        if need_flash:
            say('\nThe Walkman needs the hook firmware (1.02-41). It flashes in about a minute; the battery must be charged'
                ' and the cable must stay connected. Sony\'s updater restores the stock firmware at any time.')
            if not ask('Flash it now?'):
                return
            flash(vol, hook, I.HOOK_MD5, '1.02', I.HOOK_SUBREV)
            say('firmware 1.02-41 installed')
        say('\nDone. Eject and unplug the Walkman; HOME for 3 s switches between Sony\'s UI and the player'
            ' (the first start asks for the language).')
    except InstallError as e:
        sys.exit('\nERROR: %s' % e)


if __name__ == '__main__':
    main()
