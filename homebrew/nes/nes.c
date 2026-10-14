/* nes.c: NES on the Sony NW-A829 with the agnes emulator core (mappers 0/1/2/4). 256x240 is shown as 240x225
 * (every 16th column and row dropped) at the top of the portrait screen, one rendered frame in two.
 * Keys: d-pad, PLAY = A, OPTION = B, BACK tap = Start, BACK held 2 s = quit, BT tap = Select. No sound.
 * usage: nes ROM  (HB_SAVEDIR for save states: slot 0 saved on quit and restored on start) */
#include "agnes.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define SW 240
#define SH 320
static unsigned short *screen;
static int keyfd = -1;
static int xmap[SW], ymap[225];
static struct timeval back_down;
static int back_held, quit_now;
static agnes_input_t in1;
static char state_path[512];

struct kev { long sec, usec; unsigned short type, code; int value; };

static double now_ms(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }

static void poll_keys(void)
{
	struct kev k;
	while (read(keyfd, &k, sizeof k) == sizeof k) {
		if (k.type != 1) continue;
		int pressed = k.value == 0, code = k.code & 0x7f;
		switch (code) {
		case 3: in1.up = pressed; break;
		case 4: in1.down = pressed; break;
		case 2: in1.left = pressed; break;
		case 1: in1.right = pressed; break;
		case 0: in1.a = pressed; break;
		case 19: in1.b = pressed; break;
		case 20: in1.select = pressed; break;
		case 18:
			if (pressed) { back_held = 1; gettimeofday(&back_down, 0); }
			else if (back_held) { back_held = 0; in1.start = 1; }	/* released on the next frame */
			break;
		}
	}
	if (back_held) {
		struct timeval t; gettimeofday(&t, 0);
		if ((t.tv_sec - back_down.tv_sec) * 1000 + (t.tv_usec - back_down.tv_usec) / 1000 > 2000) { back_held = 0; quit_now = 1; }
	}
}

static void draw(agnes_t *nes)
{
	for (int y = 0; y < 225; y++) {
		unsigned short *d = screen + (20 + y) * SW;
		int sy = ymap[y];
		for (int x = 0; x < SW; x++) {
			agnes_color_t c = agnes_get_screen_pixel(nes, xmap[x], sy);
			d[x] = (unsigned short)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
		}
	}
}

static void save_state(agnes_t *nes)
{
	if (!state_path[0]) return;
	agnes_state_t *st = malloc(agnes_state_size());
	if (!st) return;
	agnes_dump_state(nes, st);
	FILE *f = fopen(state_path, "wb");
	if (f) { fwrite(st, 1, agnes_state_size(), f); fclose(f); }
	free(st);
}

static void load_state(agnes_t *nes)
{
	FILE *f = state_path[0] ? fopen(state_path, "rb") : 0;
	if (!f) return;
	agnes_state_t *st = malloc(agnes_state_size());
	if (st && fread(st, 1, agnes_state_size(), f) == agnes_state_size()) agnes_restore_state(nes, st);
	free(st); fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: nes ROM\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { fprintf(stderr, "cannot open rom\n"); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *rom = malloc(n);
	if (!rom || fread(rom, 1, n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
	fclose(f);
	agnes_t *nes = agnes_make();
	if (!nes || !agnes_load_ines_data(nes, rom, n)) { fprintf(stderr, "unsupported rom (mapper)\n"); return 3; }
	const char *sd = getenv("HB_SAVEDIR");
	if (sd) {
		const char *base = strrchr(argv[1], '/'); base = base ? base + 1 : argv[1];
		snprintf(state_path, sizeof state_path, "%s/%s.nesstate", sd, base);
		load_state(nes);
	}
	int fd = open("/dev/fb/0", O_RDWR);
	if (fd < 0) return 1;
	screen = mmap(0, SW * SH * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memset(screen, 0, SW * SH * 2);
	for (int x = 0; x < SW; x++) xmap[x] = x * 256 / SW;
	for (int y = 0; y < 225; y++) ymap[y] = y * 240 / 225;
	keyfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	double t0 = now_ms(), next = t0;
	int frames = 0, shown = 0;
	while (!quit_now) {
		poll_keys();
		agnes_set_input(nes, &in1, 0);
		if (in1.start) in1.start = 0;	/* Start is a one-frame tap (BACK release) */
		if (!agnes_next_frame(nes)) { fprintf(stderr, "emulation error\n"); break; }
		frames++;
		if (!(frames & 1)) { draw(nes); shown++; }
		next += 1000.0 / 60.0;
		double d = next - now_ms();
		if (d > 0) usleep((useconds_t)(d * 1000));
		else if (d < -200) next = now_ms();	/* too slow: don't try to catch up */
		if (now_ms() - t0 >= 10000) { fprintf(stderr, "fps %.1f\n", frames * 1000.0 / (now_ms() - t0)); fflush(stderr); t0 = now_ms(); frames = 0; }
	}
	save_state(nes);
	agnes_destroy(nes);
	return 0;
}
