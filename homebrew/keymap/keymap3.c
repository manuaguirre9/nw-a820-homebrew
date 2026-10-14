/* keymap: guided key mapping - asks for each button by name, records the event code(s), writes a table. */
#include "../common/gfx.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>

struct ev { long sec, usec; unsigned short type, code; int value; };
static const char *names[] = { "VOLUMEN +", "VOLUMEN -", "ARRIBA" };
static const char *unused[] = {
	"CENTRO (play)", "OPTION", "BACK / HOME", "BLUETOOTH", "HOLD (mover)", "HOLD (volver)" };
#define N (int)(sizeof names / sizeof names[0])

static void screen(int i, const char *msg, int left)
{
	char b[40];
	gfx_fill(0, 0, W, H, RGB(10, 10, 40));
	gfx_text(8, 8, "MAPA DE BOTONES", RGB(255, 200, 0), 2);
	snprintf(b, sizeof b, "paso %d de %d", i + 1, N);
	gfx_text(8, 50, b, RGB(200, 200, 200), 1);
	gfx_text(8, 90, "Apretá:", RGB(255, 255, 255), 2);
	gfx_text(8, 130, names[i], RGB(120, 255, 120), 2);
	snprintf(b, sizeof b, "(salta en %2d s)", left);
	gfx_text(8, 180, b, RGB(160, 160, 160), 1);
	gfx_text(8, 220, msg, RGB(255, 255, 0), 1);
}

int main(int argc, char **argv)
{
	FILE *log = fopen(argc > 1 ? argv[1] : "/tmp/keymap.txt", "w");
	if (gfx_open()) return 1;
	int fd = open("/dev/input/event0", O_RDONLY);
	if (fd < 0) return 2;
	char msg[40] = "";
	for (int i = 0; i < N; i++) {
		time_t end = time(0) + 30;
		int got = 0;
		/* drain pending events */
		for (;;) { fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs); struct timeval tv = {0, 0}; struct ev e;
			if (select(fd + 1, &rs, 0, 0, &tv) <= 0 || read(fd, &e, sizeof e) != sizeof e) break; }
		fprintf(log, "%s:", names[i]);
		while (time(0) < end && got < 4) {
			screen(i, msg, (int)(end - time(0)));
			fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
			struct timeval tv = {0, 300000};
			if (select(fd + 1, &rs, 0, 0, &tv) > 0) {
				struct ev e;
				if (read(fd, &e, sizeof e) == sizeof e) {
					fprintf(log, " [type=%u code=%u value=%d]", e.type, e.code, e.value); fflush(log);
					got++;
					snprintf(msg, sizeof msg, "OK: código %u", e.code);
					if (got >= 2) { end = time(0) + 1; }
				}
			}
		}
		fprintf(log, "%s\n", got ? "" : " (nada)"); fflush(log);
		if (!got) snprintf(msg, sizeof msg, "(salteado)");
	}
	gfx_fill(0, 0, W, H, 0);
	gfx_text(8, 150, "¡Listo! arranca Sony", RGB(255, 255, 255), 1);
	fclose(log);
	return 0;
}
