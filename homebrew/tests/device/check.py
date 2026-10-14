"""Reads the on-device test results (H:\\HB\\out\\test, written by the run stage.py set up), checks the logs and turns
the screenshots into PNGs and contact sheets (out\\device\\). python check.py [DRIVE]"""
import glob, os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = (sys.argv[1] if len(sys.argv) > 1 else 'H:').rstrip('\\/') + os.sep
SRC = os.path.join(DRIVE, 'HB', 'out', 'test')
OUT = os.path.join(os.path.dirname(HERE), 'out', 'device')
results = []


def step(name, ok, detail=''):
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + (': ' + detail if detail else ''))


def read(name):
    p = os.path.join(SRC, name)
    return open(p, encoding='utf-8', errors='replace').read() if os.path.exists(p) else ''


if not os.path.isdir(SRC):
    sys.exit('no %s: did the Walkman boot with the tests staged (stage.py)?' % SRC)
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
run = read('run.txt')
print(run.strip())
step('the test run finished', 'end ' in run)

# selftest: AAC on the normal path, then the MP3s on the DSP's direct path
st = read('selftest_log.txt')
plays = re.findall(r'eng: play (.*?) from \d+ s \[(.*?)[,\]]', st)
for path, why in plays:
    print('   played %s -> %s' % (os.path.basename(path), why))
mp3s = [w for p, w in plays if p.endswith('.mp3')]
# 75 s of play: the change AAC -> MP3 is what counts (a long first MP3 never reaches the second)
step('AAC then MP3: the MP3s play direct', len(mp3s) >= 1 and all(w == 'direct' for w in mp3s), ', '.join(mp3s))
bad = [l for l in st.splitlines() if re.search(r'direct start failed|direct session error|direct playback failed|helper gone|no data', l)]
step('no DSP direct-mode errors', not bad and bool(st), '; '.join(bad[:3]) or ('no log' if not st else ''))

for sc, want in (('a', 'a01_idioma'), ('b', 'b01_inicio')):
    log = read('%s_log.txt' % sc)
    fatal = [l for l in log.splitlines() if 'FATAL' in l]
    step('scenario %s: no crash' % sc, bool(log) and not fatal and 'rc=0' in re.search(r'%s rc=\d+' % sc, run).group(0) if re.search(r'%s rc=\d+' % sc, run) else False,
         '; '.join(fatal) or ('rc: ' + (re.search(r'%s rc=\d+' % sc, run).group(0) if re.search(r'%s rc=\d+' % sc, run) else 'missing')))
    step('scenario %s: the key script ran to the end' % sc, 'keys: quit' in log)
    if sc == 'a':
        step('scenario a: English chosen and saved', 'lang 1' in read('a_pos.txt'))
    if sc == 'b':
        net = [l for l in log.splitlines() if re.search(r'pan: up|podcasts: search|pan_up failed|fetch failed', l)]
        for l in net:
            print('   ' + l)
        step('scenario b: podcast search over the phone', any('podcasts: search' in l and 'results' in l for l in net), 'turn on Bluetooth tethering for this one' if not net else '')

# screenshots -> PNG, one sheet per scenario
try:
    from PIL import Image
    for sc in ('a', 'b'):
        pngs = []
        for f in sorted(glob.glob(os.path.join(SRC, sc + '*.rgb565'))):
            b = open(f, 'rb').read()
            px = bytes(x for i in range(0, len(b), 2) for x in (
                ((b[i + 1] >> 3) * 255 // 31), ((((b[i + 1] & 7) << 3) | (b[i] >> 5)) * 255 // 63), ((b[i] & 31) * 255 // 31)))
            im = Image.frombytes('RGB', (240, 320), px).resize((480, 640), Image.NEAREST)
            png = os.path.join(OUT, os.path.basename(f)[:-7] + '.png')
            im.save(png); pngs.append(png)
        if pngs:
            cols = 5
            sheet = Image.new('RGB', (cols * 250, ((len(pngs) + cols - 1) // cols) * 350), (40, 40, 40))
            for i, q in enumerate(pngs):
                sheet.paste(Image.open(q).resize((240, 320)), ((i % cols) * 250 + 5, (i // cols) * 350 + 5))
            sheet.save(os.path.join(OUT, 'screens_%s.png' % sc))
        step('scenario %s: screenshots' % sc, len(pngs) > 5, '%d' % len(pngs))
except ImportError:
    print('(no Pillow: screenshots left as raw RGB565)')
for f in glob.glob(os.path.join(SRC, '*.txt')):
    shutil.copy(f, OUT)

bad = [n for n, ok in results if not ok]
print('\n%d/%d passed' % (len(results) - len(bad), len(results)) + ('' if not bad else '; FAILED: ' + ', '.join(bad)))
print('screens and logs: ' + OUT)
sys.exit(1 if bad else 0)
