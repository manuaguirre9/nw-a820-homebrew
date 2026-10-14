from PIL import Image, ImageDraw
import glob, os

def sheet(groups, out, W=1300, maxdim=250):
    files = [f for g in groups for f in sorted(glob.glob('tex/g%02d_*.png' % g))]
    ims = []
    for f in files:
        im = Image.open(f).convert('RGB')
        if im.width > maxdim or im.height > maxdim:
            im = im.resize((im.width // 2, im.height // 2))
        ims.append((os.path.basename(f)[:-4], im))
    x = y = rowh = 0
    pos = []
    for n, im in ims:
        w = max(im.width, 70)
        if x + w > W:
            x = 0; y += rowh + 14; rowh = 0
        pos.append((x, y)); x += w + 4; rowh = max(rowh, im.height)
    H = y + rowh + 14
    s = Image.new('RGB', (W, H), (90, 0, 90)); d = ImageDraw.Draw(s)
    for (n, im), p in zip(ims, pos):
        s.paste(im, (p[0], p[1] + 12)); d.text(p, n, fill=(255, 255, 0))
    s.save(out); print(out, H)

sheet([21, 22, 20, 26, 19], 'sheetA.png')
sheet([17, 18, 24, 32, 27, 33, 34], 'sheetB.png')
sheet([23, 25], 'sheetC.png')
