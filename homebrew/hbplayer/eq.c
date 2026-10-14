/* eq.c: sound effects in integer math for the ARM926 (no FPU):
 *   - 5-band graphic EQ (60 Hz, 250 Hz, 1 kHz, 4 kHz, 12 kHz; peaking biquads, -6..+6 dB per band)
 *   - "Bajos" (bass boost: low shelf +0/3/6 dB below 120 Hz)
 *   - "Estéreo amplio" (stereo widening, the idea behind Sony's Clear Stereo / SRS WOW: the side signal is
 *     boosted and slightly delayed, the mid stays put, so the image opens up on headphones)
 * Biquads run in direct form I with Q28 coefficients and 64-bit accumulators; the coefficient design
 * (needs sin/cos/pow) runs only when a setting changes. A 6 dB headroom pre-gain avoids clipping. */
#include "hb.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

struct eq_settings eq = { 0, { 0, 0, 0, 0, 0 }, 0, 0 };

#define NB 5
static const double band_hz[NB] = { 60, 250, 1000, 4000, 12000 };
static const double band_q[NB] = { 0.9, 0.9, 0.9, 0.9, 0.9 };
#define QC 28

/* state in 32-bit ints (samples; e = the remainder of the last shift, < 2^28): every product is one ARM SMULL/SMLAL
 * (32x32 -> 64). With 64-bit state each product took several instructions: 5 bands cost ~23% of the CPU. */
struct biq { int b0, b1, b2, a1, a2; int x1[2], x2[2], y1[2], y2[2], e[2]; int on; };
static struct biq bands[NB], bass;
static int cur_rate;
static int pre_gain = 4096;	/* Q12: 4096 = 0 dB */
static int wide_on, wide_gain;	/* Q12 side gain */
#define WD 24			/* side delay in samples (~0.5 ms at 44.1 kHz) */
static int side_hist[64];
static int side_pos;

/* headphone effects (Ajustes > Auriculares):
 *  - crossfeed: each ear also gets the other channel, low-passed (700 Hz) and 0.3 ms late at -9 dB, the way it
 *    reaches the far ear from speakers (after Bauer / bs2b): hard-panned old stereo stops feeling "inside the head"
 *  - graves a volumen bajo: loudness compensation (equal-loudness contours): the lower the volume, the more bass
 *    (and a little treble) is added back, up to +6 dB
 *  - perfil de auriculares: an AutoEq parametric profile (H:\HB\auriculares.txt, "ParametricEQ.txt" format:
 *    "Preamp: -6.2 dB" and "Filter N: ON PK|LSC|HSC Fc 105 Hz Gain 5.6 dB Q 0.70"), up to 10 filters */
int hp_xfeed, hp_loud, hp_autoeq;
#define AUTOEQ_FILE "/contents/HB/auriculares.txt"
#define NAQ 10
static struct biq aq[NAQ], loud_lo, loud_hi;
static int naq, aq_loaded, loud_vol = -1;
static double aq_pre;
static int xf_lp[2], xf_hist[2][32], xf_pos, xf_a;	/* crossfeed: one-pole low-pass state, delay line */
#define XF_D 13			/* ~0.3 ms at 44.1 kHz */
#define XF_FEED 11600		/* Q15: -9 dB */
#define XF_NORM 24270		/* Q15: 1 / (1 + feed), so a mono signal keeps its low-frequency level */
#define XF_FN (XF_FEED * XF_NORM >> 15)

static void design_peak(struct biq *b, double f0, double q, double gain_db, int rate)
{
	double A = pow(10, gain_db / 40), w0 = 2 * M_PI * f0 / rate, alpha = sin(w0) / (2 * q), c = cos(w0);
	double b0 = 1 + alpha * A, b1 = -2 * c, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * c, a2 = 1 - alpha / A;
	double s = (double)(1 << QC) / a0;
	b->b0 = (int)(b0 * s); b->b1 = (int)(b1 * s); b->b2 = (int)(b2 * s); b->a1 = (int)(a1 * s); b->a2 = (int)(a2 * s);
	b->on = gain_db != 0;
}

static void design_lowshelf(struct biq *b, double f0, double gain_db, int rate)
{
	double A = pow(10, gain_db / 40), w0 = 2 * M_PI * f0 / rate, c = cos(w0), sq = sqrt(A);
	double alpha = sin(w0) / 2 * sqrt(2);	/* S = 1 */
	double b0 = A * ((A + 1) - (A - 1) * c + 2 * sq * alpha), b1 = 2 * A * ((A - 1) - (A + 1) * c), b2 = A * ((A + 1) - (A - 1) * c - 2 * sq * alpha);
	double a0 = (A + 1) + (A - 1) * c + 2 * sq * alpha, a1 = -2 * ((A - 1) + (A + 1) * c), a2 = (A + 1) + (A - 1) * c - 2 * sq * alpha;
	double s = (double)(1 << QC) / a0;
	b->b0 = (int)(b0 * s); b->b1 = (int)(b1 * s); b->b2 = (int)(b2 * s); b->a1 = (int)(a1 * s); b->a2 = (int)(a2 * s);
	b->on = gain_db != 0;
}

static void set_coefs(struct biq *b, double b0, double b1, double b2, double a0, double a1, double a2, int on)
{
	double s = (double)(1 << QC) / a0;
	b->b0 = (int)(b0 * s); b->b1 = (int)(b1 * s); b->b2 = (int)(b2 * s); b->a1 = (int)(a1 * s); b->a2 = (int)(a2 * s);
	b->on = on;
}

/* RBJ cookbook with an explicit Q: 'P' peak, 'L' low shelf, 'H' high shelf */
static void design_q(struct biq *b, char type, double f0, double gain_db, double q, int rate)
{
	if (f0 >= rate / 2.0) f0 = rate / 2.0 - 100;
	double A = pow(10, gain_db / 40), w0 = 2 * M_PI * f0 / rate, c = cos(w0), alpha = sin(w0) / (2 * q), sq2 = 2 * sqrt(A) * alpha;
	if (type == 'P') set_coefs(b, 1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A, gain_db != 0);
	else if (type == 'L') set_coefs(b, A * ((A + 1) - (A - 1) * c + sq2), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq2),
		(A + 1) + (A - 1) * c + sq2, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq2, gain_db != 0);
	else set_coefs(b, A * ((A + 1) + (A - 1) * c + sq2), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq2),
		(A + 1) - (A - 1) * c + sq2, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq2, gain_db != 0);
}

static double aq_param[NAQ][3]; static char aq_type[NAQ];	/* Fc, gain, Q and type of each AutoEq filter */

/* the AutoEq profile (0 = no usable file); read once, again after autoeq_reload(); the filters are designed per
 * sample rate in eq_apply */
void autoeq_reload(void) { aq_loaded = 0; }
int autoeq_load(void)
{
	if (aq_loaded) return naq;
	aq_loaded = 1; naq = 0; aq_pre = 0;
	FILE *f = fopen(AUTOEQ_FILE, "r");
	if (!f) return 0;
	char l[160];
	while (fgets(l, sizeof l, f) && naq < NAQ) {
		double a, b, c; char on[8], ty[8]; int k;
		if (sscanf(l, "Preamp: %lf", &a) == 1) { aq_pre = a; continue; }
		if (sscanf(l, "Filter %d: %7s %7s Fc %lf Hz Gain %lf dB Q %lf", &k, on, ty, &a, &b, &c) == 6 && !strcmp(on, "ON")) {
			char t = !strcmp(ty, "PK") ? 'P' : !strncmp(ty, "LS", 2) ? 'L' : !strncmp(ty, "HS", 2) ? 'H' : 0;
			if (!t || a <= 0 || c <= 0) continue;
			aq_type[naq] = t; aq_param[naq][0] = a; aq_param[naq][1] = b; aq_param[naq][2] = c; naq++;
		}
	}
	fclose(f);
	lg("eq: AutoEq profile, %d filters, preamp %.1f dB", naq, aq_pre);
	return naq;
}
static void loud_design(int rate)
{
	loud_vol = volume;
	double boost = (VOLMAX - volume) * 0.45;	/* 1.5 dB per volume step -> ~0.45 dB of bass per step */
	if (boost > 6) boost = 6;
	if (!hp_loud) boost = 0;
	design_q(&loud_lo, 'L', 100, boost, 0.7, rate);
	design_q(&loud_hi, 'H', 10000, boost / 3, 0.7, rate);
}

void eq_reset(void)
{
	for (int i = 0; i < NB; i++) memset(bands[i].x1, 0, sizeof bands[i].x1 * 5);
	memset(bass.x1, 0, sizeof bass.x1 * 5);
	memset(side_hist, 0, sizeof side_hist); side_pos = 0;
	for (int i = 0; i < NAQ; i++) memset(aq[i].x1, 0, sizeof aq[i].x1 * 5);
	memset(loud_lo.x1, 0, sizeof loud_lo.x1 * 5); memset(loud_hi.x1, 0, sizeof loud_hi.x1 * 5);
	memset(xf_lp, 0, sizeof xf_lp); memset(xf_hist, 0, sizeof xf_hist); xf_pos = 0;
}

/* (re)compute everything for the current settings and sample rate; cheap enough to call on every change */
void eq_apply(int rate)
{
	cur_rate = rate;
	double maxdb = 0;
	for (int i = 0; i < NB; i++) { design_peak(&bands[i], band_hz[i], band_q[i], eq.on ? eq.band[i] : 0, rate); if (eq.on && eq.band[i] > maxdb) maxdb = eq.band[i]; }
	design_lowshelf(&bass, 120, eq.bass * 3.0, rate);
	if (eq.bass * 3.0 > maxdb) maxdb = eq.bass * 3.0;
	wide_on = eq.wide;
	wide_gain = eq.wide ? 4096 * 3 / 2 : 4096;
	/* headphone effects */
	if (hp_autoeq && autoeq_load()) {
		for (int i = 0; i < naq; i++) design_q(&aq[i], aq_type[i], aq_param[i][0], aq_param[i][1], aq_param[i][2], rate);
	} else for (int i = 0; i < NAQ; i++) aq[i].on = 0;
	loud_design(rate);
	if (hp_loud && (VOLMAX - volume) * 0.45 > maxdb) maxdb = (VOLMAX - volume) * 0.45 > 6 ? 6 : (VOLMAX - volume) * 0.45;
	xf_a = (int)(32768 * (1 - exp(-2 * M_PI * 700 / rate)));	/* one-pole low-pass coefficient (Q15) */
	/* headroom: pull the level down by the largest boost (max 9 dB) so peaks can't clip */
	double pre = -maxdb - (wide_on ? 1.5 : 0) + (hp_autoeq && naq ? aq_pre : 0);	/* AutoEq brings its own preamp */
	if (pre > 0) pre = 0;
	pre_gain = (int)(pow(10, pre / 20) * 4096);
	eq_reset();
}

/* estimated CPU of the active filters, % at a true 124 MHz (measured on the device 2026-09-29: one stereo biquad
 * ~4.6 %, crossfeed ~8.6 %, loudness (2 biquads + redesign) ~14 %) */
/* the active effects by name, for the sound menu's "why not Máxima batería" */
void eq_effects_desc(char *out, int max)
{
	out[0] = 0;
	#define ADD(cond, name) if (cond) { int l = (int)strlen(out); snprintf(out + l, max - l, "%s%s", l ? ", " : "", name); }
	ADD(eq.on, "ecualizador")
	ADD(eq.bass, "realce de bajos")
	ADD(eq.wide, "estéreo amplio")
	ADD(hp_xfeed, "crossfeed")
	ADD(hp_loud, "graves a volumen bajo")
	ADD(hp_autoeq && naq, "perfil de auriculares")
	#undef ADD
}

int eq_load_pct(void)
{
	int l = 0;
	if (eq.on) for (int i = 0; i < NB; i++) if (eq.band[i]) l += 5;
	if (eq.bass) l += 5;
	if (eq.wide) l += 3;
	if (hp_xfeed) l += 9;
	if (hp_loud) l += 14;
	if (hp_autoeq) l += 5 * naq;
	return l;
}

int eq_active(void) { return eq.on || eq.bass || eq.wide || hp_xfeed || hp_loud || (hp_autoeq && naq); }

static inline int run_biq(struct biq *b, int ch, int x)
{
	/* error feedback: the part cut off by the shift goes into the next sample. Without it, a low-frequency filter
	 * (poles near DC) amplifies the truncation bias thousands of times into a large DC offset. */
	long long acc = (long long)b->b0 * x + (long long)b->b1 * b->x1[ch] + (long long)b->b2 * b->x2[ch]
		- (long long)b->a1 * b->y1[ch] - (long long)b->a2 * b->y2[ch] + b->e[ch];
	int y = (int)(acc >> QC);
	b->e[ch] = (int)acc & ((1 << QC) - 1);	/* acc - (y << QC): the low 28 bits */
	b->x2[ch] = b->x1[ch]; b->x1[ch] = x;
	b->y2[ch] = b->y1[ch]; b->y1[ch] = y;
	return y;
}

/* in place, interleaved stereo S16 */
void eq_process(short *pcm, int frames, int rate)
{
	if (!eq_active()) return;
	if (rate != cur_rate) eq_apply(rate);
	if (hp_loud && volume != loud_vol) eq_apply(rate);	/* the loudness curve follows the volume */
	for (int i = 0; i < frames; i++) {
		int l = (pcm[2 * i] * pre_gain) >> 12, r = (pcm[2 * i + 1] * pre_gain) >> 12;
		for (int k = 0; k < NB; k++) if (bands[k].on) { l = run_biq(&bands[k], 0, l); r = run_biq(&bands[k], 1, r); }
		if (bass.on) { l = run_biq(&bass, 0, l); r = run_biq(&bass, 1, r); }
		for (int k = 0; k < naq; k++) if (aq[k].on) { l = run_biq(&aq[k], 0, l); r = run_biq(&aq[k], 1, r); }
		if (loud_lo.on) { l = run_biq(&loud_lo, 0, l); r = run_biq(&loud_lo, 1, r); }
		if (loud_hi.on) { l = run_biq(&loud_hi, 0, l); r = run_biq(&loud_hi, 1, r); }
		if (hp_xfeed) {
			xf_lp[0] += ((l - xf_lp[0]) * xf_a) >> 15; xf_lp[1] += ((r - xf_lp[1]) * xf_a) >> 15;
			int dl = xf_hist[0][(xf_pos - XF_D) & 31], dr = xf_hist[1][(xf_pos - XF_D) & 31];
			xf_hist[0][xf_pos & 31] = xf_lp[0]; xf_hist[1][xf_pos & 31] = xf_lp[1]; xf_pos++;
			int nl2 = (int)(((long long)l * XF_NORM + (long long)dr * XF_FN) >> 15), nr2 = (int)(((long long)r * XF_NORM + (long long)dl * XF_FN) >> 15);
			l = nl2; r = nr2;
		}
		if (wide_on) {
			int mid = (l + r) >> 1, side = (l - r) >> 1;
			int old = side_hist[(side_pos - WD) & 63];
			side_hist[side_pos & 63] = side; side_pos++;
			int ws = ((side + old / 2) * wide_gain) >> 12;
			l = mid + ws; r = mid - ws;
		}
		pcm[2 * i] = hb_clip(l); pcm[2 * i + 1] = hb_clip(r);
	}
}

/* presets: 60, 250, 1k, 4k, 12k */
const struct eq_preset eq_presets[] = {
	{ "Plano", { 0, 0, 0, 0, 0 } },
	{ "Rock", { 4, 2, -1, 2, 4 } },
	{ "Pop", { -1, 2, 4, 2, -1 } },
	{ "Jazz", { 3, 1, 0, 1, 3 } },
	{ "Voz / podcast", { -3, 0, 3, 4, 1 } },
	{ "Bajos", { 6, 3, 0, 0, 0 } },
	{ "Agudos", { 0, 0, 1, 3, 6 } },
};
const int eq_npresets = sizeof eq_presets / sizeof eq_presets[0];

int sony_vpt = -1, sony_eq, sony_dn, sony_dsee;
int sony_cb, sony_band[5], sony_cs;	/* Sony EQ Personalizado: Clear Bass 0..3, bands -3..+3; Clear Stereo 0..2 */	/* DSP effects: VPT -1 off / 0..5, EQ 0 off / 1..4 presets, normalizer */

void eq_load(const char *statedir)
{
	char fn[PATHLEN + 16]; snprintf(fn, sizeof fn, "%s/eq.txt", statedir);
	FILE *f = fopen(fn, "r");
	if (!f) return;
	int on, b[5], bass_, wide;
	if (fscanf(f, "%d %d %d %d %d %d %d %d", &on, &b[0], &b[1], &b[2], &b[3], &b[4], &bass_, &wide) == 8) {
		eq.on = !!on; eq.bass = bass_ < 0 ? 0 : bass_ > 2 ? 2 : bass_; eq.wide = !!wide;
		for (int i = 0; i < 5; i++) eq.band[i] = b[i] < -6 ? -6 : b[i] > 6 ? 6 : b[i];
	}
	int v, e, d, dsp, ds = 0;
	if (fscanf(f, " sony %d %d %d %d", &v, &e, &d, &dsp) == 4) {
		sony_vpt = v < -1 || v > 5 ? -1 : v; sony_eq = e < 0 || e > 5 ? 0 : e; sony_dn = !!d; dsp_enabled = !!dsp;
		if (fscanf(f, " %d", &ds) == 1) sony_dsee = !!ds;
		int dd, ver = 1;
		if (fscanf(f, " %d", &dd) == 1) dsp_direct = !!dd;
		if (fscanf(f, " %d", &ver) != 1) ver = 1;
		if (ver < 2) dsp_direct = 1;	/* v2 (2026-09-29): "Máxima batería" is the default, turned on once */
	}
	int ec, e1, e2, e3, e4, e5, ecs;	/* Sony EQ Personalizado and Clear Stereo */
	long pos = ftell(f);
	if (fscanf(f, " seq %d %d %d %d %d %d %d", &ec, &e1, &e2, &e3, &e4, &e5, &ecs) == 7) {
		sony_cb = ec < 0 ? 0 : ec > 3 ? 3 : ec; int b[5] = { e1, e2, e3, e4, e5 };
		for (int i = 0; i < 5; i++) sony_band[i] = b[i] < -3 ? -3 : b[i] > 3 ? 3 : b[i];
		sony_cs = 0; (void)ecs;	/* Clear Stereo: no audible effect on this unit, always off */
	} else fseek(f, pos, SEEK_SET);
	int ft, fb, fx, fl, fa;	/* voice and headphone effects */
	if (fscanf(f, " fx %d %d %d %d %d", &ft, &fb, &fx, &fl, &fa) == 5) {
		voice_trim_on = !!ft; voice_boost_on = !!fb; hp_xfeed = !!fx; hp_loud = !!fl; hp_autoeq = !!fa;
		if (hp_autoeq && !autoeq_load()) hp_autoeq = 0;	/* the profile file is gone */
	}
	fclose(f);
}

void eq_save(const char *statedir)
{
	char fn[PATHLEN + 16], tmp[PATHLEN + 24]; snprintf(fn, sizeof fn, "%s/eq.txt", statedir);
	FILE *f = safe_open(fn, tmp, sizeof tmp);
	if (!f) return;
	fprintf(f, "%d %d %d %d %d %d %d %d\n", eq.on, eq.band[0], eq.band[1], eq.band[2], eq.band[3], eq.band[4], eq.bass, eq.wide);
	fprintf(f, "sony %d %d %d %d %d %d %d\n", sony_vpt, sony_eq, sony_dn, dsp_enabled, sony_dsee, dsp_direct, 2);
	fprintf(f, "seq %d %d %d %d %d %d %d\n", sony_cb, sony_band[0], sony_band[1], sony_band[2], sony_band[3], sony_band[4], sony_cs);
	fprintf(f, "fx %d %d %d %d %d\n", voice_trim_on, voice_boost_on, hp_xfeed, hp_loud, hp_autoeq);
	safe_close(f, tmp, fn);
}
