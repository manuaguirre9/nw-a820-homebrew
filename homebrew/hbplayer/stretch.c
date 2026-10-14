/* stretch.c: speed change with the pitch unchanged (0.75x..3x) for audiobooks and podcasts, on Sonic
 * (third_party/sonic, Bill Cox, Apache-2.0: the PICOLA-style algorithm Android's ExoPlayer uses for speech; integer
 * sample math, pitch periods found by AMDF on a decimated signal). Replaces the old WSOLA, whose correlation search
 * and per-hop memmove of the whole input buffer cost far more CPU. Same interface as before. */
#include "hb.h"
#include "sonic.h"
#include <stdlib.h>

static sonicStream st;
static int rate, factor = 100;

void stretch_set_rate(int r)
{
	if (r == rate && st) return;
	rate = r;
	if (st) sonicDestroyStream(st);
	st = sonicCreateStream(r, 2);
	if (st) { sonicSetQuality(st, 0); sonicSetSpeed(st, factor / 100.0f); }	/* quality 0: decimated pitch search */
}

void stretch_set_speed(int f)
{
	f = f < 50 ? 50 : f > 300 ? 300 : f;
	if (f != factor && st) sonicSetSpeed(st, f / 100.0f);
	factor = f;
}


void stretch_reset(void)
{
	if (!rate) return;
	if (st) sonicDestroyStream(st);
	st = sonicCreateStream(rate, 2);
	if (st) { sonicSetQuality(st, 0); sonicSetSpeed(st, factor / 100.0f); }
}

/* push decoded frames; 0, or -1 on error (the caller resets) */
int stretch_push(const short *pcm, int n)
{
	if (!st) return -1;
	return sonicWriteShortToStream(st, pcm, n) ? 0 : -1;
}


/* up to 2048 frames of output; 0 when it needs more input */
int stretch_pull(short *out)
{
	if (!st) return 0;
	int n = sonicReadShortFromStream(st, out, 2048);
	return n > 0 ? n : 0;
}
