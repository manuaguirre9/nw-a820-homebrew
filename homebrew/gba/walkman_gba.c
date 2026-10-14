/*
 * walkman_gba.c: Game Boy Advance for the Sony NW-A829 (homebrew).
 * A small libretro frontend around the libretro gpSP core (ARM dynarec, open-source built-in BIOS), in the
 * style of our gnuboy port (homebrew/third_party/gnuboy-master/sys/walkman/walkman.c).
 *
 *   gba /path/rom.gba          exit code 0 = quit by the user, 1 = could not start
 *
 * Video: the core renders 240x160 RGB565. Screen modes (in-game menu, saved in walkman_gba.cfg):
 *          0 = landscape 1.33x (320x213, rotated 90 deg clockwise like gnuboy's "rotada"; default)
 *          1 = landscape 1:1  (240x160 rotated, centred)
 *          2 = portrait 1:1   (240x160, device upright, centred)
 *        Frameskip is automatic (the core's "auto" mode, fed by our audio-buffer / clock status).
 * Sound: OSS /dev/sound/dsp (or $HB_DSP, e.g. /dev/sound/adsp for Bluetooth), S16 stereo. The core mixes at
 *        32768 Hz; we ask OSS for that and resample to whatever it grants (22050 on the wired DSP by default,
 *        $HB_RATE overrides). $HB_SOUND=0 = muted (clock-paced); $HB_VOL = 0..20 start volume.
 * Keys:  d-pad = d-pad (remapped when rotated), PLAY = A, OPTION = B, VOL- = L, VOL+ = R,
 *        BACK tap = START, BACK held 2 s = quit, BT tap = SELECT, BT held 2 s = in-game menu
 *        (continue, save / load state slot 0, screen mode, volume, quit). HOLD switch on = fast forward
 *        (sound muted, 1 frame in 6 drawn); keys keep working under HOLD.
 * Saves: <savedir>/<rom name>.sav (battery save, written when it changes, checked every 5 s and on exit),
 *        <savedir>/<rom name>.ss0 (state slot 0). savedir = $HB_SAVEDIR or the ROM's folder.
 * Memory: ROMs are loaded fully up to $HB_GBA_ROMMB MB (default 8, lowered if /proc/meminfo says memory is
 *        short); bigger ROMs are paged in from the file in 32 KB blocks (gpSP's own pager).
 * Logs the frame rate every 10 s to stderr. $HB_GBA_DRC=0 = interpreter only (debug).
 * Licence: GPL v2 (like gpSP).
 */
#undef _GNU_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "libretro.h"
#define GFX_NO_FB_ALIAS
#include "gfx.h"
#include "game_i18n.h"
#include "keys.h"

extern unsigned gpsp_rom_buffer_mb;	/* patched gba_memory.c: 1 MB ROM buffers to allocate */

#define SW 240
#define SH 320
#define GW 240
#define GH 160
#define FRAME_US 16743		/* 1 / 59.7275 Hz */

static const char *mode_names[] = { "Pantalla: horizontal 1.33x", "Pantalla: horizontal 1:1", "Pantalla: vertical 1:1" };

static uint16_t *screen;			/* /dev/fb/0, 240x320 RGB565 portrait */
static int mode, vol = 14, sound = 1, turbo, quit_req, menu_req, keyfd = -1, dsp = -1, out_rate;
static char rompath[512], savepath[600], statepath[600], cfgpath[600];

/* ---------------------------------------------------------------- misc */

static long long now_us(void)
{
	struct timeval t;
	gettimeofday(&t, 0);
	return (long long)t.tv_sec * 1000000 + t.tv_usec;
}

static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_INFO) return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void on_signal(int s) { (void)s; quit_req = 1; }

static int write_file(const char *path, const void *data, size_t len)
{
	char tmp[640];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int ok = fwrite(data, 1, len, f) == len;
	ok &= fflush(f) == 0;
	fsync(fileno(f));
	fclose(f);
	if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

static long read_file(const char *path, void *data, size_t len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	long n = (long)fread(data, 1, len, f);
	fclose(f);
	return n;
}

static void load_cfg(void)
{
	FILE *f = fopen(cfgpath, "r");
	if (f) { if (fscanf(f, "mode %d vol %d", &mode, &vol) < 1) mode = 0; fclose(f); }
	if (mode < 0 || mode > 2) mode = 0;
	if (vol < 0 || vol > 20) vol = 14;
}

static void save_cfg(void)
{
	FILE *f = fopen(cfgpath, "w");
	if (f) { fprintf(f, "mode %d vol %d\n", mode, vol); fclose(f); sync(); }
}

/* full-screen message with the Sony font (errors before / after the game) */
static void message(const char *l1, const char *l2, int secs)
{
	fprintf(stderr, "gba: %s %s\n", l1, l2 ? l2 : "");
	if (!gfx_fb) return;
	gfx_fill(0, 0, W, H, 0);
	gfx_text_center(W / 2, 120, l1, 0xFFFF, 1);
	if (l2) gfx_text_wrap(10, 150, l2, 0xFCA0, W - 20, 5, LINE_H);
	gfx_flip();
	if (secs) sleep(secs);
}

/* ---------------------------------------------------------------- core options */

static struct retro_audio_buffer_status_callback buf_status;
static int have_buf_status;

static const char *option(const char *key)
{
	if (!strcmp(key, "gpsp_bios")) return "builtin";		/* never Nintendo's BIOS */
	if (!strcmp(key, "gpsp_boot_mode")) return "game";
	if (!strcmp(key, "gpsp_drc")) return getenv("HB_GBA_DRC") && !strcmp(getenv("HB_GBA_DRC"), "0") ? "disabled" : "enabled";
	if (!strcmp(key, "gpsp_frameskip")) return "auto";
	if (!strcmp(key, "gpsp_sound_rate")) return "32768";		/* half the mixing work of 65536 */
	if (!strcmp(key, "gpsp_color_correction")) return "disabled";
	if (!strcmp(key, "gpsp_frame_mixing")) return "disabled";
	if (!strcmp(key, "gpsp_rumble")) return "disabled";
	if (!strcmp(key, "gpsp_serial")) return "disabled";
	return 0;
}

static bool environ_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_printf;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		v->value = v->key ? option(v->key) : 0;
		return v->value != 0;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
		if (data) buf_status = *(struct retro_audio_buffer_status_callback *)data;
		have_buf_status = data && buf_status.callback;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		if (data) fprintf(stderr, "gba: %s\n", ((struct retro_message *)data)->msg);
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return true;
	default:
		return false;
	}
}

/* ---------------------------------------------------------------- video */

static int ymap[SH], xoff[SW];	/* per mode: source column for each screen row, source row offset per screen x */
static int map_pitch = -1, map_mode = -1, drawn, skipped;

static void build_maps(int pitch_px)
{
	map_pitch = pitch_px; map_mode = mode;
	if (mode == 0) {	/* 320x213: screen (x, y) shows game (y * 240 / 320, 159 - (x - 13) * 160 / 213) */
		for (int y = 0; y < SH; y++) ymap[y] = y * GW / SH;
		for (int x = 0; x < 213; x++) xoff[x] = (GH - 1 - x * GH / 213) * pitch_px;
	} else if (mode == 1) {	/* 240x160 rotated: screen rows 40..279, columns 40..199 */
		for (int y = 0; y < GW; y++) ymap[y] = y;
		for (int x = 0; x < GH; x++) xoff[x] = (GH - 1 - x) * pitch_px;
	}
}

static void clear_screen(void) { if (screen) memset(screen, 0, SW * SH * 2); }

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	static int ff;
	if (!data) { skipped++; return; }
	if (turbo && ff++ % 6) { skipped++; return; }
	drawn++;
	if (!screen || w != GW || h != GH) return;
	const uint16_t *src = data;
	int pp = (int)(pitch / 2);
	if (pp != map_pitch || mode != map_mode) build_maps(pp);
	if (mode == 2) {
		for (int y = 0; y < GH; y++) memcpy(screen + (80 + y) * SW, src + y * pp, GW * 2);
	} else if (mode == 0) {
		/* consecutive screen rows read neighbouring source columns, so the ~7 KB of source lines they touch stay
		   in the D-cache; screen writes are sequential, two pixels per 32-bit store (x0 = 13 is odd: 1 + 106 * 2) */
		for (int y = 0; y < SH; y++) {
			const uint16_t *s = src + ymap[y];
			uint16_t *d = screen + y * SW + 13;
			d[0] = s[xoff[0]];
			uint32_t *d32 = (uint32_t *)(d + 1);
			for (int x = 1; x < 213; x += 2) *d32++ = s[xoff[x]] | (uint32_t)s[xoff[x + 1]] << 16;
		}
	} else {
		for (int y = 0; y < GW; y++) {
			const uint16_t *s = src + ymap[y];
			uint32_t *d32 = (uint32_t *)(screen + (40 + y) * SW + 40);
			for (int x = 0; x < GH; x += 2) *d32++ = s[xoff[x]] | (uint32_t)s[xoff[x + 1]] << 16;
		}
	}
}

/* ---------------------------------------------------------------- audio */

#define OUTMAX 4096
static int16_t outbuf[OUTMAX * 2];
static int outn;
static uint32_t rs_pos, rs_step;	/* 16.16 resampler position / step (input samples per output sample) */
static int16_t rs_prev[2];

static int gain_for(int v)
{
	int g = 4096;	/* 2 dB steps, 20 = 0 dB */
	for (int s = v; s < 20; s++) g = g * 3254 / 4096;
	return v <= 0 ? 0 : g;
}

static void audio_open(void)
{
	const char *dev = getenv("HB_DSP");
	int bt = dev && strstr(dev, "adsp");
	if (getenv("HB_VOL")) vol = atoi(getenv("HB_VOL"));
	if (getenv("HB_SOUND") && !strcmp(getenv("HB_SOUND"), "0")) sound = 0;
	if (!sound) return;
	dsp = open(dev ? dev : "/dev/sound/dsp", O_WRONLY | (bt ? O_NONBLOCK : 0));
	if (dsp < 0) { fprintf(stderr, "gba: no dsp (%s), muted\n", strerror(errno)); sound = 0; return; }
	int n = 0x0008000B;	/* 8 fragments of 2 KB: ~90 ms at 22050 Hz stereo, keeps latency low */
	ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &n);
	n = AFMT_S16_LE; ioctl(dsp, SNDCTL_DSP_SETFMT, &n);
	n = 2; ioctl(dsp, SNDCTL_DSP_CHANNELS, &n);
	int want = getenv("HB_RATE") ? atoi(getenv("HB_RATE")) : bt ? 44100 : 22050;
	n = want;
	if (ioctl(dsp, SNDCTL_DSP_SPEED, &n) < 0 || n < 8000 || n > 96000) n = want;
	out_rate = n;
	rs_step = (uint32_t)(((uint64_t)32768 << 16) / (uint32_t)out_rate);
	fprintf(stderr, "gba: sound %d Hz on %s\n", out_rate, dev ? dev : "/dev/sound/dsp");
}

/* 32768 Hz stereo from the core -> out_rate, linear interpolation, volume */
static size_t audio_batch_cb(const int16_t *in, size_t frames)
{
	if (dsp < 0 || turbo) return frames;
	int g = gain_for(vol);
	uint32_t pos = rs_pos;
	uint32_t end = (uint32_t)frames << 16;
	while (pos < end && outn < OUTMAX) {
		uint32_t i = pos >> 16, f = pos & 0xFFFF;
		for (int c = 0; c < 2; c++) {
			int a = i ? in[(i - 1) * 2 + c] : rs_prev[c], b = in[i * 2 + c];
			int v = a + (int)(((b - a) * (int)(f >> 1)) >> 15);
			v = (v * g) >> 12;
			outbuf[outn * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		outn++;
		pos += rs_step;
	}
	rs_pos = pos - end;
	if (frames) { rs_prev[0] = in[(frames - 1) * 2]; rs_prev[1] = in[(frames - 1) * 2 + 1]; }
	return frames;
}

static void audio_sample_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio_batch_cb(s, 1); }

static void audio_flush(void)
{
	if (dsp >= 0 && outn) {
		int n = write(dsp, outbuf, outn * 4);
		(void)n;	/* blocking write = the game's pacing; Bluetooth (non-blocking) drops when full */
	}
	outn = 0;
}

/* queued output in microseconds (-1 if unknown) */
static int audio_queued_us(void)
{
	audio_buf_info bi;
	if (dsp < 0 || ioctl(dsp, SNDCTL_DSP_GETOSPACE, &bi) < 0 || bi.fragstotal <= 0) return -1;
	int queued = bi.fragstotal * bi.fragsize - bi.bytes;
	return (int)((long long)queued * 1000000 / (out_rate * 4));
}

/* ---------------------------------------------------------------- input */

struct kev { long sec, usec; unsigned short type, code; int value; };
static unsigned pad;			/* RETRO_DEVICE_ID_JOYPAD_* bits */
static long long back_down, bt_down;
static int back_held, bt_held, tap_bit = -1, tap_frames;
static int fps_shown10;			/* last measured fps x10, shown in the menu */

static void read_hold(void)
{
	int v = 0;
	if (keyfd >= 0 && ioctl(keyfd, 0x80044519, &v) >= 0) turbo = v & 1;
}

static void set_bit(int id, int pressed)
{
	if (pressed) pad |= 1u << id; else pad &= ~(1u << id);
}

static void tap(int id)	/* START / SELECT: pressed now, released 4 frames later */
{
	if (tap_frames > 0 && tap_bit >= 0) set_bit(tap_bit, 0);
	set_bit(id, 1); tap_bit = id; tap_frames = 4;
}

static void input_poll_cb(void)
{
	struct kev k;
	static int first = 1;
	if (keyfd < 0) return;
	if (first) { first = 0; read_hold(); }
	while (read(keyfd, &k, sizeof k) == sizeof k) {
		if (k.type != 1) continue;
		int pressed = k.value == 0;	/* Sony driver: 0 = pressed, 1 = released */
		if ((k.code & 0x7f) == K_HOLD_B) { back_held = bt_held = 0; read_hold(); continue; }
		int code = k.code & 0x7f;	/* under HOLD the driver adds 0x80 */
		if (mode != 2) {	/* rotated: the Walkman is held sideways (turned counter-clockwise), as in gnuboy */
			if (code == K_UP) code = K_LEFT; else if (code == K_RIGHT) code = K_UP;
			else if (code == K_DOWN) code = K_RIGHT; else if (code == K_LEFT) code = K_DOWN;
		}
		switch (code) {
		case K_UP: set_bit(RETRO_DEVICE_ID_JOYPAD_UP, pressed); break;
		case K_DOWN: set_bit(RETRO_DEVICE_ID_JOYPAD_DOWN, pressed); break;
		case K_LEFT: set_bit(RETRO_DEVICE_ID_JOYPAD_LEFT, pressed); break;
		case K_RIGHT: set_bit(RETRO_DEVICE_ID_JOYPAD_RIGHT, pressed); break;
		case K_PLAY: set_bit(RETRO_DEVICE_ID_JOYPAD_A, pressed); break;
		case K_OPTION: set_bit(RETRO_DEVICE_ID_JOYPAD_B, pressed); break;
		case K_VOLDN: set_bit(RETRO_DEVICE_ID_JOYPAD_L, pressed); break;
		case K_VOLUP: set_bit(RETRO_DEVICE_ID_JOYPAD_R, pressed); break;
		case K_BACK:	/* tap = START, hold 2 s = quit */
			if (pressed) { back_held = 1; back_down = now_us(); }
			else if (back_held) { back_held = 0; tap(RETRO_DEVICE_ID_JOYPAD_START); }
			break;
		case K_BT:	/* tap = SELECT, hold 2 s = in-game menu */
			if (pressed) { bt_held = 1; bt_down = now_us(); }
			else if (bt_held) { bt_held = 0; tap(RETRO_DEVICE_ID_JOYPAD_SELECT); }
			break;
		}
	}
	if (tap_frames > 0 && !--tap_frames && tap_bit >= 0) set_bit(tap_bit, 0);
	long long t = now_us();
	if (back_held && t - back_down > 2000000) { back_held = 0; quit_req = 1; }
	if (bt_held && t - bt_down > 2000000) { bt_held = 0; menu_req = 1; }	/* opened between frames, not inside retro_run */
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port || device != RETRO_DEVICE_JOYPAD) return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pad;
	return id < 16 ? (pad >> id) & 1 : 0;
}

/* ---------------------------------------------------------------- saves */

static uint8_t sram_shadow[0x20000];
static int sram_exists;

static void sram_load(void)
{
	uint8_t *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!p || n > sizeof sram_shadow) return;
	long r = read_file(savepath, p, n);
	sram_exists = r > 0;
	memcpy(sram_shadow, p, n);
	if (r > 0) fprintf(stderr, "gba: loaded %s (%ld bytes)\n", savepath, r);
}

static void sram_save(int force)
{
	uint8_t *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!p || n > sizeof sram_shadow) return;
	if (!force && !memcmp(p, sram_shadow, n)) return;
	if (!sram_exists) {	/* games without a battery save never produce a .sav */
		size_t i = 0;
		while (i < n && p[i] == 0xFF) i++;
		if (i == n) return;
	}
	if (write_file(savepath, p, n) == 0) { memcpy(sram_shadow, p, n); sram_exists = 1; fprintf(stderr, "gba: saved %s\n", savepath); }
	else fprintf(stderr, "gba: cannot write %s: %s\n", savepath, strerror(errno));
}

static int state_save(void)
{
	size_t n = retro_serialize_size();
	void *b = n ? malloc(n) : 0;
	int ok = b && retro_serialize(b, n) && write_file(statepath, b, n) == 0;
	free(b);
	return ok;
}

static int state_load(void)
{
	size_t n = retro_serialize_size();
	void *b = n ? malloc(n) : 0;
	int ok = b && read_file(statepath, b, n) == (long)n && retro_unserialize(b, n);
	free(b);
	return ok;
}

/* ---------------------------------------------------------------- in-game menu (like gnuboy's) */

static int wait_key(void)
{
	struct kev k;
	for (;;) {
		fd_set rs; FD_ZERO(&rs); FD_SET(keyfd, &rs);
		if (select(keyfd + 1, &rs, 0, 0, 0) <= 0) continue;
		while (read(keyfd, &k, sizeof k) == sizeof k)
			if (k.type == 1 && k.value == 0 && (k.code & 0x7f) != K_HOLD_B) return k.code & 0x7f;
	}
}

static void menu_draw(const char *const *items, int n, int sel, const char *note)
{
	struct img bar = gfx_tex(14, 4), status = gfx_tex(16, 0);
	char info[64];
	gfx_fill(0, 0, W, H, 0);
	if (status.w) gfx_blit(status, 0, 0); else gfx_fill(0, 0, W, 29, 0x4208);
	gfx_text(6, 4, "Juego en pausa", 0xFFFF, 1);
	for (int i = 0; i < n; i++) {
		int y = 40 + i * 34;
		if (i == sel) { if (bar.w) gfx_blit(bar, 0, y); else gfx_fill(0, y, W, 28, 0xFCA0); }
		gfx_text(10, y + 3, items[i], i == sel ? 0 : 0xFFFF, 1);
	}
	if (note) gfx_text_center(W / 2, 250, note, 0xFCA0, 1);
	snprintf(info, sizeof info, "%d.%d fps", fps_shown10 / 10, fps_shown10 % 10);
	gfx_text_center(W / 2, 290, info, 0x8410, 1);
	gfx_flip();
}

static void game_menu(void)
{
	char volname[32];
	const char *items[6] = { "Continuar", "Guardar estado", "Cargar estado", 0, 0, "Salir del juego" };
	int sel = 0;
	const char *note = 0;
	sram_save(0);
	for (;;) {
		items[3] = mode_names[mode];
		snprintf(volname, sizeof volname, sound ? "Volumen: %d  < >" : "Volumen: (sin sonido)", vol);
		items[4] = volname;
		menu_draw(items, 6, sel, note);
		note = 0;
		int k = wait_key();
		if (k == K_UP) sel = (sel + 5) % 6;
		else if (k == K_DOWN) sel = (sel + 1) % 6;
		else if (k == K_BACK) break;	/* continue */
		else if (sel == 4 && (k == K_LEFT || k == K_RIGHT || k == K_PLAY)) {
			if (k == K_LEFT) vol = vol > 0 ? vol - 1 : 0;
			else vol = vol < 20 ? vol + 1 : (k == K_PLAY ? 0 : 20);
			save_cfg();
		} else if (sel == 3 && (k == K_LEFT || k == K_RIGHT || k == K_PLAY)) {
			mode = (mode + (k == K_LEFT ? 2 : 1)) % 3; save_cfg();
		} else if (k == K_PLAY) {
			if (sel == 0) break;
			if (sel == 1) note = state_save() ? "Estado guardado" : "No se pudo guardar";
			if (sel == 2) { if (state_load()) break; note = "No hay estado guardado"; }
			if (sel == 5) { quit_req = 1; break; }
		}
	}
	clear_screen();
	pad = 0; tap_frames = 0;	/* release everything that was held when the menu opened */
	read_hold();
}

/* ---------------------------------------------------------------- setup */

/* MB of RAM we can reasonably take: MemFree + Cached from /proc/meminfo (-1 if unknown) */
static int avail_mb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	long kb = 0, v;
	int found = 0;
	if (!f) return -1;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "MemFree: %ld", &v) == 1) { kb += v; found = 1; }
		else if (sscanf(line, "Cached: %ld", &v) == 1) kb += v;
	}
	fclose(f);
	return found ? (int)(kb / 1024) : -1;
}

static void make_paths(void)
{
	const char *sd = getenv("HB_SAVEDIR");
	char dir[512], base[256];
	const char *slash = strrchr(rompath, '/');
	const char *name = slash ? slash + 1 : rompath;
	if (sd && *sd) { snprintf(dir, sizeof dir, "%s", sd); mkdir(dir, 0777); }
	else if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - rompath), rompath);
	else snprintf(dir, sizeof dir, ".");
	snprintf(base, sizeof base, "%s", name);
	char *dot = strrchr(base, '.');
	if (dot && dot != base) *dot = 0;
	snprintf(savepath, sizeof savepath, "%s/%s.sav", dir, base);
	snprintf(statepath, sizeof statepath, "%s/%s.ss0", dir, base);
	snprintf(cfgpath, sizeof cfgpath, "%s/walkman_gba.cfg", dir);
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: gba /path/rom.gba\n"); return 1; }
	snprintf(rompath, sizeof rompath, "%s", argv[1]);
	make_paths();
	load_cfg();
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGPIPE, SIG_IGN);

	gfx_open();
	game_i18n_init();
	gfx_load_res("/usr/local/share/gemini/res");
	int fd = open("/dev/fb/0", O_RDWR);
	if (fd >= 0) {
		screen = mmap(0, SW * SH * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (screen == MAP_FAILED) screen = 0;
	}
	if (!screen) fprintf(stderr, "gba: no framebuffer, running blind\n");
	clear_screen();

	struct stat st;
	if (stat(rompath, &st) || !S_ISREG(st.st_mode) || st.st_size < 192) { message("No se puede abrir la ROM", rompath, 3); return 1; }
	if (st.st_size > 32 * 1024 * 1024) { message("ROM demasiado grande", "Las ROM de GBA tienen como máximo 32 MB.", 3); return 1; }

	/* ROM buffer: whole ROM up to HB_GBA_ROMMB (8) MB and what memory allows; the rest is paged from the file */
	int rom_mb = (int)((st.st_size + 0xFFFFF) >> 20), cap = getenv("HB_GBA_ROMMB") ? atoi(getenv("HB_GBA_ROMMB")) : 8;
	int avail = avail_mb();
	if (avail > 0 && cap > avail - 4) cap = avail - 4;	/* ~3.5 MB for the rest of the emulator (JIT caches, RAM, video) */
	if (cap < 1) cap = 1;
	if (cap > 32) cap = 32;
	gpsp_rom_buffer_mb = rom_mb < cap ? rom_mb : cap;
	fprintf(stderr, "gba: %s: %ld KB, ROM buffer %u MB%s (free ~%d MB)\n", rompath, (long)(st.st_size >> 10),
		gpsp_rom_buffer_mb, rom_mb > (int)gpsp_rom_buffer_mb ? ", paging from file" : "", avail);

	keyfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	audio_open();

	retro_set_environment(environ_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();
	struct retro_game_info gi = { rompath, 0, 0, 0 };
	if (!retro_load_game(&gi)) { message("No se pudo cargar el juego", "¿Memoria insuficiente o ROM dañada?", 3); return 1; }
	sram_load();

	long long next = now_us(), t0 = next, last_sram = next;
	int frames = 0, drawn0 = 0, consec_skip = 0, last_skipped = 0;
	while (!quit_req) {
		/* frameskip: tell the core an underrun is near when we are behind the clock (or the sound buffer
		   is running dry); never skip more than 4 frames in a row so the picture keeps moving */
		long long t = now_us();
		int behind;
		int q = sound && !turbo ? audio_queued_us() : -1;
		if (q >= 0) behind = q < 2 * FRAME_US;
		else behind = t > next + FRAME_US / 2;
		if (consec_skip >= 4 || turbo) behind = 0;	/* turbo does its own 1-in-6 skipping in video_cb */
		if (have_buf_status) buf_status.callback(true, behind ? 0 : 100, behind);

		retro_run();
		audio_flush();
		if (menu_req) { menu_req = 0; game_menu(); }
		if (skipped != last_skipped) { consec_skip++; last_skipped = skipped; } else consec_skip = 0;
		frames++;

		/* pacing: blocking sound writes pace us when sound is on; otherwise follow the clock */
		next += FRAME_US;
		t = now_us();
		if (turbo) next = t;
		else if (q < 0 && t < next) usleep((useconds_t)(next - t));
		else if (t > next + 200000 || (q >= 0 && t < next - 200000)) next = t;	/* too far off: resync */

		if (t - last_sram > 5000000) { last_sram = t; sram_save(0); }
		if (t - t0 >= 10000000) {
			fps_shown10 = (int)((long long)frames * 10000000 / (t - t0));
			fprintf(stderr, "fps %d.%d drawn %d/%d mode %d sound %d turbo %d\n", fps_shown10 / 10, fps_shown10 % 10,
				drawn - drawn0, frames, mode, sound, turbo);
			fflush(stderr);
			frames = 0; drawn0 = drawn; t0 = t;
		}
	}
	sram_save(0);
	retro_unload_game();
	retro_deinit();
	clear_screen();
	if (dsp >= 0) close(dsp);
	return 0;
}
