"""Runs every test that needs no Walkman: unit tests of the player's logic, the RSS parser against saved feeds
(and the PC script's file names), and every screen rendered in Spanish and in English with a check that no Spanish
text is left untranslated in English. Usage: python run_tests.py [RESDIR]  (RESDIR: Sony's gemini res folder for the
screen renders; skipped if missing). Needs zig (ZIG env var or the toolchain folder next to homebrew) and Pillow."""
import glob, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HB = os.path.dirname(HERE)
ZIG = os.environ.get('ZIG') or os.path.join(os.path.dirname(HB), 'toolchain', 'zig-x86_64-windows-0.15.2', 'zig.exe')
OUT = os.path.join(HERE, 'out')
os.makedirs(OUT, exist_ok=True)
results = []


def cc(exe, sources, extra=()):
    cmd = [ZIG, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-g', '-Wall', '-Wno-unused-function', '-o', exe] + list(extra) + sources
    r = subprocess.run(cmd, capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if 'error' in l]
    if r.returncode:
        print(r.stderr[-3000:])
    return r.returncode == 0, errs


def step(name, ok, detail=''):
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + (': ' + detail if detail else ''))


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)
    return r.returncode, r.stdout + r.stderr


# 1. unit tests
p = os.path.join(HB, 'hbplayer')
exe = os.path.join(OUT, 'unit.exe')
ok, _ = cc(exe, [os.path.join(HERE, 'unit.c')] + [os.path.join(p, f) for f in ('podsearch.c', 'fsname.c', 'batt.c', 'i18n.c', 'screens.c')] + [os.path.join(HB, 'common', 'gfx.c')])
if ok:
    rc, out = run([exe, os.path.join(HERE, 'fixtures')])
    print(out.strip())
    step('unit tests', rc == 0, out.strip().splitlines()[-1] if out.strip() else '')
else:
    step('unit tests (build)', False)

# 2. RSS parser: items and the same file names as the PC script podsync.py
t = os.path.join(HB, 'net', 'test')
exe = os.path.join(OUT, 'cmp_names.exe')
ok, _ = cc(exe, [os.path.join(t, 'cmp_names.c'), os.path.join(HB, 'net', 'rss.c')])
feeds = sorted(glob.glob(os.path.join(t, '*.xml')))
if ok and feeds:
    rc, out = run([exe] + feeds)
    print(out.strip())
    step('RSS file names = podsync.py', rc == 0)
elif not feeds:
    print('(RSS test skipped: no saved feeds in net/test)')
else:
    step('RSS test (build)', False)

# 3. screens in both languages; untranslated Spanish in English is a failure
res = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HB), 'research', 'ui', 'res')
if os.path.isdir(res):
    exe = os.path.join(OUT, 'shots.exe')
    ok, _ = cc(exe, [os.path.join(p, f) for f in ('shots.c', 'screens.c', 'cover.c', 'i18n.c')] +
               [os.path.join(HB, 'third_party', 'tjpgd', 'src', 'tjpgd.c'), os.path.join(HB, 'common', 'gfx.c')])
    if ok:
        for lang in ('es', 'en'):
            d = os.path.join(OUT, 'shots_' + lang)
            os.makedirs(d, exist_ok=True)
            for f in glob.glob(os.path.join(d, '*')):
                os.remove(f)
            rc, out = run([exe, res, d, '', lang])
            n = len(glob.glob(os.path.join(d, '*.rgb565')))
            if lang == 'en':
                left = open(os.path.join(d, 'untranslated.txt'), encoding='utf-8').read().split('\n') if os.path.exists(os.path.join(d, 'untranslated.txt')) else []
                left = [l for l in left if l]
                for l in left:
                    print('   untranslated:', l)
                step('screens in English (%d), all translated' % n, rc == 0 and n > 0 and not left, '%d strings left' % len(left))
            else:
                step('screens in Spanish (%d)' % n, rc == 0 and n > 0)
            try:
                from PIL import Image
                pngs = []
                for f in sorted(glob.glob(os.path.join(d, '*.rgb565'))):
                    b = open(f, 'rb').read()
                    im = Image.frombytes('RGB', (240, 320), bytes(x for i in range(0, len(b), 2) for x in (
                        ((b[i + 1] >> 3) * 255 // 31), ((((b[i + 1] & 7) << 3) | (b[i] >> 5)) * 255 // 63), ((b[i] & 31) * 255 // 31))))
                    im = im.resize((480, 640), Image.NEAREST)
                    png = f[:-7] + '.png'
                    im.save(png); pngs.append(png); os.remove(f)
                cols = 6
                sheet = Image.new('RGB', (cols * 250, ((len(pngs) + cols - 1) // cols) * 330), (40, 40, 40))
                for i, q in enumerate(pngs):
                    sheet.paste(Image.open(q).resize((240, 320)), ((i % cols) * 250 + 5, (i // cols) * 330 + 5))
                sheet.save(os.path.join(d, 'todas.png'))
            except ImportError:
                pass
    else:
        step('screens (build)', False)
else:
    print('(screens skipped: no res folder %s)' % res)

# 4. every interface text in the code (formats filled with sample values) has an English entry
sys.path.insert(0, HERE)
import trcover
ok, msg = trcover.check(HB, ZIG, OUT, cc)
step('every Spanish text has an English entry', ok, msg)

# 5. the installer's firmware chain (repository layout: ../../firmware): Sony's updater -> official firmware -> hook
# firmware and Bluetooth helper, each against its known MD5 (downloads the 12 MB updater once into ~/.walkman-hb)
fw = os.path.join(os.path.dirname(HB), 'firmware')
if os.path.exists(os.path.join(fw, 'install.py')) and sys.platform == 'win32':
    rc, out = run([sys.executable, '-c', 'import sys, hashlib; sys.path.insert(0, %r); import install as i; i.CACHE = i.os.path.join(i.os.path.expanduser("~"), ".walkman-hb"); '
                   'o = i.official_firmware(i.CACHE); h = i.build_hb.build_hook(o); a = i.a2d_hb(o); '
                   'print(i.md5(o) == i.OFFICIAL_MD5, i.md5(h) == i.HOOK_MD5, i.md5(a) == i.A2D_MD5)' % fw])
    step('installer firmware chain (official, hook, a2d_hb = known MD5s)', rc == 0 and out.strip().endswith('True True True'), out.strip()[-200:])
else:
    print('(installer test skipped: no firmware/ folder here)')

# 6. the device build links only against what the Walkman has
rc, out = run(['sh', os.path.join(p, 'build.sh')])
step('device build (hbplayer)', rc == 0 and 'NEEDED OK' in out, '' if rc == 0 else out[-500:])

bad = [n for n, ok in results if not ok]
print('\n%d/%d passed' % (len(results) - len(bad), len(results)) + ('' if not bad else '; FAILED: ' + ', '.join(bad)))
sys.exit(1 if bad else 0)
