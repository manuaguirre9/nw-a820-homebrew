/* keytest: shows every key event from /dev/input/event0 (and the HOLD switch) on screen for 60 s, logs to argv[1]. */
#include "../common/gfx.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>

struct ev { long sec, usec; unsigned short type, code; int value; };

int main(int argc, char **argv)
{
	FILE *log = fopen(argc > 1 ? argv[1] : "/tmp/keys.txt", "w");
	if (gfx_open()) { fprintf(log, "fb open failed\n"); return 1; }
	int fd = open("/dev/input/event0", O_RDONLY);
	if (fd < 0) { fprintf(log, "event0 open failed\n"); return 2; }
	char lines[10][40] = {{0}};
	int n = 0, hold = -1;
	time_t end = time(0) + 60;
	for (;;) {
		int h = 0;
		if (ioctl(fd, 0x80044519, &h) == 0 && h != hold) {
			hold = h; fprintf(log, "HOLD=%d\n", h); fflush(log);
		}
		int left = (int)(end - time(0));
		gfx_fill(0, 0, W, H, RGB(10, 10, 40));
		gfx_text(8, 6, "PRUEBA DE BOTONES", RGB(255, 200, 0), 2);
		gfx_text(8, 42, "Apretá cada botón", RGB(255, 255, 255), 1);
		gfx_text(8, 58, "(también HOLD)", RGB(255, 255, 255), 1);
		char buf[40];
		snprintf(buf, sizeof buf, "quedan %2d s   HOLD=%d", left, hold);
		gfx_text(8, 80, buf, RGB(150, 255, 150), 1);
		for (int i = 0; i < 10; i++)
			gfx_text(8, 110 + i * 20, lines[(n + i) % 10], i == 9 ? RGB(255, 255, 0) : RGB(180, 180, 180), 1);
		if (left <= 0) break;
		fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
		struct timeval tv = {0, 250000};
		if (select(fd + 1, &rs, 0, 0, &tv) > 0) {
			struct ev e;
			if (read(fd, &e, sizeof e) == sizeof e) {
				fprintf(log, "t=%ld.%06ld type=%u code=%u value=%d\n", e.sec, e.usec, e.type, e.code, e.value); fflush(log);
				if (e.type == 1) {
					snprintf(lines[n % 10], 40, "tecla %2u  %s", e.code, e.value == 1 ? "APRETADA" : e.value == 0 ? "suelta" : "repite");
					n++;
				}
			}
		}
	}
	gfx_fill(0, 0, W, H, 0);
	gfx_text(8, 150, "fin - arranca Sony", RGB(255, 255, 255), 1);
	fclose(log);
	return 0;
}
