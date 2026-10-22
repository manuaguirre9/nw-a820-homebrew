/* hbplayer: music / audiobook / podcast player for the Sony NW-A829 (homebrew mode, runs instead of the Sony UI).
 *
 *   hbplayer ask SECONDS       boot prompt: exit 0 if HOME is pressed within SECONDS, else 1
 *   hbplayer run STATEDIR      the player (library in /contents/Music, /contents/Audiolibros, /contents/Podcasts)
 *   hbplayer watch STATEDIR    background helper next to the Sony UI: HOME held 3 s reboots into the player
 *
 * UI: Sony's own textures, font and home animation (screens.c), double buffered, slide transitions.
 * Playback runs in the background (engine.c): every UI loop calls eng_step(). Keys (../common/keys.h).
 * State in STATEDIR: pos.txt (positions, last file, volume), bt.txt, bt/, gb/, log.txt. */
#include "hb.h"
#include "podsearch.h"
#include <sys/soundcard.h>
#include <ucontext.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RESDIR "/usr/local/share/gemini/res"
#define MAXITEMS 1000	/* per folder; the arrays are static, only the entries used take RAM */
int ui_brightness = 80, ui_dim_secs = 30;
int ui_big_rows;	/* Ajustes > Pantalla > Filas grandes con portada */
int ui_album_grid;	/* album lists as a mosaic (OPTION in the album list) */
int ui_beep;		/* Ajustes > Pitido */

/* Sony's key beep: the codec's beep generator (/dev/sound/beep, icx1087_beep.h) with the patterns Sony's own
 * player uses (mptapp's table: 2 kHz; one 100 ms beep, two or three of 50 ms, a long one of 300 ms).
 * Wired headphones only: the generator is in the codec, a Bluetooth headset would not hear it. */
struct beep_time { unsigned short on, off; };
struct beep_play { unsigned freq; int pri; int repeat; unsigned bytes; const struct beep_time *pattern; };
#define BEEP_PLAY_IOC 0x40146203	/* _IOW('b', 3, beep_play_t) */
void beep(int kind)
{
	static const struct beep_time one[] = { { 100, 0 } }, two[] = { { 50, 50 }, { 50, 0 } },
		three[] = { { 50, 50 }, { 50, 50 }, { 50, 0 } }, lng[] = { { 300, 0 } };
	static int fd = -2;
	if (!ui_beep || ui_st.bt) return;
	if (fd == -2) { fd = open("/dev/sound/beep", O_WRONLY); if (fd < 0) lg("beep: no /dev/sound/beep"); else fcntl(fd, F_SETFD, FD_CLOEXEC); }
	if (fd < 0) return;
	struct beep_play p = { 2000, 2, 0, 0, 0 };	/* BEEP_PRI_NORMAL, no repeat */
	switch (kind) {
	case BEEP_TWO: p.pattern = two; p.bytes = sizeof two; break;
	case BEEP_THREE: p.pattern = three; p.bytes = sizeof three; break;
	case BEEP_LONG: p.pattern = lng; p.bytes = sizeof lng; break;
	default: p.pattern = one; p.bytes = sizeof one;
	}
	/* the beep driver lifts the codec's output volume while it sounds (that is where our wired volume lives), so the
	 * song jumped up and covered it: the music's own mixer input (SOUND_MIXER_PCM, before the beep is mixed in)
	 * goes down for the length of the beep and back after it */
	static int mfd = -2;
	if (mfd == -2) { mfd = open("/dev/sound/mixer", O_RDWR); if (mfd >= 0) fcntl(mfd, F_SETFD, FD_CLOEXEC); }
	int pcm = -1, low = 0;
	if (mfd >= 0 && ioctl(mfd, SOUND_MIXER_READ_PCM, &pcm) == 0) ioctl(mfd, SOUND_MIXER_WRITE_PCM, &low);
	else pcm = -1;
	if (ioctl(fd, BEEP_PLAY_IOC, &p) < 0) lg("beep: ioctl failed (%d)", errno);
	int ms = 0;
	for (unsigned i = 0; i < p.bytes / sizeof p.pattern[0]; i++) ms += p.pattern[i].on + p.pattern[i].off;
	double until = now() + (ms + 40) / 1000.0;	/* the whole pattern, then the music comes back */
	while (now() < until) { if (eng_playing()) eng_step(); usleep(5000); }
	if (pcm >= 0) ioctl(mfd, SOUND_MIXER_WRITE_PCM, &pcm);
}	/* backlight %, s without keys before it goes off while playing */
#define DIM_IDLE 60
#define AUTO_SUSPEND (10 * 60)	/* s idle and not playing before suspending (screen on: browsing) */
#define AUTO_SUSPEND_DARK (3 * 60)	/* s idle, paused / stopped and the screen already off: amp + codec + CPU off sooner */
#define BRIGHT ui_brightness

struct ev { long sec, usec; unsigned short type, code; int value; };

static int keyfd = -1;
static FILE *logf;
static int cpu_mhz;	/* 0 until the first set_cpu: the unit BOOTS AT 248 MHz (measured), not 124 */
static int in_game, in_slide, force_redraw, pos_dirty;
static int held_code = -1;	/* direction key currently held, for auto-repeat */
static double held_since, last_repeat;
char statedir[PATHLEN] = "/tmp";

void set_cloexec(int fd) { if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC); }

#define LOG_MAX (128 << 10)	/* log.txt -> log.1.txt past this: /var is 4 MB and the Sony UI writes there too */
static void log_rotate(void);
void lg(const char *fmt, ...)
{
	if (!logf) return;
	va_list ap; va_start(ap, fmt);
	fprintf(logf, "[%ld] ", (long)time(0));
	vfprintf(logf, fmt, ap); fputc('\n', logf); fflush(logf);
	va_end(ap);
	if (ftell(logf) > LOG_MAX) log_rotate();
}

/* settings files: write <fn>.tmp, fsync, rename over <fn>, so a reset mid-save keeps the old file intact */
FILE *safe_open(const char *fn, char *tmp, size_t tmpmax)
{
	snprintf(tmp, tmpmax, "%s.tmp", fn);
	return fopen(tmp, "w");
}
void safe_close(FILE *f, const char *tmp, const char *fn)
{
	int bad = ferror(f) || fflush(f) || fsync(fileno(f));
	if (fclose(f) || bad) { unlink(tmp); lg("save %s failed (disk full?)", fn); return; }	/* the old file stays */
	if (rename(tmp, fn)) lg("save %s failed", fn);
}

double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static void log_rotate(void)
{
	char lf[PATHLEN + 16], old[PATHLEN + 16];
	snprintf(lf, sizeof lf, "%s/log.txt", statedir); snprintf(old, sizeof old, "%s/log.1.txt", statedir);
	fclose(logf);
	rename(lf, old);
	logf = fopen(lf, "a");
	if (logf) set_cloexec(fileno(logf));
}

/* ---------- hardware watchdog (/dev/misc/watchdog, like Sony's watchdog_test: SETTIMEOUT + KEEPALIVE) ---------- */

static int wdfd = -1;

static void wd_arm(void)
{
	if (wdfd >= 0) return;
	wdfd = open("/dev/misc/watchdog", O_RDWR);
	if (wdfd < 0) { lg("watchdog: open failed"); return; }
	set_cloexec(wdfd);
	int t = 15;
	if (ioctl(wdfd, 0xC0045706, &t) < 0) lg("watchdog: settimeout failed");
	ioctl(wdfd, 0x80045705, 0);
	lg("watchdog armed, %d s", t);
}

void hb_alive(void)
{
	static time_t last;
	if (wdfd < 0 || time(0) == last) return;
	last = time(0);
	ioctl(wdfd, 0x80045705, 0);	/* WDIOC_KEEPALIVE */
}

static void wd_disarm(void);
void set_cpu(int mhz);
static void on_fatal(int sig);
/* crash recovery: once a second idle_tick keeps this line up to date ("bytes seconds playing path"); the crash
 * handler writes it to <statedir>/resume.txt with plain syscalls (nothing that could deadlock in a broken process)
 * and the next start continues from there, playing again if it was playing */
static char crash_line[PATHLEN + 64];
static int crash_len;
static void crash_save(void)
{
	if (crash_len <= 0) return;
	char fn[PATHLEN + 16];
	int n = 0; const char *s = statedir;
	while (*s && n < PATHLEN) fn[n++] = *s++;
	const char *leaf = "/resume.txt";
	while (*leaf) fn[n++] = *leaf++;
	fn[n] = 0;
	int fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return;
	ssize_t w = write(fd, crash_line, crash_len); (void)w;
	fsync(fd); close(fd);
}
/* crash report: signal, fault address, PC / LR (addresses match the unstripped hbplayer.sym build). Written with
 * plain write(): a crash inside stdio or malloc would deadlock on their locks */
static int put_hex(char *b, const char *label, unsigned long v)
{
	int n = 0;
	while (*label) b[n++] = *label++;
	for (int i = 28; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(v >> i) & 15];
	return n;
}
static void on_fatal_info(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	char b[200];
	int n = put_hex(b, "FATAL sig ", (unsigned long)sig);
	n += put_hex(b + n, " addr ", si ? (unsigned long)si->si_addr : 0);
	n += put_hex(b + n, " pc ", uc ? uc->uc_mcontext.arm_pc : 0);
	n += put_hex(b + n, " lr ", uc ? uc->uc_mcontext.arm_lr : 0);
	n += put_hex(b + n, " sp ", uc ? uc->uc_mcontext.arm_sp : 0);
	b[n++] = '\n';
	ssize_t w = write(2, b, n); (void)w;
	crash_save();	/* first: the position */
	if (logf) { w = write(fileno(logf), b, n); (void)w; }	/* straight to the file, past stdio's buffer */
	on_fatal(sig);
}

static void on_fatal(int sig)
{
	{ int v = 0; ioctl(gfx_fd, 0x40044E35, &v); }	/* LCD refresh back on for the Sony UI that follows */
	if (wdfd >= 0) { int opt = 1; ioctl(wdfd, 0x80045704, &opt); ssize_t w = write(wdfd, "V", 1); (void)w; close(wdfd); wdfd = -1; }
	{ int fd = open("/proc/pm", O_WRONLY); if (fd >= 0) { ssize_t w = write(fd, "M124", 4); (void)w; close(fd); } }
	signal(sig, SIG_DFL);
	raise(sig);
}

static void wd_disarm(void)
{
	if (wdfd < 0) return;
	int opt = 1;	/* WDIOS_DISABLECARD */
	ioctl(wdfd, 0x80045704, &opt);
	if (write(wdfd, "V", 1) < 0) lg("watchdog: magic close failed");
	close(wdfd);
	wdfd = -1;
	lg("watchdog disarmed");
}

/* ---------- status: battery, hold ---------- */

/* /dev/icx1087_power: 0x80045000 status (0x80 charging), 0x80045002 raw ADC (~53.95 per volt), 0x80045003 level 0..5.
 * The ADC is not linear in charge: measured on this unit (2026-10, full discharges while playing, screen off) it
 * falls ~2/h from 224 to 207, sits on a plateau around 205..197 and drops fast below 195; the player cuts off at
 * 177. Load lowers the reading by a few units (248 MHz, the first seconds after boot), so the shown percentage
 * follows a slow average and the cut-off needs three low readings in a row (one a minute apart is 20 s each). */
static int last_adc, batt_low;	/* batt_low: consecutive readings at the cut-off level */
static double adc_avg;
static void battery_update(void)
{
	static time_t when;
	if (when && time(0) - when < 20) return;
	when = time(0);
	int fd = open("/dev/icx1087_power", O_RDONLY);
	if (fd < 0) return;
	int st = 0, adc = -100, lvl = -1;
	ioctl(fd, 0x80045000, &st);
	ioctl(fd, 0x80045002, &adc);
	ioctl(fd, 0x80045003, &lvl);
	close(fd);
	int was_charging = ui_st.charging;
	ui_st.charging = (st & 0x80) != 0;
	last_adc = adc;
	if (adc > 0) {
		/* the average restarts when charging starts or stops (the charger lifts the reading at once) */
		if (!adc_avg || was_charging != ui_st.charging) adc_avg = adc;
		else adc_avg += (adc - adc_avg) * (adc > adc_avg ? 0.5 : 0.25);	/* recovers quicker than it sags */
		ui_st.batt = batt_pct(adc_avg);
		batt_low = adc <= 177 && !ui_st.charging ? batt_low + 1 : 0;
	}
	else if (lvl >= 0) ui_st.batt = lvl * 20;
	/* battery trace every 5 min (for measuring consumption per mode): adc ~ volts * 53.95 */
	static time_t traced;
	if (time(0) - traced >= 300) {
		traced = time(0);
		lg("batt adc=%d lvl=%d st=0x%x pct=%d playing=%d bt=%d screen=%d cpu=%d", adc, lvl, st, ui_st.batt, eng_playing(), bt_is_on(), backlight_on, cpu_mhz);
	}
}

/* HOLD: EVIOCGLED(4) on event0 returns 4 (bytes), value 1 = HOLD on */
static void read_hold(void)
{
	int v = 0;
	if (ioctl(keyfd, 0x80044519, &v) >= 0) ui_st.hold = v & 1;
}

/* ---------- screen helpers ---------- */

int backlight_on = 1;
static time_t last_key;
static double popup_until;
static int pending_slide;
static double key_at;	/* when the last key came: how long a new screen takes to show (flip) */

/* between the slide's 4 steps: no engine step (a track start can wait seconds for the DSP helper and froze the
 * animation; the audio buffers cover the ~40 ms) */
static void slide_step(void) { hb_alive(); usleep(9000); }

void ui_tick(void) { hb_alive(); if (eng_playing()) eng_step(); }

void flip(void)
{
	if (pending_slide) {
		double t = now();
		in_slide = 1; gfx_slide(pending_slide, slide_step); in_slide = 0; pending_slide = 0;
		int ms = (int)((now() - key_at) * 1000);
		if (key_at && ms > 150 && ms < 10000) lg("ui: new screen %d ms after the key (slide %d ms)", ms, (int)((now() - t) * 1000));
	}
	else gfx_flip();
}

void wake(void)
{
	last_key = time(0);
	if (!backlight_on) { gfx_lcd(1); gfx_backlight(BRIGHT); backlight_on = 1; }
}

int popup_active(void) { return popup_until != 0; }
int popup_expired(void)
{
	if (popup_until && now() > popup_until) { popup_until = 0; return 1; }
	return 0;
}

void show_volume(void)
{
	wake();
	aout_set_volume();
	scr_volume(volume, VOLMAX);
	gfx_flip();
	popup_until = now() + 1.5;
}

static void show_hold(void)
{
	if (!backlight_on) return;
	scr_hold();
	gfx_flip();
	popup_until = now() + 1.0;
}

void eng_note(const char *msg)
{
	lg("note: %s", msg);
	if (!backlight_on || in_game || in_slide) return;
	scr_toast(msg);
	gfx_flip();
	popup_until = now() + 2.0;
}

/* how long the UI may sleep before the audio engine needs feeding: the 16-bit wired queue holds ~740 ms, the
 * 24-bit one ~370 ms; in direct mode the DSP feeds itself from ~6 s of input; Bluetooth keeps the short period */
static int audio_period(void)
{
	if (eng_direct()) {	/* the DSP plays by itself; near the end of a track look more often, so the next one starts sooner */
		int t = dec_total_sec();
		return t > 0 && dec_pos_sec() > t - 2.5 ? 50 : 500;
	}
	if (aout_is_bt() || !aout_space_known()) return 20;
	if (aout_is32() || dec_on_dsp() || dspfx_running()) return 100;	/* DSP decoding / effects: PCM comes back in steps */
	return 250;
}

/* waits for a key event. Meanwhile it keeps the background work going: podcast downloads (every ~10 ms) and the
 * audio engine (every ~20 ms; the buffer holds ~0.4 s), so no screen that waits for a key can starve the audio. */
static int raw_event(struct ev *e, int timeout_ms)
{
	double end = now() + timeout_ms / 1000.0;
	for (;;) {
		int slice = timeout_ms, feed = eng_playing() && !in_game, net = dl_active() || pod_net_busy();
		if (net || feed) {
			if (net) dl_tick();	/* downloads, or just the phone link a podcast screen holds */
			if (feed) eng_step();
			int cap = dl_active() ? 10 : net ? 50 : audio_period();
			if (feed && audio_period() < cap) cap = audio_period();
			double rem = (end - now()) * 1000;
			slice = rem > cap ? cap : rem > 0 ? (int)rem : 0;
		}
		fd_set rs; FD_ZERO(&rs); FD_SET(keyfd, &rs);
		struct timeval tv = { slice / 1000, (slice % 1000) * 1000 };
		if (select(keyfd + 1, &rs, 0, 0, &tv) > 0) return read(keyfd, e, sizeof *e) == sizeof *e;
		if ((!dl_active() && !pod_net_busy() && !(eng_playing() && !in_game)) || now() >= end) return 0;
	}
}

int key_waiting(void)
{
	fd_set rs; FD_ZERO(&rs); FD_SET(keyfd, &rs);
	struct timeval tv = { 0, 0 };
	return select(keyfd + 1, &rs, 0, 0, &tv) > 0;
}

static double back_since;
static double ui_boost_until;	/* 248 MHz until then (the last key + 3 s), see cpu_governor */
static void drain_keys(void)
{
	struct ev e;
	while (raw_event(&e, 30)) ;
	held_code = -1;
	back_since = 0;	/* a swallowed release must not look like HOME held */
}

static void back_to_sony(void);
static void usb_check(void);
/* back_since (above): BACK/HOME held down since (0 = up): 3 s anywhere returns to the Sony UI */

#ifdef HB_DEV
/* on-device UI tests (hbtest build): HB_KEYS=file plays keys from a script, one command per line: a key name (PLAY UP
 * DOWN LEFT RIGHT BACK OPTION VOLUP VOLDN BT), "wait MS", "shot NAME" (the screen to $HB_SHOTS/NAME.rgb565),
 * "log TEXT", "quit" (leave the player). Keys come 0.5 s apart unless a wait says otherwise. */
static FILE *ks;
static double ks_next;
static int ks_init;
static void ks_shot(const char *name)
{
	const char *d = getenv("HB_SHOTS");
	char p[PATHLEN + 64]; snprintf(p, sizeof p, "%s/%s.rgb565", d ? d : "/tmp", name);
	FILE *f = fopen(p, "wb");
	if (f) { fwrite(gfx_fb, 2, W * H, f); fclose(f); }
	lg("keys: shot %s", name);
}
static int script_key(int *code)
{
	if (!ks_init) { ks_init = 1; const char *f = getenv("HB_KEYS"); if (f && (ks = fopen(f, "r"))) { ks_next = now() + 2; lg("keys: script %s", f); } }
	static const char *names[] = { "PLAY", "RIGHT", "LEFT", "UP", "DOWN", "VOLDN", "VOLUP", "BACK", "OPTION", "BT" };
	static const int codes[] = { K_PLAY, K_RIGHT, K_LEFT, K_UP, K_DOWN, K_VOLDN, K_VOLUP, K_BACK, K_OPTION, K_BT };
	while (ks && now() >= ks_next) {
		char l[256];
		if (!fgets(l, sizeof l, ks)) { fclose(ks); ks = 0; lg("keys: script ended"); break; }
		l[strcspn(l, "\r\n")] = 0;
		if (!l[0] || l[0] == '#') continue;
		if (!strncmp(l, "wait ", 5)) { ks_next = now() + atoi(l + 5) / 1000.0; continue; }
		if (!strncmp(l, "shot ", 5)) { ks_shot(l + 5); continue; }
		if (!strncmp(l, "log ", 4)) { lg("keys: %s", l + 4); continue; }
		if (!strcmp(l, "quit")) { lg("keys: quit"); pos_save(); wd_disarm(); int v = 0; ioctl(gfx_fd, 0x40044E35, &v); exit(0); }
		for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++)
			if (!strcmp(l, names[i])) { ks_next = now() + 0.5; *code = codes[i]; lg("keys: %s", l); return 1; }
		lg("keys: unknown \"%s\"", l);
	}
	return 0;
}
#endif

/* a pressed key code, -1 (nothing) or K_REDRAW (screen woke up / hold changed). Held direction keys repeat. */
int getkey(int timeout_ms)
{
	struct ev e;
	hb_alive();
#ifdef HB_DEV
	{
		int code;
		if (script_key(&code)) { wake(); return code; }
		if (ks) { int until = (int)((ks_next - now()) * 1000); if (until < 0) until = 0; if (timeout_ms > until) timeout_ms = until; }
	}
#endif
	if (back_since && now() - back_since >= 3) { back_since = 0; lg("HOME held 3 s: back to the Sony UI"); back_to_sony(); }
	if (force_redraw) { force_redraw = 0; return K_REDRAW; }
	if (held_code >= 0 && timeout_ms > 90) timeout_ms = 90;
	if (back_since && timeout_ms > 200) timeout_ms = 200;
	if (!raw_event(&e, timeout_ms)) {
		double every = held_code == K_VOLUP || held_code == K_VOLDN ? 0.15 : 0.09;	/* volume steps a bit slower */
		if (held_code >= 0 && now() - held_since > 0.4 && now() - last_repeat > every) { last_repeat = now(); wake(); return held_code; }
		return -1;
	}
	if (e.type != 1) return -1;
	if (!KEY_PRESSED(e.value)) { if ((e.code & 0x7f) == held_code) held_code = -1; if ((e.code & 0x7f) == K_BACK) back_since = 0; return -1; }
	if ((e.code & 0x7f) == K_HOLD_B) {	/* HOLD switch moved: 15 = off, 143 = on */
		read_hold(); lg("hold %d", ui_st.hold);
		if (backlight_on) { show_hold(); return K_REDRAW; }
		return -1;
	}
	if (!KEY_PRESSED(e.value)) return -1;
	if (e.code & 0x80) {	/* key pressed while HOLD is on: show the badge, don't wake up */
		ui_st.hold = 1;
		show_hold();
		return -1;
	}
	ui_st.hold = 0;
	int was_dark = !backlight_on;
	wake();
	ui_boost_until = now() + 3;	/* snappy menus: full clock while the user is pressing keys */
	key_at = now();
	if (cpu_mhz != 248 && !in_game) set_cpu(248);
	if (e.code == K_UP || e.code == K_DOWN || e.code == K_LEFT || e.code == K_RIGHT || e.code == K_VOLUP || e.code == K_VOLDN) { held_code = e.code; held_since = last_repeat = now(); }
	back_since = e.code == K_BACK ? now() : 0;	/* another key: not a HOME hold */
	return was_dark ? K_REDRAW : e.code;
}

/* keys for the current loop: waits up to ms, but never while audio is playing (the engine paces the loop) */
static int poll_key(int ms)
{
	if (eng_playing()) { eng_step(); int p = audio_period(); if (ms > p) ms = p; }	/* the engine paces the loop */
	int a = bt_avrcp_key();	/* headset buttons act on the engine only, never as UI navigation */
	if (a == K_PLAY) eng_toggle(); else if (a == K_RIGHT) eng_next(); else if (a == K_LEFT) eng_prev();
	return getkey(ms);
}

static void suspend_now(void);
static void power_off_full(const char *why);

/* CPU clock via /proc/pm ("M248" / "M124" / "M83"): the Sony UI switches the same way */
void set_cpu(int mhz)
{
	if (mhz == cpu_mhz) return;
	char m[8]; snprintf(m, sizeof m, "M%d", mhz);
	int fd = open("/proc/pm", O_WRONLY);
	if (fd >= 0) { if (write(fd, m, strlen(m)) < 0) lg("cpu %s failed", m); else cpu_mhz = mhz; close(fd); }	/* not logged: 2 lines per key */
}

/* governor. The unit boots at 248 MHz; memory speed scales with the clock, so a true 124 is half of 248 and 83 a
 * third (measured). Choice by the estimated load of the playback chain (eng_load_pct, % at 124):
 *   - 248 for 3 s after a key (menus and animations as fast as right after boot), and for heavy chains (> 40 %:
 *     speed change, several effects), hi-res resampling, podcast downloads
 *   - 83 with the screen off when the chain is light (< 20 %: direct / DSP decoding, few filters) and the radio is
 *     off (Bluetooth's 2.6 Mbaud UART and the SBC encoder want at least 124)
 *   - 124 otherwise */
static void cpu_governor(void)
{
	static time_t last;
	if (in_game) return;	/* games set 248 MHz themselves */
	if (now() < ui_boost_until) { if (cpu_mhz != 248) set_cpu(248); return; }
	if (time(0) == last && cpu_mhz) return;
	last = time(0);
	int load = eng_playing() ? eng_load_pct() : 0;
	int want = 124;
	if (!backlight_on && !bt_is_on() && !dl_active() && !eng_resampling() && load < 20) want = 83;
	if (load > 40) want = 248;	/* measured: a 49 % chain ran at 72 % busy at 124 - too tight with the UI on top */
	if (eng_playing() && eng_resampling()) want = 248;	/* 24/96 FLAC: the resampler (24/44.1 plays at the low clock) */
	if (dl_active() || pod_net_busy()) want = 248;	/* podcast transfers are CPU-bound (Bluetooth + TCP + TLS): ~70 -> ~130 KB/s */
	/* down to 83 only after 10 s of wanting it: a track start estimates the load before the DSP has answered, and
	 * the clock used to flap 124 -> 83 -> 124 within a second */
	static time_t low_since;
	if (want == 83 && cpu_mhz != 83) { if (!low_since) low_since = time(0); if (time(0) - low_since < 10) want = 124; }
	else if (want != 83) low_since = 0;
	set_cpu(want);
}

/* what the bottom status bar shows: the song title, and on the home screen the artist (music) */
static void np_update(void)
{
	static char t[96], a[96];
	t[0] = a[0] = 0;
	if (eng_active()) {
		struct eng_info e; eng_info(&e);
		snprintf(t, sizeof t, "%s", e.title ? e.title : "");
		if (e.mode == ENG_MUSIC && e.sub) {	/* music: sub = "artist - album" */
			const char *d = strstr(e.sub, " - ");
			int n = d ? (int)(d - e.sub) : (int)strlen(e.sub);
			snprintf(a, sizeof a, "%.*s", n, e.sub);
		}
	}
	ui_st.np_title = t; ui_st.np_artist = a;
}

static void settings_flush(void);
void idle_tick(int playing)
{
	{	/* the housekeeping below needs no more than 5 runs a second; Bluetooth playback calls this 50 times */
		static double last; double t = now();
		if (t - last < 0.2 && t >= last) return;
		last = t;
	}
	settings_flush();
	{	/* the boot script sets <statedir>/starting before the player; a hang (watchdog reset) leaves it there and the
		 * next boot goes to the Sony UI instead of looping. A minute of normal running clears it. */
		static double t0; static int done;
		if (!t0) t0 = now();
		if (!done && now() - t0 > 60) { char f[PATHLEN + 16]; snprintf(f, sizeof f, "%s/starting", statedir); unlink(f); done = 1; }
	}
	{	/* the crash-recovery line, once a second */
		static time_t last;
		if (time(0) != last) {
			last = time(0);
			if (eng_active()) {
				struct eng_info e; eng_info(&e);
				crash_len = e.path ? snprintf(crash_line, sizeof crash_line, "%ld %d %d %s\n", dec_pos_bytes(), e.sec, eng_playing(), e.path) : 0;
				if (crash_len >= (int)sizeof crash_line) crash_len = 0;
			} else crash_len = 0;
		}
	}
	battery_update();
	usb_check();
	{ int w = warm_poll(); if (w) { music_preload(w == 2); warm_covers_start(); } }	/* the first visit of Música then opens at once, its covers ready */
	bt_poll();
	eng_tick();
	cpu_governor();
	ui_st.bt = bt_connected();
	ui_st.playing = eng_active() ? (eng_playing() ? 1 : 2) : 0;
	np_update();
	if (eng_take_sleep()) { suspend_now(); return; }
	int idle = (int)(time(0) - last_key);
	playing |= eng_playing();
	if (backlight_on && idle > (playing ? ui_dim_secs : (ui_dim_secs > DIM_IDLE ? ui_dim_secs : DIM_IDLE))) { gfx_backlight(0); gfx_lcd(0); backlight_on = 0; }
	if (batt_low >= 3) power_off_full("Batería agotada");
	if (!playing && !dl_active() && idle > (backlight_on ? AUTO_SUSPEND : AUTO_SUSPEND_DARK)) suspend_now();
	static time_t last_alarm;
	if (alarm_due() && time(0) - last_alarm > 300) { last_alarm = time(0); alarm_ring(); }
}

void message(const char *title, const char *l1, const char *l2)
{
	wake();
	scr_message(title, l1, l2);
	flip();
}

/* keeps a message on screen for up to secs; any key dismisses it early. Audio keeps playing (getkey feeds it). */
void msg_wait(double secs)
{
	double end = now() + secs;
	while (now() < end) {
		int k = getkey((int)((end - now()) * 1000) + 1);
		if (k >= 0) break;
	}
}

static int change_volume(int k)
{
	if (k == K_VOLUP && volume < VOLMAX) volume++;
	else if (k == K_VOLDN && volume > 0) volume--;
	else if (k != K_VOLUP && k != K_VOLDN) return 0;
	pos_dirty = 1;
	show_volume();
	return 1;
}

int ui_volume_key(int k) { return change_volume(k); }	/* for screens with their own key loop (settings.c) */

/* ---------- lists ---------- */

int (*list_idle_hook)(void);
int list_vis_top;	/* first visible row of the running list: thumbnail hooks decode those first */

/* the selection bar slides from row `from` to row `to` (both visible) in 4 quick frames */
static void list_slide(struct list_view *v, int from, int to)
{
	int y0 = LIST_Y0 + (from - v->top) * lv_rowh(v), y1 = LIST_Y0 + (to - v->top) * lv_rowh(v);
	static const int steps[] = { 55, 88, 100 };	/* 3 frames (was 4, ~70 ms): only the rows it crosses reach the screen */
	for (int k = 0; k < 3; k++) {
		v->bar_y = y0 + (y1 - y0) * steps[k] / 100;
		scr_list(v); gfx_flip();
		ui_tick();
		if (!eng_playing()) usleep(7000);
	}
	v->bar_y = -1;
}

int list_run(struct list_view *v)
{
	int redraw = 1, last_sel = -1, marquee_runs = 0;
	double t_sel = now();
	v->scroll = 0; v->bar_y = -1;
	if (v->sel >= v->n) v->sel = v->n ? v->n - 1 : 0;
	for (;;) {
		int old_top = v->top;
		if (v->grid) {	/* mosaic: the top is always the start of a row of 3 */
			if (v->sel < v->top) v->top = v->sel - v->sel % 3;
			if (v->sel >= v->top + GRID_N) v->top = (v->sel / 3 - 2) * 3;
		} else {
			if (v->sel < v->top) v->top = v->sel;
			if (v->sel >= v->top + lv_rows(v)) v->top = v->sel - lv_rows(v) + 1;
		}
		if (v->sel != last_sel) {
			if (!v->grid && last_sel >= 0 && v->top == old_top && backlight_on && held_code < 0 && abs(v->sel - last_sel) == 1) list_slide(v, last_sel, v->sel);
			last_sel = v->sel; v->scroll = 0; t_sel = now(); redraw = 1; marquee_runs = 0;
		}
		int over = backlight_on && marquee_runs < 2 ? scr_list_overflow(v) : 0;
		if (over) {	/* marquee: wait 1.2 s, scroll at 60 px/s, pause at the end, restart; twice, then it rests */
			double t = now() - t_sel - 1.2;
			int s = t > 0 ? (int)(t * 60) : 0;
			if (s > over + 30) { t_sel = now(); s = 0; if (++marquee_runs >= 2) over = 0; }
			if (s != v->scroll) { v->scroll = s; redraw = 1; }
		}
		if (popup_expired()) redraw = 1;
		{ static double lastdl; if (dl_active() && now() - lastdl > 1) { lastdl = now(); redraw = 1; } }
		if (redraw && backlight_on && !popup_active()) { scr_list(v); flip(); redraw = 0; }
		idle_tick(0);
		/* background work (thumbnails): one item per pass, then the keys are checked without waiting, so a
		 * long list of covers never blocks navigation; also while music plays (the decoder feeds the audio) */
		list_vis_top = v->top;
		int busy = 0;
		if (list_idle_hook) {	/* items from the cache take a few ms: several per pass (30 ms at most), one redraw */
			double t0 = now();
			while (list_idle_hook()) { redraw = 1; if (now() - t0 > 0.03) { busy = 1; break; } ui_tick(); }
		}
		int k = poll_key(busy ? 0 : over ? 50 : 500);	/* marquee at 20 fps */
		if (k == K_REDRAW) { redraw = 1; continue; }
		if (k < 0) continue;
		redraw = 1;
		if (v->grid && v->n && (k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT)) {	/* mosaic: up/down a row, left/right one */
			if (k == K_UP) v->sel = v->sel >= 3 ? v->sel - 3 : v->sel;
			else if (k == K_DOWN) v->sel = v->sel + 3 < v->n ? v->sel + 3 : v->sel / 3 < (v->n - 1) / 3 ? v->n - 1 : v->sel;
			else if (k == K_LEFT) v->sel = v->sel > 0 ? v->sel - 1 : v->n - 1;
			else v->sel = v->sel < v->n - 1 ? v->sel + 1 : 0;
		}
		else if (k == K_UP) v->sel = v->sel > 0 ? v->sel - 1 : v->n - 1;
		else if (k == K_DOWN) v->sel = v->sel < v->n - 1 ? v->sel + 1 : 0;
		else if (k == K_RIGHT) v->sel = v->sel + lv_rows(v) < v->n ? v->sel + lv_rows(v) : v->n - 1;
		else if (k == K_LEFT) v->sel = v->sel - lv_rows(v) > 0 ? v->sel - lv_rows(v) : 0;
		else if (k == K_PLAY && v->n) { pending_slide = 1; return v->sel; }
		else if (k == K_BACK) { pending_slide = -1; return -1; }
		else if (k == K_OPTION && v->option && v->n) { pending_slide = 1; int r = v->option(v->sel); pending_slide = -1; redraw = 1; if (r == 1) return -2; }
		else if (k == K_OPTION && !quick_open) { pending_slide = 1; if (sound_quick_menu(eng_active())) nowplaying_run(); pending_slide = -1; redraw = 1; }
		else if (k == K_BT) { pending_slide = 1; bt_menu(); pending_slide = -1; redraw = 1; }
		else change_volume(k);
		if (v->sel < 0) v->sel = 0;
	}
}

/* ---------- saved state ---------- */

int volume = 22;	/* -12 dB */
static int gains[VOLMAX + 1];

int gain_q12(int v)
{
	if (!gains[VOLMAX]) {
		for (int i = 0; i <= VOLMAX; i++) {
			int g = 4096;	/* -1.5 dB per step: x0.8414 (3446/4096) */
			for (int s = i; s < VOLMAX; s++) g = g * 3446 / 4096;
			gains[i] = i ? g : 0;
		}
	}
	return gains[v < 0 ? 0 : v > VOLMAX ? VOLMAX : v];
}

struct pos { char path[PATHLEN]; long bytes; int sec; };
static struct pos poss[400];
static int npos;
static char lastfile[PATHLEN];
void set_lastfile(const char *path) { if (strcmp(lastfile, path)) { snprintf(lastfile, sizeof lastfile, "%s", path); pos_dirty = 1; } }

static void pos_load(void)
{
	char fn[PATHLEN + 16]; snprintf(fn, sizeof fn, "%s/pos.txt", statedir);
	FILE *f = fopen(fn, "r");
	if (!f) return;
	char line[PATHLEN + 64];
	while (fgets(line, sizeof line, f) && npos < 400) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "last ", 5)) { snprintf(lastfile, sizeof lastfile, "%s", line + 5); continue; }
		if (!strncmp(line, "vol30 ", 6)) { volume = atoi(line + 6); if (volume < 0 || volume > VOLMAX) volume = 22; continue; }
		if (!strncmp(line, "vol ", 4)) {	/* the old 0..20 scale (2 dB steps): the same loudness on the new one */
			int o = atoi(line + 4);
			volume = o <= 0 ? 0 : o > 20 ? VOLMAX : VOLMAX - ((20 - o) * 4 + 1) / 3;
			if (o > 0 && volume < 1) volume = 1;
			continue;
		}
		if (!strncmp(line, "rows ", 5)) { ui_big_rows = atoi(line + 5) == 1; continue; }
		if (!strncmp(line, "grid ", 5)) { ui_album_grid = atoi(line + 5) == 1; continue; }
		if (!strncmp(line, "beep ", 5)) { ui_beep = atoi(line + 5) == 1; continue; }
		if (!strncmp(line, "focus ", 6)) { focus_load(line + 6); continue; }
		if (!strncmp(line, "lang ", 5)) { ui_lang_set(atoi(line + 5) == 1); continue; }
		if (!strncmp(line, "podc ", 5)) { if (strlen(line + 5) == 2) snprintf(pod_country, sizeof pod_country, "%s", line + 5); continue; }
		if (!strncmp(line, "ui ", 3)) { sscanf(line + 3, "%d %d", &ui_brightness, &ui_dim_secs); if (ui_brightness < 20 || ui_brightness > 100) ui_brightness = 80; if (ui_dim_secs < 15 || ui_dim_secs > 120) ui_dim_secs = 30; continue; }
		if (!strncmp(line, "alarm ", 6)) { alarm_load(line + 6); continue; }
		if (!strncmp(line, "speed ", 6)) { eng_speed = atoi(line + 6); if (eng_speed < 50 || eng_speed > 300) eng_speed = 100; continue; }
		if (!strncmp(line, "rg ", 3)) { replaygain_mode = atoi(line + 3); if (replaygain_mode < 0 || replaygain_mode > 2) replaygain_mode = 0; continue; }
		if (!strncmp(line, "mode ", 5)) { sscanf(line + 5, "%d %d", &eng_shuffle, &eng_repeat); eng_shuffle = !!eng_shuffle; if (eng_repeat < 0 || eng_repeat > 2) eng_repeat = 0; continue; }
		char *p1 = strchr(line, ' '), *p2 = p1 ? strchr(p1 + 1, ' ') : 0;
		if (!p2) continue;
		*p1 = *p2 = 0;
		poss[npos].bytes = atol(line); poss[npos].sec = atoi(p1 + 1);
		snprintf(poss[npos].path, PATHLEN, "%s", p2 + 1);
		npos++;
	}
	fclose(f);
}

void pos_save(void)
{
	if (!pos_dirty) return;
	char fn[PATHLEN + 16], tmp[PATHLEN + 16];
	snprintf(fn, sizeof fn, "%s/pos.txt", statedir);
	snprintf(tmp, sizeof tmp, "%s/pos.tmp", statedir);
	FILE *f = fopen(tmp, "w");
	if (!f) { lg("cannot write %s", tmp); return; }
	fprintf(f, "vol30 %d\nmode %d %d\nui %d %d\nalarm %d %d %d %d\nspeed %d\nrg %d\n", volume, eng_shuffle, eng_repeat, ui_brightness, ui_dim_secs, alarm_cfg.on, alarm_cfg.hour, alarm_cfg.min, alarm_cfg.src, eng_speed, replaygain_mode);
	{ char fo[32]; focus_save(fo, sizeof fo); fprintf(f, "rows %d\ngrid %d\nbeep %d\npodc %s\nlang %d\nfocus %s\n", ui_big_rows, ui_album_grid, ui_beep, pod_country, ui_lang, fo); }
	if (lastfile[0]) fprintf(f, "last %s\n", lastfile);
	for (int i = 0; i < npos; i++) fprintf(f, "%ld %d %s\n", poss[i].bytes, poss[i].sec, poss[i].path);
	int bad = ferror(f);
	if (fflush(f) || fsync(fileno(f))) bad = 1;
	if (fclose(f)) bad = 1;
	if (bad) { lg("pos save failed (disk full?)"); unlink(tmp); return; }
	rename(tmp, fn);
	pos_dirty = 0;
}

/* most recently used entries live at the end; finished files are evicted first */
static struct pos *pos_find(const char *path, int create)
{
	for (int i = 0; i < npos; i++)
		if (!strcmp(poss[i].path, path)) {
			struct pos t = poss[i];
			memmove(poss + i, poss + i + 1, sizeof poss[0] * (npos - i - 1));
			poss[npos - 1] = t;
			return &poss[npos - 1];
		}
	if (!create) return 0;
	if (npos == 400) {
		int victim = 0;
		for (int i = 0; i < npos; i++) if (poss[i].sec == 0) { victim = i; break; }
		memmove(poss + victim, poss + victim + 1, sizeof poss[0] * (npos - victim - 1));
		npos--;
	}
	struct pos *p = &poss[npos++];
	snprintf(p->path, PATHLEN, "%s", path); p->bytes = 0; p->sec = 0;
	return p;
}

/* saved seconds of a file without touching the recently-used order: -1 never played, 0 finished (or skipped) */
static int pos_peek(const char *path)
{
	for (int i = npos - 1; i >= 0; i--) if (!strcmp(poss[i].path, path)) return poss[i].sec;
	return -1;
}

int pos_get(const char *path, long *bytes, int *sec)
{
	struct pos *p = pos_find(path, 0);
	if (!p) return 0;
	*bytes = p->bytes; *sec = p->sec;
	return 1;
}

/* settings changes and playback positions (pause, skip, track picked) are written 5 s after the first one (each
 * used to rewrite and fsync pos.txt on the spot: a visible hitch per key); suspend, power off, USB and leaving the
 * player save right away */
static double settings_since;
void settings_save(void) { pos_dirty = 1; if (!settings_since) settings_since = now(); }
static void settings_flush(void) { if (settings_since && now() - settings_since > 5) { settings_since = 0; pos_save(); } }

void pos_set(const char *path, long bytes, int sec)
{
	struct pos *p = pos_find(path, 1);
	if (p->bytes != bytes || p->sec != sec) { p->bytes = bytes; p->sec = sec; pos_dirty = 1; }
}

/* ---------- directory listing ---------- */

struct item { char name[256]; char path[PATHLEN]; };
static struct item items[MAXITEMS];
static int sort_desc;

static int cmp_item(const void *a, const void *b)
{
	int r = strcmp(((const struct item *)a)->path, ((const struct item *)b)->path);
	return sort_desc ? -r : r;
}

static int endswith_ci(const char *s, const char *ext)
{
	size_t n = strlen(s), m = strlen(ext);
	return n > m && !strcasecmp(s + n - m, ext);
}

/* dirs=1: subfolders; dirs=0: files with extension ext. Podcast files "YYYY-MM-DD - title" show just the title. */
static int list_dir(const char *root, int dirs, const char *ext, int desc)
{
	DIR *d = opendir(root);
	if (!d) return 0;
	struct dirent *e;
	int n = 0, over = 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		if (n >= MAXITEMS) { over++; continue; }
		char p[PATHLEN];
		if (!fs_path(root, e->d_name, p, sizeof p)) continue;	/* a '?' in the name and no short name: it can't be opened */
		/* the type from readdir when the filesystem gives it (no stat per entry: on FAT each one searches the
		 * directory again); stat only when it doesn't */
		int isdir, isreg;
		if (e->d_type != DT_UNKNOWN) { isdir = e->d_type == DT_DIR; isreg = e->d_type == DT_REG; }
		else { struct stat st; if (stat(p, &st)) continue; isdir = S_ISDIR(st.st_mode); isreg = S_ISREG(st.st_mode); }
		if (dirs ? !isdir : !(isreg && (!strcmp(ext, ".mp3") ? dec_is_audio(e->d_name) : endswith_ci(e->d_name, ext)))) continue;
		struct item *it = &items[n];
		const char *name = e->d_name;
		int y, m, dd;
		if (!dirs && sscanf(name, "%4d-%2d-%2d - ", &y, &m, &dd) == 3 && strlen(name) > 13) name += 13;
		fs_to_utf8(name, it->name, sizeof it->name);	/* Latin-1 on disk -> UTF-8 on screen */
		if (!dirs) { char *dot = strrchr(it->name, '.'); if (dot && dot > it->name) *dot = 0; }
		snprintf(it->path, PATHLEN, "%s", p);
		n++;
	}
	closedir(d);
	sort_desc = desc;
	qsort(items, n, sizeof items[0], cmp_item);
	if (over) { lg("list_dir: %s has more than %d entries, %d not shown", root, MAXITEMS, over); eng_note("Carpeta muy grande: se muestran 1000"); }
	return n;
}

/* ---------- now playing ---------- */

static char opt_items[5][40];
static const char *opt_label(int i) { return opt_items[i]; }
static const char *repeat_names[] = { "no", "todo", "una" };

static const char *chap_label(int i) { return chap_title(i); }
static const char *chap_value(int i)
{
	static char s[16]; int t = (int)chap_start(i);
	if (t >= 3600) snprintf(s, sizeof s, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60); else snprintf(s, sizeof s, "%d:%02d", t / 60, t % 60);
	return s;
}

static void chapters_menu(void)
{
	struct eng_info e; eng_info(&e);
	struct list_view v = { "Capítulos", chap_count(), chap_index(e.sec) < 0 ? 0 : chap_index(e.sec), 0, chap_label, chap_value, 0, LI_NONE, 0, 0 };
	int s = list_run(&v);
	if (s >= 0) eng_seek_to(chap_start(s));
}

static void play_options(int music)
{
	struct list_view v = { "Opciones", 0, 0, 0, opt_label, 0, 0, LI_NONE, 0, 0 };
	for (;;) {
		int snd = music ? 3 : 2 + (chap_count() > 0);	/* the last row: the quick sound menu */
		v.n = snd + 1;
		snprintf(opt_items[snd], 40, "Sonido");
		int m = eng_sleep_left();
		if (m) snprintf(opt_items[0], 40, "Temporizador: %d min", m); else snprintf(opt_items[0], 40, "Temporizador: no");
		if (music) {
			snprintf(opt_items[1], 40, "Aleatorio: %s", eng_shuffle ? "sí" : "no");
			snprintf(opt_items[2], 40, "Repetir: %s", repeat_names[eng_repeat]);
		} else snprintf(opt_items[1], 40, "Velocidad: %d.%02dx", eng_speed / 100, eng_speed % 100);
		if (!music && chap_count()) snprintf(opt_items[2], 40, "Capítulos (%d)", chap_count());
		int s = list_run(&v);
		if (s < 0) return;
		pending_slide = 0;
		if (s == snd) { pending_slide = 1; sound_quick_menu(0); pending_slide = -1; continue; }
		if (s == 0) { int cur = eng_sleep_min; eng_set_sleep(cur >= 60 ? 0 : cur + 15); }
		else if (s == 1 && music) { eng_shuffle = !eng_shuffle; pos_dirty = 1; }
		else if (s == 1) { static const int sp[] = { 100, 125, 150, 200, 75 }; int i = 0; while (i < 5 && sp[i] != eng_speed) i++; eng_speed = sp[(i + 1) % 5]; pos_dirty = 1; }
		else if (s == 2 && !music) { pending_slide = 1; chapters_menu(); return; }
		else if (s == 2) { eng_repeat = (eng_repeat + 1) % 3; pos_dirty = 1; }
		pos_save();
	}
}

void nowplaying_run(void)
{
	static uint16_t art[PLAYER_ART * PLAYER_ART];
	struct player_view pv;
	struct eng_info e;
	char lastpath[384] = "";
	int has_art = 0, art_pending = 0, full = 1, lastsec = -1, show_lrc = 0, last_lidx = -2, last_cidx = -2;
	/* LEFT / RIGHT: a short press skips (music) or jumps 30 s (books); held, it scrubs: the time and the bar run
	 * (10 s of audio per second held, 30 after 2 s, 90 after 5 s) and one seek happens on release, so a direct
	 * DSP session isn't restarted over and over */
	int lr_key = -1, scrub = 0;
	double lr_t = 0, scrub_pos = 0, scrub_t = 0;
	char chapline[120];
	double t_in = now();	/* the first frame's preparation, for the timing line below */
	wake();
	for (;;) {
		if (!eng_active()) { pending_slide = -1; return; }
		eng_info(&e);
		if (strcmp(lastpath, e.path)) {	/* new track: its cover from the cache right away, else after the first frame */
			snprintf(lastpath, sizeof lastpath, "%s", e.path);
			double t = now();
			has_art = cover_cached(e.path, art, PLAYER_ART);
			art_pending = !has_art;
			if (now() - t > 0.05) lg("art: cache read %d ms", (int)((now() - t) * 1000));
			show_lrc = lrc_count() > 0;
			full = 1;
		}
		int lidx = show_lrc ? lrc_index(e.sec) : -2;
		if (lidx != last_lidx) { last_lidx = lidx; full = 1; }
		int cidx = chap_count() ? chap_index(e.sec) : -2;
		if (cidx != last_cidx) { last_cidx = cidx; full = 1; }
		if (eng_changed()) full = 1;
		if (popup_expired()) full = 1;
		if (backlight_on && !popup_active() && (full || e.sec != lastsec || scrub)) {
			memset(&pv, 0, sizeof pv);
			pv.book = e.context; pv.track = e.title; pv.sub = e.sub; pv.genre = e.genre; pv.year = e.year;
			if (e.mode == ENG_BOOK && e.path) {	/* the kind in the title bar (like "Canciones"), the show on the first line */
				static char date[24];
				static const char *mes[12] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };
				const char *base = strrchr(e.path, '/');
				base = base ? base + 1 : e.path;
				int yy, mm, dd;
				date[0] = 0;
				if (sscanf(base, "%4d-%2d-%2d", &yy, &mm, &dd) == 3 && mm >= 1 && mm <= 12) snprintf(date, sizeof date, "%d %s %d", dd, mes[mm - 1], yy);
				pv.book = !strncmp(e.path, "/contents/Podcasts/", 19) ? "Podcasts" : "Audiolibros";
				pv.meta = 1; pv.date = date;
			}
			if (cidx >= 0) { snprintf(chapline, sizeof chapline, "%d/%d · %s", cidx + 1, chap_count(), chap_title(cidx)); pv.chap = chapline; }
			pv.idx = e.idx; pv.ntr = e.n; pv.sec = scrub ? (int)scrub_pos : e.sec; pv.total = e.total; pv.paused = e.paused;
			pv.vol = volume; pv.volmax = VOLMAX; pv.sleep_left = eng_sleep_left();
			pv.out = bt_connected() ? "BT" : "";
			if (has_art) { pv.art.w = pv.art.h = PLAYER_ART; pv.art.px = art; }
			pv.speed = e.mode == ENG_BOOK ? eng_speed : 100;
			pv.lrc_prev = show_lrc && lidx > 0 ? lrc_text(lidx - 1) : 0;
			pv.lrc_cur = show_lrc ? lrc_text(lidx) : 0;
			pv.lrc_next = show_lrc ? lrc_text(lidx + 1) : 0;
			pv.show_lrc = show_lrc;
			ui_st.playing = e.paused ? 2 : 1;
			scr_player(&pv, full);
			if (t_in) { double ms = (now() - t_in) * 1000; if (ms > 60) lg("ui: player screen drawn in %d ms (cover %s)", (int)ms, has_art ? "cached" : "later"); t_in = 0; }
			if (full) flip(); else gfx_flip();
			full = 0; lastsec = e.sec;
			/* the title is on screen: now decode the cover (never with the screen off: it waits for the wake-up) */
			if (art_pending) {
				double t = now();
				cover_cancel = key_waiting;	/* skipping tracks quickly: never wait for a cover nobody will see */
				art_pending = 0; has_art = cover_load(e.path, art, PLAYER_ART);
				cover_cancel = 0;
				if (cover_aborted()) art_pending = 1;	/* again once the keys settle */
				else lg("art: decoded in %d ms (%s)", (int)((now() - t) * 1000), has_art ? "ok" : "none");
				if (has_art) { full = 1; continue; }
			}
		}
		if (!backlight_on) full = 1;
		idle_tick(0);
		int k = poll_key(lr_key >= 0 ? 60 : 500);
		if (lr_key >= 0) {
			int music = e.mode == ENG_MUSIC, dir = lr_key == K_RIGHT ? 1 : -1;
			if (held_code != lr_key) {	/* released */
				if (scrub) eng_seek_to(scrub_pos);
				else if (music) { beep(dir > 0 ? BEEP_TWO : BEEP_THREE); if (dir > 0) eng_next(); else eng_prev(); }
				else eng_seek(30 * dir);
				lr_key = -1; scrub = 0; full = 1;
				if (k < 0 || k == K_LEFT || k == K_RIGHT) continue;
			} else if (now() - lr_t >= 0.45) {	/* held: scrub */
				double t = now(), held = t - lr_t, rate = held < 2.45 ? 10 : held < 5.45 ? 30 : 90;
				if (!scrub) { scrub = 1; scrub_pos = e.sec; scrub_t = t; }
				scrub_pos += dir * rate * (t - scrub_t); scrub_t = t;
				if (scrub_pos < 0) scrub_pos = 0;
				if (e.total > 0 && scrub_pos > e.total - 1) scrub_pos = e.total - 1;
				if (k == K_LEFT || k == K_RIGHT) continue;	/* the auto-repeat of the held key */
			} else if (k == K_LEFT || k == K_RIGHT) continue;
		}
		if (k == K_REDRAW) { full = 1; continue; }
		if (k < 0) continue;
		int music = e.mode == ENG_MUSIC;
		if (k == K_PLAY) { beep(BEEP_ONE); eng_toggle(); }
		else if (k == K_BACK) { pending_slide = -1; return; }
		else if (k == K_LEFT || k == K_RIGHT) { lr_key = k; lr_t = now(); scrub = 0; continue; }	/* decided on release / hold */
		else if (k == K_UP) {
			if (lrc_count()) { show_lrc = !show_lrc; last_lidx = -2; }
			else if (music) eng_seek(-15);
			else if (chap_count()) { int c = chap_index(e.sec); if (c > 0 && e.sec - chap_start(c) < 3) c--; eng_seek_to(c < 0 ? 0 : chap_start(c)); }	/* start of this chapter, or the previous one */
			else eng_prev();
		}
		else if (k == K_DOWN) {
			if (music) eng_seek(15);
			else if (chap_count() && chap_index(e.sec) + 1 < chap_count()) eng_seek_to(chap_start(chap_index(e.sec) + 1));
			else eng_next();
		}
		else if (k == K_OPTION) { pending_slide = 1; play_options(music); pending_slide = -1; full = 1; }
		else change_volume(k);
		full = 1;
	}
}

/* ---------- leaving: suspend, power off, back to Sony ---------- */

static void prepare_shutdown(void)
{
	{ char f[PATHLEN + 16]; snprintf(f, sizeof f, "%s/starting", statedir); unlink(f); }	/* a clean power-off, not a hang */
	gfx_lcd(1);
	dl_cancel_quiet();
	dsp_stop_helper();
	eng_stop();
	pos_save();
	bt_shutdown();
	aout_stop();
	lg("shutdown");
	wd_disarm();
	if (logf) { fclose(logf); logf = 0; }
	sync();
	system("mount -o remount,ro /contents > /dev/null 2>&1");
	system("umount /var > /dev/null 2>&1");
	sync();
}

static void suspend_now(void)
{
	dl_cancel_quiet();	/* never suspend with the PAN link up */
	eng_pause(1);
	dsp_stop_helper();	/* the DSP is restarted (firmware reload) on the next track after resume */
	pos_save();
	aout_stop();
	bt_shutdown();
	lg("suspend");
	sync();
	gfx_fill(0, 0, W, H, 0); gfx_flip();
	gfx_backlight(0); backlight_on = 0;
	wd_disarm();
	alarm_program_rtc();
	int fd = open("/proc/pm", O_WRONLY);
	if (fd >= 0) { if (write(fd, "S3", 2) < 0) lg("suspend failed"); close(fd); }	/* returns after wake-up */
	lg("resumed");
	wd_arm();
	drain_keys();
	read_hold();
	wake();
	force_redraw = 1;
	gfx_invalidate();	/* someone else (suspend, a game, the remote) drew on the screen */
	if (alarm_due()) alarm_ring(); else bt_auto_start();
}

static void power_off_full(const char *why)
{
	lg("power off: %s", why);
	scr_poweroff(); gfx_flip();
	prepare_shutdown();
	sleep(2);
	reboot(0x4321FEDC);	/* RB_POWER_OFF, what the Sony UI does */
	sleep(30);
}

/* which UI the unit boots into next time: <statedir>/mode = "player" | "sony" (the boot script reads it, so there
 * is no 3 s prompt any more: it boots into whatever was used last) */
static void mode_set(const char *m)
{
	char fn[PATHLEN + 16], tmp[PATHLEN + 24]; snprintf(fn, sizeof fn, "%s/mode", statedir);
	FILE *f = safe_open(fn, tmp, sizeof tmp);
	if (f) { fprintf(f, "%s\n", m); safe_close(f, tmp, fn); }
}

/* the player ends with everything saved and released: exit(rc) and the boot script goes on (0 = to the Sony UI,
 * 3 = USB: it hands the FAT partition to the computer with hbusb, then starts the player again) */
static void leave_player(const char *splash, int rc)
{
	gfx_lcd(1);	/* the Sony UI expects the controller running */
	scr_splash(splash); gfx_flip();
	warm_stop();
	if (rc == 0) mode_set("sony");
	dl_cancel_quiet();
	dsp_stop_helper();
	eng_stop();
	pos_save();
	bt_shutdown();
	aout_stop();
	lg(rc == 0 ? "back to the Sony UI (no reboot)" : "USB: exiting for the hand-over");
	wd_disarm();
	set_cpu(248);	/* the clock the Sony UI gets at a normal boot */
	if (logf) { fclose(logf); logf = 0; }
	sync();
	exit(rc);
}

/* back to the Sony UI without a reboot, several seconds sooner than a reboot */
static void back_to_sony(void) { leave_player("Volviendo al Walkman...", 0); }

/* a computer has enumerated the Walkman as a disk: status 1 from Sony's usbconn driver (the one Sony's apl_conn
 * waits on; a charger never gets past "connecting"). Checked once a second. /tmp/hb_usb_off = the last hand-over
 * failed (hbusb could not unmount /contents): the cable is ignored until it is unplugged. */
static void usb_check(void)
{
	static int fd = -2; static time_t last;
#ifdef HB_DEV
	if (getenv("HB_NOUSB")) return;	/* device tests started with the cable in: the PC waits until they are done */
#endif
	if (time(0) == last) return;
	last = time(0);
	if (fd == -2) { fd = open("/dev/usbconn", O_RDONLY); if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC); }
	int st = -1;
	if (fd < 0 || ioctl(fd, 0x80046701, &st) < 0) return;
	if (st == 0) { unlink("/tmp/hb_usb_off"); return; }
	if (st != 1 || !access("/tmp/hb_usb_off", F_OK)) return;
	lg("USB: a computer connected");
	leave_player("USB...", 3);
}

/* ---------- audiobooks / podcasts ---------- */

static struct item tracks[MAXITEMS];
static int ntracks;
static struct item folders[MAXITEMS];
static struct track queue[MAXITEMS];
static const char *tr_label(int i) { return tracks[i].name; }
static int tr_mark(int i) { return !strcmp(lastfile, tracks[i].path); }
static int fo_extra;	/* rows before the folders: podcasts have "Buscar episodios nuevos" and "Agregar podcast" */
static const char *fo_label(int i)
{
	if (i < fo_extra) return i == 0 ? (dl_active() ? "Descargas" : "Buscar nuevos") : "Agregar podcast";
	return folders[i].name;
}
static const char *fo_value(int i) { return i < fo_extra ? (i == 0 ? dl_row_value() : 0) : 0; }
static int fo_mark(int i) { size_t l = strlen(folders[i].path); return l && lastfile[0] && !strncmp(lastfile, folders[i].path, l) && lastfile[l] == '/'; }

/* OPTION on a podcast episode: delete it (it is remembered, so the automatic sync won't bring it back) */
static char ep_items[3][48];
static const char *ep_label(int i) { return ep_items[i]; }
static int episode_option(int i)
{
	snprintf(ep_items[0], 48, "Borrar episodio");
	snprintf(ep_items[1], 48, "Sonido");
	snprintf(ep_items[2], 48, "Cancelar");
	struct list_view m = { "Episodio", 3, 2, 0, ep_label, 0, 0, LI_NONE, 0, 0, 0, -1, 0 };
	int r0 = list_run(&m);
	if (r0 == 1) { if (sound_quick_menu(eng_active())) nowplaying_run(); return 0; }
	if (r0 != 0) return 0;
	snprintf(ep_items[0], 48, "Sí, borrarlo");
	snprintf(ep_items[1], 48, "No");
	struct list_view c = { tracks[i].name, 2, 1, 0, ep_label, 0, 0, LI_NONE, 0, 0, 0, -1, 0 };
	if (list_run(&c) != 0) return 0;
	struct eng_info e; eng_info(&e);
	if (eng_active() && e.path && !strcmp(e.path, tracks[i].path)) eng_stop();	/* it's playing: stop first */
	if (podcast_forget(tracks[i].path)) { message("Podcasts", "No pude borrar el episodio.", ""); msg_wait(2); return 0; }
	return 1;
}

/* podcast episodes as rows with their cover (the file's own, else the show's), date and length, a dot when
 * never played, a bar while started, dimmed when finished. Covers and lengths are read lazily between keys. */
#define EP_MAXTH 200	/* covers kept for the first 200 episodes of a show (46x46x2 = 4 KB each) */
static uint16_t *ep_th;
static signed char *ep_ts;	/* 0 not read yet, 1 cover ready, -1 no cover */
static short *ep_min;		/* length in minutes, -1 unknown */
static int ep_cap;

/* rough length of an MP3 from its size and the first frame's bitrate (podcasts are CBR); -1 unknown */
static int mp3_minutes(const char *path)
{
	if (!endswith_ci(path, ".mp3")) return -1;
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	unsigned char b[4096];
	long start = 0;
	if (fread(b, 1, 10, f) == 10 && !memcmp(b, "ID3", 3)) start = 10 + ((b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127));
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, start, SEEK_SET);
	size_t n = fread(b, 1, sizeof b, f);
	fclose(f);
	static const short br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
	static const short br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
	for (size_t i = 0; i + 4 <= n; i++)
		if (b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0 && ((b[i + 1] >> 1) & 3) == 1) {	/* layer III frame */
			int v = (b[i + 1] >> 3) & 3, kb = (v == 3 ? br1 : br2)[b[i + 2] >> 4];
			if (kb && v != 1) return (int)((size - start) / 125 / kb / 60);
		}
	return -1;
}

static void ep_free(void) { free(ep_th); free(ep_ts); free(ep_min); ep_th = 0; ep_ts = 0; ep_min = 0; ep_cap = 0; }
static void ep_setup(int n)
{
	ep_free();
	ep_cap = n < EP_MAXTH ? n : EP_MAXTH;
	ep_th = calloc((size_t)(ep_cap ? ep_cap : 1) * EP_THUMB * EP_THUMB, 2);
	ep_ts = calloc(n ? n : 1, 1);
	ep_min = malloc(sizeof(short) * (n ? n : 1));
	for (int i = 0; i < n && ep_min; i++) ep_min[i] = -1;
}
static struct img ep_thumb(int i)
{
	struct img im = { 0, 0, 0 };
	if (ep_ts && i < ep_cap && ep_ts[i] == 1) { im.w = im.h = EP_THUMB; im.px = ep_th + (size_t)i * EP_THUMB * EP_THUMB; }
	return im;
}
static int ep_pending(void)
{
	if (!ep_ts || !ep_min) return 0;
	for (int j = 0; j < ntracks; j++) {
		int i = (list_vis_top + j) % ntracks;	/* the visible rows first */
		if (ep_ts[i]) continue;
		ep_ts[i] = -1;
		ep_min[i] = (short)mp3_minutes(tracks[i].path);
		if (i < ep_cap && ep_th && cover_load(tracks[i].path, ep_th + (size_t)i * EP_THUMB * EP_THUMB, EP_THUMB)) ep_ts[i] = 1;
		return 1;
	}
	return 0;
}
static int ep_state(int i) { int s = pos_peek(tracks[i].path); return s < 0 ? 1 : s == 0 ? 2 : 0; }
static int ep_progress(int i)
{
	int s = pos_peek(tracks[i].path), mn = ep_min ? ep_min[i] : -1;
	if (s <= 0 || mn <= 0) return -1;
	int p = s * 100 / (mn * 60);
	return p > 100 ? 100 : p;
}
/* second line: "25 jun · 1 h 12 min", "quedan 22 min" or "escuchado" */
static const char *ep_value(int i)
{
	static char b[64];
	static const char *mes[12] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };
	char d[16] = "", len[24] = "";
	const char *base = strrchr(tracks[i].path, '/');
	base = base ? base + 1 : tracks[i].path;
	int y, m, dd;
	if (sscanf(base, "%4d-%2d-%2d", &y, &m, &dd) == 3 && m >= 1 && m <= 12) snprintf(d, sizeof d, "%d %s", dd, mes[m - 1]);
	int s = pos_peek(tracks[i].path), mn = ep_min ? ep_min[i] : -1;
	if (mn >= 60) snprintf(len, sizeof len, "%d h %d min", mn / 60, mn % 60);
	else if (mn > 0) snprintf(len, sizeof len, "%d min", mn);
	if (s == 0) snprintf(b, sizeof b, "escuchado");
	else if (s > 0 && mn > 0) snprintf(b, sizeof b, "quedan %d min", mn - s / 60 > 0 ? mn - s / 60 : 1);
	else if (s > 0) snprintf(b, sizeof b, "en %d:%02d", s / 60, s % 60);
	else snprintf(b, sizeof b, "%s%s%s", d, d[0] && len[0] ? " · " : "", len);
	return b;
}

/* podcast show thumbnails: <folder>/cover.jpg (downloaded by the sync), decoded lazily between key polls */
#define MAXSHOWS 64
static uint16_t fo_thumbs[MAXSHOWS][BIG_THUMB * BIG_THUMB];
static signed char fo_tstate[MAXSHOWS];	/* 0 unknown, 1 ready, -1 none */
static int fo_n, fo_px = 28, fo_count[MAXSHOWS], fo_new[MAXSHOWS], fo_books;
static char fo_val[32];
/* the "Buscar nuevos" row's picture: a big orange download arrow over a base line on a dark tile (the same sign as
 * the status bar's download indicator), built once at the row's size */
static struct img search_icon(int px)
{
	static uint16_t buf[BIG_THUMB * BIG_THUMB];
	static int made;
	struct img im = { px, px, buf };
	if (made != px) {
		uint16_t bg = RGB(46, 50, 60), fg = RGB(255, 150, 0);
		int c = px / 2, sw = px / 5, top = px / 6, head = px * 11 / 20, tip = px * 4 / 5 - 2, base = px * 17 / 20;
		for (int y = 0; y < px; y++)
			for (int x = 0; x < px; x++) {
				int on = y >= top && y < head && x >= c - sw / 2 && x < c + (sw + 1) / 2;	/* shaft */
				if (y >= head && y <= tip) { int hw = (tip - y) * (px * 2 / 5) / (tip - head + 1); on |= x >= c - hw && x <= c + hw - 1; }	/* head */
				on |= y >= base && y < base + 3 && x >= px / 6 && x < px - px / 6;	/* base line */
				buf[y * px + x] = on ? fg : bg;
			}
		made = px;
	}
	return im;
}

/* the "Agregar podcast" row's picture: an orange plus on the same dark tile */
static struct img add_icon(int px)
{
	static uint16_t buf[BIG_THUMB * BIG_THUMB];
	static int made;
	struct img im = { px, px, buf };
	if (made != px) {
		uint16_t bg = RGB(46, 50, 60), fg = RGB(255, 150, 0);
		int c = px / 2, arm = px * 3 / 10, th = px / 10 + 2;
		for (int y = 0; y < px; y++)
			for (int x = 0; x < px; x++) {
				int on = (abs(x - c) < th / 2 + 1 && abs(y - c) <= arm) || (abs(y - c) < th / 2 + 1 && abs(x - c) <= arm);
				buf[y * px + x] = on ? fg : bg;
			}
		made = px;
	}
	return im;
}

static struct img fo_thumb(int i)
{
	struct img im = { 0, 0, 0 };
	if (i < fo_extra) return fo_px >= 28 ? (i == 0 ? search_icon(fo_px) : add_icon(fo_px)) : im;
	if (i < MAXSHOWS && fo_tstate[i] == 1) { im.w = im.h = fo_px; im.px = fo_thumbs[i]; }
	return im;
}
/* big rows: "12 episodios" / "8 archivos" under the name (counted with the covers, in the background) */
static const char *fo_value_big(int i)
{
	if (i < fo_extra) {
		if (i == 1) return "buscar por nombre";
		const char *d = dl_row_value();
		return d && *d ? d : "desde el teléfono";
	}
	if (i >= MAXSHOWS || fo_count[i] <= 0) return 0;
	if (!fo_books && fo_new[i] > 0) snprintf(fo_val, sizeof fo_val, "%d nuevo%s de %d", fo_new[i], fo_new[i] == 1 ? "" : "s", fo_count[i]);
	else snprintf(fo_val, sizeof fo_val, "%d %s", fo_count[i], fo_books ? (fo_count[i] == 1 ? "archivo" : "archivos") : (fo_count[i] == 1 ? "episodio" : "episodios"));
	return fo_val;
}
static int fo_thumbs_pending(void)
{
	for (int j = 0; j < fo_n; j++) {
		int i = (list_vis_top + j) % fo_n;	/* the visible rows first */
		if (fo_tstate[i]) continue;
		fo_tstate[i] = -1;
		if (!folders[i].path[0]) return 1;
		/* the folder's cover.jpg / folder.jpg (cover_load looks next to the path given), else the first audio
		 * file's own cover; the audio files are counted on the way (the second line of big rows) */
		char first[PATHLEN + 16] = "", cj[PATHLEN + 16];
		DIR *d = opendir(folders[i].path);
		if (d) {
			struct dirent *e; int n = 0;
			int nw = 0;
			while ((e = readdir(d))) if (e->d_name[0] != '.' && dec_is_audio(e->d_name)) {
				char fp[PATHLEN + 256];
				if (!fs_path(folders[i].path, e->d_name, fp, sizeof fp)) continue;
				if (!first[0]) snprintf(first, sizeof first, "%s", fp);
				if (pos_peek(fp) < 0) nw++;	/* never played */
				n++;
			}
			closedir(d);
			fo_count[i] = n; fo_new[i] = nw;
		}
		snprintf(cj, sizeof cj, "%s/cover.jpg", folders[i].path);
		int have = !access(cj, F_OK);
		if (!have) { snprintf(cj, sizeof cj, "%s/folder.jpg", folders[i].path); have = !access(cj, F_OK); }
		if (have) fo_tstate[i] = cover_load(cj, fo_thumbs[i], fo_px) ? 1 : -1;
		else if (first[0]) fo_tstate[i] = cover_load(first, fo_thumbs[i], fo_px) ? 1 : -1;
		return 1;
	}
	return 0;
}

/* OPTION in the Podcasts list: the phone / directory on the top rows, the show's own menu on a show */
static int fo_option(int i)
{
	if (i < fo_extra) { pod_menu_option(); return 0; }
	return pod_show_option(folders[i].path, folders[i].name);
}

static void browse(const char *root, const char *title, int podcasts)
{
	int keep_sel = -1;
reload:;
	if (podcasts) mkdir(root, 0755);
	int nf = list_dir(root, 1, 0, 0), extra = 0;
	if (podcasts && nf + 2 <= MAXITEMS) {	/* first rows: new episodes over the phone's Bluetooth tethering, a new show */
		memmove(items + 2, items, sizeof items[0] * nf);
		snprintf(items[0].name, sizeof items[0].name, "Buscar episodios nuevos");
		snprintf(items[1].name, sizeof items[1].name, "Agregar podcast");
		items[0].path[0] = items[1].path[0] = 0;
		nf += 2; extra = 2;
	}
	memcpy(folders, items, sizeof folders[0] * nf);
	fo_extra = extra;
	struct list_view v = { title, nf, 0, 0, fo_label, fo_value, fo_mark, LI_FOLDER, podcasts ? "Copiá carpetas a H:\\Podcasts" : "Copiá carpetas a H:\\Audiolibros", 0 };
	for (int i = extra; i < nf; i++) if (fo_mark(i)) v.sel = i;
	if (keep_sel >= 0 && keep_sel < nf) v.sel = keep_sel;
	/* covers for podcasts and audiobooks (big rows: 60 px and a file count as the second line) */
	fo_n = nf < MAXSHOWS ? nf : MAXSHOWS; memset(fo_tstate, 0, sizeof fo_tstate); memset(fo_count, 0, sizeof fo_count); memset(fo_new, 0, sizeof fo_new);
	fo_px = podcasts ? EP_THUMB : ui_big_rows ? BIG_THUMB : 28; fo_books = !podcasts;
	v.thumb = fo_thumb; v.bar_y = -1;
	if (podcasts) { v.ep = 1; v.value = fo_value_big; v.option = fo_option; }	/* shows like their episodes: cover, name, "12 episodios · 2 sin escuchar" */
	else { v.big = ui_big_rows; if (ui_big_rows) v.value = fo_value_big; }
	for (;;) {
		list_idle_hook = fo_thumbs_pending;
		int b = list_run(&v);
		list_idle_hook = 0;
		if (b == -2) { keep_sel = v.sel; goto reload; }	/* a show was added / removed from its OPTION menu */
		if (b < 0) return;
		if (extra && b == 0) { podsync_update(-1); keep_sel = 0; goto reload; }
		if (extra && b == 1) { pod_add(); keep_sel = 1; goto reload; }
	tracks_reload:;
		ntracks = list_dir(folders[b].path, 0, ".mp3", podcasts);
		memcpy(tracks, items, sizeof tracks[0] * ntracks);
		struct list_view t = { folders[b].name, ntracks, 0, 0, tr_label, 0, tr_mark, LI_TRACK, "No hay archivos MP3", 0, 0, -1, podcasts ? episode_option : 0 };
		if (podcasts) { ep_setup(ntracks); t.ep = 1; t.thumb = ep_thumb; t.value = ep_value; t.state = ep_state; t.progress = ep_progress; }
		for (int i = 0; i < ntracks; i++) if (tr_mark(i)) t.sel = i;
		for (;;) {
			if (podcasts) list_idle_hook = ep_pending;
			int s = list_run(&t);
			list_idle_hook = 0;
			if (s == -2) { ep_free(); goto tracks_reload; }	/* an episode was deleted: list the folder again */
			if (s < 0) { ep_free(); break; }
			for (int i = 0; i < ntracks; i++) {
				snprintf(queue[i].path, sizeof queue[i].path, "%s", tracks[i].path);
				snprintf(queue[i].title, sizeof queue[i].title, "%s", tracks[i].name);
				snprintf(queue[i].sub, sizeof queue[i].sub, "%s", folders[b].name);
			}
			eng_play(queue, ntracks, s, ENG_BOOK, folders[b].name);
			nowplaying_run();
			for (int i = 0; i < ntracks; i++) if (tr_mark(i)) t.sel = i;
		}
		if (b < MAXSHOWS) fo_tstate[b] = 0;	/* back to the shows: count its episodes (heard / deleted) again */
	}
}

/* ---------- Game Boy (gnuboy, separate process; the CPU runs at 248 MHz while emulating) ---------- */

static struct item games[MAXITEMS];
static char game_kind[MAXITEMS];	/* 'g' Game Boy, 'n' NES, 'd' Doom */
static const char *ga_label(int i) { return games[i].name; }
static const char *ga_value(int i) { return game_kind[i] == 'n' ? "NES" : game_kind[i] == 'd' ? "Doom" : game_kind[i] == 'a' ? "GBA" : "GB"; }

static int add_games(int n, const char *dir, const char *ext, char kind)
{
	int m = list_dir(dir, 0, ext, 0);
	for (int i = 0; i < m && n < MAXITEMS; i++) { games[n] = items[i]; game_kind[n++] = kind; }
	return n;
}

/* ---------- Bluetooth remote (homebrew/hid/hidtest as a helper process with its own screen) ---------- */

void remote_mode(void)
{
	if (access(HB_BIN "/hidtest", X_OK)) { message("Mando", "Falta hidtest en H:/HB/bin", ""); msg_wait(3); return; }
	message("Mando Bluetooth", "Encendiendo Bluetooth...", "");
	char logfn[PATHLEN + 16]; snprintf(logfn, sizeof logfn, "%s/hid.log", statedir);
	lg("remote: start");
	pid_t p = fork();
	if (p == 0) {
		int o = open(logfn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (o >= 0) { dup2(o, 1); dup2(o, 2); }
		execl(HB_BIN "/hidtest", "hidtest", "-s", "-t", "7200", (char *)0);
		_exit(127);
	}
	int stt = 0;
	double back_t = 0;
	/* the helper reads the keys and draws; we keep the music going and the watchdog fed */
	while (p > 0 && waitpid(p, &stt, WNOHANG) == 0) {
		hb_alive();
		if (eng_playing()) eng_step();
		usleep(15000);
		struct ev e;
		while (raw_event(&e, 0)) if (e.type == 1 && (e.code & 0x7f) == K_BACK) back_t = KEY_PRESSED(e.value) ? now() : 0;
		battery_update();
		if (batt_low >= 3) kill(p, 15);
		if (back_t && now() - back_t > 6) { lg("remote: BACK held 6 s, stopping it"); kill(p, SIGKILL); back_t = 0; }	/* hung */
	}
	lg("remote: exit status %d", stt);
	drain_keys();
	wake();
	force_redraw = 1;
	gfx_invalidate();	/* someone else (suspend, a game, the remote) drew on the screen */
	bt_auto_start();
}

static void games_menu(void)
{
	if (dl_active()) { message("Juegos", "Hay podcasts bajándose.", "Esperá o cancelá en Podcasts > Descargas"); msg_wait(3); return; }
	int n = 0;
	n = add_games(n, "/contents/Juegos", ".gbc", 'g');
	n = add_games(n, "/contents/Juegos", ".gb", 'g');
	n = add_games(n, "/contents/Juegos/GBA", ".gba", 'a');
	n = add_games(n, "/contents/Juegos/NES", ".nes", 'n');
	n = add_games(n, "/contents/Juegos/Doom", ".wad", 'd');
	struct list_view v = { "Juegos", n, 0, 0, ga_label, ga_value, 0, LI_NONE, "Copiá juegos a H:\\Juegos (GB), Juegos\\GBA, Juegos\\NES, Juegos\\Doom", 0 };
	for (;;) {
		int s = list_run(&v);
		if (s < 0) return;
		char kind = game_kind[s];
		const char *bin = kind == 'n' ? HB_BIN "/nes" : kind == 'd' ? HB_BIN "/doom" : kind == 'a' ? HB_BIN "/gba" : HB_BIN "/gnuboy";
		const char *title = kind == 'n' ? "NES" : kind == 'd' ? "Doom" : kind == 'a' ? "Game Boy Advance" : "Game Boy";
		if (access(bin, X_OK)) { message(title, "Falta el emulador en H:/HB/bin", ""); msg_wait(3); continue; }
		int with_music = eng_playing();
		const char *hint = kind == 'd' ? "PLAY dispara · OPTION usa · BT corre · VOL: lateral · BACK: menú, 2 s: salir"
			: kind == 'n' ? "PLAY = A · OPTION = B · BACK = Start (2 s: salir) · BT = Select"
			: kind == 'a' ? "PLAY = A · OPTION = B · VOL = L/R · BACK = Start (2 s: salir) · BT 2 s: menú"
			: with_music ? "Con tu música de fondo. BACK 2 s: salir. BT 2 s: menú." : "BACK 2 s: salir. BT 2 s: menú del juego.";
		message(title, games[s].name, hint);
		if (kind == 'd' || kind == 'a') with_music = 0;	/* Doom and the GBA need the whole CPU */
		if (kind == 'd' || kind == 'a') eng_pause(1);
		for (double t0 = now(); now() - t0 < 1.5; ) poll_key(100);
		char savedir[PATHLEN + 8], volb[8];
		snprintf(savedir, sizeof savedir, "%s/gb", statedir);
		mkdir(savedir, 0755);
		snprintf(volb, sizeof volb, "%d", volume);
		if (!with_music) { aout_stop(); aout_hw_unity(); }
		lg("gnuboy %s (music %d)", games[s].path, with_music);
		set_cpu(248);
		/* music must never starve: the player gets priority, the emulator runs niced */
		if (with_music) { setpriority(PRIO_PROCESS, 0, -3); eng_lowpower(1); }
		pid_t p = fork();
		if (p == 0) {
			setpriority(PRIO_PROCESS, 0, 0);
			if (chdir(savedir)) _exit(126);
			setenv("HB_SAVEDIR", savedir, 1);
			setenv("HB_VOL", volb, 1);
			setenv("HB_LANG", ui_lang ? "en" : "es", 1);
			if (with_music) setenv("HB_SOUND", "0", 1);
			else if (bt_connected()) setenv("HB_DSP", "/dev/sound/adsp", 1);
			char logfn[PATHLEN + 16]; snprintf(logfn, sizeof logfn, "%s/gnuboy.log", statedir);
			int o = open(logfn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (o >= 0) { dup2(o, 1); dup2(o, 2); }
			if (kind == 'd') execl(bin, "doom", "-iwad", games[s].path, "-mb", "5", (char *)0);
			else execl(bin, kind == 'n' ? "nes" : kind == 'a' ? "gba" : "gnuboy", games[s].path, (char *)0);
			_exit(127);
		}
		int stt = 0;
		double back_t = 0;
		in_game = 1;
		/* keep the music going while the game runs; VOL+/- control the music (HOLD = turbo adds 0x80 to codes).
		 * The games quit with BACK held 2 s; BACK held 6 s with the game still there = it hangs: stopped here */
		while (p > 0 && waitpid(p, &stt, WNOHANG) == 0) {
			hb_alive();
			if (eng_playing()) eng_step();
			usleep(15000);	/* the buffer holds ~0.4 s; leave the CPU to the emulator */
			struct ev e;
			while (raw_event(&e, 0)) {
				if (e.type == 1 && (e.code & 0x7f) == K_BACK) back_t = KEY_PRESSED(e.value) ? now() : 0;
				if (e.type == 1 && KEY_PRESSED(e.value) && with_music) {
					int c = e.code & 0x7f;
					if (c == K_VOLUP && volume < VOLMAX) { volume++; aout_set_volume(); pos_dirty = 1; }
					else if (c == K_VOLDN && volume > 0) { volume--; aout_set_volume(); pos_dirty = 1; }
				}
			}
			battery_update();
			if (batt_low >= 3) kill(p, 15);
			if (back_t && now() - back_t > 6) { lg("game: BACK held 6 s, stopping it"); kill(p, SIGKILL); back_t = 0; }
		}
		in_game = 0;
		set_cpu(124);
		setpriority(PRIO_PROCESS, 0, 0);
		eng_lowpower(0);
		lg("gnuboy exit status %d", stt);
		drain_keys();
		wake();
		pending_slide = 0;
	}
}

static char pw_items[3][40];
static const char *pw_label(int i) { return pw_items[i]; }

static void power_menu(void)
{
	snprintf(pw_items[0], 40, "Suspender");
	snprintf(pw_items[1], 40, "Apagar del todo");
	snprintf(pw_items[2], 40, "Volver al Walkman (Sony)");
	struct list_view v = { "Apagar", 3, 0, 0, pw_label, 0, 0, LI_NONE, 0, 0 };
	int s = list_run(&v);
	if (s == 0) suspend_now();
	else if (s == 1) {
		if (alarm_cfg.on) {	/* the RTC can wake the unit from suspend, not from a full power-off */
			message("Apagar del todo", "El despertador no suena con el equipo apagado del todo.", "Usá Suspender para que suene.");
			msg_wait(4);
			snprintf(pw_items[0], 40, "Apagar del todo"); snprintf(pw_items[1], 40, "Suspender");
			struct list_view c = { "Apagar", 2, 1, 0, pw_label, 0, 0, LI_NONE, 0, 0 };
			int r = list_run(&c);
			if (r == 1) suspend_now();
			if (r != 0) return;
		}
		power_off_full("menu");
	}
	else if (s == 2) back_to_sony();
}

static int resume_last(void);

/* the wake-up alarm: resume the last thing played (or bells) with the volume rising; a key ends the ramp */
void alarm_ring(void)
{
	lg("alarm: ring");
	wake();
	int target = volume;
	set_cpu(124);
	if (alarm_cfg.src == 0 && (eng_active() || resume_last())) {
		eng_pause(0);
		volume = 4; aout_set_volume();
		double t0 = now();
		for (;;) {
			int step = (int)((now() - t0) / 5);
			int v = 4 + step; if (v > target) v = target;
			if (v != volume) { volume = v; aout_set_volume(); }
			struct eng_info e; eng_info(&e);
			scr_message("Despertador", e.title ? e.title : "", "cualquier tecla: seguir");
			char b[16]; time_t t = time(0); struct tm tm = *localtime(&t); snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
			gfx_text_center(W / 2, 60, b, C_ORANGE, 2); gfx_flip();
			int k = poll_key(300);
			if (k >= 0 && k != K_REDRAW) break;
			if (now() - t0 > 600) break;
		}
		volume = target; aout_set_volume(); pos_dirty = 1;
	} else {
		scr_message("Despertador", "", "cualquier tecla: apagar"); gfx_flip();
		focus_bells();
	}
	force_redraw = 1;
	gfx_invalidate();	/* someone else (suspend, a game, the remote) drew on the screen */
}

/* after a restart nothing is playing: rebuild the queue around the last file and continue where it was */
static int resume_last(void)
{
	if (!lastfile[0] || access(lastfile, R_OK)) return 0;
	char dir[PATHLEN]; snprintf(dir, sizeof dir, "%s", lastfile);
	char *sl = strrchr(dir, '/');
	if (!sl) return 0;
	*sl = 0;
	int music = strstr(lastfile, "/Music/") != 0, podcast = strstr(lastfile, "/Podcasts/") != 0;
	int n = list_dir(dir, 0, ".mp3", podcast), idx = -1;
	for (int i = 0; i < n; i++) if (!strcmp(items[i].path, lastfile)) idx = i;
	if (idx < 0) return 0;
	const char *folder = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
	for (int i = 0; i < n; i++) {
		snprintf(queue[i].path, sizeof queue[i].path, "%s", items[i].path);
		snprintf(queue[i].title, sizeof queue[i].title, "%s", items[i].name);
		snprintf(queue[i].sub, sizeof queue[i].sub, "%s", folder);
	}
	eng_resume_music = 1;	/* this one song continues where it was left */
	eng_play(queue, n, idx, music ? ENG_MUSIC : ENG_BOOK, folder);
	lg("resume last: %s", lastfile);
	return eng_active();
}

/* ---------- home: 3x3 grid with Sony's zoom/glow animation ---------- */

static int home_frame[HOME_SLOTS];

static void home_draw(int sel)
{
	char nowline[160] = "";
	if (eng_active()) { struct eng_info e; eng_info(&e); if (e.sub && *e.sub && e.mode == ENG_MUSIC) snprintf(nowline, sizeof nowline, "%s · %s", e.title, e.sub); else snprintf(nowline, sizeof nowline, "%s", e.title); }
	ui_st.playing = eng_active() ? (eng_playing() ? 1 : 2) : 0;
	np_update();
	scr_home(sel, home_frame, nowline);
}

/* Sony's 9-frame zoom: only the two cells that change are redrawn and pushed to the screen (a full redraw +
 * flip per frame made it crawl); the label is updated with the last frame */
static void home_animate(int from, int to)
{
	double t0 = now();
	for (int k = 1; k <= 8; k++) {
		if (from >= 0) { home_frame[from] = k; scr_home_icon(from, k); gfx_flip_rect(HOME_CELL_X(from), HOME_CELL_Y(from), HOME_CELL_W, HOME_CELL_H); }
		home_frame[to] = 8 - k; scr_home_icon(to, 8 - k); gfx_flip_rect(HOME_CELL_X(to), HOME_CELL_Y(to), HOME_CELL_W, HOME_CELL_H);
		if (eng_playing()) eng_step();
		double next = t0 + k * 0.016;	/* ~16 ms per frame: the whole zoom in ~130 ms */
		while (now() < next) usleep(2000);
	}
	home_draw(to);
	gfx_flip();
	int ms = (int)((now() - t0) * 1000);
	if (ms > 220) lg("ui: home animation took %d ms (130 planned)", ms);
}

static int home_valid(int i) { return i >= 0 && i < HOME_SLOTS && home_labels[i][0]; }

static void home(void)
{
	int sel = 4, redraw = 1;	/* Música, in the centre */
	for (int i = 0; i < HOME_SLOTS; i++) home_frame[i] = 8;
	if (lastfile[0] || eng_active()) sel = 3;	/* "Reproduciendo": one press resumes where it was */
	home_frame[sel] = 0;
	ui_pod_new = dl_pending();
	for (;;) {
		if (popup_expired()) redraw = 1;
		if (redraw && backlight_on && !popup_active()) { home_draw(sel); flip(); redraw = 0; }
		{ static int warmed; if (!warmed) { warmed = 1; warm_start(); } }	/* the home screen is up: warm the rest behind it */
		idle_tick(0);
		int k = poll_key(1000);
		if (k == K_REDRAW) { redraw = 1; continue; }
		if (k < 0) {	/* refresh the "now playing" line only when the track changes */
			static char shown[96];
			static int was_dl;	/* the badge follows downloads running in the background (and their end) */
			if (dl_active() || was_dl) { int pn = dl_pending(); if (pn != ui_pod_new) { ui_pod_new = pn; redraw = 1; } }
			was_dl = dl_active();
			struct eng_info e; eng_info(&e);
			const char *t = eng_active() && e.title ? e.title : "";
			if (strcmp(shown, t)) { snprintf(shown, sizeof shown, "%s", t); redraw = 1; }
			continue;
		}
		int ns = sel;
		if (k == K_LEFT) { do ns = (ns + HOME_SLOTS - 1) % HOME_SLOTS; while (!home_valid(ns)); }
		else if (k == K_RIGHT) { do ns = (ns + 1) % HOME_SLOTS; while (!home_valid(ns)); }
		else if (k == K_UP) { ns = (sel + HOME_SLOTS - 3) % HOME_SLOTS; if (!home_valid(ns)) ns = sel; }
		else if (k == K_DOWN) { ns = (sel + 3) % HOME_SLOTS; if (!home_valid(ns)) ns = sel; }
		else if (k == K_PLAY) {
			pending_slide = 1;
			switch (sel) {
			case 0: focus_menu(); break;
			case 1: browse("/contents/Audiolibros", "Audiolibros", 0); break;
			case 2: browse("/contents/Podcasts", "Podcasts", 1); break;
			case 3: if (eng_active() || resume_last()) nowplaying_run(); else { pending_slide = 0; eng_note("No hay nada sonando"); } break;
			case 4: music_menu(); break;
			case 5: games_menu(); break;
			case 6: bt_menu(); break;
			case 7: power_menu(); break;
			case 8: settings_menu(); break;
			}
			pos_save();
			ui_pod_new = dl_pending();
			redraw = 1;
			continue;
		}
		else if (k == K_OPTION && eng_active()) { pending_slide = 1; nowplaying_run(); redraw = 1; continue; }
		else if (k == K_BT) { pending_slide = 1; bt_menu(); pending_slide = -1; redraw = 1; continue; }
		else change_volume(k);
		if (ns != sel) { home_animate(sel, ns); sel = ns; continue; }	/* the animation left the final frame on screen */
		redraw = 1;
	}
}

static void tick_audio(void) { if (eng_playing()) eng_step(); }

/* after a crash: the saved position goes into the position table; returns 1 if it was playing */
static int crash_resume(void)
{
	char fn[PATHLEN + 16]; snprintf(fn, sizeof fn, "%s/resume.txt", statedir);
	FILE *f = fopen(fn, "r");
	if (!f) return 0;
	long bytes = 0; int sec = 0, playing = 0; char path[PATHLEN] = "";
	int ok = fscanf(f, "%ld %d %d %511[^\n]", &bytes, &sec, &playing, path) == 4;
	fclose(f);
	unlink(fn);
	if (!ok || access(path, R_OK)) return 0;
	pos_set(path, bytes, sec);
	snprintf(lastfile, sizeof lastfile, "%s", path); pos_dirty = 1;
	pos_save();
	lg("crash recovery: %s at %d s%s", path, sec, playing ? ", playing" : "");
	return playing;
}

/* Ajustes > Restablecer todo: the player's files on /var go (positions, settings, EQ, Bluetooth pairings, boot mode,
 * podcast badge); the music, the podcasts, the games and their saves stay. Then a restart, like a first boot. */
void factory_reset(void)
{
	static const char *const files[] = { "pos.txt", "eq.txt", "bt.txt", "bt_auto", "mode", "resume.txt", "boot_player",
		"bt/sc_db.db", "bt/hid_host", "log.1.txt", 0 };
	message("Restablecer todo", "Reiniciando...", "");
	eng_stop();
	for (int i = 0; files[i]; i++) { char p[PATHLEN + 32]; snprintf(p, sizeof p, "%s/%s", statedir, files[i]); unlink(p); }
	unlink("/contents/HB/pendientes.txt");
	lg("factory reset");
	prepare_shutdown();
	sleep(1);
	reboot(RB_AUTOBOOT);
	sleep(30);
}

/* the first start (no pos.txt yet): the language, then how to get around */
static void first_run(void)
{
	static const char *const langs[2] = { "Español", "English" };
	static char l[2][16];
	for (int i = 0; i < 2; i++) snprintf(l[i], sizeof l[i], "%s", langs[i]);
	struct list_view v = { .title = "Idioma / Language", .n = 2, .label = 0, .bar_y = -1 };
	v.label = pw_label;
	snprintf(pw_items[0], 40, "%s", l[0]); snprintf(pw_items[1], 40, "%s", l[1]);
	int s;
	while ((s = list_run(&v)) < 0) ;
	ui_lang_set(s == 1);
	settings_save();
	scr_message("Bienvenido", "HOME 3 s en cualquier pantalla: volver al Walkman de Sony.", "En el Walkman de Sony, HOME 3 s: volver acá.");
	gfx_text_wrap(14, 220, "OPTION: más opciones en casi todas las pantallas.", C_GREY, W - 28, 2, 22);
	gfx_flip();
	int k;
	do { idle_tick(0); k = getkey(1000); } while (k != K_PLAY && k != K_BACK);
}

static int run(void)
{
	cover_tick = tick_audio;
	scr_splash("Abriendo..."); gfx_flip();
	int first = 0;
	{ char p[PATHLEN + 16]; snprintf(p, sizeof p, "%s/pos.txt", statedir); first = access(p, F_OK) != 0; }
	pos_load();
	int was_playing = crash_resume();
	mode_set("player");
	eq_load(statedir);
	eq_apply(44100);
	read_hold();
	battery_update();
	wake();
	srand((unsigned)time(0));
	wd_arm();
	if (first) first_run();
	bt_auto_start();
	if (was_playing) resume_last();	/* after a crash: music goes on from where it was */
	home();
	return 0;
}

static int ask(int secs)
{
	double end = now() + secs;
	scr_ask(); gfx_flip();
	while (now() < end) {
		struct ev e;
		if (raw_event(&e, 200) && e.type == 1 && (e.code & 0x7f) == K_BACK && KEY_PRESSED(e.value)) {
			scr_splash("Abriendo..."); gfx_flip();
			return 0;
		}
	}
	scr_splash(0); gfx_flip();
	return 1;
}

#ifdef HB_DEV	/* development tools (build with -DHB_DEV): self-test, benchmarks. Not in the shipped player. */
/* "hbplayer selftest STATEDIR FILE SECONDS": plays FILE through the normal engine for SECONDS without UI and
 * logs which path it took (DSP / 24-bit / libmad) and the CPU it cost (whole system, from /proc/stat). Used by
 * one-shot boot tests. */
static void cpu_sample(unsigned long long *busy, unsigned long long *total)
{
	unsigned long long v[7] = { 0 };
	FILE *f = fopen("/proc/stat", "r");
	*busy = *total = 0;
	if (!f) return;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) >= 4)
		for (int i = 0; i < 7; i++) { *total += v[i]; if (i != 3 && i != 4) *busy += v[i]; }
	fclose(f);
}

/* this process's CPU ticks (utime + stime) */
static unsigned long self_ticks(void)
{
	char b[512]; unsigned long ut = 0, stt = 0;
	FILE *f = fopen("/proc/self/stat", "r");
	if (!f) return 0;
	if (fgets(b, sizeof b, f)) { char *q = strrchr(b, ')'); if (q) sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &stt); }
	fclose(f);
	return ut + stt;
}

/* context switches and interrupts so far (/proc/stat "ctxt" and "intr" totals): how often the CPU wakes up */
static void wake_sample(unsigned long long *ctxt, unsigned long long *intr)
{
	*ctxt = *intr = 0;
	FILE *f = fopen("/proc/stat", "r");
	if (!f) return;
	char l[256];
	while (fgets(l, sizeof l, f)) {
		if (!strncmp(l, "ctxt ", 5)) *ctxt = strtoull(l + 5, 0, 10);
		else if (!strncmp(l, "intr ", 5)) *intr = strtoull(l + 5, 0, 10);
	}
	fclose(f);
}

/* lcd interrupts so far (/proc/interrupts line "lcd") */
static unsigned long lcd_irq_count(void)
{
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) return 0;
	char l[160]; unsigned long v = 0;
	while (fgets(l, sizeof l, f)) if (strstr(l, " lcd")) { int n; sscanf(l, " %d: %lu", &n, &v); }
	fclose(f);
	return v;
}

static int selftest(const char *file, int secs)
{
	eq_load(statedir);
	if (getenv("HB_MHZ")) set_cpu(atoi(getenv("HB_MHZ")));
	if (getenv("HB_NODSP")) dsp_enabled = 0;
	if (getenv("HB_NODIRECT")) dsp_direct = 0;	/* "Máxima batería" is on by default now */
	/* tanda 4 effects: HB_TRIM / HB_BOOST (voice, with HB_BOOK), HB_SPEED=150, HB_XFEED / HB_LOUD (headphones) */
	voice_trim_on = getenv("HB_TRIM") != 0; voice_boost_on = getenv("HB_BOOST") != 0;
	hp_xfeed = getenv("HB_XFEED") != 0; hp_loud = getenv("HB_LOUD") != 0;
	if (getenv("HB_SPEED")) eng_speed = atoi(getenv("HB_SPEED"));
	{ extern int eng_prof; eng_prof = getenv("HB_PROF") != 0; }
	if (hp_loud) volume = VOLMAX / 2;
	/* FILE may be several, "a.m4a|b.mp3": they play as one queue (track changes, e.g. a normal path -> direct) */
	static struct track tl[8];
	int nt = 0;
	for (const char *p = file; *p && nt < 8; nt++) {
		const char *e = strchr(p, '|');
		int len = e ? (int)(e - p) : (int)strlen(p);
		snprintf(tl[nt].path, sizeof tl[nt].path, "%.*s", len, p);
		snprintf(tl[nt].title, sizeof tl[nt].title, "selftest %d", nt + 1);
		p += len + (e ? 1 : 0);
	}
	lg("selftest: %s for %d s (dsp %d, eq %d, sony vpt %d eq %d dn %d)", file, secs, dsp_enabled, eq_active(), sony_vpt, sony_eq, sony_dn);
	if (getenv("HB_DARK")) { gfx_backlight(0); if (!getenv("HB_LCDON")) gfx_lcd(0); }	/* HB_LCDON: dark but refresh left on (to compare) */
	if (getenv("HB_WAITUNPLUG")) {	/* battery test: only meaningful on battery */
		double w0 = now();
		battery_update();
		while (ui_st.charging && now() - w0 < 600) { sleep(5); battery_update(); }
		lg("selftest: %s after %.0f s", ui_st.charging ? "still charging, giving up" : "unplugged", now() - w0);
		if (ui_st.charging) return 1;
		sleep(20);	/* let the voltage settle after the charger goes away */
	}
	eng_play(tl, nt, 0, getenv("HB_BOOK") ? ENG_BOOK : ENG_MUSIC, "selftest");
	double t0 = now(), next = t0 + 5, next_batt = t0;
	unsigned long long b0, a0, b1, a1;
	cpu_sample(&b0, &a0);
	unsigned long p0 = self_ticks(), p1;
	unsigned long long c0, i0, c1, i1;
	unsigned long l0 = lcd_irq_count(), l1;
	wake_sample(&c0, &i0);
	double tw = now();
	/* HB_UILOOP=1: the player's real key loop (what a user gets) instead of a tight 5 ms test loop */
	int uiloop = getenv("HB_UILOOP") != 0;
	if (uiloop) lg("selftest: real UI loop");
	while (now() - t0 < secs && eng_active()) {
		if (uiloop) { idle_tick(1); poll_key(500); }
		else { eng_step(); usleep(5000); }
		if (getenv("HB_WAITUNPLUG") && now() >= next_batt) {
			next_batt += 60;
			battery_update();
			lg("selftest: batt t %.0f pct %d charging %d adc %d", now() - t0, ui_st.batt, ui_st.charging, last_adc);
		}
		if (now() >= next) {
			next += getenv("HB_WAITUNPLUG") ? 300 : 5;
			cpu_sample(&b1, &a1);
			p1 = self_ticks();
			struct eng_info e; eng_info(&e);
			lg("selftest: t %.0f pos %d/%d s | dsp %d hires %d out32 %d rate %d bits %d | cpu busy %.1f%% player %.1f%% (%d MHz)", now() - t0, e.sec, e.total,
			   dec_on_dsp(), eng_hires(), aout_is32(), dec_rate(), dec_bits(), a1 > a0 ? 100.0 * (b1 - b0) / (a1 - a0) : 0,
			   a1 > a0 ? 100.0 * (p1 - p0) / (a1 - a0) : 0, cpu_mhz);
			wake_sample(&c1, &i1);
			double dt = now() - tw;
			eng_prof_report();
			l1 = lcd_irq_count();
			if (dt > 0) lg("selftest: wakeups: %.0f ctxt/s, %.0f irq/s (lcd %.1f), underruns %d, saved %.1f s", (c1 - c0) / dt, (i1 - i0) / dt, (l1 - l0) / dt, eng_underruns(), voice_saved_sec());
			c0 = c1; i0 = i1; l0 = l1; tw = now();
			b0 = b1; a0 = a1; p0 = p1;
		}
	}
	eng_stop();
	dsp_stop_helper();
	aout_stop();
	lg("selftest: end");
	return 0;
}

/* hbplayer bench: CPU cost of each audio stage on 10 s of synthetic stereo 44.1 kHz (speech-like bursts), in ms of
 * CPU per second of audio (user+system time of this process, so it doesn't matter what else runs) */
double sin(double);	/* (math.h would clash with our log file variable logf) */
static double cpu_now(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6; }	/* (clock_gettime needs librt: not on the device) */
/* UI cost per operation (ms), averaged over many repeats (getrusage has 10 ms resolution) */
static const char *bl_names[9] = { "Aleatorio (todas)", "Canciones", "Artistas", "Álbumes", "Alter Bridge - Blackbird", "Fortress", "The Last Hero", "Pawns & Kings", "Walk the Sky 2.0" };
static const char *bl_label(int i) { return bl_names[i]; }
static const char *bl_value(int i) { static const char *v[9] = { "", "1.25x", "", "", "12", "", "sí", "", "" }; return v[i]; }
static void bench_ui(void)
{
	double t0; int R;
	#define UI(name, reps, code) do { R = reps; t0 = cpu_now(); for (int _r = 0; _r < R; _r++) { code; } lg("bench ui: %-26s %7.2f ms (%d MHz)", name, (cpu_now() - t0) * 1000 / R, cpu_mhz); } while (0)
	struct list_view v = { "Música", 9, 2, 0, bl_label, bl_value, 0, LI_FOLDER, 0, 0, 0, -1 };
	UI("scr_list 9 rows", 100, scr_list(&v));
	UI("gfx_flip all rows changed", 100, { gfx_invalidate(); gfx_flip(); });
	UI("gfx_flip nothing changed", 200, gfx_flip());
	{ static uint16_t tmp[W * H]; UI("memcpy frame (glibc)", 200, memcpy(tmp, fb, W * H * 2)); UI("gfx_copy32 frame", 200, gfx_copy32(tmp, fb, W * H * 2)); }
	UI("scr_status (title+bottom)", 200, scr_status("Canciones"));
	UI("gfx_text 26 chars", 1000, gfx_text(10, 100, "Ghost of Days Gone By (Liv", 0xFFFF, 1));
	struct player_view pv; memset(&pv, 0, sizeof pv);
	pv.book = "Canciones"; pv.track = "Ghost of Days Gone By"; pv.sub = "Alter Bridge - AB III.5"; pv.genre = "Alternative";
	pv.idx = 2; pv.ntr = 14; pv.sec = 71; pv.total = 233; pv.vol = 14; pv.volmax = VOLMAX;
	UI("scr_player full", 100, scr_player(&pv, 1));
	UI("scr_player seconds tick", 200, scr_player(&pv, 0));
	int fr[HOME_SLOTS]; for (int i = 0; i < HOME_SLOTS; i++) fr[i] = 8; fr[4] = 0;
	UI("scr_home", 100, scr_home(4, fr, "Slip to the Void"));
	UI("scr_home_icon (anim frame)", 400, scr_home_icon(4, 3));
	static const char *nm[7] = { "Efecto de son. des.", "Heavy", "Pop", "Jazz", "Exclusivo", "Personalizado", "EQ avanzado" };
	int z[5] = { 1, 0, -1, 1, 2 };
	UI("scr_sony_eq", 100, scr_sony_eq(nm, 7, 1, 0, 1, 2, z, -1, 0));
	#undef UI
}

static int bench(void)
{
	enum { N = 441000 };
	static short src[N * 2], a[(N + 2000) * 2], o[4096 * 2];
	unsigned rnd = 1;
	for (int i = 0; i < N; i++) {	/* 300 Hz tone + noise, 1.5 s on / 0.5 s off */
		int on = (i / 22050) % 4 != 3;
		rnd = rnd * 1103515245 + 12345;
		int v = on ? (int)(8000 * sin(2 * 3.14159265358979 * 300 * i / 44100.0)) + (int)((rnd >> 16) & 2047) - 1024 : (int)((rnd >> 16) & 63) - 32;
		src[2 * i] = (short)v; src[2 * i + 1] = (short)(v / 2);
	}
	double t0;
	#define STAGE(name, code) do { memcpy(a, src, sizeof src); t0 = cpu_now(); code; lg("bench: %-22s %6.1f ms per s of audio (%d MHz)", name, (cpu_now() - t0) * 1000 / 10, cpu_mhz); } while (0)
	STAGE("copy only", { });
	STAGE("voice_trim", { int m = 0; for (int i = 0; i < N; i += 1152) m += voice_trim(a + 2 * i, N - i < 1152 ? N - i : 1152, 44100, o); (void)m; });
	STAGE("voice_boost", { for (int i = 0; i < N; i += 1152) voice_boost(a + 2 * i, N - i < 1152 ? N - i : 1152, 44100); });
	STAGE("stretch 1.5x", { stretch_set_rate(44100); stretch_set_speed(150); for (int i = 0; i < N; i += 1152) { stretch_push(a + 2 * i, N - i < 1152 ? N - i : 1152); while (stretch_pull(o) > 0) ; } });
	hp_xfeed = 1; hp_loud = 0;
	STAGE("eq crossfeed", { eq_apply(44100); for (int i = 0; i < N; i += 1152) eq_process(a + 2 * i, N - i < 1152 ? N - i : 1152, 44100); });
	hp_xfeed = 0; hp_loud = 1; volume = VOLMAX / 2;
	STAGE("eq loudness", { eq_apply(44100); for (int i = 0; i < N; i += 1152) eq_process(a + 2 * i, N - i < 1152 ? N - i : 1152, 44100); });
	hp_loud = 0; eq.on = 1; for (int k = 0; k < 5; k++) eq.band[k] = 3;
	STAGE("eq 5 bands", { eq_apply(44100); for (int i = 0; i < N; i += 1152) eq_process(a + 2 * i, N - i < 1152 ? N - i : 1152, 44100); });
	return 0;
}

#endif

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: hbplayer ask SECONDS | run STATEDIR | wdoff\n"); return 2; }
	if (gfx_open()) { fprintf(stderr, "fb failed\n"); return 3; }
	gfx_load_res(RESDIR);
	set_cloexec(gfx_fd);
	keyfd = open("/dev/input/event0", O_RDONLY);
	if (keyfd < 0) { fprintf(stderr, "keys failed\n"); return 3; }
	set_cloexec(keyfd);
	if (!strcmp(argv[1], "wdoff")) {	/* boot script, after the player: watchdog off, LCD refresh on, backlight on */
		wdfd = open("/dev/misc/watchdog", O_RDWR); wd_disarm();
		int v = 0; ioctl(gfx_fd, 0x40044E35, &v);	/* gfx_lcd(1) would trust this process' own idea that it is on */
		gfx_backlight(80);
		return 0;
	}
	if (!strcmp(argv[1], "ask")) return ask(argc > 2 ? atoi(argv[2]) : 3);
#ifdef HB_DEV
	if (!strcmp(argv[1], "bench")) { logf = fopen(argc > 2 ? argv[2] : "/tmp/bench.txt", "a"); if (getenv("HB_MHZ")) set_cpu(atoi(getenv("HB_MHZ"))); if (getenv("HB_UI")) { bench_ui(); return 0; } return bench(); }
#endif
	if (argc > 2) snprintf(statedir, sizeof statedir, "%s", argv[2]);
	char lf[PATHLEN + 16]; snprintf(lf, sizeof lf, "%s/log.txt", statedir);
	struct stat st;
	if (stat(lf, &st) == 0 && st.st_size > 65536) {	/* rotate: the previous log stays as log.1.txt (crash evidence) */
		char old[PATHLEN + 16]; snprintf(old, sizeof old, "%s/log.1.txt", statedir);
		rename(lf, old);
	}
	logf = fopen(lf, "a");
	if (logf) set_cloexec(fileno(logf));
	lg("hbplayer start");
	{	/* every way to die leaves the LCD refresh on and the watchdog off for the Sony UI that follows */
		struct sigaction sa; memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = on_fatal_info; sa.sa_flags = SA_SIGINFO;
		int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTERM, SIGHUP, SIGQUIT };
		for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);
		signal(SIGPIPE, SIG_IGN);
	}
#ifdef HB_DEV
	if (!strcmp(argv[1], "selftest") && argc > 4) return selftest(argv[3], atoi(argv[4]));
#endif
	gfx_lcd(1);
	gfx_backlight(BRIGHT);
	return run();
}
