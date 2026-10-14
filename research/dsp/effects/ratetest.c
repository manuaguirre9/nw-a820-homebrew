/* ratetest: what the wired output (/dev/sound/dsp: MP201 PCM0 slave <- CS42L51 master, MCLK = SoC REFCLKO)
 * really accepts and plays. See effects/hires_notes.txt.
 *
 * From the GPL source (sound/oss/icx1087-sound.c) and the device kernel (SNDCTL_DSP_SPEED handler at
 * 0xc0155348 in jp_file1_kernel.bin): SPEED only knows 11025 / 22050 / 44100 (anything > 24000 -> 44100),
 * implemented by dividing MCLK (REFCLKO = 11.2896 MHz OSC /1 /2 /4, or a ~259.66 MHz PLL /23 /46 /92);
 * SETFMT accepts AFMT_S16_LE (0x10) and Sony's AFMT_S32_LE (0x800: 32-bit slots, the CS42L51 takes 24 bits).
 * The codec is the I2S master, so its speed mode (reg 03h, /proc/icx1087audio/reg03h, default 0x2f =
 * single speed + MCLKDIV2) decides LRCK. This program:
 *   1. prints SMU clock registers (read-only /dev/mem) and the codec registers from /proc/icx1087audio
 *   2. for each requested rate x format: SPEED/SETFMT/CHANNELS as returned by the driver, then plays a
 *      1 kHz tone (-20 dBFS) for ~3 s and MEASURES the real consumption rate (frames/s) from blocking writes
 *   3. optional (-H): 88.2 kHz experiment - writes reg03h (0x0f = double speed, 0x2e = single speed without
 *      MCLKDIV2) while a 44.1k stream runs, measures the new LRCK, restores 0x2f. The tone is written for
 *      88.2k so it sounds right (1 kHz) only if the rate really doubled.
 * usage: ratetest [-H] [-d secs]      (run with gemini stopped; result on stdout)
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef SONY_AFMT_S32_LE
#define SONY_AFMT_S32_LE 0x00000800	/* Sony's soundcard.h (OSS4 uses 0x1000 for S32_LE) */
#endif

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static void smu_dump(void)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { printf("SMU: /dev/mem %s\n", strerror(errno)); return; }
	volatile uint32_t *s = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, 0xC8010000);
	close(fd);
	if (s == MAP_FAILED) { printf("SMU: mmap failed\n"); return; }
	printf("SMU PLLSEL=%08x PLLSEL2=%08x CLK_DOMAIN_DIV=%08x OSC_CTRL=%08x\n", s[0x40 / 4], s[0x44 / 4], s[0x48 / 4], s[0x4c / 4]);
	printf("SMU LPLL_CTRL1/2=%08x %08x HPLL_CTRL1..4=%08x %08x %08x %08x\n", s[0x50 / 4], s[0x54 / 4],
	       s[0x58 / 4], s[0x5c / 4], s[0x60 / 4], s[0x64 / 4]);
	printf("SMU REFCLKO_SEL=%08x REFCLKO_DIV=%08x PM0_SCLK_SEL=%08x PM0_SCLK_DIV=%08x\n",
	       s[0x12c / 4], s[0x164 / 4], s[0x110 / 4], s[0x150 / 4]);
	munmap((void *)s, 0x1000);
}

static void proc_dump(void)
{
	static const char *regs[] = { "reg02h", "reg03h", "reg04h", "reg08h", "reg09h", "reg16h", "reg17h", "outputdev", "mute", "vol" };
	char path[64], buf[128];
	for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
		snprintf(path, sizeof path, "/proc/icx1087audio/%s", regs[i]);
		int fd = open(path, O_RDONLY);
		if (fd < 0) { printf("  %s: %s\n", path, strerror(errno)); continue; }
		int n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n > 0) { buf[n] = 0; printf("  %s", buf); if (buf[n - 1] != '\n') putchar('\n'); }
	}
}

static int proc_write(const char *reg, const char *val)
{
	char path[64];
	snprintf(path, sizeof path, "/proc/icx1087audio/%s", reg);
	int fd = open(path, O_WRONLY);
	if (fd < 0) { printf("  write %s: %s\n", path, strerror(errno)); return 0; }
	int n = write(fd, val, strlen(val));
	close(fd);
	printf("  %s <- %s (%s)\n", reg, val, n == (int)strlen(val) ? "ok" : strerror(errno));
	return n == (int)strlen(val);
}

/* fill buf with a tone for the given nominal rate; returns bytes */
static double phase;
static int tone(uint8_t *buf, int frames, int rate, int fmt)
{
	int bps = fmt == SONY_AFMT_S32_LE ? 4 : 2;
	for (int i = 0; i < frames; i++) {
		double v = 0.1 * sin(phase);	/* -20 dBFS */
		phase += 2 * M_PI * 1000.0 / rate;
		if (phase > 2 * M_PI) phase -= 2 * M_PI;
		if (bps == 4) { int32_t s = (int32_t)(v * 2147483647.0); memcpy(buf + 8 * i, &s, 4); memcpy(buf + 8 * i + 4, &s, 4); }
		else { int16_t s = (int16_t)(v * 32767); memcpy(buf + 4 * i, &s, 2); memcpy(buf + 4 * i + 2, &s, 2); }
	}
	return frames * 2 * bps;
}

/* write for secs seconds after the buffer is full; returns measured frames/s */
static double play_measure(int fd, int nominal, int fmt, double secs)
{
	static uint8_t buf[4096 * 8];
	int bpf = fmt == SONY_AFMT_S32_LE ? 8 : 4, frames = sizeof buf / bpf / 4;
	/* prefill: keep writing until a write blocks noticeably (buffer full) or 2 s pass */
	double t = now(), tend = t + 2;
	while (now() < tend) {
		int n = tone(buf, frames, nominal, fmt);
		double a = now();
		if (write(fd, buf, n) != n) { printf("  write: %s\n", strerror(errno)); return -1; }
		if (now() - a > 0.5 * frames / (double)nominal) break;	/* blocked -> full */
	}
	long long total = 0;
	double start = now();
	while (now() - start < secs) {
		int n = tone(buf, frames, nominal, fmt);
		if (write(fd, buf, n) != n) { printf("  write: %s\n", strerror(errno)); return -1; }
		total += n / bpf;
	}
	double el = now() - start;
	return total / el;
}

static int open_dsp(void)
{
	int fd = open("/dev/sound/dsp", O_WRONLY);
	if (fd < 0) fd = open("/dev/dsp", O_WRONLY);
	if (fd < 0) printf("open /dev/sound/dsp: %s\n", strerror(errno));
	return fd;
}

static void try_one(int rate, int fmt, double secs)
{
	int fd = open_dsp();
	if (fd < 0) return;
	int caps = 0, f = fmt, ch = 2, sp = rate;
	ioctl(fd, SNDCTL_DSP_GETFMTS, &caps);
	int rf = ioctl(fd, SNDCTL_DSP_SETFMT, &f);
	int rc = ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
	int rs = ioctl(fd, SNDCTL_DSP_SPEED, &sp);
	printf("REQ %6d Hz fmt %#06x -> GETFMTS %#x SETFMT(r=%d) %#x CHANNELS(r=%d) %d SPEED(r=%d) %d\n",
	       rate, fmt, caps, rf, f, rc, ch, rs, sp);
	if (rf == 0 && f == fmt) {
		double m = play_measure(fd, sp, f, secs);
		printf("  MEASURED %.0f frames/s (driver says %d)\n", m, sp);
	} else printf("  format not accepted, not played\n");
	fflush(stdout);
	close(fd);
	usleep(300000);
}

int main(int argc, char **argv)
{
	int hires = 0;
	double secs = 3;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-H")) hires = 1;
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) secs = atof(argv[++i]);
	}
	setvbuf(stdout, 0, _IOLBF, 0);
	printf("== clocks before\n");
	smu_dump();
	proc_dump();
	static const int rates[] = { 44100, 48000, 88200, 96000, 32000, 22050 };
	static const int fmts[] = { AFMT_S16_LE, SONY_AFMT_S32_LE, 0x1000 /* OSS4 S32_LE */, 0x8000 /* OSS4 S24_LE */ };
	for (unsigned fi = 0; fi < sizeof fmts / sizeof fmts[0]; fi++)
		for (unsigned ri = 0; ri < sizeof rates / sizeof rates[0]; ri++) {
			if (fi >= 2 && ri > 0) break;	/* unknown formats: one probe is enough */
			try_one(rates[ri], fmts[fi], ri < 2 ? secs : 1.5);
		}
	printf("== clocks after SPEED tests\n");
	smu_dump();
	if (hires) {
		static const char *modes[] = { "0x0f", "0x2e" };	/* double speed + MCLK/2 ; single speed, MCLK/1 */
		for (int m = 0; m < 2; m++) for (int fi = 0; fi < 2; fi++) {
			int fmt = fi ? SONY_AFMT_S32_LE : AFMT_S16_LE;
			printf("== 88.2k experiment: reg03h=%s fmt %#x\n", modes[m], fmt);
			int fd = open_dsp();
			if (fd < 0) break;
			int f = fmt, ch = 2, sp = 44100;
			ioctl(fd, SNDCTL_DSP_SETFMT, &f); ioctl(fd, SNDCTL_DSP_CHANNELS, &ch); ioctl(fd, SNDCTL_DSP_SPEED, &sp);
			double base = play_measure(fd, 44100, f, 1.5);
			printf("  44.1k mode measured %.0f frames/s\n", base);
			if (proc_write("reg03h", modes[m])) {
				proc_dump();
				double hi = play_measure(fd, 88200, f, secs);
				printf("  MEASURED with reg03h=%s: %.0f frames/s (88200 expected if double rate works)\n", modes[m], hi);
			}
			proc_write("reg03h", "0x2f");
			double back = play_measure(fd, 44100, f, 1.0);
			printf("  restored: %.0f frames/s\n", back);
			close(fd);
			usleep(300000);
		}
		proc_dump();
	}
	printf("done\n");
	return 0;
}
