/* voice.c: processing for spoken word (podcasts / audiobooks), in integer math for the ARM926 (no FPU):
 *
 *  - Silence trimming ("Recortar silencios", like Overcast's Smart Speed / ExoPlayer's silence skipping): the PCM is
 *    cut in 10 ms blocks; a block is quiet when its peak stays under -30 dBFS. The first 150 ms of every pause play
 *    as they are; beyond that only one quiet block in five is kept. Where blocks were dropped, the block before
 *    the gap fades out and the one after fades in over ~1.5 ms, so the joins can't click.
 *
 *  - Voice boost ("Realce de voz", after Overcast's Voice Boost 2 / the usual broadcast chain):
 *      rumble cut (2nd-order high-pass, 80 Hz) -> presence (+3 dB peak at 3 kHz) -> loudness levelling (the speech
 *      level measured on 50 ms blocks, silence gated out, pulled toward -20 dBFS RMS within -6..+15 dB, slowly)
 *      -> compressor (3:1 above -18 dBFS, 5 ms attack, ~100 ms release) -> soft limiter.
 *    Filters and levels are recomputed per block (floating point there is cheap: tens of times per second); the
 *    per-sample work is multiplies and shifts. */
#include "hb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int voice_trim_on, voice_boost_on;
static double trimmed_sec;	/* time saved by the silence trimming, this session */
double voice_saved_sec(void) { return trimmed_sec; }

/* ---------- silence trimming ---------- */

#define TRIM_THR 1036		/* -30 dBFS */
#define TRIM_GRACE 15		/* quiet blocks (x 10 ms) always kept at the start of a pause */
#define TRIM_KEEP 5		/* beyond that, 1 in 5 */
#define FADE 64			/* frames */

static int t_rate, t_blk;
static short t_in[480 * 2], t_pend[480 * 2];	/* block being filled, last kept block (not yet output) */
static int t_fill, t_have_pend, t_quiet, t_gap;

void voice_trim_reset(void) { t_fill = 0; t_have_pend = 0; t_quiet = 0; t_gap = 0; }

static void fade(short *b, int n, int in)
{
	int f = n < FADE ? n : FADE;
	for (int i = 0; i < f; i++) {
		int k = in ? i : f - 1 - i;	/* gain k / f */
		short *p = in ? b + 2 * i : b + 2 * (n - f + i);
		p[0] = (short)(p[0] * k / f); p[1] = (short)(p[1] * k / f);
	}
}

/* in: n frames; out: room for n + 480 frames. Returns the frames written to out. */
int voice_trim(const short *in, int n, int rate, short *out)
{
	if (rate != t_rate) { t_rate = rate; t_blk = rate / 100 > 480 ? 480 : rate / 100; voice_trim_reset(); }
	int m = 0;
	while (n > 0) {
		int take = t_blk - t_fill < n ? t_blk - t_fill : n;
		memcpy(t_in + 2 * t_fill, in, (size_t)take * 4);
		t_fill += take; in += 2 * take; n -= take;
		if (t_fill < t_blk) break;
		t_fill = 0;
		int peak = 0;
		for (int i = 0; i < 2 * t_blk; i++) { int a = abs(t_in[i]); if (a > peak) peak = a; }
		int keep;
		if (peak >= TRIM_THR) { t_quiet = 0; keep = 1; }
		else { t_quiet++; keep = t_quiet <= TRIM_GRACE || (t_quiet - TRIM_GRACE) % TRIM_KEEP == 0; }
		if (!keep) { t_gap = 1; trimmed_sec += (double)t_blk / t_rate; continue; }
		if (t_have_pend) {
			if (t_gap) { fade(t_pend, t_blk, 0); fade(t_in, t_blk, 1); }
			memcpy(out + 2 * m, t_pend, (size_t)t_blk * 4); m += t_blk;
		}
		memcpy(t_pend, t_in, (size_t)t_blk * 4); t_have_pend = 1; t_gap = 0;
	}
	return m;
}

/* ---------- voice boost ---------- */

#define QC 28
struct biq { int b0, b1, b2, a1, a2; int x1[2], x2[2], y1[2], y2[2], e[2]; };	/* e: error feedback (low 28 bits) */
static struct biq hp, pres;
static int b_rate;

static void set_biq(struct biq *q, double b0, double b1, double b2, double a0, double a1, double a2)
{
	double s = (double)(1 << QC) / a0;
	q->b0 = (int)(b0 * s); q->b1 = (int)(b1 * s); q->b2 = (int)(b2 * s); q->a1 = (int)(a1 * s); q->a2 = (int)(a2 * s);
	memset(q->x1, 0, sizeof q->x1); memset(q->x2, 0, sizeof q->x2); memset(q->y1, 0, sizeof q->y1); memset(q->y2, 0, sizeof q->y2); memset(q->e, 0, sizeof q->e);
}

static inline int biq_run(struct biq *q, int ch, int x)
{
	long long acc = (long long)q->b0 * x + (long long)q->b1 * q->x1[ch] + (long long)q->b2 * q->x2[ch]
		- (long long)q->a1 * q->y1[ch] - (long long)q->a2 * q->y2[ch] + q->e[ch];
	int y = (int)(acc >> QC);
	q->e[ch] = (int)acc & ((1 << QC) - 1);	/* error feedback: no DC offset from the truncation (80 Hz poles) */
	q->x2[ch] = q->x1[ch]; q->x1[ch] = x;
	q->y2[ch] = q->y1[ch]; q->y1[ch] = y;
	return y;
}

/* levelling and compressor state */
static double lvl_db = -20;	/* the speech level being tracked (dBFS RMS) */
static int agc_q12 = 4096, env;	/* current levelling gain; compressor envelope (peak follower, 16-bit scale) */
static int comp_q12 = 4096;
static long long blk_ss; static int blk_n, blk_len;
#define CT 1024
static unsigned short comp_tab[CT + 1];	/* compressor gain (Q12) for envelope / 128: 3:1 above -18 dBFS (4125) */

static void boost_setup(int rate)
{
	b_rate = rate;
	double w = 2 * M_PI * 80 / rate, c = cos(w), al = sin(w) / (2 * 0.707);	/* high-pass 80 Hz, Q 0.707 */
	set_biq(&hp, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
	double A = pow(10, 3.0 / 40), w2 = 2 * M_PI * 3000 / rate, al2 = sin(w2) / (2 * 1.0), c2 = cos(w2);	/* +3 dB at 3 kHz */
	set_biq(&pres, 1 + al2 * A, -2 * c2, 1 - al2 * A, 1 + al2 / A, -2 * c2, 1 - al2 / A);
	blk_len = rate / 20; blk_ss = 0; blk_n = 0;
	for (int i = 0; i <= CT; i++) { int e = i * 128; comp_tab[i] = e > 4125 ? (unsigned short)(4096 * pow(4125.0 / e, 1 - 1.0 / 3)) : 4096; }
}

void voice_boost_reset(void) { b_rate = 0; env = 0; comp_q12 = 4096; }	/* the level estimate survives (same show) */

void voice_boost(short *pcm, int n, int rate)
{
	if (rate != b_rate) boost_setup(rate);
	for (int i = 0; i < n; i++) {
		int l = biq_run(&hp, 0, pcm[2 * i]), r = biq_run(&hp, 1, pcm[2 * i + 1]);
		l = biq_run(&pres, 0, l); r = biq_run(&pres, 1, r);
		/* speech level: mean square of the filtered mono on 50 ms blocks */
		int mono = (l + r) >> 1;
		blk_ss += (long long)mono * mono;
		if (++blk_n >= blk_len) {
			double ms = (double)blk_ss / blk_n, db = ms > 0 ? 10 * log10(ms / (32768.0 * 32768.0)) : -120;
			if (db > -50) {	/* gate: pauses and room tone don't pull the level down */
				double a = db > lvl_db ? 0.08 : 0.03;	/* up faster (~0.6 s) than down (~1.7 s) */
				lvl_db += (db - lvl_db) * a;
			}
			double g = -20 - lvl_db;
			if (g > 15) g = 15;
			if (g < -6) g = -6;
			int target = (int)(4096 * pow(10, g / 20));
			agc_q12 += (target - agc_q12) / 4;	/* glide between blocks */
			blk_ss = 0; blk_n = 0;
		}
		l = (int)(((long long)l * agc_q12) >> 12); r = (int)(((long long)r * agc_q12) >> 12);
		/* compressor: peak follower, attack ~6 ms (1/256 per sample), release ~95 ms (1/4096) */
		int a = abs(l) > abs(r) ? abs(l) : abs(r);
		if (a > env) env += (a - env) >> 8; else env -= (env - a) >> 12;
		if (!(i & 31)) comp_q12 = comp_tab[env >> 7 < CT ? env >> 7 : CT];	/* gain every 32 samples, from the table */
		l = (int)(((long long)l * comp_q12) >> 12); r = (int)(((long long)r * comp_q12) >> 12);
		/* makeup +6 dB, then a soft limiter: above 24000 the curve bends toward 32000 and never reaches it
		 * (y = k + e * 8000 / (e + 8000), e = excess), so peaks round off instead of clipping */
		l *= 2; r *= 2;
		if (l > 24000) { int e = l - 24000; l = 24000 + e * 8000 / (e + 8000); } else if (l < -24000) { int e = -24000 - l; l = -24000 - e * 8000 / (e + 8000); }
		if (r > 24000) { int e = r - 24000; r = 24000 + e * 8000 / (e + 8000); } else if (r < -24000) { int e = -24000 - r; r = -24000 - e * 8000 / (e + 8000); }
		pcm[2 * i] = hb_clip(l); pcm[2 * i + 1] = hb_clip(r);
	}
}
