#include "gfx.h"
#include "font8x16.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

uint16_t *fb;
int gfx_fd = -1;
static uint16_t *screen, *shown;	/* hardware frame buffer, copy of what is on it */

int gfx_open(void)
{
	fb = calloc(W * H, 2);
	shown = calloc(W * H, 2);
	if (!fb || !shown) return -1;
#ifndef _WIN32
	int fd = open("/dev/fb/0", O_RDWR);
	if (fd < 0) return -1;
	gfx_fd = fd;
	screen = mmap(0, W * H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (screen == MAP_FAILED) { screen = 0; return -1; }
#endif
	return 0;
}

/* show only a rectangle of the back buffer (animations that change a small part of the screen) */
void gfx_flip_rect(int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	for (int j = y; j < y + h; j++) {
		if (screen) memcpy(screen + j * W + x, fb + j * W + x, w * 2);
		memcpy(shown + j * W + x, fb + j * W + x, w * 2);
	}
}

/* only the rows that changed since the last flip reach the (uncached) frame buffer: a list step or a clock tick
 * touches a few dozen rows instead of 320. gfx_invalidate() after something else drew on the screen (a game). */
static int shown_valid;
/* one row equal? 32-bit words, 8 per step, OR of the XORs: the device glibc's memcmp was slower than copying the
 * whole frame (measured: 9.6 ms to find "nothing changed" vs 6.3 ms for a full copy at 248 MHz) */
/* copy n bytes (a multiple of 32, 4-aligned) 8 words at a time: the compiler turns it into LDM/STM bursts */
void gfx_copy32(void *dst, const void *src, int n)
{
	uint32_t *d = dst; const uint32_t *s = src;
	for (int i = 0; i < n / 4; i += 8) {
		uint32_t a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3], a4 = s[i + 4], a5 = s[i + 5], a6 = s[i + 6], a7 = s[i + 7];
		d[i] = a0; d[i + 1] = a1; d[i + 2] = a2; d[i + 3] = a3; d[i + 4] = a4; d[i + 5] = a5; d[i + 6] = a6; d[i + 7] = a7;
	}
}

static int row_same(const uint32_t *a, const uint32_t *b)
{
	for (int i = 0; i < W / 2; i += 8) {
		uint32_t d = (a[i] ^ b[i]) | (a[i + 1] ^ b[i + 1]) | (a[i + 2] ^ b[i + 2]) | (a[i + 3] ^ b[i + 3])
			| (a[i + 4] ^ b[i + 4]) | (a[i + 5] ^ b[i + 5]) | (a[i + 6] ^ b[i + 6]) | (a[i + 7] ^ b[i + 7]);
		if (d) return 0;
	}
	return 1;
}
void gfx_invalidate(void) { shown_valid = 0; }
void gfx_flip(void)
{
	if (!shown_valid) {
		if (screen) gfx_copy32(screen, fb, W * H * 2);
		gfx_copy32(shown, fb, W * H * 2);
		shown_valid = 1;
		return;
	}
	for (int j = 0; j < H; j++) {
		const uint16_t *a = fb + j * W;
		uint16_t *b = shown + j * W;
		if (row_same((const uint32_t *)a, (const uint32_t *)b)) continue;
		if (screen) gfx_copy32(screen + j * W, a, W * 2);	/* (glibc's memcpy: ~2x slower here, measured) */
		gfx_copy32(b, a, W * 2);
	}
}

/* the new frame (fb) pushes the old one (shown) out horizontally, eased, in 6 steps; rows above SLIDE_Y0 and from
 * SLIDE_Y1 down (hbplayer's title bar and bottom status bar) are replaced in place instead */
#define SLIDE_Y0 28
#define SLIDE_Y1 288
void gfx_slide(int dir, void (*between)(void))
{
	static const int steps[] = { 95, 170, 222, 240 };	/* 4 eased steps (was 6): ~40 ms instead of ~85 */
	if (screen)
		for (int k = 0; k < 4; k++) {
			int off = steps[k];	/* how far the new frame has come in */
			for (int y = SLIDE_Y0; y < SLIDE_Y1; y++) {	/* the title bar and the bottom status bar stay put */
				uint16_t *d = screen + y * W, *o = shown + y * W, *n = fb + y * W;
				if (dir > 0) { memcpy(d, o + off, (W - off) * 2); memcpy(d + W - off, n, off * 2); }
				else { memcpy(d, n + W - off, off * 2); memcpy(d + off, o, (W - off) * 2); }
			}
			memcpy(screen, fb, SLIDE_Y0 * W * 2);
			memcpy(screen + SLIDE_Y1 * W, fb + SLIDE_Y1 * W, (H - SLIDE_Y1) * W * 2);
			if (between) between();
		}
	gfx_invalidate();	/* the screen holds a composed frame: copy everything once */
	gfx_flip();
}

/* the LCD controller's automatic refresh (QFB_SET_LCD_DISABLE): off while the screen is dark, like the Sony UI.
 * Otherwise it keeps reading the whole frame from SDRAM ~30 times a second (~4.5 MB/s, 30 interrupts/s) for a screen
 * nobody sees. Measured 2026-09-29: off -> 0.6 lcd irq/s, back on -> the panel updates again at once. */
static int lcd_off;
void gfx_lcd(int on)
{
#ifndef _WIN32
	if (gfx_fd < 0 || lcd_off == !on) return;
	int v = !on;
	if (ioctl(gfx_fd, 0x40044E35, &v) == 0) lcd_off = !on;
	if (on) gfx_invalidate();	/* the next flip rewrites the whole frame */
#else
	(void)on;
#endif
}

/* Backlight level 0..100. The A829 kernel takes it the way Sony's gemini sets it: ioctl 'N' 15 with
 * { level 0..100, steps 1..100, period >= 10 } (the old 'N' 8 with a bare int only switches on/off);
 * fallbacks: 'N' 12 { 0..5, flag } and the old call. */
int gfx_backlight(int level)
{
#ifndef _WIN32
	if (level < 0) level = 0;
	if (level > 100) level = 100;
	int set[3] = { level, 1, 10 };
	if (ioctl(gfx_fd, 0x40044E0F, set) == 0) return 0;
	int bl[2] = { (level * 5 + 50) / 100, 0 };
	if (level > 0 && bl[0] == 0) bl[0] = 1;
	if (ioctl(gfx_fd, 0x40044E0C, bl) == 0) return 0;
	return ioctl(gfx_fd, 0x40044E08, &level);
#else
	(void)level; return 0;
#endif
}

/* ---------- resources (read-only, mapped from the firmware's files) ---------- */

static const unsigned char *texdata, *fontdata, *animdata;
static long texlen, fontlen, animlen;

static const unsigned char *map_file(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	const unsigned char *p = 0;
#ifndef _WIN32
	p = mmap(0, *len, PROT_READ, MAP_PRIVATE, fileno(f), 0);
	if (p == MAP_FAILED) p = 0;
#else
	unsigned char *b = malloc(*len);
	if (b && fread(b, 1, *len, f) == (size_t)*len) p = b; else free(b);
#endif
	fclose(f);
	return p;
}

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* Sony font, sub-font 0 (Latin / Latin-1 / Cyrillic, 22 px, 1 bit per pixel) */
static const unsigned char *fsub;
static unsigned ffirst, flast, fmtab, fbmp;

int gfx_load_res(const char *resdir)
{
	char p[512];
	snprintf(p, sizeof p, "%s/dui_tex/dui_tex.dpk", resdir);
	texdata = map_file(p, &texlen);
	snprintf(p, sizeof p, "%s/dui_tex/ICX1115_home_anim.dpk", resdir);
	animdata = map_file(p, &animlen);
	snprintf(p, sizeof p, "%s/font/dui_font.dpk", resdir);
	fontdata = map_file(p, &fontlen);
	if (fontdata && fontlen > 200) {
		fsub = fontdata + rd32(fontdata + 16 + 8);
		ffirst = rd16(fsub + 12); flast = rd16(fsub + 14);
		fmtab = rd32(fsub + 0x18); fbmp = rd32(fsub + 0x1c);
	}
	return (texdata ? 1 : 0) | (fontdata ? 2 : 0);
}

struct img gfx_tex(int group, int item)
{
	struct img im = { 0, 0, 0 };
	if (!texdata || group < 0 || (unsigned)group >= rd32(texdata + 4)) return im;
	unsigned go = rd32(texdata + 16 + group * 16 + 8);
	if (go + 16 > (unsigned)texlen || item < 0 || (unsigned)item >= rd32(texdata + go + 4)) return im;
	unsigned io = go + rd32(texdata + go + 16 + item * 16 + 8);
	if (io + 16 > (unsigned)texlen) return im;
	im.w = rd16(texdata + io); im.h = rd16(texdata + io + 2);
	if (io + 16 + (unsigned)(im.w * im.h * 2) > (unsigned)texlen) { im.w = im.h = 0; return im; }
	im.px = (const uint16_t *)(texdata + io + 16);
	return im;
}

/* Sony's bold condensed digits (the player's time and "1/86" counter): group 31, 8-bit masks (format 4, one byte
 * per pixel, 0..255 coverage); digit d = item 2*d (odd items are the same glyph rotated), ':' 22, '.' 26, '-' 30,
 * '/' 36. Painted in colour c blended over what is there. Returns the width; draws only if draw. */
static int digit_item(int ch)
{
	if (ch >= '0' && ch <= '9') return (ch - '0') * 2;
	return ch == ':' ? 22 : ch == '.' ? 26 : ch == '-' ? 30 : ch == '/' ? 36 : -1;
}

static int sony_digits(int x, int y, const char *s, uint16_t c, int draw)
{
	int x0 = x;
	for (; *s; s++) {
		int it = digit_item((unsigned char)*s);
		const unsigned char *px = 0; int w = 0, h = 0;
		if (it >= 0 && texdata && 31 < rd32(texdata + 4)) {
			unsigned go = rd32(texdata + 16 + 31 * 16 + 8);
			if (go + 16 <= (unsigned)texlen && (unsigned)it < rd32(texdata + go + 4)) {
				unsigned io = go + rd32(texdata + go + 16 + it * 16 + 8);
				if (io + 16 <= (unsigned)texlen && rd16(texdata + io + 4) == 4) {
					w = rd16(texdata + io); h = rd16(texdata + io + 2);
					if (io + 16 + (unsigned)(w * h) <= (unsigned)texlen) px = texdata + io + 16; else w = 0;
				}
			}
		}
		if (!px) { char one[2] = { *s, 0 }; x += draw ? gfx_text(x, y - 2, one, c, 1) - x : gfx_text_w(one, 1); continue; }
		if (draw)
			for (int j = 0; j < h; j++) {
				int yy = y + j;
				if (yy < 0 || yy >= H) continue;
				for (int i = 0; i < w; i++) {
					int a = px[j * w + i], xx = x + i;
					if (!a || xx < 0 || xx >= W) continue;
					uint16_t *d = fb + yy * W + xx;
					if (a >= 250) { *d = c; continue; }
					int r = ((*d >> 11) * (255 - a) + (c >> 11) * a) / 255, g = (((*d >> 5) & 63) * (255 - a) + ((c >> 5) & 63) * a) / 255, b = ((*d & 31) * (255 - a) + (c & 31) * a) / 255;
					*d = (uint16_t)(r << 11 | g << 5 | b);
				}
			}
		x += w;
	}
	return x - x0;
}

int gfx_digits(int x, int y, const char *s, uint16_t c) { return x + sony_digits(x, y, s, c, 1); }
int gfx_digits_w(const char *s) { return sony_digits(0, 0, s, 0, 0); }

int gfx_tex_n(int group)
{
	if (!texdata || group < 0 || (unsigned)group >= rd32(texdata + 4)) return 0;
	unsigned go = rd32(texdata + 16 + group * 16 + 8);
	return go + 16 > (unsigned)texlen ? 0 : (int)rd32(texdata + go + 4);
}

/* the resource pages every screen needs, for a warm-up pass (see hbplayer/warm.c): the whole home animation, the
 * font's sub-font 0 and the texture pack's index (each texture's pixels: scr_warm) */
void gfx_warm_res(void (*touch)(const void *p, long n))
{
	if (animdata) touch(animdata, animlen);
	if (fsub && fontdata) { long o = fsub - fontdata, n = fontlen - o; touch(fsub, n < 65536 ? n : 65536); }
	if (texdata) { unsigned ng = rd32(texdata + 4); long n = 16 + (long)ng * 16; touch(texdata, n < texlen ? n : texlen); }
}

struct img gfx_anim(int item)
{
	struct img im = { 0, 0, 0 };
	if (!animdata || item < 0 || (unsigned)item >= rd32(animdata + 4)) return im;
	unsigned io = rd32(animdata + 16 + item * 16 + 8);
	if (io + 16 > (unsigned)animlen) return im;
	im.w = rd16(animdata + io); im.h = rd16(animdata + io + 2);
	if (io + 16 + (unsigned)(im.w * im.h * 2) > (unsigned)animlen) { im.w = im.h = 0; return im; }
	im.px = (const uint16_t *)(animdata + io + 16);
	return im;
}

/* ---------- primitives ---------- */

void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	if (w <= 0 || h <= 0) return;
	uint32_t c2 = c | (uint32_t)c << 16;
	for (int j = y; j < y + h; j++) {
		uint16_t *d = fb + j * W + x;
		int n = w;
		if (((uintptr_t)d & 2) && n) { *d++ = c; n--; }	/* align to 32 bits, then two pixels per store */
		uint32_t *d2 = (uint32_t *)d;
		for (int i = 0; i < n / 2; i++) d2[i] = c2;
		if (n & 1) d[n - 1] = c;
	}
}

void gfx_vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
	int r0 = top >> 11, g0 = (top >> 5) & 63, b0 = top & 31, r1 = bottom >> 11, g1 = (bottom >> 5) & 63, b1 = bottom & 31;
	for (int j = 0; j < h; j++) {
		int r = r0 + (r1 - r0) * j / (h > 1 ? h - 1 : 1), g = g0 + (g1 - g0) * j / (h > 1 ? h - 1 : 1), b = b0 + (b1 - b0) * j / (h > 1 ? h - 1 : 1);
		gfx_fill(x, y + j, w, 1, (uint16_t)(r << 11 | g << 5 | b));
	}
}

void gfx_frame(int x, int y, int w, int h, uint16_t c)
{
	gfx_fill(x, y, w, 1, c); gfx_fill(x, y + h - 1, w, 1, c);
	gfx_fill(x, y, 1, h, c); gfx_fill(x + w - 1, y, 1, h, c);
}

static void blit(struct img im, int x, int y, int key)
{
	if (!im.w) return;
	for (int j = 0; j < im.h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= H) continue;
		const uint16_t *s = im.px + j * im.w;
		for (int i = 0; i < im.w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= W || (key && !s[i])) continue;
			fb[yy * W + xx] = s[i];
		}
	}
}
void gfx_blit(struct img im, int x, int y) { blit(im, x, y, 0); }
void gfx_blit_key(struct img im, int x, int y) { blit(im, x, y, 1); }

struct img gfx_scaled(struct img s, int w, int h, uint16_t *buf)
{
	struct img d = { w, h, buf };
	if (!s.w) { d.w = 0; return d; }
	/* nearest neighbour with 16.16 steps: no division per pixel (the ARM926 has no divide instruction) */
	unsigned sx = ((unsigned)s.w << 16) / w, sy = ((unsigned)s.h << 16) / h, fy = 0;
	for (int j = 0; j < h; j++, fy += sy) {
		const uint16_t *row = s.px + (fy >> 16) * s.w;
		unsigned fx = 0;
		for (int i = 0; i < w; i++, fx += sx) buf[j * w + i] = row[fx >> 16];
	}
	return d;
}

/* ---------- text ---------- */

static unsigned next_cp(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	unsigned c = *s++;
	if (c >= 0xE0 && s[0] && s[1]) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
	else if (c >= 0xC0 && s[0]) { c = (c & 31) << 6 | (s[0] & 63); s++; }
	*ps = (const char *)s;
	return c;
}

/* Sony glyph: returns advance; draws if fb != 0 */
static int sony_glyph(unsigned cp, int x, int y, uint16_t col, int scale, int draw)
{
	if (cp < ffirst || cp > flast) cp = '?';
	unsigned g = rd16(fsub + 0x60 + (cp - ffirst) * 2);
	if (!g) g = rd16(fsub + 0x60 + ('?' - ffirst) * 2);
	const unsigned char *m = fsub + fmtab + g * 8;
	int adv = rd16(m), rb = rd16(m + 2) >> 11, ht = rd16(m + 4);
	const unsigned char *bm = fsub + fbmp + rd16(m + 6);
	if (draw && scale == 1 && x >= 0 && x + adv <= W && y >= 0 && y + ht <= H) {	/* common case: fully on screen */
		for (int r = 0; r < ht; r++) {
			const unsigned char *row = bm + r * rb;
			uint16_t *d = fb + (y + r) * W + x;
			for (int i = 0; i < adv; i += 8) {
				unsigned b = row[i >> 3];
				if (!b) continue;	/* 8 empty pixels */
				for (int k = 0; k < 8 && i + k < adv; k++) if (b & (0x80 >> k)) d[i + k] = col;
			}
		}
	} else if (draw)
		for (int r = 0; r < ht; r++)
			for (int i = 0; i < adv; i++)
				if (bm[r * rb + (i >> 3)] & (0x80 >> (i & 7))) {
					if (scale == 1) { int xx = x + i, yy = y + r; if (xx >= 0 && xx < W && yy >= 0 && yy < H) fb[yy * W + xx] = col; }
					else gfx_fill(x + i * scale, y + r * scale, scale, scale, col);
				}
	return adv * scale;
}

static int fallback_glyph(unsigned cp, int x, int y, uint16_t col, int scale, int draw)
{
	int gi = cp >= 32 && cp < 127 ? (int)cp - 32 : -1;
	if (gi < 0) { const char *p = cp < 256 && cp ? strchr(font_extra, (int)cp) : 0; gi = p ? 95 + (int)(p - font_extra) : '?' - 32; }
	if (draw)
		for (int r = 0; r < 16; r++)
			for (int b = 0; b < 8; b++)
				if (font8x16[gi][r] & (0x80 >> b)) gfx_fill(x + b * scale, y + (r + 3) * scale, scale, scale, col);
	return 8 * scale;
}

static int glyph(unsigned cp, int x, int y, uint16_t col, int scale, int draw)
{
	return fsub ? sony_glyph(cp, x, y, col, scale, draw) : fallback_glyph(cp, x, y, col, scale, draw);
}

static int text_raw(int x, int y, const char *s, uint16_t c, int scale)
{
	while (*s) x += glyph(next_cp(&s), x, y, c, scale, 1);
	return x;
}

static int text_w_raw(const char *s, int scale)
{
	int w = 0;
	while (*s) w += glyph(next_cp(&s), 0, 0, 0, scale, 0);
	return w;
}

static void clip_raw(int x, int y, const char *s, uint16_t c, int maxw)
{
	if (text_w_raw(s, 1) <= maxw) { text_raw(x, y, s, c, 1); return; }
	int dots = text_w_raw("...", 1), cx = x;
	while (*s) {
		const char *t = s;
		int adv = glyph(next_cp(&t), 0, 0, 0, 1, 0);
		if (cx + adv - x > maxw - dots) break;
		cx += glyph(next_cp(&s), cx, y, c, 1, 1);
	}
	text_raw(cx, y, "...", c, 1);
}


static int wrap_raw(int x, int y, const char *s, uint16_t c, int maxw, int lines, int lineh)
{
	char line[256];
	int l = 0;
	while (*s && l < lines) {
		while (*s == ' ') s++;
		int n = 0, lastsp = -1;
		/* grow the line word by word */
		while (s[n]) {
			int k = n;
			while (s[k] && s[k] != ' ') k++;
			if (k >= (int)sizeof line - 1) break;
			memcpy(line, s, k); line[k] = 0;
			if (text_w_raw(line, 1) > maxw && lastsp >= 0) break;
			lastsp = k; n = k;
			if (!s[n]) break;
			n++;
			if (text_w_raw(line, 1) > maxw) break;
		}
		int len = lastsp >= 0 ? lastsp : n;
		if (len <= 0) len = (int)strlen(s);
		if (len > (int)sizeof line - 1) len = sizeof line - 1;
		memcpy(line, s, len); line[len] = 0;
		if (l == lines - 1 && s[len]) clip_raw(x, y + l * lineh, s, c, maxw);
		else clip_raw(x, y + l * lineh, line, c, maxw);
		s += len;
		l++;
	}
	return l;
}

/* the player's translation (hbplayer i18n.c): every text drawn or measured passes through it once, here at the
 * public entry points (the raw ones above never translate again: a wrapped fragment must stay as it is) */
const char *(*gfx_tr)(const char *s);
#define TR(s) (gfx_tr ? gfx_tr(s) : (s))
int gfx_text(int x, int y, const char *s, uint16_t c, int scale) { return text_raw(x, y, TR(s), c, scale); }
int gfx_text_w(const char *s, int scale) { return text_w_raw(TR(s), scale); }
void gfx_text_clip(int x, int y, const char *s, uint16_t c, int maxw) { clip_raw(x, y, TR(s), c, maxw); }
void gfx_text_center(int cx, int y, const char *s, uint16_t c, int scale) { s = TR(s); text_raw(cx - text_w_raw(s, scale) / 2, y, s, c, scale); }
int gfx_text_wrap(int x, int y, const char *s, uint16_t c, int maxw, int lines, int lineh) { return wrap_raw(x, y, TR(s), c, maxw, lines, lineh); }
