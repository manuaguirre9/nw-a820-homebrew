/* doomgeneric_walkman.c: Doom on the Sony NW-A829 (homebrew). Renders 320x200 rotated 90 degrees clockwise onto the
 * 240x320 screen (hold the Walkman sideways, buttons under the right thumb). Keys: d-pad = move/turn,
 * PLAY = fire, OPTION = use, BT button = run, VOL-/VOL+ = strafe, BACK tap = menu (ESC), BACK held 2 s = quit.
 * In menus PLAY = Enter, BACK = Escape, d-pad = navigate. No sound in this version. */
#include "doomgeneric.h"
#include "doomkeys.h"
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
static int rot_col[SW];	/* screen x -> doom row */
static struct { int pressed; unsigned char key; } kq[32];
static int kq_head, kq_tail;
static struct timeval back_down;
static int back_held, quit_now;

static void push(int pressed, unsigned char key)
{
	int n = (kq_head + 1) & 31;
	if (n == kq_tail) return;
	kq[kq_head].pressed = pressed; kq[kq_head].key = key; kq_head = n;
}

void DG_Init()
{
	int fd = open("/dev/fb/0", O_RDWR);
	if (fd < 0) { fprintf(stderr, "no fb\n"); exit(1); }
	screen = mmap(0, SW * SH * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (screen == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); exit(1); }
	memset(screen, 0, SW * SH * 2);
	for (int x = 0; x < SW; x++) rot_col[x] = 199 - x * 200 / SW;
	keyfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
}

void DG_DrawFrame()
{
	/* DG_ScreenBuffer is 320x200 xRGB8888; screen (x, y) shows doom (col = y, row = rot_col[x]) */
	for (int y = 0; y < SH; y++) {
		unsigned short *d = screen + y * SW;
		const uint32_t *col = DG_ScreenBuffer + y;	/* column y, stride 320 per row */
		for (int x = 0; x < SW; x++) {
			uint32_t p = col[rot_col[x] * DOOMGENERIC_RESX];
			d[x] = (unsigned short)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
		}
	}
}

void DG_SleepMs(uint32_t ms) { usleep(ms * 1000); }

uint32_t DG_GetTicksMs()
{
	struct timeval tv; gettimeofday(&tv, 0);
	return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

struct kev { long sec, usec; unsigned short type, code; int value; };

static void poll_keys(void)
{
	struct kev k;
	if (keyfd < 0) return;
	while (read(keyfd, &k, sizeof k) == sizeof k) {
		if (k.type != 1) continue;
		int pressed = k.value == 0;	/* Sony driver: 0 = pressed, 1 = released */
		int code = k.code & 0x7f;	/* +0x80 while HOLD is on: ignore the lock in-game */
		if (code == 15) continue;	/* HOLD switch */
		switch (code) {
		case 3: push(pressed, KEY_UPARROW); break;
		case 4: push(pressed, KEY_DOWNARROW); break;
		case 2: push(pressed, KEY_LEFTARROW); break;
		case 1: push(pressed, KEY_RIGHTARROW); break;
		case 0: push(pressed, KEY_FIRE); push(pressed, KEY_ENTER); break;
		case 19: push(pressed, KEY_USE); break;
		case 20: push(pressed, KEY_RSHIFT); break;
		case 16: push(pressed, KEY_STRAFE_L); break;
		case 17: push(pressed, KEY_STRAFE_R); break;
		case 18:	/* BACK: tap = Escape, hold 2 s = quit */
			if (pressed) { back_held = 1; gettimeofday(&back_down, 0); }
			else if (back_held) { back_held = 0; push(1, KEY_ESCAPE); push(0, KEY_ESCAPE); }
			break;
		}
	}
	if (back_held) {
		struct timeval t; gettimeofday(&t, 0);
		if ((t.tv_sec - back_down.tv_sec) * 1000 + (t.tv_usec - back_down.tv_usec) / 1000 > 2000) { back_held = 0; quit_now = 1; }
	}
	if (quit_now) { fprintf(stderr, "quit\n"); exit(0); }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	if (kq_head == kq_tail) poll_keys();
	if (kq_head == kq_tail) return 0;
	*pressed = kq[kq_tail].pressed; *key = kq[kq_tail].key;
	kq_tail = (kq_tail + 1) & 31;
	return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv)
{
	doomgeneric_Create(argc, argv);
	for (;;) doomgeneric_Tick();
	return 0;
}
