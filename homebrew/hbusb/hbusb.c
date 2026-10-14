/* hbusb: USB mass storage for the player, the job Sony's apl_conn (/usr/local/bin/apl_conn -a) does for the Sony
 * UI. The player exits with 3 when a computer has enumerated the Walkman as a disk; the boot script (running from
 * /tmp, cwd /) then runs this copy in RAM, which hands the FAT partition to the computer and takes it back when the
 * cable comes out. Protocol of Sony's usbconn driver (from apl_conn):
 *   select() on /dev/usbconn wakes on a change; ioctl 0x80046701 (int *) reads the status:
 *   0 disconnected, 1 MSC connected, 2 firmware update requested, 3 MTP connected, 4 connection starting.
 *   MSC: umount /contents, then ioctl 0x40046702 (value 0) lets the computer at the disk ("fsg_allow_acc").
 *   Firmware update (the updater or fwtrig): 0x70555766 written to /dev/icx1087_nvp/9, sync, reboot.
 *   hbusb STATEDIR      exit 0 = done (back to the player), 2 = /contents busy (touches /tmp/hb_usb_off) */
#include "../common/gfx.h"
#include "../common/keys.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RESDIR "/usr/local/share/gemini/res"
#define USB_GET_STS 0x80046701
#define USB_ALLOW 0x40046702

static const char *statedir = "/var/hb";
static int en;

static void lg(const char *fmt, ...)
{
	char fn[512]; snprintf(fn, sizeof fn, "%s/usb.log", statedir);
	FILE *f = fopen(fn, "a");
	if (!f) return;
	va_list a; va_start(a, fmt);
	fprintf(f, "[%ld] ", (long)time(0)); vfprintf(f, fmt, a); fputc('\n', f);
	va_end(a);
	fclose(f);
}

/* the player's language setting ("lang 1" in STATEDIR/pos.txt = English) */
static int lang_en(void)
{
	char fn[512], line[256]; int r = 0;
	snprintf(fn, sizeof fn, "%s/pos.txt", statedir);
	FILE *f = fopen(fn, "r");
	if (!f) return 0;
	while (fgets(line, sizeof line, f)) if (!strncmp(line, "lang ", 5)) r = atoi(line + 5) == 1;
	fclose(f);
	return r;
}

/* the WALKMAN splash (as hbwatch) with a title and up to two grey lines ("a\nb") in the band below it */
static void screen(const char *t_es, const char *t_en, const char *s_es, const char *s_en)
{
	static int ok = -1;
	if (ok < 0) { ok = !gfx_open(); if (ok) gfx_load_res(RESDIR); }
	if (!ok) return;
	struct img s = gfx_tex(17, 19);
	if (s.w) gfx_blit(s, 0, 0); else gfx_fill(0, 0, W, H, 0);
	gfx_fill(0, 240, W, 80, 0);
	gfx_text_center(W / 2, 246, en ? t_en : t_es, 0xFFFF, 1);
	const char *sub = en ? s_en : s_es;
	for (int y = 274; sub && *sub && y < 320; y += LINE_H) {
		char l[96]; size_t n = strcspn(sub, "\n"); if (n >= sizeof l) n = sizeof l - 1;
		memcpy(l, sub, n); l[n] = 0;
		gfx_text_center(W / 2, y, l, RGB(150, 150, 150), 1);
		sub += n; if (*sub) sub++;
	}
	gfx_flip();
}

static int status(int fd) { int s = -1; return ioctl(fd, USB_GET_STS, &s) < 0 ? -1 : s; }

/* who keeps /contents busy: every process whose cwd, executable, open files or mappings are on it, to the log */
static void log_holders(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	if (!d) return;
	while ((e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		char p[300], t[300], cmd[80] = "";
		snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) { size_t n = fread(cmd, 1, sizeof cmd - 1, f); cmd[n] = 0; fclose(f); }
		static const char *const links[] = { "cwd", "exe", "root" };
		for (int i = 0; i < 3; i++) {
			snprintf(p, sizeof p, "/proc/%s/%s", e->d_name, links[i]);
			int n = readlink(p, t, sizeof t - 1);
			if (n > 0) { t[n] = 0; if (!strncmp(t, "/contents", 9)) lg("busy: pid %s (%s) %s %s", e->d_name, cmd, links[i], t); }
		}
		snprintf(p, sizeof p, "/proc/%s/fd", e->d_name);
		DIR *fd = opendir(p);
		struct dirent *g;
		while (fd && (g = readdir(fd))) {
			char q[400];
			snprintf(q, sizeof q, "%s/%s", p, g->d_name);
			int n = readlink(q, t, sizeof t - 1);
			if (n > 0) { t[n] = 0; if (!strncmp(t, "/contents", 9)) lg("busy: pid %s (%s) fd %s %s", e->d_name, cmd, g->d_name, t); }
		}
		if (fd) closedir(fd);
		snprintf(p, sizeof p, "/proc/%s/maps", e->d_name);
		f = fopen(p, "r");
		char line[400];
		while (f && fgets(line, sizeof line, f)) if (strstr(line, " /contents")) { line[strcspn(line, "\n")] = 0; lg("busy: pid %s (%s) map %s", e->d_name, cmd, line); break; }
		if (f) fclose(f);
	}
	closedir(d);
}

/* the mount options /contents has now (the hook's mount), to mount it back the same way */
static void contents_opts(char *o, size_t max)
{
	char dev[128], dir[128], type[32], opts[256];
	snprintf(o, max, "%s", "");
	FILE *f = fopen("/proc/mounts", "r");
	while (f && fscanf(f, "%127s %127s %31s %255s %*d %*d", dev, dir, type, opts) == 4)
		if (!strcmp(dir, "/contents")) { snprintf(o, max, "%s", opts); break; }
	if (f) fclose(f);
}

static int contents_mounted(void) { char o[256]; contents_opts(o, sizeof o); return o[0] != 0; }

static void mount_back(const char *opts)
{
	char cmd[512];
	if (contents_mounted()) return;
	snprintf(cmd, sizeof cmd, "mount -t vfat %s%s /dev/mtdblock/2 /contents > /dev/null 2>&1", opts[0] ? "-o " : "", opts);
	if (system(cmd) != 0 && !contents_mounted()) {
		lg("mount with \"%s\" failed, plain mount", opts);
		system("mount -t vfat /dev/mtdblock/2 /contents > /dev/null 2>&1");
	}
	lg("contents %s", contents_mounted() ? "mounted again" : "NOT mounted");
}

static void firmware_update(void)
{
	screen("Actualizando firmware...", "Updating firmware...", "No lo desconectes", "Keep it connected");
	lg("firmware update requested: flag, reboot");
	int f = open("/dev/icx1087_nvp/9", O_RDWR);
	unsigned v = 0x70555766;
	if (f < 0 || write(f, &v, 4) != 4) lg("update flag: write failed (%d)", errno);
	if (f >= 0) close(f);
	sync();
	sleep(2);
	reboot(RB_AUTOBOOT);
	sleep(10);
}

int main(int argc, char **argv)
{
	if (argc > 1) statedir = argv[1];
	en = lang_en();
	int fd = open("/dev/usbconn", O_RDWR);
	if (fd < 0) { lg("no /dev/usbconn"); return 1; }
	int st = status(fd);
	lg("start, status %d", st);
	if (st == 2) firmware_update();
	if (st != 1) { close(fd); return 0; }
	gfx_lcd(1); gfx_backlight(80);
	screen("Conectando por USB...", "Connecting over USB...", 0, 0);
	char opts[256]; contents_opts(opts, sizeof opts);
	sync();
	int ok = 0;
	for (int i = 0; i < 20 && !ok; i++) { if (umount("/contents") == 0 || !contents_mounted()) ok = 1; else usleep(250000); }
	if (!ok) {
		lg("umount /contents failed (%d)", errno);
		log_holders();
		close(open("/tmp/hb_usb_off", O_WRONLY | O_CREAT, 0644));	/* the player ignores the cable until it is unplugged */
		screen("USB no disponible", "USB unavailable", "Desenchufá el cable", "Unplug the cable");
		sleep(3);
		return 2;
	}
	sleep(1);	/* apl_conn -s 1 */
	if (ioctl(fd, USB_ALLOW, 0) < 0) lg("allow ioctl failed (%d)", errno);
	lg("contents handed to the computer");
	screen("Conectado por USB", "Connected over USB", "Expulsalo en la PC\nantes de desenchufar", "Eject it on the PC\nbefore unplugging");
	int keys = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	time_t lit = time(0); int light = 1;
	for (;;) {
		fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
		if (keys >= 0) FD_SET(keys, &rs);
		struct timeval tv = { 2, 0 };
		select((fd > keys ? fd : keys) + 1, &rs, 0, 0, &tv);
		if (keys >= 0 && FD_ISSET(keys, &rs)) {	/* any key lights the screen for 30 s more */
			char buf[64]; while (read(keys, buf, sizeof buf) > 0) ;
			lit = time(0); if (!light) { gfx_backlight(80); light = 1; }
		}
		if (light && time(0) - lit > 30) { gfx_backlight(0); light = 0; }
		st = status(fd);
		if (st == 2) firmware_update();
		if (st == 0 || st < 0) break;
	}
	lg("disconnected (status %d)", st);
	if (!light) gfx_backlight(80);
	screen("Volviendo...", "Returning...", 0, 0);
	sync();
	mount_back(opts);
	if (keys >= 0) close(keys);
	close(fd);
	return 0;
}
