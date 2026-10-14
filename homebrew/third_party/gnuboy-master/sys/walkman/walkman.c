/*
 * walkman.c: gnuboy system backend for the Sony NW-A829 (homebrew).
 * Video: 160x144 RGB565 rendered off-screen every 2nd frame, then either
 *        - normal: scaled 1.5x to 240x216, or
 *        - rotated 90 degrees clockwise (hold the Walkman sideways, buttons under the right thumb): 240x266.
 * Sound: OSS /dev/sound/dsp (or $HB_DSP, e.g. /dev/sound/adsp for Bluetooth), S16 stereo; $HB_SOUND=0 mutes the
 *        game (the player keeps your music going instead).
 * Keys:  d-pad = d-pad (remapped when rotated), PLAY = A, OPTION = B, BACK tap = START, BACK held 2 s = quit,
 *        BT tap = SELECT, BT held 2 s = in-game menu (continue, save / load state, rotation, turbo factor, quit),
 *        VOL+/- = volume. HOLD switch on = TURBO (1.5x / 2x / 3x, game sound muted); keys keep working under HOLD.
 * Settings (rotation) are kept in walkman.cfg in the save directory. Logs the frame rate every 10 s.
 * This file may be distributed under the terms of the GNU GPL (like gnuboy).
 */
#undef _GNU_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "../../defs.h"
#include "../../fb.h"
#include "../../pcm.h"
#include "../../rc.h"
#include "../../input.h"
#include "../../loader.h"
#include "../../sound.h"
#include "../../cpu.h"
#include "../../hw.h"
#define GFX_NO_FB_ALIAS
#include "../../../../common/gfx.h"
#include "../../../../common/game_i18n.h"

struct fb fb;
struct pcm pcm;

#define SW 240
#define SH 320

static unsigned short *screen;
static unsigned short gbuf[160 * 144];
static int xmap[SW], ymap[266], rot_sy[SW], rot_sx[266];
static int frame, rotate, turbo, turbo_idx = 1, saved_rate;
static const int turbo_x10[] = { 15, 20, 30 };
static int keyfd = -1, dsp = -1, vol = 14, sound = 1;
static short *obuf;
static char cfgpath[512];

rcvar_t vid_exports[] = { RCV_END };
rcvar_t joy_exports[] = { RCV_END };
rcvar_t pcm_exports[] = { RCV_END };

static void load_cfg(void)
{
	const char *d = getenv("HB_SAVEDIR") ? getenv("HB_SAVEDIR") : ".";
	snprintf(cfgpath, sizeof cfgpath, "%s/walkman.cfg", d);
	FILE *f = fopen(cfgpath, "r");
	if (f) { if (fscanf(f, "rotate %d turbo %d", &rotate, &turbo_idx) < 1) rotate = 0; fclose(f); }
	if (turbo_idx < 0 || turbo_idx > 2) turbo_idx = 1;
}

static void save_cfg(void)
{
	FILE *f = fopen(cfgpath, "w");
	if (f) { fprintf(f, "rotate %d turbo %d\n", rotate, turbo_idx); fclose(f); sync(); }
}

/* turbo: shorter frame period (gnuboy paces itself with framelen when pcm_submit returns 0), sound synthesis off */
static void set_turbo(int on)
{
	char b[16];
	snprintf(b, sizeof b, "%d", on ? 16743 * 10 / turbo_x10[turbo_idx] : 16743);
	char *v = b;
	rc_setvar("framelen", 1, &v);
	if (on && !turbo) { if (snd.rate) saved_rate = snd.rate; snd.rate = 0; }
	if (!on && turbo) { snd.rate = saved_rate; cpu.snd = 0; }
	turbo = on;
}

/* HOLD switch state: EVIOCGLED(4) on event0 returns 4 bytes, value 1 = HOLD on */
static void read_hold(void)
{
	int v = 0;
	if (keyfd >= 0 && ioctl(keyfd, 0x80044519, &v) >= 0) set_turbo(v & 1);
}

void vid_preinit() {}
void vid_settitle(char *title) {}
void vid_setpal(int i, int r, int g, int b) {}

void vid_init()
{
	int fd = open("/dev/fb/0", O_RDWR);
	if (fd < 0) die("cannot open /dev/fb/0\n");
	screen = mmap(0, SW * SH * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (screen == MAP_FAILED) die("mmap fb failed\n");
	memset(screen, 0, SW * SH * 2);
	fb.w = 160; fb.h = 144; fb.pelsize = 2; fb.pitch = 320; fb.indexed = 0;
	fb.ptr = (byte *)gbuf;
	fb.cc[0].r = 3; fb.cc[0].l = 11;
	fb.cc[1].r = 2; fb.cc[1].l = 5;
	fb.cc[2].r = 3; fb.cc[2].l = 0;
	fb.enabled = 1; fb.dirty = 1; fb.yuv = 0;
	for (int x = 0; x < SW; x++) xmap[x] = x * 2 / 3;
	for (int y = 0; y < 216; y++) ymap[y] = y * 2 / 3;
	/* rotated 90 deg clockwise: screen (x, y) shows game pixel (sx, sy) = (y * 160 / 266, 143 - x * 144 / 240) */
	for (int x = 0; x < SW; x++) rot_sy[x] = 143 - x * 144 / SW;
	for (int y = 0; y < 266; y++) rot_sx[y] = y * 160 / 266;
	keyfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	load_cfg();
	gfx_open();
	game_i18n_init();
	gfx_load_res("/usr/local/share/gemini/res");
}

void vid_close() {}

void vid_begin()
{
	fb.enabled = frame % (turbo ? 6 : 3) == 0;	/* render 1 frame in 3 (1 in 6 in turbo); the game itself runs at full speed */
}

void vid_end()
{
	if (frame++ % (turbo ? 6 : 3)) return;
	if (!rotate) {
		for (int y = 0; y < 216; y++) {
			unsigned short *d = screen + (40 + y) * SW, *s = gbuf + ymap[y] * 160;
			for (int x = 0; x < SW; x++) d[x] = s[xmap[x]];
		}
	} else {
		for (int y = 0; y < 266; y++) {
			unsigned short *d = screen + (27 + y) * SW;
			const unsigned short *s = gbuf + rot_sx[y];
			for (int x = 0; x < SW; x++) d[x] = s[rot_sy[x] * 160];
		}
	}
}

/* ---- sound ---- */

void pcm_init()
{
	const char *dev = getenv("HB_DSP");
	int rate = dev && strstr(dev, "adsp") ? 44100 : 22050;
	if (getenv("HB_VOL")) vol = atoi(getenv("HB_VOL"));
	if (getenv("HB_SOUND") && !strcmp(getenv("HB_SOUND"), "0")) sound = 0;
	if (!sound) {	/* muted: pcm.hz = 0 makes gnuboy skip its sound synthesis entirely (saves CPU for the music) */
		pcm.hz = 0; pcm.len = 0; pcm.buf = 0; pcm.pos = 0; pcm.stereo = 1;
		return;
	}
	pcm.hz = rate; pcm.stereo = 1;
	pcm.len = rate / 60 * 2;
	pcm.buf = malloc(pcm.len);
	obuf = malloc(pcm.len * 2);
	pcm.pos = 0;
	dsp = open(dev ? dev : "/dev/sound/dsp", O_WRONLY | (dev && strstr(dev, "adsp") ? O_NONBLOCK : 0));
	if (dsp < 0) return;
	int n = AFMT_S16_LE; ioctl(dsp, SNDCTL_DSP_SETFMT, &n);
	n = 2; ioctl(dsp, SNDCTL_DSP_CHANNELS, &n);
	n = rate; ioctl(dsp, SNDCTL_DSP_SPEED, &n);
}

void pcm_close()
{
	if (dsp >= 0) close(dsp);
	dsp = -1;
}

void pcm_pause(int dopause) {}

int pcm_submit()
{
	if (dsp < 0) { pcm.pos = 0; return 0; }
	static int gain[21];
	if (!gain[20]) for (int i = 1; i <= 20; i++) {	/* 2 dB steps, 20 = 0 dB */
		int g = 4096;
		for (int s = i; s < 20; s++) g = g * 3254 / 4096;
		gain[i] = g;
	}
	int g = gain[vol < 0 ? 0 : vol > 20 ? 20 : vol];
	for (int i = 0; i < pcm.pos; i++) {
		int v = ((((int)pcm.buf[i] - 128) << 8) * g) >> 12;
		obuf[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	int ok = !pcm.pos || write(dsp, obuf, pcm.pos * 2) > 0;
	pcm.pos = 0;
	if (turbo) return 0;	/* no audio pacing in turbo: framelen sets the speed */
	return ok;	/* 0 -> gnuboy paces itself with its own timer (e.g. Bluetooth buffer full) */
}

/* ---- input ---- */

struct kev { long sec, usec; unsigned short type, code; int value; };
static struct timeval back_down, bt_down;
static int back_held, bt_held, bar_frames;
static int tap_key, tap_frames;	/* synthetic START/SELECT taps: pressed now, released 4 frames later */

static void post(int code, int pressed)
{
	event_t e;
	memset(&e, 0, sizeof e);
	e.type = pressed ? EV_PRESS : EV_RELEASE;
	e.code = code;
	ev_postevent(&e);
}

static void draw_bar(void)
{
	unsigned short *d = screen + (rotate ? 300 : 270) * SW;
	for (int y = 0; y < 12; y++)
		for (int x = 20; x < SW - 20; x++)
			d[y * SW + x] = bar_frames <= 0 ? 0 : (x - 20) < (SW - 40) * vol / 20 ? 0xFCA0 : 0x4208;
}

/* ---- in-game menu (Sony font and orange bar, drawn with ../../common/gfx) ---- */

static int wait_key(void)
{
	struct kev k;
	for (;;) {
		fd_set rs; FD_ZERO(&rs); FD_SET(keyfd, &rs);
		if (select(keyfd + 1, &rs, 0, 0, 0) <= 0) continue;
		while (read(keyfd, &k, sizeof k) == sizeof k)
			if (k.type == 1 && k.value == 0 && (k.code & 0x7f) != 15) return k.code & 0x7f;
	}
}

static void menu_draw(const char *const *items, int n, int sel, const char *note)
{
	struct img bar = gfx_tex(14, 4), status = gfx_tex(16, 0);
	gfx_fill(0, 0, W, H, 0);
	if (status.w) gfx_blit(status, 0, 0); else gfx_fill(0, 0, W, 29, 0x4208);
	gfx_text(6, 4, "Juego en pausa", 0xFFFF, 1);
	for (int i = 0; i < n; i++) {
		int y = 40 + i * 34;
		if (i == sel) { if (bar.w) gfx_blit(bar, 0, y); else gfx_fill(0, y, W, 28, 0xFCA0); }
		gfx_text(10, y + 3, items[i], i == sel ? 0 : 0xFFFF, 1);
	}
	if (note) gfx_text_center(W / 2, 250, note, 0xFCA0, 1);
	gfx_flip();
}

static void game_menu(void)
{
	static const char *turbo_names[] = { "Turbo (HOLD): 1.5x", "Turbo (HOLD): 2x", "Turbo (HOLD): 3x" };
	const char *items[6] = { "Continuar", "Guardar estado", "Cargar estado", 0, 0, "Salir del juego" };
	int sel = 0;
	const char *note = 0;
	for (;;) {
		items[3] = rotate ? "Pantalla: rotada" : "Pantalla: normal";
		items[4] = turbo_names[turbo_idx];
		menu_draw(items, 6, sel, note);
		note = 0;
		int k = wait_key();
		if (k == 3) sel = (sel + 5) % 6;
		else if (k == 4) sel = (sel + 1) % 6;
		else if (k == 18) break;	/* BACK: continue */
		else if (k == 0) {
			if (sel == 0) break;
			if (sel == 1) { state_save(0); note = "Estado guardado"; }
			if (sel == 2) { state_load(0); break; }
			if (sel == 3) { rotate = !rotate; save_cfg(); }
			if (sel == 4) { turbo_idx = (turbo_idx + 1) % 3; save_cfg(); if (turbo) { char b[16]; snprintf(b, sizeof b, "%d", 16743 * 10 / turbo_x10[turbo_idx]); char *v = b; rc_setvar("framelen", 1, &v); } }
			if (sel == 5) { post(K_ESC, 1); break; }
		}
	}
	memset(screen, 0, SW * SH * 2);
	/* release everything that might have been held when the menu opened */
	int codes[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, 'd', 's', K_ENTER, K_SPACE };
	for (int i = 0; i < 8; i++) post(codes[i], 0);
}

void ev_poll(int wait)
{
	struct kev k;
	static int first = 1;
	if (keyfd < 0) return;
	if (first) { first = 0; read_hold(); }	/* sound_reset has run by now: turbo can mute it safely */
	while (read(keyfd, &k, sizeof k) == sizeof k) {
		if (k.type != 1) continue;
		int pressed = k.value == 0;	/* Sony driver: 0 = pressed, 1 = released */
		if ((k.code & 0x7f) == 15) { back_held = bt_held = 0; read_hold(); continue; }	/* HOLD switch = turbo */
		k.code &= 0x7f;	/* under HOLD the driver adds 0x80; in-game the keys keep working */
		int code = k.code;
		if (rotate) {	/* the Walkman is held sideways (turned counter-clockwise) */
			if (code == 3) code = 2; else if (code == 1) code = 3; else if (code == 4) code = 1; else if (code == 2) code = 4;
		}
		switch (code) {
		case 3: post(K_UP, pressed); break;
		case 4: post(K_DOWN, pressed); break;
		case 2: post(K_LEFT, pressed); break;
		case 1: post(K_RIGHT, pressed); break;
		case 0: post('d', pressed); break;		/* A */
		case 19: post('s', pressed); break;		/* OPTION = B */
		case 18:	/* BACK: tap = START, hold 2 s = quit */
			if (pressed) { back_held = 1; gettimeofday(&back_down, 0); }
			else if (back_held) { back_held = 0; if (tap_frames > 0) post(tap_key, 0); post(K_ENTER, 1); tap_key = K_ENTER; tap_frames = 4; }
			break;
		case 20:	/* BT button: tap = SELECT, hold 2 s = in-game menu */
			if (pressed) { bt_held = 1; gettimeofday(&bt_down, 0); }
			else if (bt_held) { bt_held = 0; if (tap_frames > 0) post(tap_key, 0); post(K_SPACE, 1); tap_key = K_SPACE; tap_frames = 4; }
			break;
		case 17: if (sound && pressed && vol < 20) { vol++; bar_frames = 90; draw_bar(); } break;
		case 16: if (sound && pressed && vol > 0) { vol--; bar_frames = 90; draw_bar(); } break;
		}
	}
	if (tap_frames > 0 && !--tap_frames) post(tap_key, 0);
	if (bar_frames > 0 && !--bar_frames) draw_bar();
	struct timeval now_t; gettimeofday(&now_t, 0);
	if (back_held && (now_t.tv_sec - back_down.tv_sec) * 1000 + (now_t.tv_usec - back_down.tv_usec) / 1000 > 2000) { back_held = 0; post(K_ESC, 1); }
	if (bt_held && (now_t.tv_sec - bt_down.tv_sec) * 1000 + (now_t.tv_usec - bt_down.tv_usec) / 1000 > 2000) { bt_held = 0; game_menu(); }
	/* frame-rate log */
	static struct timeval t0; static int frames;
	struct timeval t; gettimeofday(&t, 0);
	if (!t0.tv_sec) t0 = t;
	frames++;
	if (t.tv_sec - t0.tv_sec >= 10) { fprintf(stderr, "fps %.1f rotate %d sound %d\n", frames / (double)(t.tv_sec - t0.tv_sec), rotate, sound); fflush(stderr); frames = 0; t0 = t; }
}

void joy_init() {}
void joy_poll() {}
void joy_close() {}
void kb_init() {}
void kb_poll() {}
void kb_close() {}

/* ---- system ---- */

void *sys_timer()
{
	struct timeval *tv = malloc(sizeof(struct timeval));
	gettimeofday(tv, NULL);
	return tv;
}

int sys_elapsed(struct timeval *prev)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long secs = tv.tv_sec - prev->tv_sec, usecs = tv.tv_usec - prev->tv_usec;
	*prev = tv;
	if (secs > 100 || secs < 0) return 1000000;	/* clock set meanwhile: just "long", the pacing resyncs */
	return (int)(secs * 1000000 + usecs);	/* was 1000000 + usecs for any secs: gaps over 2 s came out short */
}

void sys_sleep(int us)
{
	if (us > 0) usleep(us);
}

void sys_checkdir(char *path, int wr)
{
	char *p;
	if (access(path, X_OK | (wr ? W_OK : 0))) {
		if (!access(path, F_OK)) die("cannot access %s: %s\n", path, strerror(errno));
		p = strrchr(path, '/');
		if (!p) die("descended to root trying to create dirs\n");
		*p = 0; sys_checkdir(path, wr); *p = '/';
		if (mkdir(path, 0777)) die("cannot create %s: %s\n", path, strerror(errno));
	}
}

void sys_initpath()
{
	char *buf = getenv("HB_SAVEDIR") ? getenv("HB_SAVEDIR") : "/var/hb/gb";
	rc_setvar("savedir", 1, &buf);
	buf = ".";
	rc_setvar("rcpath", 1, &buf);
}

void sys_sanitize(char *s) {}
