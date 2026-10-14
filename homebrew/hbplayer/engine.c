/* engine.c: background playback engine (libmad). The UI loops call eng_step() as often as they can; each call
 * decodes and outputs one MP3 frame (~26 ms of audio), and the blocking audio write paces the caller. In direct
 * mode (dsp_direct) the DSP plays by itself and eng_step only follows its position (a few syscalls per call).
 * Books/podcasts resume per file (pos table); music plays a queue with shuffle/repeat. */
#include "hb.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXQ 4000

/* the queue keeps only the text it needs, packed in an arena (a 744-byte struct track per song would cost ~3 MB
 * for a 4000-song shuffle). Two arenas alternate: the screen may still show the previous queue's title until the
 * next idle tick, so the old one is freed only when a third queue starts. */
struct qtrack { const char *path, *title, *sub, *genre, *year; };
static struct qtrack *q;
static char *arena[2]; static int arena_cur;
static int order[MAXQ];
static int nq, qi, mode, active, paused, changed, sleep_fired;
static char context[128];
int eng_shuffle, eng_repeat;	/* repeat: 0 off, 1 all, 2 one */
int eng_speed = 100, replaygain_mode = 0;
static int fade_left;	/* frames of fade-in remaining */
static short spcm[4096 * 2];
static short tpcm[(1152 + 480) * 2];	/* after the silence trimming */
int eng_sleep_min;
static time_t sleep_end;

/* current file */
static int have_file, lowpower, secs_offset, hires_failed, open_fails;
static short pcm[1152 * 2];
static short fxpcm[32768 * 2];	/* Sony DSP effect stream output (44.1 kHz; up to 5.5x the input frames) */
static time_t lastsave, paused_at;
static int underruns;	/* the output queue ran dry while playing (self-test counter) */
int eng_underruns(void) { return underruns; }

/* direct playback ("Máxima batería", dsp_direct): the DSP decodes and plays MP3 / ATRAC / WMA / AAC to the wired
 * output by itself; the ARM only feeds compressed data. Only when nothing in our own chain is needed: wired
 * output, no ARM EQ, normal speed, no ReplayGain. Sony's DSP effects (page) still apply. HB_DIRECT=1 in the
 * environment forces the setting (selftest). */
static int want_direct(void)
{
	static int env = -1;
	if (env < 0) env = getenv("HB_DIRECT") != 0;
	return (dsp_direct || env) && dsp_enabled && !bt_connected() && !eq_active() && !(mode == ENG_BOOK && (eng_speed != 100 || voice_trim_on || voice_boost_on));
}
int eng_direct(void) { return have_file && dec_direct(); }

static void save_pos(void)
{
	if (!have_file) return;	/* books, podcasts and music all resume where they were */
	pos_set(q[order[qi]].path, dec_pos_bytes(), (int)dec_pos_sec());
	pos_save();
}

static void close_file(void)
{
	if (!have_file) return;
	dec_close();
	have_file = 0;
}

int eng_resume_music;	/* set by resume_last(): the next song opened continues at its saved position */

static int open_file(int fade)
{
	close_file();
	const char *path = q[order[qi]].path;
	hires_failed = 0;
	dec_set_direct(want_direct());
	if (dec_open(path)) {
		lg("eng: open failed %s", path);
		if (dec_error()) eng_note(dec_error());	/* e.g. "Archivo protegido (DRM)"; eng_frame skips the track */
		return -1;
	}
	have_file = 1; open_fails = 0;
	stretch_reset();
	voice_trim_reset(); voice_boost_reset();
	/* ReplayGain: on the codec's own volume (wired: bit-exact PCM, and direct mode stays possible) */
	aout_set_track_gain(replaygain_mode ? dec_gain_cdb(replaygain_mode == 2) : 0);
	lrc_load(path);
	chap_load(path);
	fade_left = fade ? 4000 : 0;	/* a fade-in after a skip / seek / resume; none on the natural next track (gapless) */
	dec_lowpower(lowpower);
	{	/* books / podcasts always continue where they were; a song only when "Reproduciendo" resumes it (skipping
		 * to the next song, or picking one from a list, starts it from the beginning, like Sony's player) */
		long b; int s;
		int resume = mode != ENG_MUSIC || eng_resume_music;
		eng_resume_music = 0;
		if (resume && pos_get(path, &b, &s) && s > 0) {
			if (b > 0) { if (dec_seek_bytes(b)) dec_seek_sec(s); }
			else dec_seek_sec(s);
		}
	}
	if (mode == ENG_BOOK) set_lastfile(path);
	lastsave = time(0);
	changed = 1;
	if (dec_direct()) aout_set_volume();	/* the codec volume: no aout_start in direct mode (needs audio.c to apply it without an open device) */
	{	/* which path and, if not the battery-saving direct one, why */
		const char *why = dec_direct() ? "direct" : !dsp_direct ? "Máxima batería off" : !dsp_enabled ? "DSP off" : bt_connected() ? "Bluetooth"
			: eq_active() ? "EQ / headphone effects" : (mode == ENG_BOOK && (eng_speed != 100 || voice_trim_on || voice_boost_on)) ? "voice effects / speed"
			: dec_on_dsp() ? "format not direct (DSP decoding)" : "ARM decoding";
		lg("eng: play %s from %d s [%s, load %d%%]", path, (int)dec_pos_sec(), why, eng_load_pct());
	}
	return 0;
}

/* the direct / normal choice changed mid-track (Bluetooth connected, EQ or speed switched on): the same file
 * again on the other path, from here */
static void reopen_here(void)
{
	const char *path = q[order[qi]].path;
	double s = dec_pos_sec();
	long b = dec_pos_bytes();
	dec_close();
	dsp_direct_release();
	dec_set_direct(want_direct());
	if (dec_open(path)) { have_file = 0; lg("eng: reopen failed %s", path); return; }
	dec_lowpower(lowpower);
	if (b > 0) { if (dec_seek_bytes(b)) dec_seek_sec(s); }
	else if (s > 0) dec_seek_sec(s);
	fade_left = 4000;
	changed = 1;
	if (dec_direct()) aout_set_volume();
	lg("eng: %s continues %s at %.0f s", path, dec_direct() ? "direct" : "on the normal path", s);
}

void eng_play(const struct track *list, int n, int idx, int m, const char *ctx) { eng_play2(list, n, idx, m, ctx, -1); }

/* shuffle: -1 = as configured (eng_shuffle), 0/1 = force for this queue */
void eng_play2(const struct track *list, int n, int idx, int m, const char *ctx, int shuffle)
{
	eng_stop();
	bt_take_dropped();	/* a drop that happened while idle must not pause the new playback */
	if (n > MAXQ) n = MAXQ;
	size_t need = 0;
	for (int i = 0; i < n; i++) need += strlen(list[i].path) + strlen(list[i].title) + strlen(list[i].sub) + strlen(list[i].genre) + strlen(list[i].year) + 5;
	struct qtrack *nqp = realloc(q, sizeof *q * (n ? n : 1));
	int k = !arena_cur;
	char *a = malloc(need ? need : 1);
	if (!nqp || !a) { free(a); if (nqp) q = nqp; nq = 0; eng_note("Sin memoria para la lista"); return; }
	q = nqp;
	free(arena[k]); arena[k] = a; arena_cur = k;
	for (int i = 0; i < n; i++) {
		const char *f[5] = { list[i].path, list[i].title, list[i].sub, list[i].genre, list[i].year };
		const char **d[5] = { &q[i].path, &q[i].title, &q[i].sub, &q[i].genre, &q[i].year };
		for (int j = 0; j < 5; j++) { size_t l = strlen(f[j]) + 1; memcpy(a, f[j], l); *d[j] = a; a += l; }
	}
	nq = n; mode = m;
	snprintf(context, sizeof context, "%s", ctx ? ctx : "");
	for (int i = 0; i < n; i++) order[i] = i;
	if (mode == ENG_MUSIC && (shuffle < 0 ? eng_shuffle : shuffle)) {
		for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }
		for (int i = 0; i < n; i++) if (order[i] == idx) { order[i] = order[0]; order[0] = idx; }
		qi = 0;
	} else qi = idx;
	active = 1; paused = 0; open_fails = 0;
	if (open_file(1) && nq <= 1) { active = 0; return; }	/* more tracks: eng_frame skips the bad one */
}

void eng_stop(void)
{
	if (!active) return;
	save_pos();
	close_file();
	dsp_direct_release();	/* direct mode: the helper frees /dev/sound/dsp for whoever comes next */
	dspfx_reset();
	aout_stop();
	active = 0; paused = 0; changed = 1;
}

void eng_lowpower(int on) { lowpower = on; dec_lowpower(on); }

int eng_active(void) { return active; }
int eng_playing(void) { return active && !paused; }
int eng_changed(void) { int c = changed; changed = 0; return c; }
int eng_take_sleep(void) { int s = sleep_fired; sleep_fired = 0; return s; }

void eng_pause(int p)
{
	if (!active || p == paused) return;
	paused = p;
	if (p) {
		if (have_file && !dec_direct()) {	/* what is still queued in the driver is dropped: resume from there */
			double q = aout_queued_sec() * (mode == ENG_BOOK ? eng_speed / 100.0 : 1);
			if (q > 0.03 && q < 3) dec_rewind(q);
		}
		save_pos();
		if (have_file && dec_direct()) dec_direct_pause(1);	/* the DSP holds its place: instant resume */
		dspfx_reset();	/* an effect stream would keep the helper waking several times a second */
		aout_stop_now();
		paused_at = time(0);
	} else {
		fade_left = 8000;
		if (have_file && dec_direct()) dec_direct_pause(0);	/* parked meanwhile: restarts by itself on the next step */
	}
	changed = 1;
}
void eng_toggle(void) { eng_pause(!paused); }

void eng_set_sleep(int minutes) { eng_sleep_min = minutes; sleep_end = time(0) + minutes * 60; }
int eng_sleep_left(void) { int m = eng_sleep_min ? (int)((sleep_end - time(0) + 59) / 60) : 0; return m < 0 ? 0 : m; }

/* housekeeping that must run even while paused (called from idle_tick) */
/* a paused direct session frees the audio device (and the DSP) for whoever needs it: bells, focus mode, a game,
 * the headset. Also after a minute of pause, to power the DSP down. */
void eng_park_output(void)
{
	if (active && paused && have_file && dec_direct()) { dec_direct_park(); lg("eng: direct session parked"); }
}

void eng_tick(void)
{
	if (active && paused && have_file && dec_direct() && time(0) - paused_at > 60) eng_park_output();
	if (eng_sleep_min && time(0) >= sleep_end) { eng_sleep_min = 0; if (active) eng_pause(1); sleep_fired = 1; }
}

/* advance: dir +1 next, -1 previous (or restart if > 5 s in); returns 0 if the queue ended */
static int advance(int dir, int user)
{
	pos_set(q[order[qi]].path, 0, 0);	/* finished or skipped: start over next time */
	if (user) { dspfx_reset(); aout_stop_now(); }	/* a skip must not play the tail of the old track */
	if (dir < 0) {
		if (dec_pos_sec() > 5) { pos_set(q[order[qi]].path, 0, 0); return open_file(1) == 0; }
		if (qi > 0) qi--;
		return open_file(1) == 0;
	}
	if (mode == ENG_MUSIC && !user && eng_repeat == 2) return open_file(0) == 0;
	if (qi + 1 < nq) qi++;
	else if (mode == ENG_MUSIC && (eng_repeat == 1 || user)) qi = 0;
	else { pos_save(); close_file(); dsp_direct_release(); aout_stop(); active = 0; changed = 1; return 0; }
	pos_save();
	return open_file(user) == 0;
}
void eng_next(void) { if (active) advance(1, 1); }
void eng_prev(void) { if (active) advance(-1, 1); }

/* absolute seek (chapters) */
void eng_seek_to(double to)
{
	if (!active || !have_file) return;
	if (to < 0) to = 0;
	dspfx_reset();
	aout_stop_now();
	fade_left = 4000;
	voice_trim_reset(); stretch_reset();
	if (dec_seek_sec(to)) { advance(1, 0); return; }
	changed = 1;
}

void eng_seek(int secs)
{
	if (!active || !have_file) return;
	double to = dec_pos_sec() + secs;
	if (to < 0) to = 0;
	if (dec_total_sec() && to >= dec_total_sec()) { advance(1, 0); return; }
	dspfx_reset();
	aout_stop_now();
	fade_left = 4000;
	voice_trim_reset(); stretch_reset();
	if (dec_seek_sec(to)) { advance(1, 0); return; }
	changed = 1;
}

void eng_info(struct eng_info *e)
{
	memset(e, 0, sizeof *e);
	if (!active) return;
	const struct qtrack *t = &q[order[qi]];
	e->title = t->title; e->sub = t->sub; e->genre = t->genre; e->year = t->year; e->path = t->path; e->context = context;
	e->idx = qi; e->n = nq; e->sec = (int)dec_pos_sec(); e->total = dec_total_sec(); e->paused = paused; e->mode = mode;
}

/* high-resolution path: 24-bit FLAC (any rate) -> 32-bit polyphase resampling to 44.1 kHz -> 24-bit wired
 * output. Only when nothing in the 16-bit chain is needed: wired output, no EQ, normal speed. */
static int hires_rate;
int eng_hires(void) { return hires_rate != 0; }
/* work the ARM really has to do: resampling a hi-res file, time-stretching, FAAD2 */
int eng_resampling(void) { return hires_rate != 0 && hires_rate != 44100; }
/* why music (book = 0) or a podcast / book (book = 1) can't use "Máxima batería" (direct DSP playback); 0 = it can */
const char *eng_not_direct(int book)
{
	static char b[160];
	if (!dsp_enabled) return "el DSP de Sony está apagado";
	if (!dsp_direct) return "Máxima batería está desactivado";
	if (bt_connected()) return "el audio va por Bluetooth";
	if (eq_active()) { char e[120]; eq_effects_desc(e, sizeof e); snprintf(b, sizeof b, "efectos activos: %s", e); return b; }
	if (book && eng_speed != 100) return "velocidad distinta de 1x";
	if (book && (voice_trim_on || voice_boost_on)) return "efectos de voz activos";
	return 0;
}

/* estimated CPU of the playback chain, % at a true 124 MHz (device measurements 2026-09-29: libmad MP3 ~34 %,
 * DSP decoding with PCM back ~4 %, direct ~0; trim ~2.5 %, voice boost ~14 %, Sonic ~30 % per 1x of input) */
int eng_load_pct(void)
{
	if (!active || paused) return 0;
	if (have_file && dec_direct()) return 1;
	int l = dec_on_dsp() ? 4 : dec_soft_aac() ? 45 : 34;
	if (mode == ENG_BOOK) {
		int sp = eng_speed;
		if (voice_trim_on) l += 3 * sp / 100;
		if (sp != 100) l += 30 * sp / 100;
		if (voice_boost_on) l += 14;
	}
	return l + eq_load_pct();
}

static int hires_frame(void)
{
	static int in32[1152 * 2], out32[(1152 * 2 + 4) * 2];
	int rate = dec_rate();
	/* rs_process makes up to n * 44100 / rate + 2 frames: read no more than out32 holds (low-rate files) */
	int maxin = rate > 0 ? (int)((long long)(1152 * 2) * rate / 44100) : 1152;
	if (maxin > 1152) maxin = 1152;
	if (maxin < 1) maxin = 1;
	int n = dec_read32(in32, maxin);
	if (n <= 0) { if (n < 0) lg("eng: decode error, next"); advance(1, 0); return 0; }
	if (rate != hires_rate) { if (rs_init(rate)) return -1; hires_rate = rate; }
	dsp_direct_release();	/* no-op unless the DSP helper still holds /dev/sound/dsp */
	if (aout_start32()) return -1;
	int *out = in32, m = n;
	if (rate != 44100) { m = rs_process(in32, n, out32); out = out32; }
	/* the wired volume and ReplayGain are the codec's own (bit exact) */
	for (int i = 0; i < 2 * m; i++) {
		long long v = out[i];
		if (fade_left > 0 && (i & 1) == 0) fade_left--;
		if (fade_left > 0) v = v * (8000 - fade_left) / 8000;
		out[i] = v > 0x7fffffffLL ? 0x7fffffff : v < -0x80000000LL ? (int)0x80000000 : (int)v;
	}
	if (aout_write32(out, m)) { lg("eng: audio write failed"); aout_stop(); return 0; }
	return 1;
}

/* profiling (selftest HB_PROF): wall time per stage of eng_frame, logged by eng_prof_report */
int eng_prof;
static double pf[8]; static long pf_frames, pf_calls;
static const char *pf_name[8] = { "decode", "trim", "stretch", "fx", "boost", "eq", "write", "other" };
#define PF(k) do { if (eng_prof) { double _t = now(); pf[k] += _t - pf_t; pf_t = _t; } } while (0)
void eng_prof_report(void)
{
	if (!eng_prof) return;
	char b[256]; int n = 0;
	for (int k = 0; k < 8; k++) n += snprintf(b + n, sizeof b - n, " %s %.0f", pf_name[k], pf[k] * 1000);
	lg("prof ms:%s | %ld calls, %ld frames out", b, pf_calls, pf_frames);
	memset(pf, 0, sizeof pf); pf_frames = pf_calls = 0;
}

static int eng_frame(void)
{
	double pf_t = eng_prof ? now() : 0;
	if (eng_prof) pf_calls++;
	if (!active) return 0;
	if (bt_take_dropped()) { eng_pause(1); eng_note("Bluetooth desconectado"); return 0; }
	if (paused) return 0;
	if (!have_file) {	/* the track could not be opened (protected ATRAC, bad file): next, unless all failed */
		if (++open_fails > nq) { lg("eng: nothing playable"); eng_stop(); return 0; }
		advance(1, 0);
		return 0;
	}
	/* books / podcasts: a hang (watchdog reset, no crash handler) loses at most two minutes. Songs only resume
	 * from "Reproduciendo", saved on pause / track change: no flash write every minute for them */
	if (mode != ENG_MUSIC && time(0) - lastsave >= 120) { save_pos(); lastsave = time(0); }
	if (dec_direct()) {	/* the DSP plays by itself: follow its position, go on at the end */
		if (!want_direct()) { reopen_here(); return 0; }
		/* a natural (gapless) step from a normal-path track leaves /dev/sound/dsp open: the DSP can't take the codec
		 * while we hold it (the session errors after a few seconds). Play the tail out and free it first. */
		if (aout_space_known()) { dspfx_reset(); aout_stop(); }
		int r = dec_direct_step();
		if (r > 0) return 0;
		if (r == 0) { advance(1, 0); return 0; }
		lg("eng: direct playback failed, normal path from %.0f s", dec_pos_sec());
		return 0;	/* dec_read works from the next call */
	}
	if (!hires_failed && dec_hires() && !bt_connected() && !eq_active() && !(mode == ENG_BOOK && eng_speed != 100)) {
		int r = hires_frame();
		if (r >= 0) return r;
		hires_failed = 1;	/* don't retry (device reopen, filter design) every frame: 16-bit for this track */
		lg("eng: hi-res path unavailable, 16-bit");	/* falls through: dec_read gives S16 */
	}
	hires_rate = 0;
	PF(7);
	int n = dec_read(pcm, 1152);
	PF(0);
	if (n <= 0) { if (n < 0) lg("eng: decode error, next"); advance(1, 0); return 0; }
	int rate = dec_rate();
	short *out = pcm;
	if (mode == ENG_BOOK && voice_trim_on) {	/* shorter pauses (before the speed change) */
		n = voice_trim(pcm, n, rate, tpcm);
		PF(1);
		if (!n) return 1;
		out = tpcm;
	}
	if (mode == ENG_BOOK && eng_speed != 100) {	/* time stretching for speech */
		stretch_set_rate(rate); stretch_set_speed(eng_speed);
		if (stretch_push(out, n)) stretch_reset();
		int got = 0, hop;
		while (got + 2048 <= 4096 && (hop = stretch_pull(spcm + got * 2)) > 0) got += hop;
		PF(2);
		if (!got) return 1;
		out = spcm; n = got;
	}
	/* Sony's DSP effects (VPT / EQ / normalizer) for PCM the DSP did not decode itself: through LPCMD in the
	 * dspdec helper, 44.1 kHz back, ~0.1-0.2 s later (the stream is continuous; seeks / skips reset it) */
	int orate = rate;
	if (dspfx_active(rate)) {
		int m = dspfx_process(out, n, rate, fxpcm, 32768);
		if (m >= 0) {
			if (!m) return 1;	/* pipeline still filling */
			out = fxpcm; n = m; orate = 44100;
		}
	} else dspfx_reset();	/* effects switched off / DSP decoding: stop a running stream (no-op otherwise) */
	dsp_direct_release();	/* no-op unless the DSP helper still holds /dev/sound/dsp (after direct playback) */
	if (aout_start(orate)) {
		static double first_fail;
		if (!first_fail) first_fail = now();
		if (bt_connected()) { if (now() - first_fail > 2) { first_fail = 0; bt_link_lost(); } else usleep(100000); return 0; }
		first_fail = 0;
		eng_pause(1); eng_note("No hay salida de audio"); return 0;
	}
	PF(3);
	if (mode == ENG_BOOK && voice_boost_on) voice_boost(out, n, orate);	/* level, presence, compression */
	PF(4);
	eq_process(out, n, orate);
	PF(5);
	int g = aout_gain();	/* 4096 on the wired output (codec volume); Bluetooth: volume and ReplayGain in software */
	if (g != 4096) for (int i = 0; i < 2 * n; i++) out[i] = hb_clip((out[i] * g) >> 12);
	if (fade_left > 0) {	/* short fade-in so pause/resume and track starts don't click */
		int total = 8000;
		for (int i = 0; i < n && fade_left > 0; i++, fade_left--) {
			int f = 4096 - fade_left * 4096 / total;
			out[2 * i] = (short)((out[2 * i] * f) >> 12); out[2 * i + 1] = (short)((out[2 * i + 1] * f) >> 12);
		}
	}
	int wr = aout_write(out, n);
	PF(6);
	if (eng_prof) pf_frames += n;
	if (wr) {
		lg("eng: audio write failed");
		aout_stop();
		if (bt_connected()) bt_link_lost();
		return 0;
	}
	return 1;
}

/* top up the output buffer: decode frames while the driver has room (up to ~0.5 s per call), so the UI can
 * spend time drawing without the audio running dry. If the driver can't report its space, one (blocking) frame. */
void eng_step(void)
{
	if (!active || paused) { eng_frame(); return; }
	int sp = aout_space();
	if (sp < 0) { eng_frame(); return; }
	static double started;
	if (aout_empty()) { if (started && now() - started > 1.5) underruns++; } else if (!started) started = now();
	for (int i = 0; i < 20; i++) {
		sp = aout_space();
		if (sp >= 0 && sp < 1152 * 4) break;	/* full enough */
		if (!eng_frame() || !active || paused) break;
	}
}
