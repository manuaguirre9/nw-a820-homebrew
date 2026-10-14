/* screens.c: every screen of the homebrew player, drawn with Sony's own UI textures and font, laid out like the
 * NW-A820 series UI: silver title bar with the centered title on top, dark blue-grey background, and at the bottom
 * the grey status bar (play state, what is playing, time, battery, our Hold / Bluetooth / download indicators).
 * Texture ids are (group, item) in /usr/local/share/gemini/res/dui_tex/dui_tex.dpk (JP 1.02 firmware). */
#include "screens.h"
int dl_active(void);
int dl_percent(void);
#include "../common/icons.h"
#include <stdio.h>
#include <string.h>

struct ui_status ui_st = { -1, 0, 0, 0, 0, 0, 0 };

#define T_BARSEL 14, 4		/* 240x28 orange selection bar */
#define T_BARSEL_S 14, 5	/* 232x28 (lists with a scrollbar) */
#define T_BOTTOM 23, 14		/* 240x32 status bar; the 23/x status icons are cut from it at y = 5 */
#define T_BT 10, 1		/* 20x22 Bluetooth */
#define T_HOLD_S 19, 6		/* 18x18 "H" */
#define T_HOLD_POP 18, 7	/* 100x50 "Hold" */
#define T_S_PLAY 23, 34		/* 24x24 green play (status bar) */
#define T_S_PAUSE 23, 29	/* 24x24 pause (status bar) */
#define T_S_NOTE 23, 28		/* 24x24 note (status bar) */
#define T_S_PERSON 23, 1	/* 24x24 person (status bar) */
#define T_NOTE 25, 50		/* 18x18 note (white on black) */
#define T_FOLDER 25, 52		/* 18x18 folder */
#define T_PERSON 25, 11		/* 18x18 artist */
#define T_DISC 25, 9		/* 18x18 album */
#define T_GENRE 25, 13		/* 18x18 double note in a box: genre (Sony's) */
#define T_YEAR 25, 15		/* 18x18 calendar grid: release year (Sony's) */
#define T_CHAP 25, 57		/* 18x18 page with a play mark: chapters */
#define T_PLAYBOX 25, 55	/* 18x18 play mark in a box (selected row) */
#define T_ARROW 19, 2		/* 18x18 right arrow ("resume here") */
#define T_CLOCK 25, 46		/* 18x18 clock */
#define T_PROG 29, 1		/* 236x8 progress fill */
#define T_PROG_BG 29, 3		/* 236x8 progress track */
#define T_ART 15, 19		/* 200x200 music placeholder */
#define T_SPLASH 17, 19		/* 240x320 WALKMAN logo */
#define T_POWEROFF 17, 7	/* 240x320 POWER OFF */
#define T_RING 17, 17		/* 240x260 blue ring */

const char *const home_labels[HOME_SLOTS] = { "Modo foco", "Audiolibros", "Podcasts", "Reproduciendo", "Música", "Juegos", "Bluetooth", "Apagar", "Ajustes" };	/* Música in the centre, like Sony */
/* icon per slot: >= 100 -> Sony's own animation (first frame index - 100), else our ICON_*, -1 empty */
static const int home_src[HOME_SLOTS] = { ICON_LOTUS, ICON_BOOK, 100 + 45, 100 + 27, 100 + 54, ICON_GAMEPAD, 100 + 72, ICON_POWER, 100 + 9 };
static const int home_fallback[HOME_SLOTS] = { ICON_LOTUS, ICON_BOOK, ICON_MIC, ICON_PLAY, ICON_PLAY, ICON_GAMEPAD, ICON_BT, ICON_POWER, ICON_GEAR };

/* ---------- colours and blending ---------- */

static uint16_t mix(int r0, int g0, int b0, int r1, int g1, int b1, int t, int n)	/* t/n of the way from 0 to 1 */
{
	if (n <= 0) n = 1;
	if (t < 0) t = 0;
	if (t > n) t = n;
	return RGB(r0 + (r1 - r0) * t / n, g0 + (g1 - g0) * t / n, b0 + (b1 - b0) * t / n);
}

/* background colour of row y, taken from Sony's own assets: pure black (menus, and the home frames are composited on
 * black), and on home the deep blue band of Sony's backgrounds (the "POWER OFF"/splash textures: black down to ~y 213,
 * then pure blue with a touch of red, ~(8,0,41) at y 244 and ~(16,0,100) near the status bar). */
static uint16_t bg_row(int y, int home)
{
	static uint16_t t[2][H];
	static int made;
	if (!made) {
		for (int j = 0; j < H; j++) {
			int b = j > 213 ? (j - 213) * 145 / 100 : 0;
			if (b > 150) b = 150;
			t[0][j] = 0;
			t[1][j] = RGB(b / 6, 0, b);
		}
		made = 1;
	}
	return t[home ? 1 : 0][y < 0 ? 0 : y >= H ? H - 1 : y];
}

static void bg_fill(int x, int y, int w, int h, int home)
{
	for (int j = y; j < y + h; j++) gfx_fill(x, j, w, 1, bg_row(j, home));
}

/* "lighten" blend: per channel max. Sony's home frames (and the ring) are glows pre-composited on black; drawn
 * like this they sit on our background with their soft edges intact. */
static void blit_max(struct img im, int x, int y)
{
	if (!im.w) return;
	for (int j = 0; j < im.h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= H) continue;
		const uint16_t *s = im.px + j * im.w;
		uint16_t *d = fb + yy * W;
		for (int i = 0; i < im.w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= W) continue;
			uint16_t a = s[i], b = d[xx];
			int r = (a >> 11) > (b >> 11) ? (a >> 11) : (b >> 11);
			int g = ((a >> 5) & 63) > ((b >> 5) & 63) ? ((a >> 5) & 63) : ((b >> 5) & 63);
			int bl = (a & 31) > (b & 31) ? (a & 31) : (b & 31);
			d[xx] = (uint16_t)(r << 11 | g << 5 | bl);
		}
	}
}

/* like blit_max, for Sony's home frames: the frames Sony drew for its bottom row carry the blue band baked in
 * (pure blue, no green, a touch of red); we place icons elsewhere, so those pixels are dropped and our own
 * background shows through */
static void blit_home(struct img im, int x, int y)
{
	if (!im.w) return;
	for (int j = 0; j < im.h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= H) continue;
		const uint16_t *s = im.px + j * im.w;
		uint16_t *d = fb + yy * W;
		for (int i = 0; i < im.w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= W) continue;
			uint16_t a = s[i], b = d[xx];
			if (((a >> 5) & 63) == 0 && (a >> 11) <= 4 && (a & 31) <= 20) continue;	/* baked band */
			int r = (a >> 11) > (b >> 11) ? (a >> 11) : (b >> 11);
			int g = ((a >> 5) & 63) > ((b >> 5) & 63) ? ((a >> 5) & 63) : ((b >> 5) & 63);
			int bl = (a & 31) > (b & 31) ? (a & 31) : (b & 31);
			d[xx] = (uint16_t)(r << 11 | g << 5 | bl);
		}
	}
}

/* white-on-black icon used as a stencil: pixels brighter than the threshold are painted in colour c */
static void blit_mask(struct img im, int x, int y, uint16_t c)
{
	if (!im.w) return;
	for (int j = 0; j < im.h; j++)
		for (int i = 0; i < im.w; i++) {
			uint16_t p = im.px[j * im.w + i];
			int lum = (p >> 11) * 2 + ((p >> 5) & 63) + (p & 31) * 2;	/* 0..186 */
			int xx = x + i, yy = y + j;
			if (lum > 80 && xx >= 0 && xx < W && yy >= 0 && yy < H) fb[yy * W + xx] = c;
		}
}

/* status icon cut from Sony's status bar (23/14) at bar row 5: the pixels that are still the bar are skipped, so
 * the icon sits on whatever bar we drew */
static void blit_barkey(struct img im, int x, int y)
{
	struct img bar = gfx_tex(T_BOTTOM);
	if (!im.w) return;
	for (int j = 0; j < im.h; j++)
		for (int i = 0; i < im.w; i++) {
			uint16_t p = im.px[j * im.w + i];
			int xx = x + i, yy = y + j;
			if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
			if (bar.w && 5 + j < bar.h) {
				uint16_t q = bar.px[(5 + j) * bar.w + (xx < bar.w ? xx : bar.w - 1)];
				int dr = (p >> 11) - (q >> 11), dg = ((p >> 5) & 63) - ((q >> 5) & 63), db = (p & 31) - (q & 31);
				if (dr >= -1 && dr <= 1 && dg >= -2 && dg <= 2 && db >= -1 && db <= 1) continue;
			}
			fb[yy * W + xx] = p;
		}
}

/* filled rectangle with 3 px rounded corners (the corner pixels trimmed) */
static void rbox(int x, int y, int w, int h, uint16_t c)
{
	static const int cut[3] = { 2, 1, 1 };
	for (int j = 0; j < h; j++) {
		int k = j < 3 ? cut[j] : j >= h - 3 ? cut[h - 1 - j] : 0;
		gfx_fill(x + k, y + j, w - 2 * k, 1, c);
	}
}

#define C_TRACK RGB(56, 60, 72)	/* empty part of the thin progress bars */
#define C_SURF RGB(28, 30, 36)	/* raised card */

/* 4 px progress bar with rounded ends */
static void pbar(int x, int y, int w, int pct, uint16_t fg)
{
	if (w < 6) return;
	rbox(x, y, w, 4, C_TRACK);
	int f = w * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
	if (f > 2) rbox(x, y, f, 4, fg);
}

/* image at half brightness (an episode already heard) */
static void blit_dim(struct img im, int x, int y)
{
	for (int j = 0; j < im.h; j++) {
		const uint16_t *s = im.px + j * im.w;
		uint16_t *d = fb + (y + j) * W + x;
		for (int i = 0; i < im.w; i++) d[i] = (s[i] >> 1) & 0x7BEF;
	}
}

static void text_outline(int x, int y, const char *s, uint16_t c, uint16_t o)
{
	gfx_text(x - 1, y, s, o, 1); gfx_text(x + 1, y, s, o, 1);
	gfx_text(x, y - 1, s, o, 1); gfx_text(x, y + 1, s, o, 1);
	gfx_text(x, y, s, c, 1);
}

/* ---------- bars ---------- */

static void title_bar(const char *title)
{
	gfx_vgrad(0, 0, W, TITLE_H - 1, RGB(132, 138, 148), RGB(200, 205, 212));
	gfx_fill(0, 0, W, 1, RGB(120, 126, 134));
	gfx_fill(0, TITLE_H - 1, W, 1, RGB(64, 68, 78));
	if (!title || !*title) return;
	int tw = gfx_text_w(title, 1), maxw = W - 14;
	if (tw <= maxw) { text_outline((W - tw) / 2, 3, title, C_WHITE, RGB(38, 42, 52)); return; }
	/* too long: clip with "..." (outline drawn by clipping the same text four times) */
	gfx_text_clip(6, 3, title, RGB(38, 42, 52), maxw); gfx_text_clip(8, 3, title, RGB(38, 42, 52), maxw);
	gfx_text_clip(7, 2, title, RGB(38, 42, 52), maxw); gfx_text_clip(7, 4, title, RGB(38, 42, 52), maxw);
	gfx_text_clip(7, 3, title, C_WHITE, maxw);
}

static void batt_icon(int x, int y)
{
	if (ui_st.batt < 0) return;
	static const int items[] = { 2, 4, 6, 8, 10, 12 };
	int lvl = ui_st.batt >= 95 ? 5 : ui_st.batt / 20;
	struct img b = gfx_tex(23, items[lvl < 0 ? 0 : lvl > 5 ? 5 : lvl]);
	if (b.w) blit_barkey(b, x, y);
	else { char s[8]; snprintf(s, sizeof s, "%d%%", ui_st.batt); gfx_text(x, y, s, C_WHITE, 1); }
	if (ui_st.charging) gfx_text(x - 9, y - 1, "+", C_ORANGE, 1);
}

enum { BB_NONE, BB_NOTE, BB_PERSON, BB_TEXT, BB_TIME };	/* BB_TIME: the text in Sony's player digits */

/* bottom status bar: [play state] [icon + text] ... [download] [Hold] [BT] [battery]. Returns nothing; text is clipped */
static int bb_no_dl;
static void bottom_bar(int kind, const char *text, uint16_t tc)
{
	int y = BOTTOM_Y, iy = BOTTOM_Y + 5;
	gfx_vgrad(0, y, W, H - y, RGB(104, 114, 132), RGB(30, 35, 46));
	gfx_fill(0, y, W, 1, RGB(150, 158, 172));
	int x = 2, right = W - 33;
	batt_icon(W - 32, iy);
	if (ui_st.bt) { struct img b = gfx_tex(T_BT); if (b.w) { gfx_blit_key(b, right - 21, iy + 1); right -= 22; } }
	if (ui_st.hold) { struct img h = gfx_tex(T_HOLD_S); if (h.w) { gfx_blit_key(h, right - 19, iy + 3); right -= 21; } }
	if (dl_active() && !bb_no_dl) {	/* podcast downloads: a small orange down arrow + percent (not on the downloads screen) */
		char pc[8] = ""; int p = dl_percent();
		if (p >= 0) snprintf(pc, sizeof pc, "%d%%", p);
		int tw = pc[0] ? gfx_text_w(pc, 1) : 0, ax = right - tw - 13;
		for (int r = 0; r < 5; r++) gfx_fill(ax + r, iy + 11 + r, 9 - 2 * r, 1, C_ORANGE);
		gfx_fill(ax + 2, iy + 4, 5, 7, C_ORANGE);
		gfx_fill(ax, iy + 17, 9, 2, C_ORANGE);
		if (pc[0]) gfx_text(ax + 11, iy, pc, C_ORANGE, 1);
		right = ax - 4;
	}
	if (ui_st.playing) { blit_barkey(ui_st.playing == 1 ? gfx_tex(T_S_PLAY) : gfx_tex(T_S_PAUSE), x, iy); x += 25; }
	if (!text || !*text || kind == BB_NONE) return;
	if (kind == BB_NOTE || kind == BB_PERSON) { blit_barkey(kind == BB_NOTE ? gfx_tex(T_S_NOTE) : gfx_tex(T_S_PERSON), x - 2, iy); x += 21; }
	else x += 3;
	if (kind == BB_TIME) { gfx_digits(x, y + 7, text, tc); return; }
	if (right - x > 20) gfx_text_clip(x, iy, text, tc, right - x - 2);
}

/* default content of the bottom bar: what is playing */
static void bottom_now(void)
{
	if (ui_st.np_title && *ui_st.np_title) bottom_bar(BB_NOTE, ui_st.np_title, C_WHITE);
	else bottom_bar(BB_NONE, 0, 0);
}

void scr_status(const char *title)
{
	title_bar(title);
	bottom_now();
}

static void background(void) { bg_fill(0, TITLE_H, W, BOTTOM_Y - TITLE_H, 0); }

void scr_screen(const char *title)
{
	background();
	scr_status(title);
}

/* ---------- home ---------- */

/* the cells at rest (frame 8) never change: composed once, then copied (the per-pixel max-blend of 9 icons made
 * a home redraw cost 8 ms at 248 MHz) */
static uint16_t cell_cache[HOME_SLOTS][HOME_CELL_W * HOME_CELL_H];
static unsigned char cell_cached[HOME_SLOTS];

static void home_cell(int i, int frame)
{
	int f = frame < 0 ? 0 : frame > 8 ? 8 : frame;
	int x = HOME_CELL_X(i), y = HOME_CELL_Y(i);
	if (f == 8 && cell_cached[i]) {
		for (int r = 0; r < HOME_CELL_H; r++) memcpy(fb + (y + r) * W + x, cell_cache[i] + r * HOME_CELL_W, HOME_CELL_W * 2);
		return;
	}
	bg_fill(x, y, HOME_CELL_W, HOME_CELL_H, 1);
	struct img ic = { 0, 0, 0 };
	if (home_src[i] >= 100) ic = gfx_anim(home_src[i] - 100 + f);
	if (!ic.w) { ic.w = 80; ic.h = 72; ic.px = home_anim[home_fallback[i]][f]; }
	blit_home(ic, x, y);
	if (f == 8) {
		for (int r = 0; r < HOME_CELL_H; r++) memcpy(cell_cache[i] + r * HOME_CELL_W, fb + (y + r) * W + x, HOME_CELL_W * 2);
		cell_cached[i] = 1;
	}
}

int ui_pod_new;

/* number badge over the top-right corner of the Podcasts icon (drawn after the cell, never cached) */
static void home_badge(int i)
{
	if (i != 2 || ui_pod_new <= 0) return;
	static int bx = -1, by;
	if (bx < 0) {	/* the corner of the icon's lit pixels, measured once on Sony's resting frame */
		struct img im = home_src[i] >= 100 ? gfx_anim(home_src[i] - 100 + 8) : (struct img){ 0, 0, 0 };
		int x1 = 60, y0 = 12;
		if (im.w) {
			x1 = 0; y0 = im.h;
			for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) {
				uint16_t p = im.px[y * im.w + x];
				if ((p >> 11) + ((p >> 5) & 63) / 2 + (p & 31) < 20) continue;
				if (((p >> 5) & 63) == 0 && (p >> 11) <= 4 && (p & 31) <= 20) continue;	/* baked band */
				if (x > x1) x1 = x;
				if (y < y0) y0 = y;
			}
		}
		bx = x1 - 12; by = y0 - 6;
		if (by < 1) by = 1;
	}
	char n[8]; snprintf(n, sizeof n, ui_pod_new > 99 ? "99+" : "%d", ui_pod_new);
	int w = gfx_text_w(n, 1) + 10, x = HOME_CELL_X(i) + bx, y = HOME_CELL_Y(i) + by;
	if (w < 20) w = 20;
	if (x + w > HOME_CELL_X(i) + HOME_CELL_W) x = HOME_CELL_X(i) + HOME_CELL_W - w;
	rbox(x - 1, y - 1, w + 2, 22, C_BLACK);	/* a 1 px black ring separates it from the icon */
	rbox(x, y, w, 20, C_ORANGE);
	gfx_text_center(x + w / 2, y - 1, n, C_BLACK, 1);
}

void scr_home_icon(int i, int frame)
{
	if (i < 0 || i >= HOME_SLOTS || home_src[i] < 0) return;
	home_cell(i, frame);
	home_badge(i);
}

/* the textures the screens use, for the start-up warm-up (warm.c): whole groups of small icons, single big items */
void scr_warm(void (*touch)(const void *p, long n))
{
	static const int groups[] = { 10, 16, 19, 23, 25, 29, 30, 34 };	/* icons, bars, digits: 1..150 KB each */
	static const int items[][2] = { { T_BARSEL }, { T_BARSEL_S }, { T_HOLD_POP }, { T_ART } };
	for (unsigned g = 0; g < sizeof groups / sizeof *groups; g++)
		for (int i = 0, n = gfx_tex_n(groups[g]); i < n; i++) { struct img t = gfx_tex(groups[g], i); if (t.w) touch(t.px, (long)t.w * t.h * 2); }
	for (unsigned i = 0; i < sizeof items / sizeof *items; i++) { struct img t = gfx_tex(items[i][0], items[i][1]); if (t.w) touch(t.px, (long)t.w * t.h * 2); }
	for (int i = 0, n = gfx_tex_n(31); i < n; i++) { struct img t = gfx_tex(31, i); if (t.w) touch(t.px, (long)t.w * t.h); }	/* 8-bit digit masks */
}

void scr_home(int sel, const int *frame, const char *nowline)
{
	bg_fill(0, TITLE_H, W, BOTTOM_Y - TITLE_H, 1);
	title_bar("Inicio");
	for (int i = 0; i < HOME_SLOTS; i++) if (home_src[i] >= 0) { home_cell(i, frame[i]); home_badge(i); }
	if (sel >= 0 && sel < HOME_SLOTS && home_labels[sel][0]) {
		const char *l = home_labels[sel];
		int x = (W - gfx_text_w(l, 1)) / 2;
		gfx_text(x + 1, 256, l, RGB(10, 20, 60), 1);
		gfx_text(x, 255, l, C_WHITE, 1);
	}
	/* Sony shows the artist (person icon) on the home screen; otherwise the title */
	if (ui_st.np_artist && *ui_st.np_artist) bottom_bar(BB_PERSON, ui_st.np_artist, C_WHITE);
	else if (ui_st.np_title && *ui_st.np_title) bottom_bar(BB_NOTE, ui_st.np_title, C_WHITE);
	else bottom_bar(nowline && *nowline ? BB_NOTE : BB_NONE, nowline, C_WHITE);
}

/* ---------- popups ---------- */

static void panel(int x, int y, int w, int h)
{
	gfx_vgrad(x, y, w, h, RGB(66, 72, 86), RGB(26, 29, 37));
	gfx_frame(x, y, w, h, RGB(176, 182, 192));
	gfx_frame(x + 1, y + 1, w - 2, h - 2, RGB(20, 22, 28));
}

void scr_toast(const char *msg)
{
	int x = 14, y = 128, w = W - 28;
	int lines = gfx_text_w(msg, 1) > w - 24 ? 2 : 1, h = 14 + lines * 22;
	panel(x, y, w, h);
	gfx_text_wrap(x + 12, y + 7, msg, C_WHITE, w - 24, 2, 22);
}

/* volume, as Sony shows it: it takes over the status bar for a moment (play state, Sony's volume ramp 34/3, one
 * segment per step, lit ones orange 34/7, the rest grey 34/5, and the level in Sony's digits: "06") */
void scr_volume(int vol, int volmax)
{
	int y = BOTTOM_Y, iy = BOTTOM_Y + 5;
	gfx_vgrad(0, y, W, H - y, RGB(104, 114, 132), RGB(30, 35, 46));
	gfx_fill(0, y, W, 1, RGB(150, 158, 172));
	if (ui_st.playing) blit_barkey(ui_st.playing == 1 ? gfx_tex(T_S_PLAY) : gfx_tex(T_S_PAUSE), 2, iy);
	struct img ramp = gfx_tex(34, 3), on = gfx_tex(34, 7), off = gfx_tex(34, 5);
	if (ramp.w) blit_barkey(ramp, 32, iy);
	int x0 = 60, seg = volmax > 0 ? volmax : 30, sy = y + 11;
	static uint16_t onb[4 * 10], offb[4 * 10];	/* Sony's 4x6 segments drawn 10 high, as on its screen */
	if (on.w) on = gfx_scaled(on, on.w, 10, onb);
	if (off.w) off = gfx_scaled(off, off.w, 10, offb);
	for (int i = 0; i < seg; i++) {
		struct img s = i < vol ? on : off;
		if (s.w) gfx_blit(s, x0 + i * 4, sy);
		else gfx_fill(x0 + i * 4, sy, 3, 10, i < vol ? C_ORANGE : RGB(90, 98, 112));
	}
	char b[8]; snprintf(b, sizeof b, "%02d", vol);
	gfx_digits(x0 + seg * 4 + 8, y + 7, b, C_WHITE);
}

void scr_hold(void)
{
	struct img h = gfx_tex(T_HOLD_POP);
	if (h.w) gfx_blit(h, (W - h.w) / 2, 135);
	else { gfx_fill(70, 135, 100, 50, C_BLACK); gfx_frame(70, 135, 100, 50, C_WHITE); gfx_text_center(120, 150, "Hold", C_WHITE, 1); }
}

/* ---------- lists ---------- */

/* x where a row's label starts (the selected row gets Sony's small play mark before the text) */
static int row_tx(const struct list_view *v, int sel)
{
	if (v->thumb) return 36;
	if (v->icon == LI_FOLDER) return 28;
	return sel ? 29 : 8;
}

static int row_right(const struct list_view *v, int i)
{
	int right = W - 10;
	if (v->mark && v->mark(i)) right = W - 30;
	if (v->value) { const char *val = v->value(i); if (val && *val) right -= gfx_text_w(val, 1) + 8; }
	return right;
}

static void sel_bar_h(int y, int scrollbar, int h);
static void sel_bar(int y, int scrollbar) { sel_bar_h(y, scrollbar, LIST_ROW_H); }
static void sel_bar_h(int y, int scrollbar, int h)
{
	if (h != LIST_ROW_H) {	/* big rows: Sony's bar stretched to the height, scaled once */
		static uint16_t big[2][W * BIG_ROW_H]; static struct img bi[2]; static int bh[2];
		int k = !!scrollbar;
		if (h > BIG_ROW_H) h = BIG_ROW_H;
		if (bh[k] != h) { bi[k].w = 0; bh[k] = h; }	/* big rows and episode rows have different heights */
		if (!bi[k].w) { struct img t = scrollbar ? gfx_tex(T_BARSEL_S) : gfx_tex(T_BARSEL); if (t.w) bi[k] = gfx_scaled(t, t.w, h, big[k]); }
		if (bi[k].w) gfx_blit(bi[k], scrollbar ? 1 : 0, y);
		else gfx_vgrad(0, y, scrollbar ? W - 8 : W, h, RGB(255, 222, 140), RGB(255, 170, 0));
		return;
	}
	struct img bar = scrollbar ? gfx_tex(T_BARSEL_S) : gfx_tex(T_BARSEL);
	if (bar.w) gfx_blit(bar, scrollbar ? 1 : 0, y);
	else gfx_vgrad(0, y, scrollbar ? W - 8 : W, LIST_ROW_H, RGB(255, 222, 140), RGB(255, 170, 0));
}

/* mosaic: 3x3 covers, the selected one framed in orange; its name is the title, its value under the grid */
static void scr_grid(const struct list_view *v)
{
	background();
	scr_status(v->n ? v->label(v->sel) : v->title);
	if (!v->n) { gfx_text_wrap(16, 120, v->empty ? v->empty : "(vacío)", C_GREY, W - 32, 4, 22); return; }
	for (int r = 0; r < GRID_N; r++) {
		int i = v->top + r;
		if (i >= v->n) break;
		int x = 6 + (r % 3) * 78, y = 34 + (r / 3) * 78;
		if (i == v->sel) rbox(x - 3, y - 3, GRID_TH + 6, GRID_TH + 6, C_ORANGE);
		struct img ic = v->thumb ? v->thumb(i) : (struct img){ 0, 0, 0 };
		if (ic.w == GRID_TH) gfx_blit(ic, x, y);
		else { gfx_fill(x, y, GRID_TH, GRID_TH, RGB(46, 50, 60)); blit_mask(gfx_tex(T_DISC), x + 27, y + 27, C_GREY); }
	}
	const char *val = v->value ? v->value(v->sel) : 0;
	if (val && *val) gfx_text_center(W / 2, 264, val, C_GREY, 1);
	if (v->n > GRID_N) {	/* scrollbar by grid rows */
		int rows = (v->n + 2) / 3, y0 = 32, hh = 234, th = hh * 3 / rows, ty = y0 + (hh - th) * (v->top / 3) / (rows - 3);
		if (th < 10) th = 10;
		gfx_fill(W - 4, y0, 3, hh, RGB(50, 56, 70));
		gfx_vgrad(W - 4, ty, 3, th, RGB(190, 220, 255), RGB(110, 160, 230));
	}
}

void scr_list(const struct list_view *v)
{
	if (v->grid) { scr_grid(v); return; }
	background();
	scr_status(v->title);
	if (!v->n) { gfx_text_wrap(16, 120, v->empty ? v->empty : "(vacío)", C_GREY, W - 32, 4, 22); return; }
	int rows = lv_rows(v), rh = lv_rowh(v), sb = v->n > rows;
	for (int r = 0; r < rows; r++) {
		int i = v->top + r;
		if (i >= v->n) break;
		int y = LIST_Y0 + r * rh, sel = i == v->sel, sliding = v->bar_y >= 0;
		if (sel && !sliding) sel_bar_h(y, sb, rh);
		if (sliding) sel = 0;	/* while the bar slides, every row is drawn in its normal colours */
		uint16_t fg = sel ? C_BLACK : C_WHITE;
		if (v->big || v->ep) {	/* cover on the left, the name on top, the value (if any) below in grey */
			int th = v->ep ? EP_THUMB : BIG_THUMB, st = v->ep && v->state ? v->state(i) : 0;
			struct img ic = v->thumb ? v->thumb(i) : (struct img){ 0, 0, 0 };
			if (ic.w == th) { if (st == 2 && !sel) blit_dim(ic, 4, y + 2); else gfx_blit(ic, 4, y + 2); }
			else { gfx_fill(4, y + 2, th, th, sel ? RGB(200, 120, 0) : RGB(46, 50, 60)); blit_mask(gfx_tex(v->icon == LI_FOLDER ? T_FOLDER : T_NOTE), 4 + th / 2 - 9, y + 2 + th / 2 - 9, sel ? C_BLACK : C_GREY); }
			if (st == 1) { rbox(th - 8, y, 14, 14, C_BLACK); rbox(th - 7, y + 1, 12, 12, C_ORANGE); }	/* new: a dot on the cover */
			const char *val = v->value ? v->value(i) : 0;
			int right = W - (sb ? 14 : 8), tx = 4 + th + (v->ep ? 6 : 8);
			if (v->mark && v->mark(i)) { blit_mask(gfx_tex(T_ARROW), W - 30, y + (v->ep ? 16 : 22), sel ? C_BLACK : C_GREEN); right = W - 34; }
			uint16_t tc = st == 2 && !sel ? RGB(120, 124, 132) : fg, vc = sel ? RGB(60, 40, 0) : st == 2 ? RGB(100, 104, 112) : C_GREY;
			int y1 = v->ep ? y + 3 : y + 10, y2 = v->ep ? y + 25 : y + 34;
			if (val && *val) {
				gfx_text_clip(tx, y1, v->label(i), tc, right - tx);
				gfx_text_clip(tx, y2, val, vc, right - tx);
				int pct = v->ep && v->progress ? v->progress(i) : -1, mx = tx + gfx_text_w(val, 1) + 8;
				if (pct >= 0 && right - mx >= 12) pbar(mx, y2 + 10, right - mx, pct, sel ? C_BLACK : C_ORANGE);
			}
			else gfx_text_clip(tx, y + (rh - 22) / 2, v->label(i), tc, right - tx);
			continue;
		}
		if (v->thumb) {
			static uint16_t small[26 * 26];
			struct img ic = v->thumb(i);
			if (ic.w) { gfx_blit(gfx_scaled(ic, 26, 26, small), 4, y + 1); }
			else { gfx_fill(4, y + 1, 26, 26, sel ? RGB(200, 120, 0) : RGB(46, 50, 60)); blit_mask(gfx_tex(T_NOTE), 8, y + 5, sel ? C_BLACK : C_GREY); }
		} else if (v->icon == LI_FOLDER) blit_mask(gfx_tex(T_FOLDER), 6, y + 5, fg);
		else if (sel) blit_mask(gfx_tex(T_PLAYBOX), 6, y + 5, C_BLACK);
		int tx = row_tx(v, sel), right = row_right(v, i);
		if (v->mark && v->mark(i)) blit_mask(gfx_tex(T_ARROW), W - 28, y + 5, sel ? C_BLACK : C_GREEN);
		if (v->value) {
			const char *val = v->value(i);
			if (val && *val) {
				int vr = v->mark && v->mark(i) ? W - 30 : W - 10;
				gfx_text(vr - gfx_text_w(val, 1), y + 3, val, sel ? C_BLACK : C_ORANGE, 1);
			}
		}
		const char *lb = v->label(i);
		if (sel && v->scroll > 0 && gfx_text_w(lb, 1) > right - tx) {
			/* marquee: draw shifted text into the row, clipped to [tx, right) */
			static uint16_t save[W * LIST_ROW_H];
			for (int r2 = 0; r2 < LIST_ROW_H; r2++) memcpy(save + r2 * W, fb + (y + r2) * W, W * 2);
			gfx_text(tx - v->scroll, y + 3, lb, fg, 1);
			for (int r2 = 0; r2 < LIST_ROW_H; r2++) {
				memcpy(fb + (y + r2) * W, save + r2 * W, tx * 2);
				memcpy(fb + (y + r2) * W + right, save + r2 * W + right, (W - right) * 2);
			}
		} else gfx_text_clip(tx, y + 3, lb, fg, right - tx);
	}
	if (v->bar_y >= 0) sel_bar_h(v->bar_y, sb, rh);	/* the sliding selection bar, on top of the rows */
	if (sb) {	/* thin scrollbar at the right edge */
		int y0 = LIST_Y0, hh = rows * rh;
		int th = hh * rows / v->n, ty = y0 + (hh - th) * v->top / (v->n - rows);
		if (th < 10) th = 10;
		gfx_fill(W - 5, y0, 3, hh, RGB(50, 56, 70));
		gfx_vgrad(W - 5, ty, 3, th, RGB(190, 220, 255), RGB(110, 160, 230));
	}
}

/* Sony's EQ screen (like the Sony UI: preset list with radio buttons on top, the slider panel below, textures
 * from the firmware: panel 16_06, knobs 16_01 grey / 16_02 white / 16_03 orange, radio 25_61..25_64).
 * Panel geometry measured on the texture: Clear Bass track x 41.5 (0..+3), bands x 87.5 115.5 143.5 171.5 199.5
 * (+3..-3), 9 px a step, level 0 at y 42.5 of the panel; the panel sits right above the status bar.
 * names/n/sel/top: the list; active: the preset in use (radio dot); cb/bands: the values to show;
 * edit: -1 not editing, 0 Clear Bass, 1..5 the band being edited (orange knob); grey: knobs greyed (EQ off). */
#define EQ_ROWS 5
void scr_sony_eq(const char *const *names, int n, int sel, int top, int active, int cb, const int *bands, int edit, int grey)
{
	background();
	scr_status("Ecualizador");
	int sb = n > EQ_ROWS;
	for (int r = 0; r < EQ_ROWS && top + r < n; r++) {
		int i = top + r, y = LIST_Y0 + r * LIST_ROW_H, on = i == sel && edit < 0;
		if (on) sel_bar(y, sb);
		struct img rb = gfx_tex(25, on ? (i == active ? 64 : 62) : (i == active ? 63 : 61));
		if (i < 6) { if (rb.w) gfx_blit(rb, 6, y + 5); }
		gfx_text_clip(i < 6 ? 28 : 10, y + 3, names[i], on ? C_BLACK : C_WHITE, W - 40);
	}
	if (sb) {
		int y0 = LIST_Y0, hh = EQ_ROWS * LIST_ROW_H, th = hh * EQ_ROWS / n, ty = y0 + (hh - th) * top / (n - EQ_ROWS);
		gfx_fill(W - 5, y0, 3, hh, RGB(50, 56, 70));
		gfx_vgrad(W - 5, ty, 3, th, RGB(190, 220, 255), RGB(110, 160, 230));
	}
	int py = BOTTOM_Y - 104;
	struct img panel = gfx_tex(16, 6);
	if (panel.w) gfx_blit(panel, 0, py); else gfx_fill(0, py, W, 104, RGB(72, 74, 72));
	static const int bx[5] = { 87, 115, 143, 171, 199 };
	struct img kw = gfx_tex(16, 2), ko = gfx_tex(16, 3), kg = gfx_tex(16, 1);
	for (int b = 0; b < 6; b++) {
		int x = b == 0 ? 41 : bx[b - 1], v = b == 0 ? cb : bands[b - 1];
		int y = py + 42 - v * 9 - 3;	/* knob 20x8 centred on the level */
		struct img k = grey ? kg : edit == b ? ko : kw;
		if (k.w) gfx_blit(k, x + 1 - 10, y); else gfx_fill(x - 9, y, 20, 8, edit == b ? C_ORANGE : C_WHITE);
	}
}

int scr_list_overflow(const struct list_view *v)
{
	if (!v->n || v->big || v->grid || v->ep) return 0;	/* big rows / mosaic / episodes: no marquee */
	int over = gfx_text_w(v->label(v->sel), 1) - (row_right(v, v->sel) - row_tx(v, 1));
	return over > 0 ? over : 0;
}

/* ---------- now playing ---------- */

static void fmt_time(char *b, int sec)
{
	if (sec < 0) sec = 0;
	if (sec >= 3600) sprintf(b, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
	else sprintf(b, "%02d:%02d", sec / 60, sec % 60);
}

#define P_STRIP 252	/* extras line (timer, speed, output, remaining time) */
#define P_PROG 279	/* progress bar, right above the status bar */

static void info_line(int y, struct img ic, const char *s)
{
	if (!s || !*s) return;
	blit_mask(ic, 4, y + 3, C_WHITE);
	gfx_text_clip(24, y, s, C_WHITE, W - 30);
}

void scr_player(const struct player_view *v, int full)
{
	char b[64], t1[16];
	int dy = 0;
	if (full) {
		background();
		title_bar(v->book);
		char idx[16]; snprintf(idx, sizeof idx, "%d/%d", v->idx + 1, v->ntr);
		const char *tr = v->track ? v->track : "";
		blit_mask(gfx_tex(T_NOTE), 3, 34, C_WHITE);
		/* title: always one line, truncated with "...", like Sony's, so the cover and the lines below never move */
		gfx_text_clip(22, 31, tr, C_WHITE, W - 28);
		if (v->show_lrc) {	/* lyrics instead of the cover: previous line dim, current orange, next grey */
			if (v->lrc_prev) gfx_text_clip(8, 58 + dy, v->lrc_prev, RGB(110, 116, 128), W - 16);
			gfx_text_wrap(8, 84 + dy, v->lrc_cur && *v->lrc_cur ? v->lrc_cur : "...", C_ORANGE, W - 16, 2, 22);
			if (v->lrc_next) gfx_text_clip(8, 134 + dy, v->lrc_next, C_GREY, W - 16);
		} else {
			struct img art = v->art;
			static uint16_t ph[PLAYER_ART * PLAYER_ART];
			static struct img ph_img;	/* the placeholder, scaled once */
			if (!art.w) { if (!ph_img.w) ph_img = gfx_scaled(gfx_tex(T_ART), PLAYER_ART, PLAYER_ART, ph); art = ph_img; }
			if (art.w) { gfx_blit(art, 6, 56 + dy); gfx_frame(5, 55 + dy, art.w + 2, art.h + 2, RGB(70, 76, 90)); }
			gfx_digits(W - 8 - gfx_digits_w(idx), 56 + dy + PLAYER_ART - 20, idx, C_WHITE);	/* "3/14" in Sony's digits */
		}
		int ys = 173 + dy;
		gfx_fill(0, ys, W, 1, RGB(150, 156, 168));
		/* artist / album (music: sub = "artist - album"), else the folder; then the podcast chapter */
		const char *sub = v->sub ? v->sub : "", *dash = strstr(sub, " - ");
		int y = ys + 4, maxy = P_STRIP - 20;
		if (dash) {
			char a[96]; int n = (int)(dash - sub) < (int)sizeof a - 1 ? (int)(dash - sub) : (int)sizeof a - 1;
			memcpy(a, sub, n); a[n] = 0;
			info_line(y, gfx_tex(T_PERSON), a); y += 24;
			if (y <= maxy) { info_line(y, gfx_tex(T_DISC), dash + 3); y += 24; }
			if (v->genre && *v->genre && y <= maxy) { info_line(y, gfx_tex(T_GENRE), v->genre); y += 24; }
		} else if (v->meta) {	/* podcast / audiobook: the show, when it came out, then the chapter (or the length) */
			if (*sub) { info_line(y, gfx_tex(T_FOLDER), sub); y += 24; }
			if (v->date && *v->date) { info_line(y, gfx_tex(T_YEAR), v->date); y += 24; }
			if (v->chap && *v->chap) { if (y <= maxy) { info_line(y, gfx_tex(T_CHAP), v->chap); y += 24; } }
			else if (v->total > 0 && y <= maxy) {
				char d[24]; int m = (v->total + 30) / 60;
				if (m >= 60) snprintf(d, sizeof d, "%d h %d min", m / 60, m % 60); else snprintf(d, sizeof d, "%d min", m ? m : 1);
				info_line(y, gfx_tex(T_CLOCK), d); y += 24;
			}
		} else {
			if (*sub && !(v->book && !strcmp(sub, v->book) && v->chap)) { info_line(y, gfx_tex(T_FOLDER), sub); y += 24; }
			if (v->chap && *v->chap && y <= maxy) { info_line(y, gfx_tex(T_CHAP), v->chap); y += 24; }
		}
	}
	/* dynamic part: extras line, progress bar, status bar (elapsed time) */
	bg_fill(0, P_STRIP - 2, W, BOTTOM_Y - P_STRIP + 2, 0);
	int x = 6;
	/* the album's year: the line under the genre, aligned with the info lines' text; the extras follow it */
	if (v->year && *v->year && !v->show_lrc) { blit_mask(gfx_tex(T_YEAR), 4, P_STRIP + 3, C_WHITE); x = gfx_text(24, P_STRIP, v->year, C_WHITE, 1) + 12; }
	if (v->show_lrc) { snprintf(b, sizeof b, "%d/%d", v->idx + 1, v->ntr); x = gfx_text(x, P_STRIP, b, C_GREY, 1) + 10; }
	if (v->sleep_left > 0) {
		blit_mask(gfx_tex(T_CLOCK), x, P_STRIP + 3, C_ORANGE); x += 20;
		snprintf(b, sizeof b, "%d'", v->sleep_left);
		x = gfx_text(x, P_STRIP, b, C_ORANGE, 1) + 10;
	}
	if (v->speed && v->speed != 100) { snprintf(b, sizeof b, "%d.%02dx", v->speed / 100, v->speed % 100); x = gfx_text(x, P_STRIP, b, C_ORANGE, 1) + 10; }
	fmt_time(t1, v->total - v->sec);
	int rt_x = W - 8 - gfx_digits_w(t1) - gfx_digits_w("-");	/* where the remaining time starts: the extras must end before it */
	if (v->out && *v->out && x + 24 <= rt_x) { struct img bt = gfx_tex(T_BT); if (bt.w) gfx_blit_key(bt, x, P_STRIP + 1); else gfx_text(x, P_STRIP, v->out, C_BLUE, 1); }
	fmt_time(t1, v->total - v->sec);
	snprintf(b, sizeof b, "-%s", t1);
	gfx_digits(W - 8 - gfx_digits_w(b), P_STRIP + 2, b, C_GREY);
	struct img tr = gfx_tex(T_PROG_BG), fi = gfx_tex(T_PROG);
	int pw = W - 4, fill = v->total > 0 ? pw * (v->sec > v->total ? v->total : v->sec) / v->total : 0;
	if (tr.w) gfx_blit(tr, 2, P_PROG); else gfx_fill(2, P_PROG, pw, 8, RGB(60, 66, 80));
	if (fi.w) { struct img part = fi; part.w = fill < fi.w ? fill : fi.w;	/* left part of the fill, row by row */
		for (int j = 0; j < fi.h; j++) memcpy(fb + (P_PROG + j) * W + 2, fi.px + j * fi.w, part.w * 2); }
	else gfx_vgrad(2, P_PROG, fill, 8, RGB(160, 210, 255), RGB(40, 110, 220));
	fmt_time(t1, v->sec);
	bottom_bar(BB_TIME, t1, C_WHITE);
}

/* ---------- messages / progress / downloads ---------- */

void scr_message(const char *title, const char *l1, const char *l2)
{
	scr_screen(title);
	int n = gfx_text_wrap(14, 96, l1 ? l1 : "", C_WHITE, W - 28, 3, 24);
	if (l2 && *l2) gfx_text_wrap(14, 104 + n * 24, l2, C_ORANGE, W - 28, 3, 24);
}

void scr_progress(const char *title, int step, int nsteps, const char *text, int secs, int spin, const char *hint)
{
	scr_screen(title);
	char b[48];
	snprintf(b, sizeof b, "Paso %d de %d", step, nsteps);
	gfx_text(14, 44, b, C_GREY, 1);
	gfx_text_wrap(14, 72, text, C_WHITE, W - 28, 2, 24);
	/* one segment per step: done = orange, current = pulsing, pending = dark */
	int sw = (W - 28 - (nsteps - 1) * 6) / nsteps;
	for (int i = 0; i < nsteps; i++) {
		uint16_t c = i + 1 < step ? C_ORANGE : i + 1 == step ? ((spin & 1) ? C_ORANGE : RGB(150, 90, 0)) : RGB(60, 66, 80);
		gfx_fill(14 + i * (sw + 6), 132, sw, 8, c);
	}
	static const char *sp[4] = { "   ", ".  ", ".. ", "..." };
	snprintf(b, sizeof b, "%d s %s", secs, sp[spin & 3]);
	gfx_text(14, 150, b, C_GREY, 1);
	if (hint && *hint) gfx_text_wrap(14, 196, hint, C_ORANGE, W - 28, 3, 22);
}

static void mbytes(char *s, size_t n, long b) { if (b >= 1 << 20) snprintf(s, n, "%.1f MB", b / 1048576.0); else snprintf(s, n, "%ld kB", b >> 10); }

/* downloads: the episode downloading now as a card (big percentage, size, title, bar), then the rest of the queue
 * with the feed's cover; speed and time left in the status bar. Not downloading: the header/sub lines in the card. */
int scr_downloads(const char *header, const char *sub, int active, int reading, const struct dl_row *rows, int n, int top, double rate)
{
	(void)rate;
	background();
	title_bar("Descargas");
	int cur = -1;
	for (int i = 0; i < n; i++) if (rows[i].st == 1) { cur = i; break; }
	bb_no_dl = 1;
	bottom_bar(BB_TEXT, cur >= 0 && sub && *sub ? sub : active ? "OPTION: cancelar" : "BACK: volver", C_WHITE);
	bb_no_dl = 0;
	rbox(4, 31, W - 8, 92, C_SURF);
	if (cur >= 0) {
		const struct dl_row *it = &rows[cur];
		int pct = it->total > 0 ? (int)((long long)it->got * 100 / it->total) : 0;
		char b[32]; snprintf(b, sizeof b, "%d%%", pct);
		gfx_text(12, 30, b, C_ORANGE, 2);
		snprintf(b, sizeof b, "%d de %d", cur + 1, n);
		gfx_text(W - 12 - gfx_text_w(b, 1), 34, b, C_GREY, 1);
		if (it->total > 0) snprintf(b, sizeof b, "%ld/%ld MB", it->got >> 20, it->total >> 20); else snprintf(b, sizeof b, "%ld MB", it->got >> 20);
		gfx_text(W - 12 - gfx_text_w(b, 1), 56, b, C_WHITE, 1);
		gfx_text_clip(12, 78, it->title, C_WHITE, W - 24);
		pbar(12, 108, W - 24, pct, C_ORANGE);
	} else {
		gfx_text_clip(12, 36, header, active ? C_ORANGE : C_WHITE, W - 24);
		if (sub && *sub) gfx_text_clip(12, 60, sub, C_GREY, W - 24);
	}
	if (!n) { gfx_text_wrap(8, 140, reading ? "Buscando episodios nuevos en tus podcasts..." : "No había episodios para bajar.", C_GREY, W - 16, 3, 22); return 0; }
	/* the others: queued first, then finished / failed; 3 rows of 44 px */
	static int order[512];
	int m = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n && m < 512; i++) if (i != cur && ((rows[i].st == 0) == (pass == 0))) order[m++] = i;
	if (top > m - DL_ROWS) top = m - DL_ROWS;
	if (top < 0) top = 0;
	int sb = m > DL_ROWS, rw = W - 8 - (sb ? 6 : 0), y = 150;
	if (m) gfx_text(8, 126, rows[order[top]].st == 0 ? "En cola" : "Terminados", C_GREY, 1);
	for (int j = top; j < m && j < top + DL_ROWS; j++, y += 46) {
		const struct dl_row *it = &rows[order[j]];
		rbox(4, y, rw - 4, 44, C_SURF);
		if (it->cover.w == 40) { if (it->st == 2) blit_dim(it->cover, 6, y + 2); else gfx_blit(it->cover, 6, y + 2); }
		else { gfx_fill(6, y + 2, 40, 40, RGB(46, 50, 60)); blit_mask(gfx_tex(T_NOTE), 17, y + 13, C_GREY); }
		char st[96], a[24];
		mbytes(a, sizeof a, it->total > 0 ? it->total : it->got);
		uint16_t sc = C_GREY;
		switch (it->st) {
		case 0: snprintf(st, sizeof st, "%s · %s", it->feed, it->total > 0 ? a : "en cola"); break;
		case 2: snprintf(st, sizeof st, "listo · %s", it->feed); sc = C_GREEN; break;
		case 3: snprintf(st, sizeof st, "error, se retoma · %s", it->feed); sc = C_RED; break;
		default: snprintf(st, sizeof st, "cancelado · %s", it->feed);
		}
		gfx_text_clip(52, y, it->title, it->st == 2 ? C_GREY : C_WHITE, rw - 56);
		gfx_text_clip(52, y + 21, st, sc, rw - 56);
	}
	if (sb) {
		int y0 = 150, hh = DL_ROWS * 46 - 2, th = hh * DL_ROWS / m, ty = y0 + (hh - th) * top / (m - DL_ROWS);
		gfx_fill(W - 5, y0, 3, hh, RGB(50, 56, 70));
		gfx_vgrad(W - 5, ty, 3, th, RGB(190, 220, 255), RGB(110, 160, 230));
	}
	return top;
}

/* ---------- full-screen images ---------- */

void scr_splash(const char *line)
{
	struct img s = gfx_tex(T_SPLASH);
	if (s.w) gfx_blit(s, 0, 0); else gfx_fill(0, 0, W, H, C_BLACK);
	if (line) { gfx_fill(0, 252, W, 40, C_BLACK); gfx_text_center(W / 2, 262, line, C_WHITE, 1); }
}

void scr_poweroff(void)
{
	struct img s = gfx_tex(T_POWEROFF);
	if (s.w) gfx_blit(s, 0, 0); else { gfx_fill(0, 0, W, H, C_BLACK); gfx_text_center(W / 2, 150, "POWER OFF", C_BLUE, 1); }
}

void scr_ask(void)
{
	scr_splash(0);
	gfx_fill(0, 262, W, 58, RGB(10, 10, 14));
	gfx_fill(0, 262, W, 1, RGB(80, 80, 90));
	gfx_text_center(W / 2, 270, "HOME: Audiolibros", C_ORANGE, 1);
	gfx_text_center(W / 2, 294, "esperá: Walkman", C_GREY, 1);
}

/* ---------- focus mode ---------- */

static void big_digits(int cx, int y, const char *s)
{
	int w = 0;
	for (const char *p = s; *p; p++) w += *p == ':' ? 16 : 32;
	int x = cx - w / 2;
	for (const char *p = s; *p; p++) {
		struct img d = *p == ':' ? gfx_tex(30, 12) : gfx_tex(30, 1 + (*p - '0'));
		if (d.w) blit_max(d, x, y); else { char c[2] = { *p, 0 }; gfx_text(x, y, c, C_WHITE, 2); }
		x += *p == ':' ? 16 : 32;
	}
}

void scr_focus(const struct focus_view *v)
{
	background();
	struct img ring = gfx_tex(T_RING);
	int cy = 128;
	if (ring.w) blit_max(ring, 0, cy - ring.h / 2);
	scr_status("Modo foco");
	char b[16];
	int l = v->left < 0 ? 0 : v->left;
	snprintf(b, sizeof b, "%02d:%02d", l / 60, l % 60);
	big_digits(W / 2, cy - 18, b);
	if (v->paused) gfx_text_center(W / 2, cy + 26, "PAUSA", C_ORANGE, 1);
	if (v->l1) gfx_text_center(W / 2, 238, v->l1, C_GREY, 1);
	if (v->l2) gfx_text_center(W / 2, 262, v->l2, C_GREY, 1);
}

/* ---------- on-screen keyboard ---------- */

static const char *const kb_chars[2][2][4][KBD_COLS] = {
	{	/* letters */
		{ { "a", "b", "c", "d", "e", "f", "g" }, { "h", "i", "j", "k", "l", "m", "n" },
		  { "ñ", "o", "p", "q", "r", "s", "t" }, { "u", "v", "w", "x", "y", "z", "'" } },
		{ { "A", "B", "C", "D", "E", "F", "G" }, { "H", "I", "J", "K", "L", "M", "N" },
		  { "Ñ", "O", "P", "Q", "R", "S", "T" }, { "U", "V", "W", "X", "Y", "Z", "'" } },
	},
	{	/* digits, accents, symbols */
		{ { "1", "2", "3", "4", "5", "6", "7" }, { "8", "9", "0", ".", ",", "-", "&" },
		  { "á", "é", "í", "ó", "ú", "ü", "¿" }, { "!", "?", ":", "(", ")", "+", "/" } },
		{ { "1", "2", "3", "4", "5", "6", "7" }, { "8", "9", "0", ".", ",", "-", "&" },
		  { "Á", "É", "Í", "Ó", "Ú", "Ü", "¡" }, { "!", "?", ":", "(", ")", "+", "/" } },
	},
};

int kbd_row(int mode, int upper, int row, struct kbd_key *keys)
{
	mode = !!mode; upper = !!upper;
	if (row < 4) {
		for (int c = 0; c < KBD_COLS; c++) { keys[c].kind = KB_CHAR; keys[c].col = c; keys[c].span = 1; keys[c].ch = kb_chars[mode][upper][row][c]; }
		return KBD_COLS;
	}
	if (row == 4) {
		keys[0] = (struct kbd_key){ KB_SHIFT, 0, 2, 0 };
		keys[1] = (struct kbd_key){ KB_MODE, 2, 2, 0 };
		keys[2] = (struct kbd_key){ KB_SPACE, 4, 3, 0 };
		return 3;
	}
	keys[0] = (struct kbd_key){ KB_DEL, 0, 3, 0 };
	keys[1] = (struct kbd_key){ KB_OK, 3, 4, 0 };
	return 2;
}

#define KB_Y0 72	/* first row of keys */
#define KB_H 34		/* key pitch */
#define KB_W 34

void scr_keyboard(const char *title, const char *text, const char *placeholder, const char *ok_label, int mode, int upper, int row, int key)
{
	background();
	title_bar(title);
	bottom_bar(BB_TEXT, *text ? "BACK: borrar" : "BACK: volver", C_WHITE);
	/* the text field: the end of the text is what matters while typing (a long one scrolls left) */
	rbox(6, 35, W - 12, 30, RGB(150, 156, 168));
	rbox(7, 36, W - 14, 28, RGB(22, 24, 30));
	const char *t = text;
	int maxw = W - 34;
	while (*t && gfx_text_w(t, 1) > maxw) { t++; while ((*t & 0xC0) == 0x80) t++; }
	int tx = 14;
	if (*text) { gfx_text(tx, 39, t, C_WHITE, 1); tx += gfx_text_w(t, 1) + 1; }
	else if (placeholder) gfx_text_clip(tx + 4, 39, placeholder, RGB(110, 116, 128), maxw - 4);
	gfx_fill(tx, 39, 2, 22, C_ORANGE);	/* the caret */
	for (int r = 0; r < KBD_ROWS; r++) {
		struct kbd_key k[KBD_COLS];
		int n = kbd_row(mode, upper, r, k);
		for (int i = 0; i < n; i++) {
			int x = 1 + k[i].col * KB_W, y = KB_Y0 + r * KB_H, w = k[i].span * KB_W - 2, h = KB_H - 2, on = r == row && i == key;
			const char *lb = k[i].ch;
			uint16_t fg = C_WHITE, bg = r < 4 ? RGB(46, 50, 60) : RGB(34, 37, 46);
			switch (k[i].kind) {
			case KB_SHIFT: lb = upper ? "abc" : "ABC"; fg = upper ? C_ORANGE : C_GREY; break;
			case KB_MODE: lb = mode ? "abc" : "123"; fg = C_GREY; break;
			case KB_SPACE: lb = "espacio"; fg = C_GREY; break;
			case KB_DEL: lb = "Borrar"; fg = C_GREY; break;
			case KB_OK: lb = ok_label ? ok_label : "OK"; fg = C_ORANGE; break;
			}
			if (on) {	/* the selected key in Sony's orange, like a list's selection bar */
				rbox(x, y, w, h, RGB(255, 170, 0));
				gfx_vgrad(x + 2, y + 1, w - 4, h / 2, RGB(255, 222, 140), RGB(255, 186, 40));
				fg = C_BLACK;
			} else rbox(x, y, w, h, bg);
			gfx_text_center(x + w / 2, y + (h - 22) / 2, lb, fg, 1);
		}
	}
}

/* ---------- a podcast's card ---------- */

void scr_pod_show(const char *name, const char *author, const char *info, struct img cover, const char *const *items, int n, int sel)
{
	background();
	scr_status("Podcast");
	int cx = 8, cy = 36;
	if (cover.w == POD_ART) gfx_blit(cover, cx, cy);
	else { gfx_fill(cx, cy, POD_ART, POD_ART, RGB(46, 50, 60)); blit_mask(gfx_tex(T_NOTE), cx + POD_ART / 2 - 9, cy + POD_ART / 2 - 9, C_GREY); }
	int tx = cx + POD_ART + 10, tw = W - tx - 6;
	int lines = gfx_text_wrap(tx, cy, name ? name : "", C_WHITE, tw, 3, 22);
	if (author && *author) gfx_text_wrap(tx, cy + lines * 22 + 2, author, C_GREY, tw, lines >= 3 ? 1 : 2, 22);
	if (info && *info) gfx_text_clip(cx, cy + POD_ART + 6, info, C_ORANGE, W - 16);
	int y0 = cy + POD_ART + 34;
	for (int i = 0; i < n; i++) {
		int y = y0 + i * LIST_ROW_H;
		if (y + LIST_ROW_H > BOTTOM_Y) break;
		if (i == sel) { sel_bar(y, 0); blit_mask(gfx_tex(T_PLAYBOX), 6, y + 5, C_BLACK); }
		gfx_text_clip(i == sel ? 29 : 8, y + 3, items[i], i == sel ? C_BLACK : C_WHITE, W - 40);
	}
}
