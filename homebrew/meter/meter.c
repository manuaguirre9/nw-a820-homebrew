/* meter: background power/CPU sampler for comparing the Sony UI with our player (same conditions: playing, screen off).
 * Every 10 s one line: mode (sony = gemini running, else hb), battery ADC, CPU split (/proc/stat), context switches and
 * interrupts per second, per-IRQ rates (lcd, interdsp, audio, timer...) and the busiest processes (ticks per interval).
 * Lines are buffered in RAM and appended to /var/hb/meter.txt once a minute when /var is mounted (the Sony UI mounts it
 * itself a while after boot; our boot script mounts it for the player). Runs until killed. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>

#define OUT "/var/hb/meter.txt"
#define NIRQ 64
#define NPROC 256

static char buf[16384];
static int blen;

static int var_mounted(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	if (!f) return 0;
	char l[256]; int m = 0;
	while (!m && fgets(l, sizeof l, f)) if (strstr(l, " /var ")) m = 1;
	fclose(f);
	return m;
}

static void flush_out(void)
{
	if (!blen || !var_mounted()) return;
	int fd = open(OUT, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) return;
	if (write(fd, buf, blen) == blen) blen = 0;
	fsync(fd);
	close(fd);
}

static void out(const char *fmt, ...);
#include <stdarg.h>
static void out(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(buf + blen, sizeof buf - blen, fmt, ap);
	va_end(ap);
	if (n > 0 && blen + n < (int)sizeof buf) blen += n;
}

static double uptime(void)
{
	FILE *f = fopen("/proc/uptime", "r");
	double u = 0;
	if (f) { if (fscanf(f, "%lf", &u) != 1) u = 0; fclose(f); }
	return u;
}

/* /proc/stat: cpu split, ctxt, intr */
static void stat_read(unsigned long long cpu[7], unsigned long long *ctxt, unsigned long long *intr)
{
	memset(cpu, 0, 7 * sizeof cpu[0]); *ctxt = *intr = 0;
	FILE *f = fopen("/proc/stat", "r");
	if (!f) return;
	char l[512];
	while (fgets(l, sizeof l, f)) {
		if (!strncmp(l, "cpu ", 4)) sscanf(l + 4, "%llu %llu %llu %llu %llu %llu %llu", &cpu[0], &cpu[1], &cpu[2], &cpu[3], &cpu[4], &cpu[5], &cpu[6]);
		else if (!strncmp(l, "ctxt ", 5)) *ctxt = strtoull(l + 5, 0, 10);
		else if (!strncmp(l, "intr ", 5)) *intr = strtoull(l + 5, 0, 10);
	}
	fclose(f);
}

/* /proc/interrupts: "  13:   96   lcd" */
struct irq { int n; unsigned long long c; char name[24]; };
static int irq_read(struct irq *v)
{
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) return 0;
	char l[256]; int k = 0;
	while (k < NIRQ && fgets(l, sizeof l, f)) {
		int n; unsigned long long c; char name[64];
		if (sscanf(l, " %d: %llu %63s", &n, &c, name) == 3) { v[k].n = n; v[k].c = c; snprintf(v[k].name, sizeof v[k].name, "%s", name); k++; }
	}
	fclose(f);
	return k;
}

/* per-process CPU ticks (utime + stime) by pid */
struct proc { int pid; unsigned long t, rss; char comm[20]; };	/* rss in kB */
static int proc_read(struct proc *v, int *sony)
{
	DIR *d = opendir("/proc");
	if (!d) return 0;
	struct dirent *e; int k = 0;
	*sony = 0;
	while (k < NPROC && (e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		char p[64], l[512]; snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(l, sizeof l, f)) {
			char *a = strchr(l, '('), *b = strrchr(l, ')');
			if (a && b && b > a) {
				struct proc *q = &v[k];
				q->pid = atoi(l);
				int n = (int)(b - a - 1); if (n > 19) n = 19;
				memcpy(q->comm, a + 1, n); q->comm[n] = 0;
				if (!strcmp(q->comm, "gemini")) *sony = 1;
				/* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
				unsigned long ut = 0, st = 0; char s;
				if (sscanf(b + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &s, &ut, &st) == 3) {
					q->t = ut + st; q->rss = 0;
					snprintf(p, sizeof p, "/proc/%s/statm", e->d_name);	/* size resident ... in 4 kB pages */
					FILE *m = fopen(p, "r");
					if (m) { unsigned long sz, res; if (fscanf(m, "%lu %lu", &sz, &res) == 2) q->rss = res * 4; fclose(m); }
					k++;
				}
			}
		}
		fclose(f);
	}
	closedir(d);
	return k;
}

/* /proc/meminfo: free, file cache, buffers, kernel slab (kB) */
static void mem_read(unsigned long *fr, unsigned long *ca, unsigned long *bu, unsigned long *sl)
{
	*fr = *ca = *bu = *sl = 0;
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return;
	char l[128];
	while (fgets(l, sizeof l, f)) {
		sscanf(l, "MemFree: %lu", fr); sscanf(l, "Cached: %lu", ca); sscanf(l, "Buffers: %lu", bu); sscanf(l, "Slab: %lu", sl);
	}
	fclose(f);
}

static int adc_read(int *chg)
{
	int fd = open("/dev/icx1087_power", O_RDONLY), st = 0, adc = -1;
	if (fd < 0) return -1;
	ioctl(fd, 0x80045000, &st);
	ioctl(fd, 0x80045002, &adc);
	close(fd);
	*chg = (st & 0x80) != 0;
	return adc;
}

/* the CPU clock: reading /proc/pm makes the kernel print the SMU registers to its log; SMU_AUTO_FRQ_CHANGE's upper
 * half identifies the mode (pm_pmu.c pll_tbl: 0x0009 248, 0x0005 164, 0x0104 124, 0x0202 83, 0x0401 50 MHz) */
static int clock_mhz(void)
{
	FILE *f = fopen("/proc/pm", "r");
	if (f) { char t[64]; while (fgets(t, sizeof t, f)) ; fclose(f); }
	static char kb[65536];
	int n = klogctl(3, kb, sizeof kb - 1);	/* read the whole ring, non destructive */
	if (n <= 0) return -1;
	kb[n] = 0;
	char *p = 0, *q = kb;
	while ((q = strstr(q, "SMU_AUTO_FRQ_CHANGE"))) { p = q; q++; }
	if (!p) return -1;
	char *x = strstr(p, "0x");
	if (!x) return -1;
	unsigned long v = strtoul(x, 0, 16), hi = v >> 16;
	return hi == 0x0009 ? 248 : hi == 0x0005 ? 164 : hi == 0x0104 ? 124 : hi == 0x0202 ? 83 : hi == 0x0401 ? 50 : 0;
}

int main(int argc, char **argv)
{
	int period = argc > 1 ? atoi(argv[1]) : 10;
	if (period < 2) period = 10;
	static struct irq i0[NIRQ], i1[NIRQ];
	static struct proc p0[NPROC], p1[NPROC];
	unsigned long long c0[7], c1[7], x0, x1, n0, n1;
	int ni0, ni1, np0, np1, sony;
	stat_read(c0, &x0, &n0); ni0 = irq_read(i0); np0 = proc_read(p0, &sony);
	double t0 = uptime(), lastflush = t0;
	out("meter start up=%.1f period=%d\n", t0, period);
	for (;;) {
		sleep(period);
		double t1 = uptime(), dt = t1 - t0;
		if (dt <= 0) dt = period;
		stat_read(c1, &x1, &n1); ni1 = irq_read(i1); np1 = proc_read(p1, &sony);
		unsigned long long d[7], tot = 0;
		for (int i = 0; i < 7; i++) { d[i] = c1[i] - c0[i]; tot += d[i]; }
		int chg = 0, adc = adc_read(&chg);
		int mhz = clock_mhz();
		out("T up=%.0f mode=%s adc=%d chg=%d mhz=%d busy=%.1f usr=%llu sys=%llu idle=%llu irq=%llu sirq=%llu ctxt/s=%.0f intr/s=%.0f",
		    t1, sony ? "sony" : "hb", adc, chg, mhz, tot ? 100.0 * (tot - d[3] - d[4]) / tot : 0, d[0] + d[1], d[2], d[3], d[5], d[6],
		    (x1 - x0) / dt, (n1 - n0) / dt);
		out(" irqs:");
		for (int i = 0; i < ni1; i++) {
			unsigned long long prev = 0;
			for (int j = 0; j < ni0; j++) if (i0[j].n == i1[i].n) prev = i0[j].c;
			unsigned long long dd = i1[i].c - prev;
			if (dd) out(" %s=%.1f", i1[i].name, dd / dt);
		}
		out(" procs:");
		for (int i = 0; i < np1; i++) {
			unsigned long prev = 0;
			for (int j = 0; j < np0; j++) if (p0[j].pid == p1[i].pid) prev = p0[j].t;
			unsigned long dd = p1[i].t - prev;
			if (dd && p1[i].t >= prev) out(" %s=%lu", p1[i].comm, dd);
		}
		unsigned long mf, mc, mb, ms;	/* memory: totals, then each process using 200 kB or more */
		mem_read(&mf, &mc, &mb, &ms);
		out(" mem: free=%lu cached=%lu buf=%lu slab=%lu rss:", mf, mc, mb, ms);
		for (int i = 0; i < np1; i++) if (p1[i].rss >= 200) out(" %s=%lu", p1[i].comm, p1[i].rss);
		out("\n");
		memcpy(c0, c1, sizeof c0); x0 = x1; n0 = n1;
		memcpy(i0, i1, sizeof i1); ni0 = ni1;
		memcpy(p0, p1, sizeof p1); np0 = np1;
		t0 = t1;
		if (t1 - lastflush >= 60 || blen > (int)sizeof buf - 2048) { flush_out(); lastflush = t1; }
	}
}
