/* focus.c: focus / meditation timer. Big countdown inside Sony's blue ring, a singing-bowl bell at the start,
 * at every interval and three at the end, optional background noise (white / pink / brown).
 * Integer math only at run time (the ARM926 has no FPU): the bell partials are damped oscillators
 * y[n] = a1*y[n-1] - a2*y[n-2], whose coefficients are computed once. */
#include "hb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SR 22050
#define BELL_LEN (SR * 6)

static short *bell;	/* precomputed bell, mono */

static int make_bell(void)
{
	if (bell) return 0;
	bell = malloc(BELL_LEN * sizeof(short));
	if (!bell) return -1;
	static const double fr[] = { 220.0, 604.0, 1136.0, 1792.0 }, am[] = { 0.55, 0.30, 0.12, 0.06 }, dk[] = { 1.2, 1.9, 2.8, 3.6 };
	long long a1[4], a2[4], y1[4], y2[4];	/* Q30 */
	for (int p = 0; p < 4; p++) {
		double r = exp(-dk[p] / SR), w = 2 * M_PI * fr[p] / SR;
		a1[p] = (long long)(2 * r * cos(w) * (1 << 30));
		a2[p] = (long long)(r * r * (1 << 30));
		y1[p] = (long long)(am[p] * sin(w) * (1 << 30));	/* y[0] = A sin(w), y[-1] = 0 */
		y2[p] = 0;
	}
	for (int i = 0; i < BELL_LEN; i++) {
		long long v = 0;
		for (int p = 0; p < 4; p++) {
			long long y = ((a1[p] * y1[p]) >> 30) - ((a2[p] * y2[p]) >> 30);
			y2[p] = y1[p]; y1[p] = y;
			v += y;
		}
		int s = (int)((v * 20000) >> 30);
		if (i < 88) s = s * i / 88;	/* 4 ms attack */
		bell[i] = hb_clip(s);
	}
	return 0;
}

/* ---- noise generators (integer) ---- */
static unsigned int rng = 12345;
static inline int white(void) { rng = rng * 1664525u + 1013904223u; return (int)(rng >> 16) - 32768; }
static int brown_state;
static inline int brown(void) { brown_state += white() >> 5; brown_state -= brown_state >> 7; if (brown_state > 32767) brown_state = 32767; if (brown_state < -32768) brown_state = -32768; return brown_state; }
static int pink_rows[8], pink_sum; static unsigned int pink_n;
static inline int pink(void)
{	/* Voss-McCartney: update one row per sample, chosen by the trailing zeros of a counter */
	pink_n++;
	int r = __builtin_ctz(pink_n | 0x100);
	if (r < 8) { pink_sum -= pink_rows[r]; pink_rows[r] = white() >> 3; pink_sum += pink_rows[r]; }
	return pink_sum + (white() >> 3);
}

static const int dur_opts[] = { 5, 10, 15, 20, 25, 30, 45, 60, 90 };
static const int int_opts[] = { 0, 1, 2, 5, 10, 15 };
static const char *noise_names[] = { "silencio", "ruido blanco", "ruido rosa", "ruido marrón" };
static int di = 4, ii = 3, ni = 3;	/* defaults: 25 min, bell every 5 min, brown noise */
#define NDUR (int)(sizeof dur_opts / sizeof dur_opts[0])
#define NINT (int)(sizeof int_opts / sizeof int_opts[0])
/* the choices, kept in pos.txt ("focus d i n") */
void focus_load(const char *v) { int a, b, c; if (sscanf(v, "%d %d %d", &a, &b, &c) == 3 && a >= 0 && a < NDUR && b >= 0 && b < NINT && c >= 0 && c < 4) { di = a; ii = b; ni = c; } }
void focus_save(char *out, size_t max) { snprintf(out, max, "%d %d %d", di, ii, ni); }

static void run_session(void)
{
	if (!bell) { message("Modo foco", "Preparando la campana...", ""); if (make_bell()) return; }
	eng_pause(1);	/* the session owns the audio output */
	int total = dur_opts[di] * 60, interval = int_opts[ii] * 60;
	static short buf[1024 * 2];
	int bell_pos = 0, bells_left = 1;	/* ring once at start */
	int elapsed = 0, next_bell = interval ? interval : -1, paused = 0, lastsec = -1, ending = 0;
	int noise_gain = 900;	/* /4096 */
	char l2[40];
	if (interval) snprintf(l2, sizeof l2, "campana cada %d min", int_opts[ii]); else snprintf(l2, sizeof l2, "campana al inicio y al final");
	if (aout_start(SR)) { message("Modo foco", "No hay salida de audio.", ""); msg_wait(2); return; }
	wake();
	for (;;) {
		int k = getkey(0);
		if (k == -1) k = bt_avrcp_key();
		bt_poll();
		if (bt_take_dropped()) { aout_stop(); paused = 1; lastsec = -1; }
		if (k == K_BACK) break;
		if (k == K_REDRAW) lastsec = -1;
		if (k == K_PLAY) { paused = !paused; lastsec = -1; }
		if (k == K_VOLUP) { if (volume < VOLMAX) volume++; aout_set_volume(); show_volume(); }
		if (k == K_VOLDN) { if (volume > 0) volume--; aout_set_volume(); show_volume(); }
		if (popup_expired()) lastsec = -1;
		int sec = elapsed / SR, left = total - sec;
		idle_tick(!paused);
		if (backlight_on && !popup_active() && sec != lastsec) {
			struct focus_view fv = { left, total, paused, noise_names[ni], l2 };
			ui_st.bt = bt_connected();
			scr_focus(&fv); gfx_flip();
			lastsec = sec;
		}
		if (!backlight_on) lastsec = -1;
		if (paused) { usleep(100000); continue; }
		int g = aout_gain();
		for (int i = 0; i < 1024; i++) {
			int v = 0;
			if (!ending) {
				int n = ni == 1 ? white() : ni == 2 ? pink() : ni == 3 ? brown() : 0;
				v = (n * noise_gain) >> 12;
			}
			if (bells_left) {
				v += bell[bell_pos++];
				if (bell_pos >= BELL_LEN || (bells_left > 1 && bell_pos >= SR * 2)) { bell_pos = 0; bells_left--; }
			}
			buf[2 * i] = buf[2 * i + 1] = hb_clip((v * g) >> 12);
			elapsed++;
			if (next_bell > 0 && elapsed == next_bell * SR && next_bell < total) { bells_left = 1; bell_pos = 0; next_bell += interval; }
			if (!ending && elapsed >= total * SR) { ending = 1; bells_left = 3; bell_pos = 0; }
		}
		if (aout_start(SR) || aout_write(buf, 1024)) {
			aout_stop();
			if (bt_connected()) bt_link_lost();
			else { message("Modo foco", "No hay salida de audio.", ""); msg_wait(2); break; }
		}
		if (ending && !bells_left) break;
	}
	aout_stop();
	if (ending) { message("Modo foco", "Sesión terminada.", ""); msg_wait(3); }
}

/* alarm: bells every 6 s until a key is pressed (rising volume) */
void focus_bells(void)
{
	if (!bell && make_bell()) return;
	static short buf[1024 * 2];
	int pos = 0, level = 6;
	if (aout_start(SR)) return;
	double t0 = now();
	for (;;) {
		int k = getkey(0);
		if (k >= 0 && k != K_REDRAW) break;
		if (now() - t0 > 600) break;
		int g = gain_q12(level);
		for (int i = 0; i < 1024; i++) { buf[2 * i] = buf[2 * i + 1] = hb_clip((bell[pos] * g) >> 12); if (++pos >= BELL_LEN) pos = 0; }
		if (aout_write(buf, 1024)) break;
		if (pos == 0 && level < VOLMAX) level += 2;
	}
	aout_stop();
}

static char fl[4][48], fv[4][24];
static const char *f_label(int i) { return fl[i]; }
static const char *f_value(int i) { return fv[i]; }

void focus_menu(void)
{
	struct list_view v = { "Modo foco", 4, 3, 0, f_label, f_value, 0, LI_NONE, 0, 0 };
	for (;;) {
		snprintf(fl[0], 48, "Duración"); snprintf(fv[0], 24, "%d min", dur_opts[di]);
		snprintf(fl[1], 48, "Campana cada");
		if (int_opts[ii]) snprintf(fv[1], 24, "%d min", int_opts[ii]); else snprintf(fv[1], 24, "inicio/fin");
		snprintf(fl[2], 48, "Fondo"); snprintf(fv[2], 24, "%s", noise_names[ni]);
		snprintf(fl[3], 48, "Empezar"); fv[3][0] = 0;
		int s = list_run(&v);
		if (s < 0) return;
		if (s == 0) { di = (di + 1) % NDUR; settings_save(); }
		else if (s == 1) { ii = (ii + 1) % NINT; settings_save(); }
		else if (s == 2) { ni = (ni + 1) % 4; settings_save(); }
		else run_session();
	}
}
