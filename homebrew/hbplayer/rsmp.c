/* rsmp.c: good-quality sample-rate conversion to 44.1 kHz for high-resolution files (48 / 88.2 / 96 / 176.4 /
 * 192 kHz and anything else), 32-bit samples in and out. The wired output of this Walkman only runs at 44.1 kHz
 * (the codec clock is fixed to the 44.1k family), so a 96 kHz FLAC must be converted, and the linear
 * interpolation in audio.c would fold everything above 22 kHz back into the audible band.
 *
 * Polyphase FIR: in/out = M/L (e.g. 96000 -> 44100: L = 147, M = 320). The prototype low-pass is a
 * Kaiser-windowed sinc (beta 7, ~ -70 dB stop band) cut at 0.45 x the output rate, designed once per rate
 * with floats (soft-float, ~0.1 s) and stored as Q15 per phase. Samples are left-justified 32-bit (24-bit
 * content). The inner loop uses the ARMv5TE SMLAWB instruction (32 x 16 -> top 32 bits, accumulate): each
 * product is (x * c) >> 16, i.e. Q31 x Q15 -> Q30, summed in 32 bits and doubled at the end. */
#include "hb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 44100
#define MAXT 96

#if defined(__arm__)
static inline int smlawb(int x, int c, int acc)
{
	int r;
	__asm__("smlawb %0, %1, %2, %3" : "=r"(r) : "r"(x), "r"(c), "r"(acc));
	return r;
}
#else
static inline int smlawb(int x, int c, int acc) { return acc + (int)(((long long)x * (short)c) >> 16); }
#endif

static int L, M, T, cur_in;
static short *coef;		/* [L][T], phase-major, reversed so coef[phase*T + k] multiplies x[base - k] */
static int hist[2][MAXT * 2];	/* last T input samples per channel (+ room) */
static int nhist;
static long long acc_pos;	/* next output position in units of 1/L input samples, relative to hist start */

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static double bessel_i0(double x)
{
	double s = 1, t = 1;
	for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
	return s;
}


/* (re)design for this input rate; 0 ok */
int rs_init(int in_rate)
{
	if (in_rate == cur_in && coef) { nhist = 0; acc_pos = 0; memset(hist, 0, sizeof hist); return 0; }
	int g = gcd(in_rate, OUT_RATE);
	L = OUT_RATE / g; M = in_rate / g;
	if (L > 512) { lg("rsmp: ratio %d/%d too fine", L, M); return -1; }
	double ratio = (double)M / L;			/* input samples per output sample */
	T = 16 * (int)ceil(ratio > 1 ? ratio : 1) + (ratio < 1.5 ? 16 : 0);	/* 96k: 48 taps, 48k: 48, 192k: 80 */
	if (T > MAXT) T = MAXT;
	free(coef);
	coef = malloc(sizeof(short) * L * T);
	if (!coef) return -1;
	/* prototype at the upsampled rate in*L: cutoff fc (cycles per upsampled sample) */
	double fc = 0.45 * (ratio > 1 ? 1.0 / M : 1.0 / L);
	int N = L * T;
	double beta = 7.0, i0b = bessel_i0(beta), center = (N - 1) / 2.0;
	double *h = malloc(sizeof(double) * N);
	if (!h) return -1;
	for (int n = 0; n < N; n++) {
		double x = n - center, s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
		double w = (2.0 * n / (N - 1)) - 1;
		h[n] = s * bessel_i0(beta * sqrt(1 - w * w)) / i0b;
	}
	/* phase p takes taps n = p + k*L; normalise each phase to unity DC gain (times L upsampling gain) */
	for (int p = 0; p < L; p++) {
		double sum = 0;
		for (int k = 0; k < T; k++) sum += h[p + k * L];
		for (int k = 0; k < T; k++) {
			double v = sum != 0 ? h[p + k * L] / sum : 0;
			int q = (int)lrint(v * 32768.0);
			coef[p * T + k] = (short)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
		}
	}
	free(h);
	cur_in = in_rate; nhist = 0; acc_pos = 0;
	memset(hist, 0, sizeof hist);
	lg("rsmp: %d -> %d Hz, L %d M %d, %d taps/phase", in_rate, OUT_RATE, L, M, T);
	return 0;
}

/* converts n stereo frames (interleaved int32) into out (interleaved int32); returns output frames.
 * out must hold n * L / M + 2 frames. */
int rs_process(const int *in, int n, int *out)
{
	int produced = 0;
	for (int i = 0; i < n; i++) {
		/* append one input frame to the history (kept T+ long, shifted in blocks) */
		if (nhist == MAXT * 2) {
			memmove(hist[0], hist[0] + nhist - T, sizeof(int) * T);
			memmove(hist[1], hist[1] + nhist - T, sizeof(int) * T);
			acc_pos -= (long long)(nhist - T) * L;
			nhist = T;
		}
		hist[0][nhist] = in[2 * i]; hist[1][nhist] = in[2 * i + 1];
		nhist++;
		/* every output whose newest needed input is now available */
		for (;;) {
			long long base = acc_pos / L;	/* index of the newest input sample for this output */
			if (base >= nhist) break;
			int phase = (int)(acc_pos % L);
			const short *c = coef + phase * T;
			int sl = 0, sr = 0, kmax = base + 1 < T ? (int)base + 1 : T;
			const int *hl = hist[0] + base, *hr = hist[1] + base;
			for (int k = 0; k < kmax; k++) {
				int cc = c[k];
				sl = smlawb(hl[-k] >> 1, cc, sl);	/* >> 1: headroom for filter overshoot */
				sr = smlawb(hr[-k] >> 1, cc, sr);
			}
			long long vl = (long long)sl << 2, vr = (long long)sr << 2;	/* Q30 (of x/2) -> Q31 */
			out[2 * produced] = vl > 0x7fffffffLL ? 0x7fffffff : vl < -0x80000000LL ? (int)0x80000000 : (int)vl;
			out[2 * produced + 1] = vr > 0x7fffffffLL ? 0x7fffffff : vr < -0x80000000LL ? (int)0x80000000 : (int)vr;
			produced++;
			acc_pos += M;
		}
	}
	return produced;
}
