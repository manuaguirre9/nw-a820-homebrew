/* hbwatch: the small helper that runs next to the Sony UI. Holding HOME for 3 s writes the "boot into the player"
 * flag, stops the Sony UI, shows the Walkman splash and reboots (the dual-boot script then starts the player).
 * It replaces "hbplayer watch": the 2 MB player (audio, network, TLS, decoders) used to sit in RAM next to the Sony
 * UI, which only has ~2.4 MB free, just to watch one key.
 *   hbwatch STATEDIR */
#include "../common/gfx.h"
#include "../common/keys.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RESDIR "/usr/local/share/gemini/res"

struct ev { long sec, usec; unsigned short type, code; int value; };

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

/* the player's language setting ("lang 1" in STATEDIR/pos.txt = English), read only when the splash is drawn */
static int lang_en(const char *statedir)
{
	char fn[512], line[256]; int en = 0;
	snprintf(fn, sizeof fn, "%s/pos.txt", statedir);
	FILE *f = fopen(fn, "r");
	if (!f) return 0;
	while (fgets(line, sizeof line, f)) if (!strncmp(line, "lang ", 5)) en = atoi(line + 5) == 1;
	fclose(f);
	return en;
}

/* a short log in STATEDIR/hbwatch.log (the boot script copies it to H:\HB\out). /var is the Sony UI's to check and
 * mount after we start, so nothing is written until STATEDIR exists: lines wait in memory until then. */
static char pend[1024];
static void lg_flush(const char *statedir);
static void lg(const char *statedir, const char *fmt, ...)
{
	char line[160];
	va_list a; va_start(a, fmt);
	int n = snprintf(line, sizeof line, "[%ld] ", (long)time(0));
	vsnprintf(line + n, sizeof line - n, fmt, a);
	va_end(a);
	if (strlen(pend) + strlen(line) + 2 < sizeof pend) { strcat(pend, line); strcat(pend, "\n"); }
	lg_flush(statedir);
}

/* writes the waiting lines once STATEDIR is there (also called from the idle loop: lines never wait for a key) */
static void lg_flush(const char *statedir)
{
	if (!pend[0]) return;
	char fn[512]; snprintf(fn, sizeof fn, "%s/hbwatch.log", statedir);
	if (access(statedir, W_OK)) return;	/* not mounted yet */
	struct stat st;
	FILE *f = fopen(fn, stat(fn, &st) == 0 && st.st_size > 16384 ? "w" : "a");
	if (!f) return;
	fputs(pend, f); fclose(f);
	pend[0] = 0;
}

/* HOME down right now, from the key state (EVIOCGKEY), not from events: after a USB connection in the Sony UI the
 * events stopped reaching us (2026-10-14), while the state can always be read. This driver reports a press as
 * value 0, so which bit value means "down" is not assumed: the other keys, which nobody holds all at once, show the
 * value for "up". 1 down, 0 up, -1 unknown */
static int home_state(int fd)
{
	unsigned char k[64];
	memset(k, 0, sizeof k);
	if (ioctl(fd, 0x80404518, k) < 0) return -1;	/* EVIOCGKEY(64) */
	static const int others[] = { K_PLAY, K_RIGHT, K_LEFT, K_UP, K_DOWN, K_VOLDN, K_VOLUP, K_OPTION, K_BT };
	int ones = 0, n = sizeof others / sizeof *others;
	for (int i = 0; i < n; i++) ones += (k[others[i] >> 3] >> (others[i] & 7)) & 1;
	if (ones != 0 && ones != n) return -1;	/* keys disagree: someone holds some of them, no verdict */
	int up = ones ? 1 : 0, home = (k[K_BACK >> 3] >> (K_BACK & 7)) & 1;
	return home != up;
}

int main(int argc, char **argv)
{
	const char *statedir = argc > 1 ? argv[1] : "/var/hb";
	int fd = open("/dev/input/event0", O_RDONLY);
	if (fd < 0) { lg(statedir, "no input device"); return 3; }
	lg(statedir, "watching (pid %d), key state %d", (int)getpid(), home_state(fd));
	int down = 0, polled = 0;	/* polled: the press was seen in the key state only (no event) */
	int seen_up = 0;	/* the key state has shown HOME up once: only then is a "down" in it believed (a wrong reading, or
				 * HOME still held from leaving the player, must never reboot the Walkman) */
	double t = 0;
	for (;;) {
		struct ev e;
		fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
		struct timeval tv = { 0, down ? 300000 : 0 };
		if (!down) tv.tv_sec = 1;	/* idle: one wake-up a second, for the key state */
		int r = select(fd + 1, &rs, 0, 0, &tv);
		if (r > 0) {
			ssize_t n = read(fd, &e, sizeof e);
			if (n != sizeof e) {	/* the device went away under us: open it again */
				lg(statedir, "input read %d (errno %d): reopening", (int)n, errno);
				close(fd); sleep(1);
				fd = open("/dev/input/event0", O_RDONLY);
				if (fd < 0) { lg(statedir, "input reopen failed"); sleep(5); fd = open("/dev/input/event0", O_RDONLY); if (fd < 0) return 3; }
				down = 0; continue;
			}
			/* with HOLD on every key comes with 0x80 added: HOME held in a pocket must not reboot into the player,
			 * so only the plain code counts; any other key or a HOLD move cancels */
			if (e.type == 1) {
				if (e.code == K_BACK) { down = KEY_PRESSED(e.value); polled = 0; t = now(); if (e.value != 2) lg(statedir, "HOME %s", down ? "down" : "up"); }
				else { if (down || (e.code & 0x7f) == K_BACK) lg(statedir, "key %d value %d (cancels)", e.code, e.value); down = 0; }
			}
		} else if (r == 0) {	/* no event: the key state decides (HOME held while events don't come) */
			lg_flush(statedir);
			int h = home_state(fd);
			if (h == 0) seen_up = 1;
			if (h == 1 && !down && seen_up) { down = 1; polled = 1; t = now(); lg(statedir, "HOME down (key state, no event)"); }
			else if (h == 0 && down && polled) { down = 0; lg(statedir, "HOME up (key state)"); }
		}
		if (down && now() - t >= 3) {
			int hold = 0;	/* HOLD switch state (EVIOCGLED, 1 = on), as the player reads it */
			if (ioctl(fd, 0x80044519, &hold) >= 0 && (hold & 1)) { down = 0; lg(statedir, "HOME held 3 s, but HOLD is on"); }
			else { lg(statedir, "HOME held 3 s: to the player"); break; }
		}
	}
	/* flag first (the Sony supervisor may reboot as soon as its UI dies), then stop the Sony UI, then the splash */
	char fn[512];
	int en = lang_en(statedir);	/* before the flag: /var may be remounted below */
	snprintf(fn, sizeof fn, "%s/boot_player", statedir);
	FILE *f = fopen(fn, "w");
	if (!f) { system("mount -t ext2 /dev/mtdblock/3 /var"); mkdir(statedir, 0755); f = fopen(fn, "w"); }
	if (f) { fputs("1\n", f); fclose(f); }
	sync();
	system("killall gemini > /dev/null 2>&1");
	usleep(800000);
	system("killall -9 gemini > /dev/null 2>&1");
	if (!gfx_open()) {
		gfx_load_res(RESDIR);
		struct img s = gfx_tex(17, 19);	/* the WALKMAN splash */
		if (s.w) gfx_blit(s, 0, 0); else gfx_fill(0, 0, W, H, 0);
		gfx_fill(0, 252, W, 40, 0);
		gfx_text_center(W / 2, 262, en ? "Opening..." : "Abriendo...", 0xFFFF, 1);
		gfx_flip();
	}
	sync();
	system("mount -o remount,ro /contents > /dev/null 2>&1");
	system("umount /var > /dev/null 2>&1");
	system("reboot");
	sleep(5);
	reboot(RB_AUTOBOOT);
	return 0;
}
