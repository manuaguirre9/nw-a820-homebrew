/* bcspsniff: watches the BCSP traffic between hci_bchs and the BlueCore by polling the driver's UART rings
 * through /dev/kmem (addresses from /proc/kallsyms). UART0_RX_QUEUE (4 KB) holds the raw SLIP bytes from the
 * chip, UART0_TX_QUEUE (4 KB) the bytes to the chip; *_in_PTR are the producer indices. Every new byte is fed
 * to a SLIP decoder and each frame's BCSP header is logged with a millisecond timestamp.
 * usage: bcspsniff seconds maxframes > log      (run in the background while traffic flows) */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RING 0x1000

static int km = -1;
static double t0;
static long frames, maxframes;

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static int kread(unsigned long addr, void *buf, int n)
{
	return pread(km, buf, n, (off_t)addr) == n ? 0 : -1;
}

static unsigned long sym(const char *name)
{
	FILE *f = fopen("/proc/kallsyms", "r");
	char line[256];
	unsigned long a = 0;
	if (!f) return 0;
	while (fgets(line, sizeof line, f)) {
		char n[128], t; unsigned long v;
		if (sscanf(line, "%lx %c %127s", &v, &t, n) == 3 && !strcmp(n, name)) { a = v; break; }
	}
	fclose(f);
	return a;
}

struct slip { unsigned char buf[1600]; int len, esc, in; const char *dir; };

static void frame(struct slip *s)
{
	if (s->len < 4) return;
	const unsigned char *h = s->buf;
	int seq = h[0] & 7, ack = (h[0] >> 3) & 7, crc = (h[0] >> 6) & 1, rel = h[0] >> 7;
	int chan = h[1] & 15, len = (h[1] >> 4) | (h[2] << 4);
	int ok = ((h[0] + h[1] + h[2] + h[3]) & 0xff) == 0xff;
	int plen = s->len - 4 - (crc ? 2 : 0);
	printf("%9.1f %s %s seq %d ack %d ch %2d len %4d%s%s", (now() - t0) * 1000, s->dir, rel ? "REL" : "unr",
		seq, ack, chan, len, ok ? "" : " BADHDR", plen != len ? " LENMISMATCH" : "");
	if (chan == 1 && s->len >= 8) printf(" le %02x%02x%02x%02x", h[4], h[5], h[6], h[7]);
	if ((chan == 5 || chan == 6) && s->len >= 7) printf(" p %02x %02x %02x", h[4], h[5], h[6]);
	putchar('\n');
	frames++;
}

static void feed(struct slip *s, const unsigned char *p, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned char c = p[i];
		if (c == 0xc0) { if (s->in && s->len) frame(s); s->in = 1; s->len = 0; s->esc = 0; continue; }
		if (!s->in) continue;
		if (s->esc) { c = c == 0xdc ? 0xc0 : c == 0xdd ? 0xdb : c; s->esc = 0; }
		else if (c == 0xdb) { s->esc = 1; continue; }
		if (s->len < (int)sizeof s->buf) s->buf[s->len++] = c;
	}
}

struct ring { unsigned long q, inp; unsigned last; struct slip s; unsigned long lost; };

static void poll_ring(struct ring *r)
{
	unsigned in;
	static unsigned char tmp[RING];
	if (kread(r->inp, &in, 4)) return;
	in &= RING - 1;
	if (in == r->last) return;
	unsigned n = (in - r->last) & (RING - 1);
	unsigned pos = r->last;
	while (n) {
		unsigned chunk = RING - pos < n ? RING - pos : n;
		if (kread(r->q + pos, tmp, chunk)) return;
		feed(&r->s, tmp, chunk);
		pos = (pos + chunk) & (RING - 1); n -= chunk;
	}
	r->last = in;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 60;
	maxframes = argc > 2 ? atol(argv[2]) : 20000;
	km = open("/dev/kmem", O_RDONLY);
	if (km < 0) { perror("kmem"); return 1; }
	struct ring rx = { sym("UART0_RX_QUEUE"), sym("UART0_RX_in_PTR") }, tx = { sym("UART0_TX_QUEUE"), sym("UART0_TX_in_PTR") };
	printf("rx q %08lx in %08lx  tx q %08lx in %08lx\n", rx.q, rx.inp, tx.q, tx.inp);
	if (!rx.q || !rx.inp || !tx.q || !tx.inp) return 2;
	rx.s.dir = "C>H"; tx.s.dir = "H>C";
	kread(rx.inp, &rx.last, 4); rx.last &= RING - 1;
	kread(tx.inp, &tx.last, 4); tx.last &= RING - 1;
	t0 = now();
	while (now() - t0 < secs && frames < maxframes) {
		poll_ring(&rx);
		poll_ring(&tx);
		usleep(1000);
	}
	printf("end frames %ld\n", frames);
	return 0;
}
