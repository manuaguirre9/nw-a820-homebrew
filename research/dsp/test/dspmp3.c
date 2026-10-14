/* dspmp3: decode an MP3 file on the MP201 DSP through NEC's OpenMAX IL 1.0 component
 * OMX.NEC.EL.MP3D.FTB ("FTB" = PCM comes back to the ARM) and measure the cost on the ARM.
 * gemini MUST NOT be running (only one process can own /dev/InterDSP + the DSP).
 *
 * Protocol (reversed from omf_mc_mp3d_ftb.so + mptapp/gemini, see ../notes/mp3d_notes.txt):
 *   OMX_DriverMode(&1)          as gemini does for the audio_ftb cfg: the core then does NOT open
 *                               /dev/sound/dsp itself (mode 0 = core opens it at 44.1k/2ch)
 *   OMX_Init(cfg)               NEC: takes the cfg path
 *   OMX_GetHandle               callbacks come from component/receiver threads (LinuxThreads)
 *   Get/SetParameter(0xFF000001, u16 struct) - vendor index, NO nSize/nVersion header, only in Loaded
 *   SendCommand(StateSet, Idle) then AllocateBuffer x24 on port 0 (4096 B) and x24 on port 1 (8192 B):
 *                               sizes must match exactly, the Idle transition waits for ALL 24+24
 *                               (UseBuffer = NotImplemented; buffers live in fixed DSP shared memory
 *                                in 0x33E40000 / 0x33EA4000)
 *   SendCommand(StateSet, Executing)   -> DSP start command with the params
 *   FillThisBuffer (all outputs), EmptyThisBuffer: nFilledLen 1..4096, data at pBuffer (nOffset ignored),
 *                               the component byte-swaps the input IN PLACE (16-bit DSP), nFlags EOS=1
 *   EmptyBufferDone / FillBufferDone (FBD sets nFilledLen, nFlags, nTimeStamp; EOS flag on last)
 *   Executing->Idle, Idle->Loaded + FreeBuffer, FreeHandle, OMX_Deinit
 * No pthread calls here (zig's glibc stubs version them GLIBC_2.4, the device's LinuxThreads lacks it):
 * callbacks write fixed-size records into a pipe (atomic, < PIPE_BUF) and the main thread poll()s it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

typedef uint32_t OMX_ERRORTYPE;
typedef void *OMX_HANDLETYPE;

typedef struct {		/* OMX IL 1.0 buffer header, 0x50 bytes (component mallocs 0x50, sets nSize=0x50) */
	uint32_t nSize, nVersion;
	uint8_t *pBuffer;		/* +08 virtual address inside the mapped DSP shared memory */
	uint32_t nAllocLen;		/* +0c 4096 in / 8192 out */
	uint32_t nFilledLen;		/* +10 */
	uint32_t nOffset;		/* +14 ignored by the component */
	void *pAppPrivate;		/* +18 */
	void *pPlatformPrivate;		/* +1c physical address | buffer index (low 5 bits) */
	void *pInputPortPrivate;	/* +20 used by the component as its queue link: don't touch */
	void *pOutputPortPrivate;	/* +24 */
	void *hMarkTargetComponent, *pMarkData;
	uint32_t nTickCount;		/* +30 */
	uint32_t pad0;
	int64_t nTimeStamp;		/* +38 */
	uint32_t nFlags;		/* +40 1 = EOS; 0x20000 = vendor "reset input queue" */
	uint32_t nOutputPortIndex, nInputPortIndex, pad1;
} BUFHDR;
_Static_assert(sizeof(BUFHDR) == 0x50, "BUFHDR size");
_Static_assert(__builtin_offsetof(BUFHDR, nTimeStamp) == 0x38, "nTimeStamp");
_Static_assert(__builtin_offsetof(BUFHDR, nFlags) == 0x40, "nFlags");

typedef struct {
	OMX_ERRORTYPE (*EventHandler)(OMX_HANDLETYPE, void *, uint32_t, uint32_t, uint32_t, void *);
	OMX_ERRORTYPE (*EmptyBufferDone)(OMX_HANDLETYPE, void *, BUFHDR *);
	OMX_ERRORTYPE (*FillBufferDone)(OMX_HANDLETYPE, void *, BUFHDR *);
} OMX_CALLBACKTYPE;

typedef struct {		/* OMX IL 1.0 OMX_COMPONENTTYPE (nSize 0x50) */
	uint32_t nSize, nVersion;
	void *pComponentPrivate, *pApplicationPrivate;
	OMX_ERRORTYPE (*GetComponentVersion)(OMX_HANDLETYPE, char *, uint32_t *, uint32_t *, uint8_t (*)[128]);
	OMX_ERRORTYPE (*SendCommand)(OMX_HANDLETYPE, uint32_t cmd, uint32_t param, void *data);
	OMX_ERRORTYPE (*GetParameter)(OMX_HANDLETYPE, uint32_t idx, void *p);
	OMX_ERRORTYPE (*SetParameter)(OMX_HANDLETYPE, uint32_t idx, void *p);
	OMX_ERRORTYPE (*GetConfig)(OMX_HANDLETYPE, uint32_t idx, void *p);
	OMX_ERRORTYPE (*SetConfig)(OMX_HANDLETYPE, uint32_t idx, void *p);
	OMX_ERRORTYPE (*GetExtensionIndex)(OMX_HANDLETYPE, char *, uint32_t *);
	OMX_ERRORTYPE (*GetState)(OMX_HANDLETYPE, uint32_t *);
	void *ComponentTunnelRequest, *UseBuffer;
	OMX_ERRORTYPE (*AllocateBuffer)(OMX_HANDLETYPE, BUFHDR **, uint32_t port, void *app, uint32_t size);
	OMX_ERRORTYPE (*FreeBuffer)(OMX_HANDLETYPE, uint32_t port, BUFHDR *);
	OMX_ERRORTYPE (*EmptyThisBuffer)(OMX_HANDLETYPE, BUFHDR *);
	OMX_ERRORTYPE (*FillThisBuffer)(OMX_HANDLETYPE, BUFHDR *);
	void *SetCallbacks, *ComponentDeInit;
} OMX_COMPONENTTYPE;

extern OMX_ERRORTYPE OMX_DriverMode(int *mode);
extern OMX_ERRORTYPE OMX_Init(const char *cfg);
extern OMX_ERRORTYPE OMX_Deinit(void);
extern OMX_ERRORTYPE OMX_GetHandle(OMX_HANDLETYPE *h, const char *name, void *app, OMX_CALLBACKTYPE *cb);
extern OMX_ERRORTYPE OMX_FreeHandle(OMX_HANDLETYPE h);

#define IDX_PARAM  0xFF000001u	/* NEC vendor index: decoder parameter block (u16 fields, see notes) */
#define IDX_CONFIG 0xFF000002u	/* first 4 u16 of the same block */
#define NBUF 24
#define INSZ 4096
#define OUTSZ 8192
#define CMD_STATESET 0
#define ST_LOADED 1
#define ST_IDLE 2
#define ST_EXEC 3
#define FLAG_EOS 1

/* ---------------- event pipe (callbacks run on other threads) ---------------- */
enum { EV_EVENT = 1, EV_EBD, EV_FBD };
struct ev { uint32_t type, a, b, c; void *p; };
static int evpipe[2];

static void post(uint32_t t, uint32_t a, uint32_t b, uint32_t c, void *p)
{
	struct ev e = { t, a, b, c, p };
	while (write(evpipe[1], &e, sizeof e) < 0 && errno == EINTR)
		;
}
static OMX_ERRORTYPE cb_event(OMX_HANDLETYPE h, void *app, uint32_t e, uint32_t d1, uint32_t d2, void *d)
{
	post(EV_EVENT, e, d1, d2, d);
	return 0;
}
static OMX_ERRORTYPE cb_ebd(OMX_HANDLETYPE h, void *app, BUFHDR *b) { post(EV_EBD, 0, 0, 0, b); return 0; }
static OMX_ERRORTYPE cb_fbd(OMX_HANDLETYPE h, void *app, BUFHDR *b) { post(EV_FBD, 0, 0, 0, b); return 0; }

/* ---------------- small helpers ---------------- */
static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + tv.tv_usec / 1e6;
}
static double t0;
#define LOG(...) do { printf("[%7.3f] ", now() - t0); printf(__VA_ARGS__); } while (0)

static unsigned long irq_count(void)
{
	char l[256];
	unsigned long n = 0;
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f)
		return 0;
	while (fgets(l, sizeof l, f))
		if (strstr(l, "interdsp")) {
			char *c = strchr(l, ':');
			if (c)
				n = strtoul(c + 1, 0, 10);
		}
	fclose(f);
	return n;
}

struct sysstat { unsigned long long busy, total; };
static void sys_cpu(struct sysstat *s)
{
	unsigned long long v[8] = { 0 };
	FILE *f = fopen("/proc/stat", "r");
	s->busy = s->total = 0;
	if (!f)
		return;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) >= 4) {
		int i;
		for (i = 0; i < 7; i++)
			s->total += v[i];
		s->busy = s->total - v[3] - v[4];	/* minus idle, iowait */
	}
	fclose(f);
}

/* CPU jiffies (utime+stime) of this process and all its LinuxThreads threads: LinuxThreads threads are
 * separate processes (children of the manager thread, which is our child), so getrusage(SELF) only sees
 * the main thread. */
static int nthreads_seen;
static unsigned long thread_jiffies(void)
{
	struct { int pid, ppid; unsigned long j; } p[512];
	int n = 0, i, k, me = getpid();
	unsigned long sum = 0;
	DIR *d = opendir("/proc");
	struct dirent *de;
	if (!d)
		return 0;
	while ((de = readdir(d)) && n < 512) {
		char fn[64], buf[512], *q;
		FILE *f;
		unsigned long ut, st;
		int ppid;
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(fn, sizeof fn, "/proc/%s/stat", de->d_name);
		if (!(f = fopen(fn, "r")))
			continue;
		if (!fgets(buf, sizeof buf, f)) {
			fclose(f);
			continue;
		}
		fclose(f);
		q = strrchr(buf, ')');
		if (!q)
			continue;
		/* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
		if (sscanf(q + 2, "%*c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ppid, &ut, &st) != 3)
			continue;
		p[n].pid = atoi(de->d_name);
		p[n].ppid = ppid;
		p[n].j = ut + st;
		n++;
	}
	closedir(d);
	nthreads_seen = 0;
	for (i = 0; i < n; i++) {
		int mine = p[i].pid == me || p[i].ppid == me;
		for (k = 0; !mine && k < n; k++)
			if (p[k].ppid == me && p[i].ppid == p[k].pid)
				mine = 1;
		if (mine) {
			sum += p[i].j;
			nthreads_seen++;
		}
	}
	return sum;
}

static double ru_self(void)
{
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
	return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

/* ---------------- MD5 (RFC 1321, compact) ---------------- */
typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; } md5_t;
#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
static void md5_block(md5_t *m, const uint8_t *p)
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
	static const uint8_t R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
	uint32_t w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3], f, t;
	int i, g;
	for (i = 0; i < 16; i++)
		w[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
	for (i = 0; i < 64; i++) {
		if (i < 16) { f = (b & c) | (~b & d); g = i; }
		else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
		else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
		else { f = c ^ (b | ~d); g = (7 * i) & 15; }
		t = d; d = c; c = b;
		b = b + ROL(a + f + K[i] + w[g], R[(i >> 4) * 4 + (i & 3)]);
		a = t;
	}
	m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}
static void md5_init(md5_t *m)
{
	m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476; m->len = 0;
}
static void md5_add(md5_t *m, const uint8_t *p, size_t n)
{
	size_t fill = m->len & 63;
	m->len += n;
	if (fill) {
		size_t k = 64 - fill < n ? 64 - fill : n;
		memcpy(m->buf + fill, p, k);
		p += k; n -= k;
		if (fill + k < 64)
			return;
		md5_block(m, m->buf);
	}
	for (; n >= 64; p += 64, n -= 64)
		md5_block(m, p);
	memcpy(m->buf, p, n);
}
static void md5_hex(md5_t *m, char *out)
{
	uint8_t pad[72] = { 0x80 };
	uint64_t bits = m->len * 8;
	size_t fill = m->len & 63, pl = fill < 56 ? 56 - fill : 120 - fill;
	int i;
	for (i = 0; i < 8; i++)
		pad[pl + i] = bits >> (8 * i);
	md5_add(m, pad, pl + 8);
	for (i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02x", (m->h[i / 4] >> (8 * (i % 4))) & 0xff);
}

/* ---------------- MP3 frame index ---------------- */
static const int br_tab[2][3][16] = {	/* [lsf][layer-1][idx] kbps */
	{ { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
	  { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
	  { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
	{ { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
	  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
	  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
static const int sr_tab[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };

struct mp3info { int ver, layer, sr, ch, kbps, spf, mode, sri; };
/* returns frame length or 0 */
static int mp3_hdr(const uint8_t *h, struct mp3info *mi)
{
	int ver, lay, bri, sri, pad, lsf, len;
	if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0)
		return 0;
	ver = (h[1] >> 3) & 3;	/* 0=2.5 1=res 2=MPEG2 3=MPEG1 */
	lay = 4 - ((h[1] >> 1) & 3);
	bri = h[2] >> 4;
	sri = (h[2] >> 2) & 3;
	pad = (h[2] >> 1) & 1;
	if (ver == 1 || lay == 4 || bri == 0 || bri == 15 || sri == 3)
		return 0;
	lsf = ver != 3;
	mi->ver = ver;
	mi->layer = lay;
	mi->sri = sri;
	mi->sr = sr_tab[ver][sri];
	mi->kbps = br_tab[lsf][lay - 1][bri];
	mi->mode = h[3] >> 6;
	mi->ch = mi->mode == 3 ? 1 : 2;
	if (lay == 1) {
		mi->spf = 384;
		len = (12000 * mi->kbps / mi->sr + pad) * 4;
	} else {
		mi->spf = (lay == 3 && lsf) ? 576 : 1152;
		len = (lay == 3 && lsf ? 72000 : 144000) * mi->kbps / mi->sr + pad;
	}
	return len;
}

static uint8_t *file;
static long fsize;
static int *frame_off, *frame_len, nframes;
static struct mp3info si;

static int index_frames(void)
{
	long i = 0;
	int cap = 4096;
	struct mp3info mi;
	if (fsize > 10 && !memcmp(file, "ID3", 3)) {
		i = 10 + ((file[6] & 0x7f) << 21 | (file[7] & 0x7f) << 14 | (file[8] & 0x7f) << 7 | (file[9] & 0x7f));
		if (file[5] & 0x10)
			i += 10;
		printf("ID3v2 tag: %ld bytes skipped\n", i);
	}
	frame_off = malloc(cap * sizeof(int));
	frame_len = malloc(cap * sizeof(int));
	while (i + 4 <= fsize) {
		int len = mp3_hdr(file + i, &mi);
		/* accept a frame if it is the first one matching the stream, and the next header also parses */
		if (len > 0 && (nframes == 0 ? (i + len + 4 > fsize || mp3_hdr(file + i + len, &si))
			      : (mi.sr == si.sr && mi.layer == si.layer))) {
			if (i + len > fsize)
				break;	/* truncated last frame */
			if (nframes == 0)
				si = mi;
			if (nframes == cap) {
				cap *= 2;
				frame_off = realloc(frame_off, cap * sizeof(int));
				frame_len = realloc(frame_len, cap * sizeof(int));
			}
			frame_off[nframes] = i;
			frame_len[nframes++] = len;
			i += len;
		} else if (fsize - i >= 3 && !memcmp(file + i, "TAG", 3)) {
			break;
		} else {
			i++;	/* resync */
		}
	}
	return nframes;
}

/* ---------------- globals for results ---------------- */
static OMX_HANDLETYPE h;
static OMX_COMPONENTTYPE *C;
static BUFHDR *inb[NBUF], *outb[NBUF];
static int nin, nout;
static BUFHDR *freein[NBUF];
static int nfreein;
static uint32_t cur_state = 0;
static int got_eos, n_ebd, n_fbd, n_etb, n_ftb, n_err_ev, last_err;
static int next_frame, input_done;
static long long bytes_in, bytes_out;
static int mode = 'p';		/* p = pack whole frames, f = one frame per buffer, r = raw 4096 chunks */
static long raw_pos;
static int verbose = 3;
static double t_exec, t_first_out, t_eos;
static md5_t md5;
static uint8_t head[65536];
static int headlen;
static FILE *wav;
static long wav_cap = 6 << 20, wav_bytes;
static int out_ch, out_rate, out_swap = -1;	/* forced format (-F) */
static int play_fd = -1;
static double play_secs;
static long long play_bytes;
static double sink_cpu;		/* main-thread CPU spent in md5/wav/play (subtract for pure decode cost) */
/* byte-order / L=R analysis over the first 256 KB of PCM */
static unsigned long long an_le1, an_be1, an_le2, an_be2, an_abs;
static unsigned long an_samples, an_lr_eq;
static int an_prev[2][2];

static void analyze(const uint8_t *p, int n)
{
	int i;
	for (i = 0; i + 1 < n && an_samples < 131072; i += 2, an_samples++) {
		int le = (int16_t)(p[i] | p[i + 1] << 8), be = (int16_t)(p[i] << 8 | p[i + 1]);
		if (an_samples >= 2) {
			an_le1 += abs(le - an_prev[0][1]);
			an_be1 += abs(be - an_prev[1][1]);
			an_le2 += abs(le - an_prev[0][0]);
			an_be2 += abs(be - an_prev[1][0]);
		}
		if ((an_samples & 1) && le == an_prev[0][1])
			an_lr_eq++;
		an_abs += abs(le);
		an_prev[0][0] = an_prev[0][1]; an_prev[0][1] = le;
		an_prev[1][0] = an_prev[1][1]; an_prev[1][1] = be;
	}
}

static void wav_header(uint8_t *hd, int ch, int rate, uint32_t datalen)
{
	uint32_t v;
	memcpy(hd, "RIFF", 4); v = datalen + 36; memcpy(hd + 4, &v, 4);
	memcpy(hd + 8, "WAVEfmt ", 8); v = 16; memcpy(hd + 16, &v, 4);
	hd[20] = 1; hd[21] = 0; hd[22] = ch; hd[23] = 0;
	memcpy(hd + 24, &rate, 4); v = rate * ch * 2; memcpy(hd + 28, &v, 4);
	hd[32] = ch * 2; hd[33] = 0; hd[34] = 16; hd[35] = 0;
	memcpy(hd + 36, "data", 4); memcpy(hd + 40, &datalen, 4);
}

static void sink(uint8_t *p, int n)
{
	double c0 = ru_self();
	if (out_swap == 1) {
		int i;
		for (i = 0; i + 1 < n; i += 2) {
			uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t;
		}
	}
	md5_add(&md5, p, n);
	analyze(p, n);
	if (headlen < (int)sizeof head - 44) {
		int k = n < (int)sizeof head - 44 - headlen ? n : (int)sizeof head - 44 - headlen;
		memcpy(head + 44 + headlen, p, k);
		headlen += k;
	}
	if (wav && wav_bytes < wav_cap) {
		int k = n < wav_cap - wav_bytes ? n : wav_cap - wav_bytes;
		fwrite(p, 1, k, wav);
		wav_bytes += k;
	}
	if (play_fd >= 0 && play_bytes < (long long)(play_secs * out_rate * 4)) {
		if (out_ch == 1) {	/* hardware path is stereo: duplicate */
			static int16_t st[OUTSZ];
			int i, m = n / 2;
			for (i = 0; i < m; i++)
				st[2 * i] = st[2 * i + 1] = ((int16_t *)p)[i];
			write(play_fd, st, m * 4);
			play_bytes += m * 4;
		} else {
			write(play_fd, p, n);
			play_bytes += n;
		}
	}
	sink_cpu += ru_self() - c0;
}

/* fill one free input buffer and submit it; returns 0 if nothing left */
static int feed_one(void)
{
	BUFHDR *b;
	int n = 0, first = next_frame;
	OMX_ERRORTYPE r;
	if (input_done || nfreein == 0)
		return 0;
	b = freein[--nfreein];
	if (mode == 'r') {
		long start = nframes ? frame_off[0] : 0, end = nframes ? frame_off[nframes - 1] + frame_len[nframes - 1] : fsize;
		if (raw_pos < start)
			raw_pos = start;
		n = end - raw_pos < INSZ ? end - raw_pos : INSZ;
		memcpy(b->pBuffer, file + raw_pos, n);
		raw_pos += n;
		if (raw_pos >= end)
			input_done = 1;
		next_frame = nframes * (raw_pos - start) / (end - start + 1);
	} else {
		while (next_frame < nframes && n + frame_len[next_frame] <= INSZ) {
			memcpy(b->pBuffer + n, file + frame_off[next_frame], frame_len[next_frame]);
			n += frame_len[next_frame++];
			if (mode == 'f')
				break;
		}
		if (next_frame >= nframes)
			input_done = 1;
	}
	b->nFilledLen = n;
	b->nOffset = 0;
	b->nFlags = input_done ? FLAG_EOS : 0;
	b->nTickCount = 0;
	b->nTimeStamp = (int64_t)first * si.spf * 1000000 / si.sr;
	r = C->EmptyThisBuffer(h, b);
	n_etb++;
	bytes_in += n;
	if (verbose > 0 || r) {
		LOG("ETB #%d len=%d frames %d..%d flags=%x -> %#x\n", n_etb, n, first, next_frame - 1, b->nFlags, r);
		if (verbose > 0)
			verbose--;
	}
	if (r) {
		freein[nfreein++] = b;
		input_done = 1;	/* stop feeding, let the loop time out */
		return 0;
	}
	return 1;
}

static void handle(struct ev *e, int running)
{
	if (e->type == EV_EVENT) {
		LOG("event %u data1=%#x data2=%#x pdata=%p\n", e->a, e->b, e->c, e->p);
		if (e->a == 0 && e->b == CMD_STATESET)
			cur_state = e->c;
		else if (e->a == 1) {
			n_err_ev++;
			last_err = e->b;
		} else if (e->a == 4 && e->b == 1 && (e->c & FLAG_EOS))
			got_eos = got_eos ? got_eos : 2;
	} else if (e->type == EV_EBD) {
		BUFHDR *b = e->p;
		n_ebd++;
		if (verbose > 0 || n_ebd <= 2)
			LOG("EBD #%d hdr=%p filled=%u flags=%x\n", n_ebd, b, b->nFilledLen, b->nFlags);
		if (nfreein < NBUF)
			freein[nfreein++] = b;
	} else if (e->type == EV_FBD) {
		BUFHDR *b = e->p;
		uint32_t n = b->nFilledLen;
		n_fbd++;
		if (!t_first_out)
			t_first_out = now();
		if (n_fbd <= 3 || (b->nFlags & FLAG_EOS)) {
			uint8_t *q = b->pBuffer + b->nOffset;
			LOG("FBD #%d hdr=%p len=%u off=%u flags=%x ts=%lld  %02x%02x %02x%02x %02x%02x %02x%02x\n", n_fbd, b, n,
			    b->nOffset, b->nFlags, (long long)b->nTimeStamp, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
		}
		if (n > b->nAllocLen)
			n = b->nAllocLen;
		if (n) {
			bytes_out += n;
			sink(b->pBuffer + b->nOffset, n);
		}
		if (b->nFlags & FLAG_EOS) {
			got_eos = 1;
			t_eos = now();
		} else if (running && !got_eos) {
			OMX_ERRORTYPE r;
			b->nFilledLen = 0;
			b->nFlags = 0;
			r = C->FillThisBuffer(h, b);
			n_ftb++;
			if (r)
				LOG("FTB -> %#x\n", r);
		}
	}
}

/* wait up to ms for one event; returns 1 if handled */
static int pump(int ms, int running)
{
	struct pollfd pf = { evpipe[0], POLLIN, 0 };
	struct ev e;
	if (poll(&pf, 1, ms) <= 0)
		return 0;
	if (read(evpipe[0], &e, sizeof e) != sizeof e)
		return 0;
	handle(&e, running);
	return 1;
}

static int wait_state(uint32_t st, int ms)
{
	double end = now() + ms / 1000.0;
	while (cur_state != st && now() < end)
		pump(50, 0);
	if (cur_state != st)
		LOG("timeout waiting for state %u (have %u)\n", st, cur_state);
	return cur_state == st;
}

static void dump_param(const char *what, uint16_t *p)
{
	printf("%s: +0=%u +2=%u +4=%u +6=%u +8=%u +a=%u +c=%u +e=%u +10=%u +12=%u +14=%u\n", what, p[0], p[1], p[2],
	       p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10]);
}

/* ---------------- results ---------------- */
static unsigned long irq0, j0;
static struct sysstat ss0;
static double c0, w0;
static int report_done;

static void report(const char *why)
{
	unsigned long j1 = thread_jiffies(), irq1 = irq_count();
	struct sysstat ss1;
	double wall = now() - w0, dec_wall = (t_eos ? t_eos : now()) - (t_exec ? t_exec : w0);
	double hz = sysconf(_SC_CLK_TCK), audio_s = (double)nframes * si.spf / (si.sr ? si.sr : 1);
	double proc_cpu = (j1 - j0) / hz;
	char hex[40];
	int ch = out_ch, rate = out_rate, i;
	struct tms tm;

	if (report_done++)
		return;
	sys_cpu(&ss1);
	times(&tm);
	md5_hex(&md5, hex);
	printf("\n==== RESULT (%s) ====\n", why);
	printf("stream: MPEG%s layer %d, %d Hz, %s, %d kbps, %d frames x %d samples = %.2f s audio\n",
	       si.ver == 3 ? "1" : si.ver == 2 ? "2" : "2.5", si.layer, si.sr, si.ch == 1 ? "mono" : "stereo", si.kbps,
	       nframes, si.spf, audio_s);
	printf("eos: %s  (EOS on FBD=1, BufferFlag event=2)  state=%u  error events=%d last=%#x\n",
	       got_eos ? (got_eos == 1 ? "1" : "2") : "NO", cur_state, n_err_ev, last_err);
	printf("buffers: ETB %d EBD %d FTB %d FBD %d  frames fed %d/%d\n", n_etb, n_ebd, n_ftb, n_fbd, next_frame, nframes);
	printf("bytes in %lld (%s)  bytes out %lld  pcm md5 %s\n", bytes_in, mode == 'p' ? "packed frames" : mode == 'f' ? "1 frame/buf" : "raw", bytes_out, hex);
	printf("wall: total %.3f s, Executing->EOS %.3f s, first PCM after %.3f s  => %.1fx realtime\n", wall, dec_wall,
	       t_first_out && t_exec ? t_first_out - t_exec : -1.0, dec_wall > 0 ? audio_s * ((double)next_frame / (nframes ? nframes : 1)) / dec_wall : 0);
	printf("ARM cpu: all our threads %.2f s (%d threads seen)  main-thread getrusage %.2f s  sink(md5/wav/play) %.2f s\n",
	       proc_cpu, nthreads_seen, ru_self() - c0, sink_cpu);
	printf("ARM cpu: times() utime %ld stime %ld ticks (main thread)  system busy %.1f%% of %.2f s (%llu/%llu jiffies)\n",
	       (long)tm.tms_utime, (long)tm.tms_stime, ss1.total > ss0.total ? 100.0 * (ss1.busy - ss0.busy) / (ss1.total - ss0.total) : 0,
	       (ss1.total - ss0.total) / hz, ss1.busy - ss0.busy, ss1.total - ss0.total);
	printf("ARM cpu per audio second (all threads - sink): %.4f s  => %.1f%% of one 124 MHz core at realtime\n",
	       audio_s > 0 ? (proc_cpu - sink_cpu) / audio_s : 0, audio_s > 0 ? 100 * (proc_cpu - sink_cpu) / audio_s : 0);
	printf("interdsp IRQ: %lu -> %lu (+%lu)\n", irq0, irq1, irq1 - irq0);
	/* output format inference */
	printf("pcm analysis (%lu samples): diff1 LE %llu BE %llu  diff2 LE %llu BE %llu  mean|x| %llu  L==R pairs %lu/%lu\n",
	       an_samples, an_le1, an_be1, an_le2, an_be2, an_samples ? an_abs / an_samples : 0, an_lr_eq, an_samples / 2);
	if (!ch && bytes_out && got_eos && nframes) {
		double bpf = (double)bytes_out / nframes;	/* bytes per MP3 frame */
		static const int rates[] = { 0, 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };
		double best = 1e18;
		int c, k;
		for (c = 1; c <= 2; c++)
			for (k = 0; k < 9; k++) {
				int r = k ? rates[k] : si.sr;
				double exp = (double)si.spf * r / si.sr * c * 2, d = bpf > exp ? bpf - exp : exp - bpf;
				if (d < best - 1 || (d < best + 1 && k == 0)) {
					best = d;
					ch = c;
					rate = r;
				}
			}
		printf("bytes per mp3 frame %.1f -> best fit %d ch @ %d Hz (off by %.1f)\n", bpf, ch, rate, best);
	}
	if (!ch) {
		ch = si.ch;
		rate = si.sr;
	}
	i = out_swap >= 0 ? out_swap : 0;
	if (out_swap < 0 && an_be1 * 2 < an_le1) {
		printf("NOTE: PCM looks big-endian (BE much smoother); WAV/md5 above are unswapped -> use -F ch,rate,1\n");
		i = 1;
	}
	printf("FORMAT %d %d %d\n", ch, rate, i);
	if (wav) {
		uint8_t hd[44];
		wav_header(hd, ch, rate, wav_bytes);
		fseek(wav, 0, SEEK_SET);
		fwrite(hd, 1, 44, wav);
		fclose(wav);
		wav = 0;
		printf("wav: %ld bytes of PCM written\n", wav_bytes);
	}
	fflush(stdout);
}

static const char *headpath;
static void write_head(int ch, int rate)
{
	FILE *f;
	if (!headpath || !headlen)
		return;
	wav_header(head, ch, rate, headlen);
	if ((f = fopen(headpath, "wb"))) {
		fwrite(head, 1, 44 + headlen, f);
		fclose(f);
	}
}

static void on_alarm(int s)
{
	static const char m[] = "\n!!! WATCHDOG: decode did not finish in time\n";
	write(1, m, sizeof m - 1);
	report("watchdog");
	write_head(si.ch, si.sr);
	_exit(3);
}

static void usage(void)
{
	printf("usage: dspmp3 [opts] file.mp3\n"
	       "  -c cfg      OMX cfg (default /usr/lib/omf/omx_nw_walkman_audio_ftb.cfg)\n"
	       "  -C name     component (default OMX.NEC.EL.MP3D.FTB)\n"
	       "  -D n        OMX_DriverMode(n) before OMX_Init (default 1 like gemini; -1 = don't call)\n"
	       "  -P off=val  set u16 at byte offset off of the 0xFF000001 param block (repeatable)\n"
	       "  -m p|f|r    input: packed whole frames (default) / one frame per buffer / raw 4096 chunks\n"
	       "  -w file     write WAV (PCM capped by -L bytes, default 6 MB)\n"
	       "  -H file     write the first 64 KB as a WAV\n"
	       "  -F ch,rate,swap  force output format (else inferred at the end)\n"
	       "  -p secs     also play the first secs seconds to /dev/sound/dsp (needs -F)\n"
	       "  -T secs     watchdog (default 45)\n"
	       "  -v n        log the first n buffers (default 3)\n");
}

int main(int argc, char **argv)
{
	const char *cfg = "/usr/lib/omf/omx_nw_walkman_audio_ftb.cfg", *comp = "OMX.NEC.EL.MP3D.FTB", *wavpath = 0;
	int drvmode = 1, npset = 0, pset_off[16], pset_val[16], wd = 45, i, o, rc = 0;
	uint16_t prm[16];
	OMX_CALLBACKTYPE cb = { cb_event, cb_ebd, cb_fbd };
	OMX_ERRORTYPE r;
	FILE *f;

	setvbuf(stdout, 0, _IONBF, 0);
	t0 = now();
	while ((o = getopt(argc, argv, "c:C:D:P:m:w:H:F:p:T:L:v:h")) != -1) {
		switch (o) {
		case 'c': cfg = optarg; break;
		case 'C': comp = optarg; break;
		case 'D': drvmode = atoi(optarg); break;
		case 'P':
			if (npset < 16 && sscanf(optarg, "%i=%i", &pset_off[npset], &pset_val[npset]) == 2)
				npset++;
			break;
		case 'm': mode = optarg[0]; break;
		case 'w': wavpath = optarg; break;
		case 'H': headpath = optarg; break;
		case 'F': sscanf(optarg, "%d,%d,%d", &out_ch, &out_rate, &out_swap); break;
		case 'p': play_secs = atof(optarg); break;
		case 'T': wd = atoi(optarg); break;
		case 'L': wav_cap = atol(optarg); break;
		case 'v': verbose = atoi(optarg); break;
		default: usage(); return 1;
		}
	}
	if (optind >= argc) {
		usage();
		return 1;
	}
	/* input */
	if (!(f = fopen(argv[optind], "rb"))) {
		perror(argv[optind]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	file = malloc(fsize + 8);
	if (!file || fread(file, 1, fsize, f) != (size_t)fsize) {
		printf("read failed\n");
		return 1;
	}
	fclose(f);
	if (!index_frames()) {
		printf("no MP3 frames found\n");
		return 1;
	}
	printf("file %s: %ld bytes, %d frames, first at %d; MPEG%s L%d %d Hz %s %d kbps (mode %d, sr idx %d)\n", argv[optind],
	       fsize, nframes, frame_off[0], si.ver == 3 ? "1" : si.ver == 2 ? "2" : "2.5", si.layer, si.sr,
	       si.ch == 1 ? "mono" : "stereo", si.kbps, si.mode, si.sri);
	if (pipe(evpipe) < 0) {
		perror("pipe");
		return 1;
	}
	md5_init(&md5);
	if (wavpath && (wav = fopen(wavpath, "wb")))
		fwrite(head, 1, 44, wav);	/* placeholder header */
	if (play_secs > 0) {
		if (!out_ch || !out_rate) {
			printf("-p needs -F ch,rate,swap; playback disabled\n");
		} else if ((play_fd = open("/dev/sound/dsp", O_WRONLY)) >= 0) {
			int v = AFMT_S16_LE, m;
			ioctl(play_fd, SNDCTL_DSP_SETFMT, &v);
			v = 2;
			ioctl(play_fd, SNDCTL_DSP_CHANNELS, &v);
			v = out_rate;
			ioctl(play_fd, SNDCTL_DSP_SPEED, &v);
			printf("playback: /dev/sound/dsp %d Hz (asked %d), stereo, %.1f s\n", v, out_rate, play_secs);
			out_rate = v > 0 ? v : out_rate;
			if ((m = open("/dev/sound/mixer", O_RDWR)) >= 0) {
				v = 60 | 60 << 8;	/* ~ -10 dB (76 = 0 dB) */
				ioctl(m, SOUND_MIXER_WRITE_VOLUME, &v);
				close(m);
			}
		} else
			perror("/dev/sound/dsp");
	}
	signal(SIGALRM, on_alarm);
	alarm(wd);

	irq0 = irq_count();
	sys_cpu(&ss0);
	j0 = thread_jiffies();
	c0 = ru_self();
	w0 = now();

	if (drvmode >= 0) {
		r = OMX_DriverMode(&drvmode);
		LOG("OMX_DriverMode(%d) = %#x\n", drvmode, r);
	}
	r = OMX_Init(cfg);
	LOG("OMX_Init(%s) = %#x\n", cfg, r);
	if (r)
		return 1;
	r = OMX_GetHandle(&h, comp, 0, &cb);
	LOG("OMX_GetHandle(%s) = %#x\n", comp, r);
	if (r || !h) {
		OMX_Deinit();
		return 1;
	}
	C = h;
	memset(prm, 0, sizeof prm);
	r = C->GetParameter(h, IDX_PARAM, prm);
	LOG("GetParameter(0xFF000001) = %#x\n", r);
	dump_param("  param (defaults)", prm);
	if (npset) {
		for (i = 0; i < npset; i++)
			if (pset_off[i] >= 0 && pset_off[i] < 30)
				prm[pset_off[i] / 2] = pset_val[i];
		dump_param("  param (to set) ", prm);
		r = C->SetParameter(h, IDX_PARAM, prm);
		LOG("SetParameter(0xFF000001) = %#x%s\n", r, r == 0x80001019 ? " (UnsupportedSetting: value out of range)" : "");
		memset(prm, 0, sizeof prm);
		C->GetParameter(h, IDX_PARAM, prm);
		dump_param("  param (now)    ", prm);
	}
	memset(prm, 0, sizeof prm);
	r = C->GetConfig(h, IDX_CONFIG, prm);
	LOG("GetConfig(0xFF000002) = %#x: %u %u %u %u\n", r, prm[0], prm[1], prm[2], prm[3]);
	C->GetState(h, &cur_state);

	/* Loaded -> Idle: command first, then populate both ports completely */
	r = C->SendCommand(h, CMD_STATESET, ST_IDLE, 0);
	LOG("SendCommand(Idle) = %#x\n", r);
	for (i = 0; i < NBUF; i++) {
		r = C->AllocateBuffer(h, &inb[i], 0, 0, INSZ);
		if (r) {
			LOG("AllocateBuffer(in %d) = %#x\n", i, r);
			break;
		}
		nin++;
	}
	for (i = 0; i < NBUF; i++) {
		r = C->AllocateBuffer(h, &outb[i], 1, 0, OUTSZ);
		if (r) {
			LOG("AllocateBuffer(out %d) = %#x\n", i, r);
			break;
		}
		nout++;
	}
	LOG("allocated %d in, %d out; in[0] hdr=%p buf=%p alloc=%u phys|idx=%p port=%u/%u; out[0] buf=%p alloc=%u phys|idx=%p\n",
	    nin, nout, inb[0], nin ? inb[0]->pBuffer : 0, nin ? inb[0]->nAllocLen : 0, nin ? inb[0]->pPlatformPrivate : 0,
	    nin ? inb[0]->nInputPortIndex : 0, nin ? inb[0]->nOutputPortIndex : 0, nout ? outb[0]->pBuffer : 0,
	    nout ? outb[0]->nAllocLen : 0, nout ? outb[0]->pPlatformPrivate : 0);
	if (!wait_state(ST_IDLE, 5000)) {
		rc = 1;
		goto teardown;
	}
	r = C->SendCommand(h, CMD_STATESET, ST_EXEC, 0);
	LOG("SendCommand(Executing) = %#x\n", r);
	if (!wait_state(ST_EXEC, 5000)) {
		rc = 1;
		goto teardown;
	}
	t_exec = now();
	for (i = 0; i < nout; i++) {
		outb[i]->nFilledLen = 0;
		outb[i]->nFlags = 0;
		r = C->FillThisBuffer(h, outb[i]);
		n_ftb++;
		if (r)
			LOG("FTB(%d) = %#x\n", i, r);
	}
	for (i = 0; i < nin; i++)
		freein[nfreein++] = inb[i];
	/* main loop: keep the DSP fed; stop on EOS, or after 5 s without any callback */
	{
		double last = now();
		while (!got_eos) {
			while (feed_one())
				;
			if (pump(200, 1))
				last = now();
			else if (now() - last > 5.0) {
				LOG("no callbacks for 5 s: stalled (fed %d/%d frames, %d input buffers free)\n", next_frame, nframes, nfreein);
				break;
			}
			if (play_fd >= 0 && play_bytes >= (long long)(play_secs * out_rate * 4)) {
				LOG("playback limit reached\n");
				break;
			}
		}
	}
	if (!got_eos || !bytes_out)
		rc = 2;
	report(got_eos ? "EOS" : "no EOS");
	write_head(out_ch ? out_ch : si.ch, out_rate ? out_rate : si.sr);

teardown:
	alarm(wd > 20 ? 20 : wd);	/* teardown gets its own budget */
	if (cur_state == ST_EXEC) {
		r = C->SendCommand(h, CMD_STATESET, ST_IDLE, 0);
		LOG("SendCommand(Idle) = %#x\n", r);
		wait_state(ST_IDLE, 4000);
	}
	r = C->SendCommand(h, CMD_STATESET, ST_LOADED, 0);
	LOG("SendCommand(Loaded) = %#x\n", r);
	for (i = 0; i < nin; i++)
		C->FreeBuffer(h, 0, inb[i]);
	for (i = 0; i < nout; i++)
		C->FreeBuffer(h, 1, outb[i]);
	wait_state(ST_LOADED, 4000);
	while (pump(0, 0))
		;
	LOG("OMX_FreeHandle = %#x\n", OMX_FreeHandle(h));
	LOG("OMX_Deinit = %#x\n", OMX_Deinit());
	if (!report_done)
		report("aborted");
	if (play_fd >= 0)
		close(play_fd);
	LOG("exit %d\n", rc);
	return rc;
}
