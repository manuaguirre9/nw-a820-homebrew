/* hidtest: spike for the Bluetooth HID device. The Walkman becomes "Walkman NW-A829 Remote", a keyboard +
 * media-keys device for a phone or PC; its buttons are sent as HID reports.
 * usage: hidtest [-s] [-t seconds] [-R] [-F]
 *   -s  bring the Bluetooth stack up/down ourselves (bt_test 3 + bc_attach), like pan.c; default: it is
 *       already running (boot_hidtest.sh starts it, like the pantest block of boot.sh)
 *   -t  run time (default 290 s); -R  do not reconnect to the stored host at start; -F  no screen
 * env: HID_NAME, HID_COD (default 0x002580), HID_PIN (default 0000), HID_SECL (default 0x0006)
 * keys (media mode): PLAY play/pause, RIGHT/LEFT next/prev track, UP/DOWN arrows, VOL+/- volume, BACK Esc;
 *       (slides mode) PLAY Enter, RIGHT/LEFT arrows, UP/DOWN PageUp/PageDown;
 *       OPTION toggles the mode, BT short press reconnects, BT held 2 s quits. Logs to stderr. */
#include "btcm.h"
#include "hid.h"
#include "../common/gfx.h"
#include "../common/keys.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

void lg(const char *fmt, ...)
{
	struct timeval t; gettimeofday(&t, 0);
	va_list ap; va_start(ap, fmt);
	fprintf(stderr, "[%ld.%03ld] ", (long)t.tv_sec, (long)(t.tv_usec / 1000));
	vfprintf(stderr, fmt, ap); fputc('\n', stderr); fflush(stderr);
	va_end(ap);
}

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

/* ---------- stack up/down (as in homebrew/net/pan.c) ---------- */
static pid_t pid_bca = -1;

static void set_low_latency(void)
{
	int fd = open("/dev/tts/0", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { lg("hidtest: open /dev/tts/0 failed"); return; }
	struct serial_struct ss;
	if (!ioctl(fd, TIOCGSERIAL, &ss)) {
		if (!(ss.flags & ASYNC_LOW_LATENCY)) {
			ss.flags |= ASYNC_LOW_LATENCY;
			if (ioctl(fd, TIOCSSERIAL, &ss)) lg("hidtest: TIOCSSERIAL failed"); else lg("hidtest: UART low latency set");
		} else lg("hidtest: UART already low latency");
	}
	close(fd);
}

static void stack_up(void)
{
	lg("hidtest: bt_test 3 + bc_attach");
	system("/usr/local/test/bt/bt_test 3 > /dev/null 2>&1");
	pid_bca = fork();
	if (pid_bca == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
		execl("/usr/local/test/bt/bc_attach", "bc_attach", "-b", "2592000", "-d", "/dev/tts/0", (char *)0);
		_exit(127);
	}
	sleep(3);
}

static void stack_down(void)
{
	if (pid_bca > 0) { kill(pid_bca, SIGTERM); waitpid(pid_bca, 0, 0); pid_bca = -1; }
	usleep(300000);
	system("/usr/local/test/bt/bt_test 4 > /dev/null 2>&1");
	lg("hidtest: stack down");
}

/* ---------- screen ---------- */
static int use_fb, slides;
static char last_key[40] = "-";
static int dirty = 1;

static void draw(void)
{
	if (!use_fb) return;
	char b[64];
	gfx_fill(0, 0, W, H, RGB(8, 12, 36));
	gfx_fill(0, 0, W, 34, RGB(20, 40, 110));
	gfx_text(8, 6, "Mando Bluetooth", RGB(255, 255, 255), 1);
	enum hid_state s = hid_state();
	uint16_t sc = s == HID_CONNECTED ? RGB(120, 255, 120) : s == HID_LISTENING ? RGB(255, 210, 80) : RGB(200, 200, 255);
	gfx_text(8, 46, hid_state_str(), sc, 1);
	if (hid_have_host()) { snprintf(b, sizeof b, "Host: %s", hid_host_str()); gfx_text(8, 72, b, RGB(190, 190, 190), 1); }
	else gfx_text(8, 72, "Sin host emparejado", RGB(190, 190, 190), 1);
	snprintf(b, sizeof b, "Modo: %s", slides ? "diapositivas" : "multimedia");
	gfx_text(8, 104, b, RGB(255, 255, 255), 1);
	snprintf(b, sizeof b, "Tecla: %s", last_key);
	gfx_text(8, 130, b, RGB(255, 255, 0), 1);
	if (s == HID_LISTENING)
		gfx_text_wrap(8, 170, "En el telefono: Bluetooth > buscar > \"Walkman NW-A829 Remote\". PIN 0000", RGB(170, 170, 220), W - 16, 4, 22);
	gfx_text(8, 262, "OPTION: cambiar modo", RGB(150, 150, 150), 1);
	gfx_text(8, 284, "BT 2 s: salir", RGB(150, 150, 150), 1);
	gfx_flip();
}

/* ---------- keys ---------- */
enum { A_NONE, A_KEY, A_CC };
struct act { int kind; unsigned v; const char *name; };
static struct act pressed[32];	/* what each held button is sending, so the release matches the press */
static double bt_down_at;

static struct act map_key(int code)
{
	struct act a = { A_NONE, 0, "" };
	switch (code) {
	case K_PLAY:  a = slides ? (struct act){ A_KEY, KB_ENTER, "Enter" } : (struct act){ A_CC, CC_PLAYPAUSE, "Play/Pausa" }; break;
	case K_RIGHT: a = slides ? (struct act){ A_KEY, KB_RIGHT, "Derecha" } : (struct act){ A_CC, CC_NEXT, "Siguiente" }; break;
	case K_LEFT:  a = slides ? (struct act){ A_KEY, KB_LEFT, "Izquierda" } : (struct act){ A_CC, CC_PREV, "Anterior" }; break;
	case K_UP:    a = slides ? (struct act){ A_KEY, KB_PGUP, "RePag" } : (struct act){ A_KEY, KB_UP, "Arriba" }; break;
	case K_DOWN:  a = slides ? (struct act){ A_KEY, KB_PGDN, "AvPag" } : (struct act){ A_KEY, KB_DOWN, "Abajo" }; break;
	case K_VOLUP: a = (struct act){ A_CC, CC_VOLUP, "Vol+" }; break;
	case K_VOLDN: a = (struct act){ A_CC, CC_VOLDN, "Vol-" }; break;
	case K_BACK:  a = (struct act){ A_KEY, KB_ESC, "Esc" }; break;
	}
	return a;
}

static void key_event(int code, int value)
{
	int down = KEY_PRESSED(value);
	if (value != 0 && value != 1) return;	/* autorepeat: the host repeats by itself */
	if (code & 0x80 || code == K_HOLD_B) { lg("key: code %d value %d (HOLD on, ignored)", code, value); return; }
	if (code < 0 || code >= 32) { lg("key: code %d value %d (unknown)", code, value); return; }
	lg("key: code %d %s", code, down ? "down" : "up");
	if (code == K_OPTION) {
		if (down) { slides = !slides; lg("hidtest: %s mode", slides ? "slides" : "media"); snprintf(last_key, sizeof last_key, "OPTION"); dirty = 1; }
		return;
	}
	if (code == K_BT) {
		if (down) bt_down_at = now();
		else if (bt_down_at > 0) {
			bt_down_at = 0;
			if (hid_state() == HID_LISTENING && hid_have_host()) hid_reconnect();
			dirty = 1;
		}
		return;
	}
	struct act a;
	if (down) { a = map_key(code); pressed[code] = a; }
	else { a = pressed[code]; pressed[code].kind = A_NONE; }
	if (a.kind == A_NONE) return;
	int r = a.kind == A_KEY ? hid_key(a.v, down) : hid_consumer(a.v, down);
	if (down) { snprintf(last_key, sizeof last_key, "%s%s", a.name, r ? " (sin conexion)" : ""); dirty = 1; }
	lg("hid: %s %s %s%s", a.kind == A_KEY ? "key" : "media", a.name, down ? "press" : "release", r ? " NOT SENT (not connected)" : "");
}

int main(int argc, char **argv)
{
	int own_stack = 0, secs = 290, autoreconnect = 1;
	use_fb = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s")) own_stack = 1;
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-R")) autoreconnect = 0;
		else if (!strcmp(argv[i], "-F")) use_fb = 0;
		else { fprintf(stderr, "usage: hidtest [-s] [-t seconds] [-R] [-F]\n"); return 2; }
	}
	signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGALRM, on_sig); signal(SIGPIPE, SIG_IGN);
	alarm(secs);
	lg("hidtest: start, %d s", secs);
	if (use_fb) {
		if (gfx_open()) { lg("hidtest: no framebuffer"); use_fb = 0; }
		else { int rr = gfx_load_res("/usr/local/share/gemini/res"); lg("hidtest: screen ok, res %d", rr); }
	}
	if (use_fb) { gfx_fill(0, 0, W, H, RGB(8, 12, 36)); gfx_text(8, 140, "Encendiendo Bluetooth...", RGB(255, 255, 255), 1); gfx_flip(); }
	if (own_stack) stack_up();
	set_low_latency();
	mkdir("/var/hb", 0755); mkdir("/var/hb/bt", 0700);
	if (btcm_open("/var/hb/bt/sc_db.db")) { if (own_stack) stack_down(); return 1; }
	struct bd_addr me;
	if (btcm_local_addr(&me)) lg("hidtest: local address unknown (stack not up?)");
	struct hid_cfg cfg = { 0, 0x002580, 0, 0, 0 };
	cfg.name = getenv("HID_NAME") ? getenv("HID_NAME") : "Walkman NW-A829 Remote";
	if (getenv("HID_COD")) cfg.cod = (unsigned)strtoul(getenv("HID_COD"), 0, 0);
	if (getenv("HID_PIN")) cfg.pin = getenv("HID_PIN");
	if (getenv("HID_SECL")) cfg.secl = (unsigned)strtoul(getenv("HID_SECL"), 0, 0);
	hid_init(&cfg);
	if (autoreconnect && hid_have_host()) hid_reconnect();
	int ev = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (ev < 0) lg("hidtest: /dev/input/event0 open failed (%d)", errno);
	enum hid_state last_st = hid_state();
	double last_draw = 0;
	while (!quit) {
		struct pollfd p[2] = { { btcm_fd(), POLLIN, 0 }, { ev, POLLIN, 0 } };
		poll(p, ev >= 0 ? 2 : 1, 50);
		for (int k = 0; k < 32 && btcm_poll(0) != -1; k++) ;
		hid_tick();
		if (ev >= 0) {
			struct { long sec, usec; unsigned short type, code; int value; } e;
			while (read(ev, &e, sizeof e) == (int)sizeof e)
				if (e.type == 1) key_event(e.code, e.value);
		}
		if (bt_down_at > 0 && now() - bt_down_at >= 2) { lg("hidtest: BT held 2 s, quitting"); break; }
		if (hid_state() != last_st) { last_st = hid_state(); dirty = 1; }
		if (dirty || now() - last_draw > 5) { draw(); dirty = 0; last_draw = now(); }
	}
	lg("hidtest: stopping (%s)", quit ? "signal/timeout" : "user");
	if (use_fb) { gfx_fill(0, 0, W, H, RGB(8, 12, 36)); gfx_text(8, 140, "Mando Bluetooth: fin", RGB(255, 255, 255), 1); gfx_flip(); }
	hid_shutdown();
	btcm_close();
	if (own_stack) stack_down();
	lg("hidtest: done");
	return 0;
}
