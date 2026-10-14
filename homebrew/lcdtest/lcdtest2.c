/* lcdtest2: the LCD refresh test with questions on screen (PLAY = yes, BACK = no), answers logged.
 *   1 normal counter -> "is the number going up?"
 *   2 QFB_SET_LCD_DISABLE 1 for 6 s (counter keeps being drawn), then 0 -> "did it freeze?"
 *   3 counter again -> "is it moving again?"; if not, extra ways to wake it: full redraw, backlight off/on,
 *     QFB_UPDATE_SCRN, each followed by the same question
 * Log: /contents/HB/out/lcd/log2.txt (with lcd interrupts per second per step) */
#include "../common/gfx.h"
#include "../common/keys.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define RESDIR "/usr/local/share/gemini/res"
#define QFB_UPDATE_SCRN		0x40044E01
#define QFB_SET_LCD_DISABLE	0x40044E35

struct ev { long sec, usec; unsigned short type, code; int value; };
static FILE *lf;
static int keyfd;
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

static void draw(const char *top, const char *l1, const char *l2, int count)
{
	gfx_fill(0, 0, W, H, RGB(0, 40, 120));
	gfx_text_center(W / 2, 20, top, 0xFFFF, 2);
	if (l1) gfx_text_center(W / 2, 80, l1, 0xFFFF, 1);
	if (l2) gfx_text_center(W / 2, 104, l2, 0xFFFF, 1);
	if (count >= 0) { char b[16]; snprintf(b, sizeof b, "%d", count); gfx_text_center(W / 2, 150, b, RGB(255, 200, 0), 4); }
}

/* counter for secs (drawn ~5 times a second), with a title; returns lcd irq/s */
static double counter(const char *top, const char *l1, const char *l2, double secs)
{
	unsigned long a = lcd_irqs();
	double t0 = now();
	int c = 0;
	while (now() - t0 < secs) { draw(top, l1, l2, c++); gfx_flip(); usleep(200000); }
	return (lcd_irqs() - a) / (now() - t0);
}

/* the question with the counter still running under it; 1 = PLAY (yes), 0 = BACK (no), -1 = no answer in 40 s */
static int ask(const char *top, const char *q)
{
	struct ev e;
	while (1) { fd_set rs; FD_ZERO(&rs); FD_SET(keyfd, &rs); struct timeval tv = { 0, 0 }; if (select(keyfd + 1, &rs, 0, 0, &tv) <= 0) break; read(keyfd, &e, sizeof e); }
	double t0 = now();
	int c = 0;
	while (now() - t0 < 40) {
		draw(top, q, "PLAY = si     BACK = no", c++);
		gfx_flip();
		fd_set rs; FD_ZERO(&rs); FD_SET(keyfd, &rs);
		struct timeval tv = { 0, 200000 };
		if (select(keyfd + 1, &rs, 0, 0, &tv) > 0 && read(keyfd, &e, sizeof e) == sizeof e && e.type == 1 && KEY_PRESSED(e.value)) {
			if ((e.code & 0x7f) == K_PLAY) return 1;
			if ((e.code & 0x7f) == K_BACK) return 0;
		}
	}
	return -1;
}

static void note(const char *step, int ans, double irq)
{
	char b[160];
	snprintf(b, sizeof b, "%-44s answer %s   lcd %.1f irq/s", step, ans == 1 ? "YES" : ans == 0 ? "NO" : "(none)", irq);
	lg(b);
}

int main(void)
{
	lf = fopen("/contents/HB/out/lcd/log2.txt", "a");
	lg("---- lcdtest2");
	if (gfx_open()) { lg("fb open failed"); return 1; }
	gfx_load_res(RESDIR);
	gfx_backlight(80);
	keyfd = open("/dev/input/event0", O_RDONLY);
	if (keyfd < 0) { lg("keys failed"); return 1; }

	double r = counter("PASO 1", "Mira el numero amarillo", 0, 4);
	note("1 normal: is the number going up?", ask("PASO 1", "El numero amarillo sube?"), r);

	counter("PASO 2", "Ahora deberia quedar QUIETO", "durante 6 segundos", 2);
	int one = 1, zero = 0;
	int rc = ioctl(gfx_fd, QFB_SET_LCD_DISABLE, &one);
	char b[120]; snprintf(b, sizeof b, "SET_LCD_DISABLE 1 -> %d (errno %d)", rc, rc ? errno : 0); lg(b);
	r = counter("PASO 2", "Ahora deberia quedar QUIETO", "durante 6 segundos", 6);
	rc = ioctl(gfx_fd, QFB_SET_LCD_DISABLE, &zero);
	snprintf(b, sizeof b, "SET_LCD_DISABLE 0 -> %d (errno %d), lcd while disabled %.1f irq/s", rc, rc ? errno : 0, r); lg(b);
	r = counter("PASO 3", "Deberia moverse otra vez", 0, 3);
	int a = ask("PASO 3", "El numero se mueve otra vez?");
	note("3 after LCD_DISABLE 0: moving again?", a, r);

	if (a != 1) {	/* try harder to wake the panel, asking after each */
		gfx_invalidate(); gfx_flip();
		note("3b full redraw", ask("PASO 3b", "Y ahora, se mueve?"), 0);
		gfx_backlight(0); usleep(300000); gfx_backlight(80);
		note("3c backlight off/on", ask("PASO 3c", "Y ahora, se mueve?"), 0);
		int m[6] = { 1, 0, 255, 0, 0, 1 };	/* scrn_modes: tc off, color, alpha, page 0, rot 0, update 1 */
		rc = ioctl(gfx_fd, QFB_UPDATE_SCRN, m);
		snprintf(b, sizeof b, "UPDATE_SCRN -> %d (errno %d)", rc, rc ? errno : 0); lg(b);
		note("3d QFB_UPDATE_SCRN", ask("PASO 3d", "Y ahora, se mueve?"), 0);
	}
	draw("LISTO", "Gracias. Sigue el arranque", "normal del Walkman.", -1); gfx_flip();
	sleep(2);
	lg("end");
	return 0;
}
