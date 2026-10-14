/* hello: first homebrew binary for the NW-A829 (static musl). Prints info, draws on /dev/fb/0. */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <linux/fb.h>

int main(void)
{
	struct utsname u;
	if (uname(&u) == 0) printf("hello from musl: %s %s %s\n", u.sysname, u.release, u.machine);
	int fd = open("/dev/fb/0", O_RDWR);
	if (fd < 0) { perror("open fb"); return 1; }
	struct fb_var_screeninfo v;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0) printf("fb %ux%u %ubpp\n", v.xres, v.yres, v.bits_per_pixel);
	size_t len = 240 * 320 * 2;
	uint16_t *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 2; }
	for (int f = 0; f < 200; f++) {
		for (int y = 0; y < 320; y++)
			for (int x = 0; x < 240; x++) {
				int r = (x + f * 4) & 31, g = (y + f * 2) & 63, b = ((x ^ y) >> 3) & 31;
				p[y * 240 + x] = (r << 11) | (g << 5) | b;
			}
		usleep(50000);
	}
	munmap(p, len);
	close(fd);
	printf("done\n");
	return 0;
}
