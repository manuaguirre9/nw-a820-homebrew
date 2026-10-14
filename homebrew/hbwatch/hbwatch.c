/* hbwatch: the small helper that runs next to the Sony UI. Holding HOME for 3 s writes the "boot into the player"
 * flag, stops the Sony UI, shows the Walkman splash and reboots (the dual-boot script then starts the player).
 * It replaces "hbplayer watch": the 2 MB player (audio, network, TLS, decoders) used to sit in RAM next to the Sony
 * UI, which only has ~2.4 MB free, just to watch one key.
 *   hbwatch STATEDIR */
#include "../common/gfx.h"
#include "../common/keys.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
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

static int next_event(int fd, struct ev *e, int ms)
{
	fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	if (select(fd + 1, &rs, 0, 0, &tv) <= 0) return 0;
	return read(fd, e, sizeof *e) == sizeof *e;
}

int main(int argc, char **argv)
{
	const char *statedir = argc > 1 ? argv[1] : "/var/hb";
	int fd = open("/dev/input/event0", O_RDONLY);
	if (fd < 0) return 3;
	int down = 0;
	double t = 0;
	for (;;) {
		struct ev e;
		if (next_event(fd, &e, down ? 300 : 5000) && e.type == 1) {	/* idle: one wake-up every 5 s at most */
			/* with HOLD on every key comes with 0x80 added: HOME held in a pocket must not reboot into the player,
			 * so only the plain code counts; any other key or a HOLD move cancels */
			if (e.code == K_BACK) { down = KEY_PRESSED(e.value); t = now(); }
			else down = 0;
		}
		if (down && now() - t >= 3) {
			int hold = 0;	/* HOLD switch state (EVIOCGLED, 1 = on), as the player reads it */
			if (ioctl(fd, 0x80044519, &hold) >= 0 && (hold & 1)) down = 0;
			else break;
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
