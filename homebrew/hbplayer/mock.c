/* mock.c: design proposals for a more modern UI, drawn with the player's real drawing code (Sony font, Sony
 * textures, 240x320 RGB565) so they show exactly what the screen can do. PC only, not part of the player.
 * Rules for this screen: only the Sony font (22 px line), solid shapes, lines >= 2 px, no blur or glow,
 * covers only at >= 44 px. usage: mock RESDIR OUTDIR COVERDIR (coverN.raw: RGB565 160x160, 0..8) */
#include "screens.c"

void lg(const char *fmt, ...) { (void)fmt; }
void hb_alive(void) {}
static int dl_on;
int dl_active(void) { return dl_on; }
int dl_percent(void) { return 45; }

static const char *outdir;
static void save(const char *name)
{
	char p[512]; snprintf(p, sizeof p, "%s/%s.rgb565", outdir, name);
	FILE *f = fopen(p, "wb"); if (!f) return;
	fwrite(fb, 2, W * H, f); fclose(f);
}

#define SURF RGB(28, 30, 36)	/* raised surface (cards, rows) */
#define SURF_HI RGB(44, 46, 56)
#define TRACK RGB(56, 60, 72)

/* covers: 0 Corridor Cast, 1 Reel History, 2 Quiroga, 3.. Alter Bridge albums (stand-ins) */
enum { C_CC, C_RH, C_QU, C_AB35, C_BLACKBIRD, C_FORTRESS, C_PAWNS, C_HERO, C_SKY, NCOV };
static uint16_t cov[NCOV][160 * 160];
static uint16_t sbuf[4][160 * 160];

/* filled rectangle with 3 px rounded corners (drawn by trimming the corner pixels) */
static void rbox(int x, int y, int w, int h, uint16_t c)
{
	static const int cut[3] = { 2, 1, 1 };
	for (int j = 0; j < h; j++) {
		int k = j < 3 ? cut[j] : j >= h - 3 ? cut[h - 1 - j] : 0;
		gfx_fill(x + k, y + j, w - 2 * k, 1, c);
	}
}

/* 4 px progress bar with rounded ends */
static void pbar(int x, int y, int w, int pct, uint16_t fg)
{
	rbox(x, y, w, 4, TRACK);
	int f = w * pct / 100;
	if (f > 2) rbox(x, y, f, 4, fg);
}

static void tri(int x, int y, int dir, uint16_t c)	/* solid 6x11 triangle pointing left (-1) or right (+1) */
{
	for (int j = 0; j < 11; j++) { int l = 6 - (j < 6 ? 5 - j : j - 5); gfx_fill(dir > 0 ? x : x + 6 - l, y + j, l, 1, c); }
}

static struct img cover_at(int k, int size, int slot)	/* a cover at size x size */
{
	struct img im = { 160, 160, cov[k] };
	return gfx_scaled(im, size, size, sbuf[slot]);
}

/* blit clipped to the screen, darkened to 1/div (the covers beside the selected one) */
static void blit_dim(struct img im, int x, int y, int div)
{
	for (int j = 0; j < im.h; j++) for (int i = 0; i < im.w; i++) {
		int xx = x + i, yy = y + j;
		if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
		uint16_t p = im.px[j * im.w + i];
		fb[yy * W + xx] = (uint16_t)((((p >> 11) / div) << 11) | ((((p >> 5) & 63) / div) << 5) | ((p & 31) / div));
	}
}

/* Sony's progress bar textures (29/3 track, 29/1 fill) at any y, pct 0..100 */
static void sony_prog(int y, int pct)
{
	struct img tr = gfx_tex(T_PROG_BG), fi = gfx_tex(T_PROG);
	int pw = W - 4, fill = pw * pct / 100;
	if (tr.w) gfx_blit(tr, 2, y); else gfx_fill(2, y, pw, 8, RGB(60, 66, 80));
	if (fi.w) for (int j = 0; j < fi.h; j++) memcpy(fb + (y + j) * W + 2, fi.px + j * fi.w, (fill < fi.w ? fill : fi.w) * 2);
	else gfx_fill(2, y, fill, 8, C_BLUE);
}

/* small solid badge with a number, overlapping the top-right corner of a home icon */
static void badge(int x, int y, const char *n)
{
	rbox(x - 1, y - 1, 22, 22, C_BLACK);	/* 1 px black ring separates it from the icon */
	rbox(x, y, 20, 20, C_ORANGE);
	gfx_text_center(x + 10, y - 1, n, C_BLACK, 1);
}

/* the bounding box of the lit pixels of a Sony home icon frame */
static void icon_box(struct img im, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = im.w; *y0 = im.h; *x1 = 0; *y1 = 0;
	for (int j = 0; j < im.h; j++) for (int i = 0; i < im.w; i++) {
		uint16_t p = im.px[j * im.w + i];
		if ((p >> 11) + ((p >> 5) & 63) / 2 + (p & 31) < 20) continue;
		if (((p >> 5) & 63) == 0 && (p >> 11) <= 4 && (p & 31) <= 20) continue;	/* baked blue band */
		if (i < *x0) *x0 = i; if (i > *x1) *x1 = i; if (j < *y0) *y0 = j; if (j > *y1) *y1 = j;
	}
}

/* ---------------- Inicio (chosen, corrected) ---------------- */
static void inicio(void)
{
	int fr[HOME_SLOTS];
	for (int i = 0; i < HOME_SLOTS; i++) fr[i] = 8;
	bg_fill(0, TITLE_H, W, BOTTOM_Y - TITLE_H, 1);
	title_bar("Música");
	rbox(HOME_CELL_X(4) + 2, HOME_CELL_Y(4) + 1, HOME_CELL_W - 4, HOME_CELL_H - 2, SURF_HI);
	gfx_fill(HOME_CELL_X(4) + 20, HOME_CELL_Y(4) + HOME_CELL_H - 5, HOME_CELL_W - 40, 3, C_ORANGE);
	fr[4] = 0;
	for (int i = 0; i < HOME_SLOTS; i++) if (home_src[i] >= 0) home_cell(i, fr[i]);
	/* badge on the podcast icon itself: over its top-right corner */
	int x0, y0, x1, y1;
	icon_box(gfx_anim(home_src[2] - 100 + 8), &x0, &y0, &x1, &y1);
	badge(HOME_CELL_X(2) + x1 - 12, HOME_CELL_Y(2) + y0 - 6, "2");
	/* the "now playing" card takes the place of the status bar: cover, title, artist, play state + progress */
	rbox(4, 248, 232, 68, SURF);
	gfx_blit(cover_at(C_AB35, 60, 0), 8, 252);
	gfx_text_clip(76, 248, "Slip to the Void", C_WHITE, 156);
	gfx_text_clip(76, 270, "Alter Bridge", C_GREY, 156);
	for (int j = 0; j < 11; j++) gfx_fill(77, 297 + j, j < 6 ? j + 1 : 11 - j, 1, C_GREEN);	/* playing */
	pbar(92, 300, 136, 24, C_ORANGE);
	/* battery moves up to the title bar (Sony's icon, keyed off its status-bar background) */
	{	/* Sony's own battery icon, moved to the title bar: keyed off its status-bar background like
		 * blit_barkey, with the same 1 px dark outline the title text has on the silver bar */
		static const int items[] = { 2, 4, 6, 8, 10, 12 };
		struct img im = gfx_tex(23, items[ui_st.batt >= 95 ? 5 : ui_st.batt / 20]), bar = gfx_tex(T_BOTTOM);
		int bx = W - 30, by = 2;
		static unsigned char on[32][32];
		for (int j = 0; j < im.h && j < 32; j++) for (int i = 0; i < im.w && i < 32; i++) {
			uint16_t p = im.px[j * im.w + i], q = bar.w ? bar.px[(5 + j) * bar.w + (bx + i < bar.w ? bx + i : bar.w - 1)] : 0;
			int dr = (p >> 11) - (q >> 11), dg = ((p >> 5) & 63) - ((q >> 5) & 63), db = (p & 31) - (q & 31);
			on[j][i] = !(dr >= -1 && dr <= 1 && dg >= -2 && dg <= 2 && db >= -1 && db <= 1);
		}
		for (int j = 0; j < im.h && j < 32; j++) for (int i = 0; i < im.w && i < 32; i++) if (on[j][i])
			for (int d = 0; d < 4; d++) { int x = bx + i + (d == 0) - (d == 1), y = by + j + (d == 2) - (d == 3); fb[y * W + x] = RGB(38, 42, 52); }
		for (int j = 0; j < im.h && j < 32; j++) for (int i = 0; i < im.w && i < 32; i++) if (on[j][i]) fb[(by + j) * W + bx + i] = im.px[j * im.w + i];
	}
	save("m1_inicio");
}

/* ---------------- Álbumes ---------------- */
static const int ab[7] = { C_BLACKBIRD, C_AB35, C_FORTRESS, C_PAWNS, C_HERO, C_SKY, C_AB35 };
static const char *ab_n[7] = { "Blackbird", "AB III.5", "Fortress", "Pawns & Kings", "The Last Hero", "Walk the Sky", "Live at Wembley" };

/* A: carousel — the selected cover big in the middle, the neighbours peeking in darkened at the sides */
static void albumes_a(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Álbumes");
	blit_dim(cover_at(ab[0], 104, 0), -66, 52, 3);
	blit_dim(cover_at(ab[2], 104, 1), 202, 52, 3);
	rbox(46, 34, 148, 148, C_ORANGE);
	gfx_blit(cover_at(ab[1], 140, 2), 50, 38);
	gfx_text_center(W / 2, 188, ab_n[1], C_WHITE, 1);
	gfx_text_center(W / 2, 210, "Alter Bridge", C_GREY, 1);
	gfx_text_center(W / 2, 232, "2011 · 14 canciones", C_GREY, 1);
	for (int i = 0; i < 7; i++) rbox(W / 2 - 45 + i * 13, 264, 8, 8, i == 1 ? C_ORANGE : TRACK);	/* position dots */
	bottom_now();
	save("a1_albumes_carrusel");
}

/* B: 3x3 wall of covers; the name of the selected one in the title bar, its details at the bottom */
static void albumes_b(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("AB III.5");
	for (int i = 0; i < 9; i++) {
		int x = 6 + (i % 3) * 78, y = 32 + (i / 3) * 78, sel = i == 1;
		if (sel) rbox(x - 3, y - 3, 78, 78, C_ORANGE);
		gfx_blit(cover_at(ab[i % 7], 72, 0), x, y);
	}
	gfx_text_center(W / 2, 264, "2011 · 14 canciones", C_GREY, 1);
	bottom_now();
	save("a2_albumes_mosaico");
}

/* ---------------- Reproduciendo ---------------- */
/* A: Sony's layout, modernised: cover left, big Sony digits for the time on the right, then the info lines
 * with Sony's own icons at full width and Sony's progress bar */
static void repro_a(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Canciones");
	gfx_blit(cover_at(C_AB35, 112, 0), 6, 34);
	big_digits(180, 40, "1:11");
	gfx_text_center(180, 94, "3 / 14", C_GREY, 1);
	rbox(130, 122, 100, 22, SURF_HI);
	gfx_text_center(180, 122, "Heavy", C_ORANGE, 1);
	gfx_fill(0, 152, W, 2, RGB(70, 74, 86));
	info_line(158, gfx_tex(T_NOTE), "Ghost of Days Gone By");
	info_line(182, gfx_tex(T_PERSON), "Alter Bridge");
	info_line(206, gfx_tex(T_DISC), "AB III.5");
	info_line(230, gfx_tex(T_GENRE), "Alternative");
	sony_prog(262, 30);
	bottom_bar(BB_TEXT, "-2:42", C_WHITE);
	save("r1_repro_sony");
}

/* B: cover as large as the screen allows, title/artist under it, Sony's bar, the times in the status bar */
static void repro_b(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("3 / 14");
	gfx_blit(cover_at(C_AB35, 180, 0), 30, 30);
	gfx_text_clip(8, 212, "Ghost of Days Gone By", C_WHITE, W - 16);
	gfx_text_clip(8, 234, "Alter Bridge · AB III.5", C_GREY, W - 16);
	sony_prog(264, 30);
	bottom_bar(BB_TEXT, "1:11 / 3:53", C_WHITE);
	save("r2_repro_portada");
}

/* ---------------- Episodios ---------------- */
static const char *ep_t[5] = { "EP#249 We're going to Break a Guinness World Record", "EP#248 Jordan Allen's First Week", "EP#247 YouTuber Movies Are KILLING it", "EP#246 We Made a SPLASH at NAB", "EP#245 The VFX of Dune" };
static const char *ep_m[5] = { "25 jun · 1 h 12 min", "12 jun · 58 min", "quedan 22 min", "escuchado", "escuchado" };

/* A: rows with the episode's cover (48 px); "new" is a badge on the cover, like the home icon */
static void episodios_a(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Corridor Cast");
	for (int i = 0; i < 5; i++) {
		int y = 32 + i * 51, sel = i == 1, heard = i >= 3;
		if (sel) sel_bar_h(y, 1, 50);
		struct img c = cover_at(C_CC, 46, 0);
		if (heard) blit_dim(c, 4, y + 2, 2); else gfx_blit(c, 4, y + 2);
		if (i < 2) { rbox(38, y, 14, 14, C_BLACK); rbox(39, y + 1, 12, 12, C_ORANGE); }
		uint16_t tc = sel ? C_BLACK : heard ? RGB(120, 124, 132) : C_WHITE;
		gfx_text_clip(56, y + 3, ep_t[i], tc, W - 56 - 12);
		uint16_t mc = sel ? RGB(60, 40, 0) : heard ? RGB(100, 104, 112) : C_GREY;
		gfx_text_clip(56, y + 25, ep_m[i], mc, W - 56 - 12);
		if (i == 2) { int mx = 56 + gfx_text_w(ep_m[i], 1) + 8; pbar(mx, y + 35, W - 14 - mx, 62, C_ORANGE); }
	}
	gfx_fill(W - 5, 32, 3, 255, RGB(50, 56, 70));	/* scrollbar like Sony's */
	gfx_vgrad(W - 5, 32, 3, 120, RGB(190, 220, 255), RGB(110, 160, 230));
	bottom_now();
	save("e1_episodios_portadas");
}

/* B: Sony's one-line list (more episodes at once) with the state on the right, and a detail panel for the
 * selected episode */
static void episodios_b(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Corridor Cast");
	static const char *st[5] = { "nuevo", "nuevo", "62%", "", "" };
	for (int i = 0; i < 5; i++) {
		int y = LIST_Y0 + i * LIST_ROW_H, sel = i == 0, heard = i >= 3;
		if (sel) sel_bar(y, 0);
		int right = W - 8;
		if (*st[i]) { int w = gfx_text_w(st[i], 1); gfx_text(right - w, y + 3, st[i], sel ? C_BLACK : C_ORANGE, 1); right -= w + 8; }
		else { blit_mask(gfx_tex(T_ARROW), right - 18, y + 5, sel ? C_BLACK : RGB(90, 94, 104)); right -= 24; }
		gfx_text_clip(8, y + 3, ep_t[i], sel ? C_BLACK : heard ? RGB(120, 124, 132) : C_WHITE, right - 8);
	}
	rbox(4, 176, 232, 108, SURF);
	gfx_text_wrap(10, 178, ep_t[0], C_WHITE, 220, 2, 22);
	gfx_text(10, 224, "25 jun · 1 h 12 min", C_GREY, 1);
	blit_mask(gfx_tex(T_CHAP), 10, 253, C_ORANGE);
	gfx_text(32, 250, "12 capítulos", C_ORANGE, 1);
	bottom_now();
	save("e2_episodios_lista");
}

/* ---------------- Ecualizador ---------------- */
static const char *eq_l[6] = { "Clear Bass", "400 Hz", "1 kHz", "2,5 kHz", "6,3 kHz", "16 kHz" };
static const int eq_v[6] = { 2, 1, 0, -1, 1, 2 };

static void preset_line(int y)
{
	rbox(4, y, 232, 26, SURF);
	tri(10, y + 7, -1, C_GREY); tri(W - 16, y + 7, 1, C_GREY);
	gfx_text_center(W / 2, y + 2, "Personalizado", C_ORANGE, 1);
}

/* A: one band per row, like a Sony list: the name, a horizontal bar that fills from the centre, the value */
static void eq_a(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Ecualizador");
	preset_line(31);
	for (int b = 0; b < 6; b++) {
		int y = 62 + b * 37, sel = b == 3, cx = 150, step = 18, v = eq_v[b];
		if (sel) sel_bar_h(y, 0, 34); else rbox(2, y, W - 4, 34, SURF);
		uint16_t fg = sel ? C_BLACK : C_WHITE, fillc = sel ? C_BLACK : C_ORANGE, tr = sel ? RGB(200, 120, 0) : TRACK;
		gfx_text(8, y + 6, eq_l[b], fg, 1);
		int lo = b == 0 ? cx : cx - 3 * step;	/* Clear Bass only goes up */
		gfx_fill(lo, y + 15, cx + 3 * step - lo, 4, tr);
		gfx_fill(cx - 1, y + 9, 2, 16, sel ? C_BLACK : C_GREY);	/* the 0 mark */
		if (v > 0) gfx_fill(cx, y + 13, v * step, 8, fillc);
		if (v < 0) gfx_fill(cx + v * step, y + 13, -v * step, 8, fillc);
		char s[8]; snprintf(s, sizeof s, v > 0 ? "+%d" : "%d", v);
		gfx_text(W - 8 - gfx_text_w(s, 1), y + 6, s, sel ? C_BLACK : v ? C_ORANGE : C_GREY, 1);
	}
	bottom_bar(BB_TEXT, "PLAY: guardar", C_WHITE);
	save("q1_eq_filas");
}

/* B: Sony's own EQ panel texture, kept whole, with the preset on top and the band being edited spelled out
 * big under it */
static void eq_b(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Ecualizador");
	preset_line(31);
	int py = 64;
	struct img panel = gfx_tex(16, 6);
	if (panel.w) gfx_blit(panel, 0, py); else gfx_fill(0, py, W, 104, RGB(72, 74, 72));
	static const int bx[5] = { 87, 115, 143, 171, 199 };
	struct img kw = gfx_tex(16, 2), ko = gfx_tex(16, 3);
	for (int b = 0; b < 6; b++) {
		int x = b == 0 ? 41 : bx[b - 1], y = py + 42 - eq_v[b] * 9 - 3;
		struct img k = b == 3 ? ko : kw;
		if (k.w) gfx_blit(k, x + 1 - 10, y);
	}
	rbox(4, 176, 232, 106, SURF);
	gfx_text_center(W / 2, 180, "2,5 kHz", C_WHITE, 1);
	gfx_text_center(W / 2, 208, "-1", C_ORANGE, 2);
	tri(40, 222, -1, C_GREY); tri(W - 46, 222, 1, C_GREY);
	gfx_text_center(W / 2, 258, "arriba/abajo: nivel", C_GREY, 1);
	bottom_bar(BB_TEXT, "PLAY: guardar", C_WHITE);
	save("q2_eq_panel_sony");
}

/* ---------------- Descargas (chosen, unchanged) ---------------- */
static void descargas(void)
{
	bg_fill(0, 0, W, H, 0);
	title_bar("Descargas");
	rbox(4, 31, 232, 92, SURF);
	gfx_text(12, 30, "45%", C_ORANGE, 2);
	gfx_text(W - 12 - gfx_text_w("1 de 3", 1), 34, "1 de 3", C_GREY, 1);
	gfx_text(W - 12 - gfx_text_w("28/63 MB", 1), 56, "28/63 MB", C_WHITE, 1);
	gfx_text_clip(12, 78, "EP#249 We're going to Break a Guinness World Record", C_WHITE, 216);
	pbar(12, 108, 216, 45, C_ORANGE);
	gfx_text(8, 128, "En cola", C_GREY, 1);
	static const char *q[2] = { "The Odyssey (2026) Bronze Age Collapse", "EP#250 Behind the Scenes" };
	static const char *qs[2] = { "Reel History · 76 MB", "Corridor Cast · 58 MB" };
	for (int i = 0; i < 2; i++) {
		int y = 152 + i * 52;
		rbox(4, y, 232, 48, SURF);
		gfx_blit(cover_at(i == 0 ? C_RH : C_CC, 44, 0), 6, y + 2);
		gfx_text_clip(58, y + 2, q[i], C_WHITE, 172);
		gfx_text_clip(58, y + 24, qs[i], C_GREY, 172);
	}
	gfx_fill(10, 266, 3, 9, C_GREEN); gfx_fill(13, 272, 3, 3, C_GREEN);
	for (int j = 0; j < 7; j++) gfx_fill(16 + j, 271 - j, 2, 2, C_GREEN);
	gfx_text_clip(30, 260, "Capítulos de EP#249 listos", C_GREY, 200);
	dl_on = 1; bottom_bar(BB_TEXT, "131 kB/s · 4 min", C_WHITE); dl_on = 0;
	save("m6_descargas");
}

int main(int argc, char **argv)
{
	if (argc < 4) return 1;
	outdir = argv[2];
	gfx_open();
	gfx_load_res(argv[1]);
	for (int k = 0; k < NCOV; k++) {
		char p[512]; snprintf(p, sizeof p, "%s/cover%d.raw", argv[3], k);
		FILE *f = fopen(p, "rb");
		if (!f || fread(cov[k], 2, 160 * 160, f) != 160 * 160) for (int i = 0; i < 160 * 160; i++) cov[k][i] = RGB(60 + k * 20, 50, 120);
		if (f) fclose(f);
	}
	ui_st.batt = 72; ui_st.playing = 1; ui_st.np_title = "Slip to the Void";
	inicio();
	albumes_a(); albumes_b();
	repro_a(); repro_b();
	episodios_a(); episodios_b();
	eq_a(); eq_b();
	descargas();
	return 0;
}
