# Generates icons.h: home-menu icons in the style of the Sony NW-A820 UI (80x70 RGB565, black background):
# normal = light-grey glyph; selected = bigger orange glyph with white rim, orange glow and a faint reflection.
from PIL import Image, ImageDraw, ImageFilter, ImageChops
import math

SS = 4  # supersampling


def mask_gamepad(d, s):
    w, h = 64 * s, 34 * s
    x0, y0 = (80 * s - w) // 2, 18 * s
    d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=16 * s, fill=255)
    d.ellipse([x0 - 2 * s, y0 + 10 * s, x0 + 26 * s, y0 + h + 12 * s], fill=255)
    d.ellipse([x0 + w - 26 * s, y0 + 10 * s, x0 + w + 2 * s, y0 + h + 12 * s], fill=255)
    cx, cy = x0 + 17 * s, y0 + 16 * s  # d-pad (cut out)
    d.rectangle([cx - 9 * s, cy - 3 * s, cx + 9 * s, cy + 3 * s], fill=0)
    d.rectangle([cx - 3 * s, cy - 9 * s, cx + 3 * s, cy + 9 * s], fill=0)
    bx, by = x0 + w - 17 * s, y0 + 16 * s
    for dx, dy in ((6, -5), (-6, 5), (7, 6), (-7, -6)):
        pass
    d.ellipse([bx + 2 * s, by - 9 * s, bx + 10 * s, by - 1 * s], fill=0)
    d.ellipse([bx - 10 * s, by + 1 * s, bx - 2 * s, by + 9 * s], fill=0)


def mask_bt(d, s):
    cx, top, bot = 40 * s, 12 * s, 58 * s
    mid = (top + bot) // 2
    lw = 5 * s
    pts_r = [(cx - 12 * s, mid - 12 * s), (cx + 12 * s, mid + 11 * s), (cx, bot), (cx, top), (cx + 12 * s, mid - 11 * s), (cx - 12 * s, mid + 12 * s)]
    d.line(pts_r, fill=255, width=lw, joint='curve')


def mask_power(d, s):
    cx, cy, r = 40 * s, 36 * s, 21 * s
    lw = 6 * s
    d.arc([cx - r, cy - r, cx + r, cy + r], start=-55, end=235, fill=255, width=lw)
    d.line([(cx, cy - r - 4 * s), (cx, cy - 2 * s)], fill=255, width=lw)


def mask_book(d, s):
    cx, y0, y1 = 40 * s, 16 * s, 54 * s
    d.polygon([(cx - 30 * s, y0 + 4 * s), (cx - 2 * s, y0 + 8 * s), (cx - 2 * s, y1), (cx - 30 * s, y1 - 4 * s)], fill=255)
    d.polygon([(cx + 30 * s, y0 + 4 * s), (cx + 2 * s, y0 + 8 * s), (cx + 2 * s, y1), (cx + 30 * s, y1 - 4 * s)], fill=255)
    for k in range(3):
        yy = y0 + (16 + k * 9) * s
        d.line([(cx - 25 * s, yy - 2 * s), (cx - 7 * s, yy + 1 * s)], fill=0, width=2 * s)
        d.line([(cx + 25 * s, yy - 2 * s), (cx + 7 * s, yy + 1 * s)], fill=0, width=2 * s)


def mask_mic(d, s):
    cx = 40 * s
    d.rounded_rectangle([cx - 10 * s, 10 * s, cx + 10 * s, 40 * s], radius=10 * s, fill=255)
    d.arc([cx - 17 * s, 20 * s, cx + 17 * s, 50 * s], start=0, end=180, fill=255, width=4 * s)
    d.line([(cx, 50 * s), (cx, 58 * s)], fill=255, width=4 * s)
    d.line([(cx - 11 * s, 59 * s), (cx + 11 * s, 59 * s)], fill=255, width=4 * s)


def mask_lotus(d, s):
    cx, by = 40 * s, 52 * s
    d.ellipse([cx - 7 * s, 14 * s, cx + 7 * s, by], fill=255)
    for ang in (-38, 38):
        petal = Image.new('L', (80 * s, 70 * s), 0)
        ImageDraw.Draw(petal).ellipse([cx - 7 * s, 16 * s, cx + 7 * s, by], fill=255)
        petal = petal.rotate(ang, center=(cx, by), resample=Image.BICUBIC)
        d.bitmap((0, 0), petal, fill=255)
    for ang in (-72, 72):
        petal = Image.new('L', (80 * s, 70 * s), 0)
        ImageDraw.Draw(petal).ellipse([cx - 6 * s, 22 * s, cx + 6 * s, by], fill=255)
        petal = petal.rotate(ang, center=(cx, by), resample=Image.BICUBIC)
        d.bitmap((0, 0), petal, fill=255)
    d.line([(cx - 26 * s, by + 5 * s), (cx + 26 * s, by + 5 * s)], fill=255, width=3 * s)


def mask_gear(d, s):
    cx, cy = 40 * s, 35 * s
    r_out, r_in, r_hole = 25 * s, 19 * s, 8 * s
    d.ellipse([cx - r_in, cy - r_in, cx + r_in, cy + r_in], fill=255)
    for k in range(8):
        a = k * math.pi / 4
        tooth = Image.new('L', (80 * s, 70 * s), 0)
        ImageDraw.Draw(tooth).rectangle([cx - 5 * s, cy - r_out, cx + 5 * s, cy - r_in + 2 * s], fill=255)
        tooth = tooth.rotate(math.degrees(a), center=(cx, cy), resample=Image.BICUBIC)
        d.bitmap((0, 0), tooth, fill=255)
    d.ellipse([cx - r_hole, cy - r_hole, cx + r_hole, cy + r_hole], fill=0)


def mask_play(d, s):
    cx, cy, r = 40 * s, 35 * s, 24 * s
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=5 * s)
    d.polygon([(cx - 8 * s, cy - 13 * s), (cx - 8 * s, cy + 13 * s), (cx + 14 * s, cy)], fill=255)


def frame(fn, t, W=80, Hh=72):
    """t = 1 selected (big, orange, glow) .. 0 normal (small, grey) - like Sony's 9-frame home animation"""
    s = SS
    m = Image.new('L', (80 * s, 70 * s), 0)
    fn(ImageDraw.Draw(m), s)
    m = m.resize((80, 70), Image.LANCZOS)
    sc = 0.62 + 0.38 * t
    sm = m.resize((max(1, int(80 * sc)), max(1, int(70 * sc))), Image.LANCZOS)
    mm = Image.new('L', (W, Hh), 0)
    mm.paste(sm, ((W - sm.width) // 2, (Hh - sm.height) // 2 - int(4 * t)))
    out = Image.new('RGB', (W, Hh), (0, 0, 0))
    if t > 0:
        glow = mm.filter(ImageFilter.GaussianBlur(2 + 4 * t)).point(lambda v: min(255, int(v * 2.2 * t)))
        out.paste((235, 100, 0), mask=glow)
    grey = (185, 185, 195)
    grad = Image.linear_gradient('L').resize((W, Hh))
    body = Image.merge('RGB', [Image.new('L', (W, Hh), 255), grad.point(lambda v: 170 - v // 3), grad.point(lambda v: 60 - v // 5)])
    greyimg = Image.new('RGB', (W, Hh), grey)
    col = Image.blend(greyimg, body, t)
    out.paste(col, mask=mm)
    if t > 0.3:
        rim = ImageChops.subtract(mm, mm.filter(ImageFilter.MinFilter(3))).point(lambda v: int(v * (t - 0.3) / 0.7))
        out.paste((255, 255, 255), mask=rim)
    return out


def render(fn, selected):
    s = SS
    m = Image.new('L', (80 * s, 70 * s), 0)
    fn(ImageDraw.Draw(m), s)
    m = m.resize((80, 70), Image.LANCZOS)
    scale = 1.0 if selected else 0.82
    if scale != 1.0:
        sm = m.resize((int(80 * scale), int(70 * scale)), Image.LANCZOS)
        m = Image.new('L', (80, 70), 0); m.paste(sm, ((80 - sm.width) // 2, (70 - sm.height) // 2 - 3))
    out = Image.new('RGB', (80, 70), (0, 0, 0))
    if not selected:
        out.paste((200, 200, 200), mask=m)
        return out
    glow = m.filter(ImageFilter.GaussianBlur(5))
    glow = glow.point(lambda v: min(255, int(v * 2.2)))
    out.paste((235, 100, 0), mask=glow)
    refl = m.transpose(Image.FLIP_TOP_BOTTOM)
    bbox = m.getbbox() or (0, 0, 80, 70)
    shift = 2 * bbox[3] - 70 + 2
    rimg = Image.new('L', (80, 70), 0); rimg.paste(refl, (0, shift))
    fade = Image.linear_gradient('L').resize((80, 70)).point(lambda v: max(0, 70 - v))  # fades downward
    rimg = ImageChops.multiply(rimg, Image.new('L', (80, 70), 255).point(lambda v: 0))
    body = Image.new('RGB', (80, 70), (255, 140, 20))
    grad = Image.linear_gradient('L').resize((80, 70))
    body2 = Image.merge('RGB', [Image.new('L', (80, 70), 255), grad.point(lambda v: 170 - v // 3), grad.point(lambda v: 60 - v // 5)])
    out.paste(body2, mask=m)
    rim = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(3)))
    out.paste((255, 255, 255), mask=rim)
    return out


def rgb565(im):
    px = im.load()
    return [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for y in range(im.height) for x in range(im.width) for r, g, b in [px[x, y]]]


icons = [('gamepad', mask_gamepad), ('bt', mask_bt), ('power', mask_power), ('book', mask_book), ('mic', mask_mic), ('lotus', mask_lotus), ('play', mask_play), ('gear', mask_gear)]
lines = ['/* generated by mkicons.py: 80x70 RGB565 home icons, [n][0] normal, [n][1] selected */',
         'enum { ICON_GAMEPAD, ICON_BT, ICON_POWER, ICON_BOOK, ICON_MIC, ICON_LOTUS, ICON_PLAY, ICON_GEAR, ICON_COUNT };',
         'static const unsigned short home_icons[ICON_COUNT][2][80 * 70] = {']
sheet = Image.new('RGB', (80 * len(icons), 140))
for i, (name, fn) in enumerate(icons):
    a, b = render(fn, False), render(fn, True)
    sheet.paste(a, (i * 80, 0)); sheet.paste(b, (i * 80, 70))
    lines.append('\t{ /* %s */' % name)
    for im in (a, b):
        v = rgb565(im)
        lines.append('\t\t{' + ','.join(str(x) for x in v) + '},')
    lines.append('\t},')
lines.append('};')
lines.append('/* 9-frame selection animation, 80x72: [n][0] fully selected .. [n][8] normal */')
lines.append('static const unsigned short home_anim[ICON_COUNT][9][80 * 72] = {')
asheet = Image.new('RGB', (80 * 9, 72 * len(icons)))
for i, (name, fn) in enumerate(icons):
    lines.append('	{ /* %s */' % name)
    for k in range(9):
        im = frame(fn, 1 - k / 8)
        asheet.paste(im, (k * 80, i * 72))
        lines.append('		{' + ','.join(str(x) for x in rgb565(im)) + '},')
    lines.append('	},')
lines.append('};')
asheet.save('anim_preview.png')
open('icons.h', 'w').write('\n'.join(lines) + '\n')
sheet.resize((sheet.width * 2, 280), Image.NEAREST).save('icons_preview.png')
print('ok')
