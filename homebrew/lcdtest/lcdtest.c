/* lcdtest: can we stop the LCD controller's automatic refresh while the screen is dark, like the Sony UI does?
 * Phases of 8 s, each with a big phase number and a running counter on screen; the user reports what the panel did.
 *   1 normal (baseline)
 *   2 QFB_SET_LCD_DISABLE 1          then 0
 *   3 QFB_UPDATE_TIMER {1 = auto update off}   then {0 = on} with the timeout read before (if readable)
 *   4 normal again (did everything come back?)
 * For each phase: lcd interrupts per second (/proc/interrupts), ioctl results. Log: /contents/HB/out/lcd/log.txt */
#include "../common/gfx.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define RESDIR "/usr/local/share/gemini/res"
#define QFB_UPDATE_TIMER	0x80044E04	/* _IOR('N', 4, 4): the header wraps the size in sizeof() -> 4 */
#define QFB_SET_LCD_DISABLE	0x40044E35	/* _IOW('N', 53, 4) */
#define QFB_GET_LCD_DISABLE	0x80044E36	/* _IOR('N', 54, 4) */
struct update_timer { int timerflag, timeout; };

static FILE *lf;
static void lg(const char *s) { if (lf) { fputs(s, lf); fputc('\n', lf); fflush(lf); } }

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static unsigned long lcd_irqs(void)
{
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) return 0;
	char l[160]; unsigned long v = 0;
	while (fgets(l, sizeof l, f)) if (strstr(l, " lcd")) { int n; sscanf(l, " %d: %lu", &n, &v); }
	fclose(f);
	return v;
}

static void screen(int phase, const char *what, int count)
{
	gfx_fill(0, 0, W, H, phase & 1 ? RGB(0, 40, 120) : RGB(120, 40, 0));
	char b[64];
	snprintf(b, sizeof b, "FASE %d", phase);
	gfx_text_center(W / 2, 40, b, 0xFFFF, 3);
	gfx_text_center(W / 2, 120, what, 0xFFFF, 1);
	snprintf(b, sizeof b, "%d", count);
	gfx_text_center(W / 2, 170, b, RGB(255, 200, 0), 4);
	gfx_text_center(W / 2, 270, "mirá si el número avanza", 0xFFFF, 1);
	gfx_flip();
}

/* runs one phase: counter every 0.25 s for secs; returns lcd interrupts per second */
static double run_phase(int phase, const char *what, double secs)
{
	unsigned long a = lcd_irqs();
	double t0 = now();
	int c = 0;
	while (now() - t0 < secs) { screen(phase, what, c++); usleep(250000); }
	double r = (lcd_irqs() - a) / (now() - t0);
	char b[160]; snprintf(b, sizeof b, "phase %d (%s): lcd %.1f irq/s", phase, what, r); lg(b);
	return r;
}

int main(void)
{
	lf = fopen("/contents/HB/out/lcd/log.txt", "a");
	if (gfx_open()) { lg("fb open failed"); return 1; }
	gfx_load_res(RESDIR);
	gfx_backlight(80);
	char b[200];
	int v = -1, r = ioctl(gfx_fd, QFB_GET_LCD_DISABLE, &v);
	snprintf(b, sizeof b, "GET_LCD_DISABLE r=%d errno=%d value=%d", r, r ? errno : 0, v); lg(b);
	struct update_timer ut = { -1, -1 };
	r = ioctl(gfx_fd, QFB_UPDATE_TIMER, &ut);
	snprintf(b, sizeof b, "UPDATE_TIMER (read?) r=%d errno=%d -> timerflag=%d timeout=%d", r, r ? errno : 0, ut.timerflag, ut.timeout); lg(b);
	struct update_timer orig = ut;

	run_phase(1, "normal", 8);

	int one = 1, zero = 0;
	r = ioctl(gfx_fd, QFB_SET_LCD_DISABLE, &one);
	snprintf(b, sizeof b, "SET_LCD_DISABLE 1 r=%d errno=%d", r, r ? errno : 0); lg(b);
	run_phase(2, "LCD_DISABLE = 1", 8);
	r = ioctl(gfx_fd, QFB_SET_LCD_DISABLE, &zero);
	snprintf(b, sizeof b, "SET_LCD_DISABLE 0 r=%d errno=%d", r, r ? errno : 0); lg(b);
	run_phase(2, "LCD_DISABLE = 0 (vuelve?)", 4);

	struct update_timer off = { 1, orig.timeout >= 0 ? orig.timeout : 0 };
	r = ioctl(gfx_fd, QFB_UPDATE_TIMER, &off);
	snprintf(b, sizeof b, "UPDATE_TIMER off {1,%d} r=%d errno=%d", off.timeout, r, r ? errno : 0); lg(b);
	run_phase(3, "auto update OFF", 8);
	struct update_timer on = { 0, orig.timeout >= 0 ? orig.timeout : 0 };
	r = ioctl(gfx_fd, QFB_UPDATE_TIMER, &on);
	snprintf(b, sizeof b, "UPDATE_TIMER on {0,%d} r=%d errno=%d", on.timeout, r, r ? errno : 0); lg(b);
	run_phase(3, "auto update ON (vuelve?)", 4);

	run_phase(4, "normal otra vez", 6);
	lg("end");
	return 0;
}
