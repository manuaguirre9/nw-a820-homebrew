# FINAL build (user-authorized): official NWZ-A82x 1.02 rootfs on the JP NW-A829, minimal changes:
#   1. rc.sysinfo : model 0x0301 also gets ICX_WMT=y (only used by the boot script's rescue-USB path)
#   2. /etc/init.d/gemini : the UI app is launched directly instead of through `watchman -s 20`
#      (watchman's handling of this binary tore the USB stack down ~20 s after boot); if the app ever
#      exits, the script waits for its processes to be gone and then enters rescue USB mode (PID 0324)
#   3. /etc/firmversion : sub-revision "30  " -> device reports 1.02-30
#   USB stays MSC-only PID 035D, DRM stays AAD, no GEMINI_MODEL_ID override (content handled like the JP unit:
#   SonicStage/OMGAUDIO; drag-and-drop MP3s are not indexed).
import tarfile, io, gzip, difflib, hashlib
from upgpack import pack, unpack
JPKEY, JPSIG = b'5ef17493', b'07fa0b6e'; INTLKEY = b'4abf02d3'

patches = {
 './etc/init.d/rc.sysinfo': [
   ('${MID_ICX_1113}*|${MID_ICX_1113U2}*|${MID_ICX_1113W})',
    '${MID_ICX_1113}*|${MID_ICX_1115}*|${MID_ICX_1113U2}*|${MID_ICX_1113W})')],
 './etc/init.d/gemini': [
   ('\t/usr/local/bin/watchman -s 20 -X $script_reboot $command\n',
    '\t# launch the UI app directly (watchman is not used on this unit)\n'
    '\t$command > /dev/null 2>&1\n'
    '\twhile ps | grep -q " gemini$"; do sleep 5; done\n'
    '\tfalse\n')],
}

src = open('intl_file3.dec', 'rb').read()
tin = tarfile.open(fileobj=io.BytesIO(src)); out = io.BytesIO()
tout = tarfile.open(fileobj=out, mode='w', format=tin.format)
done = []; originals = {}
for m in tin.getmembers():
    if m.isfile() and m.name in patches:
        s = tin.extractfile(m).read().decode('latin1'); originals[m.name] = s
        for a, b in patches[m.name]:
            assert s.count(a) == 1, (m.name, a[:40], s.count(a)); s = s.replace(a, b)
        data = s.encode('latin1'); m.size = len(data); tout.addfile(m, io.BytesIO(data)); done.append(m.name)
    elif m.isfile() and m.name == './etc/firmversion':
        s = tin.extractfile(m).read().decode('latin1'); originals[m.name] = s
        old = 'DOB_SUBREV="00  "'; assert s.count(old) == 2
        i = s.rindex(old); s = s[:i] + 'DOB_SUBREV="30  "' + s[i+len(old):]
        data = s.encode('latin1'); m.size = len(data); tout.addfile(m, io.BytesIO(data)); done.append(m.name)
    elif m.isfile():
        tout.addfile(m, tin.extractfile(m))
    else:
        tout.addfile(m)
tout.close(); patched = out.getvalue()
assert sorted(done) == sorted(list(patches) + ['./etc/firmversion']), done
print('patched:', done)
t2 = tarfile.open(fileobj=io.BytesIO(patched))
for n in done:
    a = originals[n].splitlines(); b = t2.extractfile(n).read().decode('latin1').splitlines()
    print('==== DIFF', n); print('\n'.join(l for l in difflib.unified_diff(a, b, lineterm='', n=0) if l[:1] in '+-' and not l.startswith(('---', '+++'))))
gz = gzip.compress(patched, compresslevel=9, mtime=0)
sig, files = unpack(open('out/NW_WM_FW.UPG', 'rb').read(), INTLKEY)
new = pack([files[0], files[1], files[2], gz], JPKEY, JPSIG)
open('NW_WM_FW_intl_final.UPG', 'wb').write(new)
s2, f2 = unpack(new, JPKEY)
assert s2 == JPSIG and f2[0] == files[0] and f2[1] == files[1] and f2[2] == files[2] and f2[3] == gz
print('built NW_WM_FW_intl_final.UPG', len(new), 'bytes md5', hashlib.md5(new).hexdigest(), '; verify-unpack OK')
