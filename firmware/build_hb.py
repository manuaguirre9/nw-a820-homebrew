# HOMEBREW HOOK build (user-authorized). Base: official JP NW-A82x 1.02 rootfs, unchanged except:
#   /etc/init.d/gemini: before the Sony UI starts, mount /contents and, if /contents/HB/boot.sh exists,
#                       run it (cwd /contents/HB, output -> /contents/HB/boot_log.txt). No boot.sh = stock boot.
#                       If boot.sh creates /tmp/hb_skip_gemini the Sony UI is not started (homebrew mode).
#   /etc/firmversion:   sub-revision "41  " (device reports 1.02-41).
#   /usr/local/bin/gemini-ICX1115: byte patches listed in BIN_PATCHES (e.g. silencing the "Simple Mode" popup);
#                       each entry is verified against the original bytes before it is applied.
# Revert = official jp/NW_WM_FW.UPG (md5 fa3c6ae0152eb26c9ce94570735f0318) via fwtrig.py.
# --variant intl: same hook on the official NWZ-A82x 1.02 (intl key, gemini-ICX1113, no BIN_PATCHES). Untested on a device.
import tarfile, io, gzip, hashlib, sys
from upgpack import pack, unpack
JPKEY, JPSIG = b'5ef17493', b'07fa0b6e'
INTLKEY, INTLSIG = b'4abf02d3', b'97ecc5b9'
SRC, BASE, OUT, SUBREV = 'jp_file3.dec', 'jp/NW_WM_FW.UPG', 'NW_WM_FW_hb.UPG', '41  '
INTL_BASE, INTL_OUT = 'intl/NW_WM_FW.UPG', 'NW_WM_FW_hb_intl.UPG'

# file in tar -> list of (file offset, original bytes, new bytes)
BIN_PATCHES = {
    # gemini: getProfile(music) in {1,2} check at vaddr 0x281dc -> 'mov r3,#2': the startup "Simple Mode" popup
    # (page 0x49) is never opened; all other DB dialogs and the profile-dependent browsing are untouched.
    './usr/local/bin/gemini-ICX1115': [(0x201dc, bytes.fromhex('013040e2'), bytes.fromhex('0230a0e3'))],
}

HOOK = '''checkdata
umount /contents > /dev/null 2>&1

# HOMEBREW hook: run /contents/HB/boot.sh if present (delete it from the PC to get a stock boot)
rm -f /tmp/hb_skip_gemini
if mount -t vfat /dev/mtdblock/2 /contents > /dev/null 2>&1; then
	if [ -f /contents/HB/boot.sh ]; then
		echo "HB: running /contents/HB/boot.sh"
		( cd /contents/HB && sh ./boot.sh > /contents/HB/boot_log.txt 2>&1 )
		echo "HB: boot.sh returned $?"
	fi
	sync
	cd /
	umount /contents > /dev/null 2>&1
fi
if [ -f /tmp/hb_skip_gemini ]; then
	echo "HB: Sony UI skipped"
	run_emergency
	exit 0
fi
'''

patches = {'./etc/init.d/gemini': [('checkdata\numount /contents > /dev/null 2>&1\n', HOOK)]}

def build_hook(base, variant='jp'):
    """the hook firmware (bytes) from the official 1.02 NW_WM_FW.UPG (bytes) of that variant: its rootfs
    (file 3, gzip'd tar) with the patches above, the other three files as they are"""
    key, want_sig, bin_patches = (JPKEY, JPSIG, BIN_PATCHES) if variant == 'jp' else (INTLKEY, INTLSIG, {})
    sig, files = unpack(base, key)
    assert sig == want_sig, (variant, sig)
    return _build(gzip.decompress(files[3]), files, key, sig, bin_patches)


def _build(tar_bytes, files, key=JPKEY, sig=JPSIG, bin_patches=BIN_PATCHES):
    tin = tarfile.open(fileobj=io.BytesIO(tar_bytes)); out = io.BytesIO()
    tout = tarfile.open(fileobj=out, mode='w', format=tin.format)
    done = []
    for m in tin.getmembers():
        if m.isfile() and m.name in patches:
            s = tin.extractfile(m).read().decode('latin1')
            for a, b in patches[m.name]:
                assert s.count(a) == 1, (m.name, a[:40], s.count(a)); s = s.replace(a, b)
            data = s.encode('latin1'); m.size = len(data); tout.addfile(m, io.BytesIO(data)); done.append(m.name)
        elif m.isfile() and m.name in bin_patches:
            data = bytearray(tin.extractfile(m).read())
            for off, orig, new in bin_patches[m.name]:
                assert bytes(data[off:off + len(orig)]) == orig, (m.name, hex(off), bytes(data[off:off + len(orig)]).hex())
                data[off:off + len(new)] = new
            m.size = len(data); tout.addfile(m, io.BytesIO(bytes(data))); done.append(m.name)
        elif m.isfile() and m.name == './etc/firmversion':
            s = tin.extractfile(m).read().decode('latin1')
            old = 'DOB_SUBREV="00  "'; assert s.count(old) == 2
            i = s.rindex(old); s = s[:i] + 'DOB_SUBREV="%s"' % SUBREV + s[i+len(old):]
            data = s.encode('latin1'); m.size = len(data); tout.addfile(m, io.BytesIO(data)); done.append(m.name)
        elif m.isfile():
            tout.addfile(m, tin.extractfile(m))
        else:
            tout.addfile(m)
    tout.close()
    assert sorted(done) == sorted(list(patches) + list(bin_patches) + ['./etc/firmversion']), done
    gz = gzip.compress(out.getvalue(), compresslevel=9, mtime=0)
    new = pack([files[0], files[1], files[2], gz], key, sig)
    s2, f2 = unpack(new, key)
    assert s2 == sig and f2[:3] == files[:3] and f2[3] == gz
    return new


if __name__ == '__main__':	# python build_hb.py [--variant intl]: jp/NW_WM_FW.UPG -> NW_WM_FW_hb.UPG (intl/ -> _hb_intl)
    variant = 'intl' if '--variant' in sys.argv and sys.argv[sys.argv.index('--variant') + 1] == 'intl' else 'jp'
    base, out = (BASE, OUT) if variant == 'jp' else (INTL_BASE, INTL_OUT)
    new = build_hook(open(base, 'rb').read(), variant)
    open(out, 'wb').write(new)
    print('built', out, len(new), 'bytes md5', hashlib.md5(new).hexdigest())
