# Builds and runs the PC screenshot tool, converts the raw RGB565 frames to PNG (2x) and a contact sheet.
import glob, os, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ZIG = os.environ.get('ZIG', 'zig')
if len(sys.argv) < 2:
    sys.exit('usage: shots.py <path to /usr/local/share/gemini/res extracted from the firmware>')
RES = sys.argv[1]
OUT = os.path.join(HERE, 'shots')
os.makedirs(OUT, exist_ok=True)
exe = os.path.join(HERE, 'shots.exe')
subprocess.check_call([ZIG, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-o', exe,
                       os.path.join(HERE, 'shots.c'), os.path.join(HERE, 'screens.c'), os.path.join(HERE, 'cover.c'), os.path.join(HERE, 'i18n.c'), os.path.join(HERE, '..', 'third_party', 'tjpgd', 'src', 'tjpgd.c'), os.path.join(HERE, '..', 'common', 'gfx.c')])
subprocess.check_call([exe, RES, OUT] + sys.argv[2:3])
pngs = []
for f in sorted(glob.glob(os.path.join(OUT, '*.rgb565'))):
    d = open(f, 'rb').read()
    im = Image.new('RGB', (240, 320))
    px = im.load()
    for i in range(240 * 320):
        v = d[2 * i] | d[2 * i + 1] << 8
        px[i % 240, i // 240] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    im = im.resize((480, 640), Image.NEAREST)
    png = f[:-7] + '.png'
    im.save(png); pngs.append(png); os.remove(f)
cols = 6
sheet = Image.new('RGB', (cols * 250, ((len(pngs) + cols - 1) // cols) * 330), (40, 40, 40))
for i, p in enumerate(pngs):
    sheet.paste(Image.open(p).resize((240, 320)), ((i % cols) * 250 + 5, (i // cols) * 330 + 5))
sheet.save(os.path.join(OUT, 'todas.png'))
print(len(pngs), 'screens ->', OUT)
