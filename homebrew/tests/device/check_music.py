"""Reads the music speed test (stage_music.py) from H:\\HB\\out\\test: the timing lines of each step, checks, and the
screenshots as PNGs + a contact sheet in out\\device_music\\.  python check_music.py [DRIVE]"""
import glob, os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = (sys.argv[1] if len(sys.argv) > 1 else 'H:').rstrip('\\/') + os.sep
SRC = os.path.join(DRIVE, 'HB', 'out', 'test')
OUT = os.path.join(os.path.dirname(HERE), 'out', 'device_music')
results = []
sys.stdout.reconfigure(encoding='utf-8')


def step(name, ok, detail=''):
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + (': ' + detail if detail else ''))


def read(name):
    p = os.path.join(SRC, name)
    return open(p, encoding='utf-8', errors='replace').read() if os.path.exists(p) else ''


if not os.path.isdir(SRC):
    sys.exit('no %s: did the Walkman boot with the test staged (stage_music.py)?' % SRC)
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
run, log = read('run.txt'), read('m_log.txt')
print(run.strip())
step('the test run finished', 'end ' in run)
step('no crash', bool(log) and 'FATAL' not in log and 'm rc=0' in run, re.search(r'm rc=\d+', run).group(0) if re.search(r'm rc=\d+', run) else 'no rc')
step('the key script ran to the end', 'keys: quit' in log)

# the timing lines, under the step they belong to
TIMING = re.compile(r'warm:|music:|eng: start took|eng: play|ui: new screen|ui: player screen|art:|albums:|dsp: direct mode released|dsp: helper|pos save|FATAL|test:')
print('\n--- timeline')
for l in log.splitlines():
    if TIMING.search(l):
        l = re.sub(r'^\[\d+\] ', '', l)
        l = l.replace('/contents/Music/', '')
        print(('\n' if 'keys: test:' in l else '   ') + l)

starts = [int(x) for x in re.findall(r'eng: start took (\d+) ms', log)]
screens = [int(x) for x in re.findall(r'ui: new screen (\d+) ms', log)]
arts = [int(x) for x in re.findall(r'art: decoded in (\d+) ms', log)]
print()
step('picks from a list: no start over 250 ms', all(s <= 250 for s in starts), 'slow starts: %s' % starts if starts else 'none logged (all under 100 ms)')
step('MP3 -> FLAC: the DSP release did not block', 'without waiting' in log or 'direct mode released (' not in log,
     '; '.join(re.findall(r'direct mode released \([^)]*\)', log)) or 'no release logged')
step('covers of skipped tracks: none decoded over 150 ms', all(a <= 150 for a in arts), 'decodes: %s ms' % arts if arts else 'no decode (all from the cache)')
m = re.search(r'warm: album covers done in ([\d.]+) s: (.*)', log)
step('background covers pass finished during the test', bool(m), m.group(0) if m else 'still going when the script ended (killed)')
m = re.search(r'music: \d+ songs from the cache the start-up check left.*', log)
step('library taken from the start-up check (no stat per file)', bool(m), m.group(0) if m else '')

try:
    from PIL import Image
    pngs = []
    for f in sorted(glob.glob(os.path.join(SRC, 'm*.rgb565'))):
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
        sheet.save(os.path.join(OUT, 'screens_m.png'))
    step('screenshots', len(pngs) >= 15, '%d' % len(pngs))
except ImportError:
    print('(no Pillow: screenshots left as raw RGB565)')
for f in glob.glob(os.path.join(SRC, '*.txt')):
    shutil.copy(f, OUT)

bad = [n for n, ok in results if not ok]
print('\n%d/%d passed' % (len(results) - len(bad), len(results)) + ('' if not bad else '; FAILED: ' + ', '.join(bad)))
print('screens and logs: ' + OUT)
sys.exit(1 if bad else 0)
