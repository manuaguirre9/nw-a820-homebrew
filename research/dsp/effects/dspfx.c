/* dspfx: Sony's sound effects (DSEE, VPT, EQ, Clear Stereo, Dynamic Normalizer) on the MP201 audio DSP,
 * driven from our own code. See effects/effects_notes.txt for the reversing behind this.
 *
 * Where the effects live: NOT in a separate OMX component (OMX.NEC.EL.PS is "ME_PowerSave", not post-
 * processing) and NOT in the decoders' vendor parameters. gemini's AudioC library mmaps ONE page of DSP
 * shared memory, phys 0x33EA0000 (just below the decoder output pool at 0x33EA4000), and writes an effect
 * block there with a two-flag handshake; the DSP decoder code (all codecs, FTB or not) polls it:
 *   +0x04 u16  "new settings" flag, ARM sets 1 after writing
 *   +0x08 u16  ARM lock: 1 while the ARM writes, 0 after
 *   +0x0a u16  DSP busy: the ARM spins until it is 0 before writing
 *   +0x0c u16  DSEE ("Dsx"): 0 off, 0x0f on
 *   +0x0e u16  Dynamic Normalizer: 0 / 1
 *   +0x10 u16  VPT: 0xffff off, 0..5 = the six VPT modes
 *   +0x12 s16  volume-linked level from /etc/AudioC_VolTbl (-727 at volume <=14 .. -8508 at 31); 0 in bypass
 *   +0x14 u32  Clear Stereo (channel separation): 0x8001 off, -38 / -34 (two strengths)
 *   +0x18 u32  EQ: 0xfffffffe off; else 7 nibbles b1 b1 b2 b3 b4 b5 b6 (low -> high), band values 0..6,
 *              3 = flat (0x03333333). Presets: Heavy 0x03224322, Pop 0x04332233, Jazz 0x02343322,
 *              Unique 0x01432311
 *   +0x1c u16  0, +0x1e u16 1 for output devices 1/3/5 (0 for headphones)
 * gemini's bypass block (BT / line out): 0, 0, 0xffff, 0, 0x8001, 0xfffffffe.
 * Settings can be committed at any time (gemini does it from its UI thread while playing).
 *
 * Modes:
 *   dspfx page                                 dump the effect page (read-only)
 *   dspfx mp3 [opts] OUT.wav IN.mp3            OMX.NEC.EL.MP3D.FTB, PCM (44.1k S16 stereo) to OUT.wav
 *   dspfx pcm [opts] OUT.wav IN.wav            OMX.NEC.EL.LPCMD.FTB: our 16-bit PCM in, processed PCM out
 *   dspfx pcm [opts] OUT.wav gen:RATE:CH:SECS[:lr]   generated input: log sweep 20 Hz..0.45*RATE at
 *                                              -12 dBFS (lr: sweep on the left channel only)
 *   dspfx mp3r [opts] - IN.mp3                 RENDER mode (listening test): DriverMode 0, Sony's "audio" cfg,
 *                                              OMX.NEC.EL.MP3D (non-FTB): the DSP plays to the headphones
 *                                              itself, nothing comes back (no WAV, no STATS); -s = wall seconds
 * opts:
 *   -x SPEC       commit SPEC before Executing (default: don't touch the page at all = "raw")
 *   -t SEC:SPEC   commit SPEC after SEC seconds of output (live change; stats are split there)
 *   -s SECS       stop after SECS seconds of output (default 20; 0 = whole input)
 *   -P off=val    vendor parameter (u16 at byte offset off of the 0xFF000001 block), before Idle
 *   -r CODE       output rate code, param +6: 4 = 44.1 kHz (default), 3 = 48 kHz
 *   -T SECS       watchdog (default 50)
 *   -a SECS:SPEC  A/B alternation: every SECS seconds (of output; wall time in mp3r) switch between the -x
 *                 block (A, default bypass) and SPEC (B). STATS seg0 = all A periods, seg1 = all B periods
 *                 (the first 0.3 s after each switch are left out: DSP buffering)
 *   -V VOL        mp3r: codec volume 0..100 (76 = 0 dB, default 60)
 * SPEC: comma list, starting from gemini's bypass block:
 *   bypass | dsee=0|1 | dn=0|1 | vpt=off|0..5 | cs=off|1|2|<int> | eq=off|flat|heavy|pop|jazz|unique|
 *   <b1>:<b2>:<b3>:<b4>:<b5>:<b6> (0..6) | eqraw=<hex> | gain=<int> | flag=<int>
 * Output: log on stdout (page dumps every 2 s, FORMAT line, per-segment stats: level, side/mid ratio and
 * a high-frequency ratio - DSEE should raise HF, VPT / Clear Stereo change side/mid, EQ both).
 * Exit 0 = ok, 1 = init/state failure, 2 = stalled, 3 = watchdog.
 * Single-threaded: callbacks (component / receiver threads) only write records into a pipe.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

typedef uint32_t OMX_ERRORTYPE;
typedef void *OMX_HANDLETYPE;
typedef struct {		/* OMX IL 1.0 buffer header, 0x50 bytes */
	uint32_t nSize, nVersion;
	uint8_t *pBuffer;
	uint32_t nAllocLen, nFilledLen, nOffset;
	void *pAppPrivate, *pPlatformPrivate, *pInputPortPrivate, *pOutputPortPrivate;
	void *hMarkTargetComponent, *pMarkData;
	uint32_t nTickCount, pad0;
	int64_t nTimeStamp;
	uint32_t nFlags, nOutputPortIndex, nInputPortIndex, pad1;
} BUFHDR;
_Static_assert(sizeof(BUFHDR) == 0x50, "BUFHDR size");
typedef struct {
	OMX_ERRORTYPE (*EventHandler)(OMX_HANDLETYPE, void *, uint32_t, uint32_t, uint32_t, void *);
	OMX_ERRORTYPE (*EmptyBufferDone)(OMX_HANDLETYPE, void *, BUFHDR *);
	OMX_ERRORTYPE (*FillBufferDone)(OMX_HANDLETYPE, void *, BUFHDR *);
} OMX_CALLBACKTYPE;
typedef struct {
	uint32_t nSize, nVersion;
	void *pComponentPrivate, *pApplicationPrivate;
	void *GetComponentVersion;
	OMX_ERRORTYPE (*SendCommand)(OMX_HANDLETYPE, uint32_t, uint32_t, void *);
	OMX_ERRORTYPE (*GetParameter)(OMX_HANDLETYPE, uint32_t, void *);
	OMX_ERRORTYPE (*SetParameter)(OMX_HANDLETYPE, uint32_t, void *);
	OMX_ERRORTYPE (*GetConfig)(OMX_HANDLETYPE, uint32_t, void *);
	OMX_ERRORTYPE (*SetConfig)(OMX_HANDLETYPE, uint32_t, void *);
	void *GetExtensionIndex;
	OMX_ERRORTYPE (*GetState)(OMX_HANDLETYPE, uint32_t *);
	void *ComponentTunnelRequest, *UseBuffer;
	OMX_ERRORTYPE (*AllocateBuffer)(OMX_HANDLETYPE, BUFHDR **, uint32_t, void *, uint32_t);
	OMX_ERRORTYPE (*FreeBuffer)(OMX_HANDLETYPE, uint32_t, BUFHDR *);
	OMX_ERRORTYPE (*EmptyThisBuffer)(OMX_HANDLETYPE, BUFHDR *);
	OMX_ERRORTYPE (*FillThisBuffer)(OMX_HANDLETYPE, BUFHDR *);
	void *SetCallbacks, *ComponentDeInit;
} OMX_COMPONENTTYPE;
extern OMX_ERRORTYPE OMX_DriverMode(int *mode);
extern OMX_ERRORTYPE OMX_Init(const char *cfg);
extern OMX_ERRORTYPE OMX_Deinit(void);
extern OMX_ERRORTYPE OMX_GetHandle(OMX_HANDLETYPE *h, const char *name, void *app, OMX_CALLBACKTYPE *cb);
extern OMX_ERRORTYPE OMX_FreeHandle(OMX_HANDLETYPE h);

#define IDX_PARAM 0xFF000001u
#define NBUF 24
#define OUTSZ 8192
#define ST_LOADED 1
#define ST_IDLE 2
#define ST_EXEC 3
#define FLAG_EOS 1
#define FX_PHYS 0x33EA0000u
#include <sys/ioctl.h>
#include <linux/soundcard.h>

static double t0;
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
#define LOG(...) do { printf("[%7.3f] ", now() - t0); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

/* ---------- effect page ---------- */
static volatile uint32_t *fxw;	/* 32-bit view (gemini writes the block with word stores) */
static volatile uint16_t *fxh;	/* 16-bit view */
struct fx { uint16_t dsee, dn, vpt; int16_t gain; uint32_t cs, eq; uint16_t w1c, flag; };
static const struct fx FX_BYPASS = { 0, 0, 0xffff, 0, 0x8001, 0xfffffffe, 0, 0 };

static int fx_map(int rw)
{
	int fd = open("/dev/mem", (rw ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) { LOG("open /dev/mem: %s", strerror(errno)); return 0; }
	void *p = mmap(0, 0x1000, rw ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, FX_PHYS);
	close(fd);
	if (p == MAP_FAILED) { LOG("mmap fx page: %s", strerror(errno)); return 0; }
	fxw = p; fxh = p;
	return 1;
}

static void fx_dump(const char *tag)
{
	if (!fxw) return;
	printf("[%7.3f] PAGE %-8s", now() - t0, tag);
	for (int i = 0; i < 16; i++) printf(" %04x", fxh[i]);
	printf(" |");
	for (int i = 16; i < 32; i++) printf(" %04x", fxh[i]);
	putchar('\n'); fflush(stdout);
}

static uint32_t eq_pack(const int b[6])
{
	uint32_t v = b[0] | b[0] << 4;
	for (int i = 1; i < 6; i++) v |= (uint32_t)b[i] << (4 * (i + 1));
	return v;
}

static int fx_parse(const char *spec, struct fx *f)
{
	char buf[256], *save = 0, *tok;
	*f = FX_BYPASS;
	snprintf(buf, sizeof buf, "%s", spec);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(0, ",", &save)) {
		char *v = strchr(tok, '=');
		if (!strcmp(tok, "bypass")) { *f = FX_BYPASS; continue; }
		if (!v) { fprintf(stderr, "bad fx item %s\n", tok); return 0; }
		*v++ = 0;
		if (!strcmp(tok, "dsee")) f->dsee = atoi(v) ? 0x0f : 0;
		else if (!strcmp(tok, "dsx")) f->dsee = strtol(v, 0, 0);	/* raw value */
		else if (!strcmp(tok, "dn")) f->dn = atoi(v);
		else if (!strcmp(tok, "vpt")) f->vpt = !strcmp(v, "off") ? 0xffff : atoi(v);
		else if (!strcmp(tok, "gain")) f->gain = atoi(v);
		else if (!strcmp(tok, "flag")) f->flag = atoi(v);
		else if (!strcmp(tok, "cs")) {
			if (!strcmp(v, "off") || !strcmp(v, "0")) f->cs = 0x8001;
			else if (!strcmp(v, "1")) f->cs = (uint32_t)-38;
			else if (!strcmp(v, "2")) f->cs = (uint32_t)-34;
			else f->cs = (uint32_t)strtol(v, 0, 0);
		} else if (!strcmp(tok, "eqraw")) f->eq = strtoul(v, 0, 16);
		else if (!strcmp(tok, "eq")) {
			int b[6];
			if (!strcmp(v, "off")) f->eq = 0xfffffffe;
			else if (!strcmp(v, "flat")) f->eq = 0x03333333;
			else if (!strcmp(v, "heavy")) f->eq = 0x03224322;
			else if (!strcmp(v, "pop")) f->eq = 0x04332233;
			else if (!strcmp(v, "jazz")) f->eq = 0x02343322;
			else if (!strcmp(v, "unique")) f->eq = 0x01432311;
			else if (sscanf(v, "%d:%d:%d:%d:%d:%d", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) f->eq = eq_pack(b);
			else { fprintf(stderr, "bad eq %s\n", v); return 0; }
		} else { fprintf(stderr, "unknown fx %s\n", tok); return 0; }
	}
	return 1;
}

/* the same sequence as gemini's AudioC commit (0x2c487c in gemini-ICX1115) */
static void fx_commit(const struct fx *f, const char *spec)
{
	if (!fxw) return;
	uint32_t blk[5];
	blk[0] = f->dsee | (uint32_t)f->dn << 16;
	blk[1] = f->vpt | (uint32_t)(uint16_t)f->gain << 16;
	blk[2] = f->cs;
	blk[3] = f->eq;
	blk[4] = f->w1c | (uint32_t)f->flag << 16;
	fx_dump("before");
	fxh[0x08 / 2] = 1;
	double end = now() + 0.5;
	int spins = 0;
	while (fxh[0x0a / 2] != 0 && now() < end) spins++;
	if (fxh[0x0a / 2] != 0) LOG("fx: DSP busy flag +0a stuck at %04x, writing anyway", fxh[0x0a / 2]);
	for (int i = 0; i < 5; i++) fxw[3 + i] = blk[i];
	fxh[0x04 / 2] = 1;
	fxh[0x08 / 2] = 0;
	LOG("fx commit \"%s\": dsee=%x dn=%u vpt=%04x gain=%d cs=%08x eq=%08x flag=%u (spins %d)",
	    spec, f->dsee, f->dn, f->vpt, f->gain, f->cs, f->eq, f->flag, spins);
	fx_dump("after");
}

/* ---------- callbacks -> pipe ---------- */
enum { EV_EVENT = 1, EV_EBD, EV_FBD };
struct ev { uint32_t type, a, b, c; void *p; };
static int evpipe[2];
static void post(uint32_t t, uint32_t a, uint32_t b, uint32_t c, void *p)
{
	struct ev e = { t, a, b, c, p };
	while (write(evpipe[1], &e, sizeof e) < 0 && errno == EINTR) ;
}
static OMX_ERRORTYPE cb_event(OMX_HANDLETYPE h, void *app, uint32_t e, uint32_t d1, uint32_t d2, void *d) { post(EV_EVENT, e, d1, d2, d); return 0; }
static OMX_ERRORTYPE cb_ebd(OMX_HANDLETYPE h, void *app, BUFHDR *b) { post(EV_EBD, 0, 0, 0, b); return 0; }
static OMX_ERRORTYPE cb_fbd(OMX_HANDLETYPE h, void *app, BUFHDR *b) { post(EV_FBD, 0, 0, 0, b); return 0; }

/* ---------- input: MP3 frames ---------- */
static const int br_tab[2][3][16] = {
	{ { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
	  { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
	  { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
	{ { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
	  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
	  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
static const int sr_tab[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };
struct hdr { int ver, layer, sr, len; };
static int parse_hdr(const uint8_t *h, struct hdr *o)
{
	if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0) return 0;
	int ver = (h[1] >> 3) & 3, lay = 4 - ((h[1] >> 1) & 3), bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
	if (ver == 1 || lay == 4 || bri == 0 || bri == 15 || sri == 3) return 0;
	int lsf = ver != 3, sr = sr_tab[ver][sri], kbps = br_tab[lsf][lay - 1][bri];
	o->ver = ver; o->layer = lay; o->sr = sr;
	o->len = lay == 1 ? (12000 * kbps / sr + pad) * 4 : (lay == 3 && lsf ? 72000 : 144000) * kbps / sr + pad;
	return o->len > 4;
}
static int in_fd = -1;
static uint8_t rbuf[65536];
static int rlen, rpos, in_eof;
static struct hdr stream;
static int have_stream;
static int refill(void)
{
	if (rpos > 0) { memmove(rbuf, rbuf + rpos, rlen - rpos); rlen -= rpos; rpos = 0; }
	while (!in_eof && rlen < (int)sizeof rbuf) {
		ssize_t n = read(in_fd, rbuf + rlen, sizeof rbuf - rlen);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { in_eof = 1; break; }
		rlen += n;
	}
	return rlen - rpos;
}
static int next_frame(uint8_t *dst, int room)
{
	for (;;) {
		if (rlen - rpos < 8 && refill() < 4) return 0;
		struct hdr h, h2;
		if (parse_hdr(rbuf + rpos, &h) && (have_stream ? h.layer == stream.layer && h.sr == stream.sr : 1)) {
			if (rlen - rpos < h.len + 4 && refill() < h.len) return 0;
			if (!have_stream && rlen - rpos >= h.len + 4 && !parse_hdr(rbuf + rpos + h.len, &h2)) { rpos++; continue; }
			if (!have_stream) { stream = h; have_stream = 1; }
			if (h.len > room) return -1;
			if (dst) memcpy(dst, rbuf + rpos, h.len);
			rpos += h.len;
			return h.len;
		}
		if (rlen - rpos >= 3 && !memcmp(rbuf + rpos, "TAG", 3) && in_eof && rlen - rpos <= 128) return 0;
		rpos++;
	}
}

/* ---------- input: PCM (WAV file or generator) ---------- */
static int pcm_rate, pcm_ch;
static long pcm_left;		/* bytes of PCM still to read (WAV) / samples frames to generate */
static int gen_mode, gen_lr;
static double gen_phase, gen_secs;
static long gen_pos, gen_total;
static const int rate_codes[] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };	/* codes 3..11 */
static int rate_code(int r) { for (int i = 0; i < 9; i++) if (rate_codes[i] == r) return i + 3; return -1; }

static int wav_open(const char *path)
{
	uint8_t h[12], ck[8];
	in_fd = open(path, O_RDONLY);
	if (in_fd < 0) { LOG("open %s failed", path); return 0; }
	if (read(in_fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { LOG("not a WAV"); return 0; }
	int bits = 0;
	for (;;) {
		if (read(in_fd, ck, 8) != 8) { LOG("no data chunk"); return 0; }
		uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4)) {
			uint8_t f[40];
			int n = len < sizeof f ? len : sizeof f;
			if (read(in_fd, f, n) != n) return 0;
			if (len > (uint32_t)n) lseek(in_fd, len - n + (len & 1), SEEK_CUR);
			int tag = f[0] | f[1] << 8;
			pcm_ch = f[2] | f[3] << 8;
			pcm_rate = f[4] | f[5] << 8 | f[6] << 16 | f[7] << 24;
			bits = f[14] | f[15] << 8;
			if (tag != 1 && tag != 0xfffe) { LOG("WAV format tag %d unsupported", tag); return 0; }
		} else if (!memcmp(ck, "data", 4)) {
			pcm_left = len;
			break;
		} else lseek(in_fd, len + (len & 1), SEEK_CUR);
	}
	if (bits != 16 || pcm_ch < 1 || pcm_ch > 2 || rate_code(pcm_rate) < 0) {
		LOG("WAV %d Hz %d ch %d bit: need 16-bit, 1-2 ch, 8k..48k", pcm_rate, pcm_ch, bits);
		return 0;
	}
	LOG("WAV in: %d Hz %d ch, %ld bytes", pcm_rate, pcm_ch, pcm_left);
	return 1;
}

/* read up to room bytes of PCM (multiple of 2*ch) */
static int pcm_read(uint8_t *dst, int room)
{
	int fb = 2 * pcm_ch, n = 0;
	room -= room % fb;
	if (gen_mode) {
		int16_t *s = (int16_t *)dst;
		double f0 = 20, f1 = 0.45 * pcm_rate, k = log(f1 / f0) / gen_secs;
		while (n + fb <= room && gen_pos < gen_total) {
			double t = (double)gen_pos / pcm_rate, f = f0 * exp(k * t);
			gen_phase += 2 * M_PI * f / pcm_rate;
			if (gen_phase > 2 * M_PI) gen_phase -= 2 * M_PI;
			int16_t v = (int16_t)(8192 * sin(gen_phase));	/* -12 dBFS */
			*s++ = v;
			if (pcm_ch == 2) *s++ = gen_lr ? 0 : v;
			n += fb; gen_pos++;
		}
		return n;
	}
	if (room > pcm_left) room = pcm_left - pcm_left % fb;
	while (n < room) {
		ssize_t r = read(in_fd, dst + n, room - n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		n += r;
	}
	n -= n % fb;
	pcm_left -= n;
	return n;
}

/* ---------- output WAV + stats ---------- */
static int out_fd = -1;
static uint32_t out_bytes, out_limit, out_rate = 44100;
static int out_ch = 2;
static void wav_hdr(uint32_t data)
{
	uint8_t h[44];
	uint32_t br = out_rate * 2 * out_ch;
	memcpy(h, "RIFF", 4); uint32_t v = 36 + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
	v = 16; memcpy(h + 16, &v, 4);
	uint16_t s = 1; memcpy(h + 20, &s, 2); s = out_ch; memcpy(h + 22, &s, 2);
	memcpy(h + 24, &out_rate, 4); memcpy(h + 28, &br, 4); s = 2 * out_ch; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
	memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
	lseek(out_fd, 0, SEEK_SET);
	if (write(out_fd, h, 44) != 44) LOG("wav header write failed");
}

struct stats { double l2, r2, m2, s2, d2, dd2, h11, h16; int peak; double n; int16_t pl, pr, ppl; };
static struct stats seg[2];
static int cur_seg, seg_skip;	/* seg_skip: frames still to leave out after an A/B switch */

/* 4th-order Butterworth high-pass (two RBJ biquads) on the mid signal: energy above ~11.5 kHz / ~16 kHz. The
 * DSP-decoded 22.05 kHz audiobook has nothing above 11 kHz, so DSEE (which rebuilds the top octave) should
 * show up there as a clear rise. */
struct bq { double b0, b1, b2, a1, a2, x1, x2, y1, y2; };
static struct bq hp11[2], hp16[2];
static void bq_hp(struct bq *q, double f0, double fs, double Q)
{
	double w = 2 * M_PI * f0 / fs, c = cos(w), al = sin(w) / (2 * Q), a0 = 1 + al;
	memset(q, 0, sizeof *q);
	q->b0 = (1 + c) / 2 / a0; q->b1 = -(1 + c) / a0; q->b2 = q->b0; q->a1 = -2 * c / a0; q->a2 = (1 - al) / a0;
}
static double bq_run(struct bq *q, double x)
{
	double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
	q->x2 = q->x1; q->x1 = x; q->y2 = q->y1; q->y1 = y;
	return y;
}
static void hp_init(double fs)
{
	bq_hp(&hp11[0], 11500, fs, 0.5412); bq_hp(&hp11[1], 11500, fs, 1.3066);
	bq_hp(&hp16[0], 16000, fs, 0.5412); bq_hp(&hp16[1], 16000, fs, 1.3066);
}
static void stats_add(const int16_t *p, int frames)
{
	struct stats *s = &seg[cur_seg];
	for (int i = 0; i < frames; i++) {
		int l = p[2 * i], r = p[2 * i + 1];
		double m = (l + r) * 0.5, sd = (l - r) * 0.5;
		double y11 = bq_run(&hp11[1], bq_run(&hp11[0], m)), y16 = bq_run(&hp16[1], bq_run(&hp16[0], m));
		if (seg_skip > 0) { seg_skip--; s->pl = l; s->pr = r; s->ppl = (int16_t)m; continue; }
		s->h11 += y11 * y11; s->h16 += y16 * y16;
		double pm = (s->pl + s->pr) * 0.5, ppm = s->ppl;
		double d = m - pm, dd = m - 2 * pm + ppm;
		s->l2 += (double)l * l; s->r2 += (double)r * r; s->m2 += m * m; s->s2 += sd * sd;
		if (s->n > 2) { s->d2 += d * d; s->dd2 += dd * dd; }
		if (abs(l) > s->peak) s->peak = abs(l);
		if (abs(r) > s->peak) s->peak = abs(r);
		s->ppl = (int16_t)pm; s->pl = l; s->pr = r;
		s->n++;
	}
}
static double db(double x) { return x > 0 ? 10 * log10(x) : -999; }
static void stats_print(void)
{
	for (int i = 0; i < 2; i++) {
		struct stats *s = &seg[i];
		if (s->n < 1000) continue;
		double fs = 32768.0 * 32768.0 * s->n;
		printf("STATS seg%d %.2fs: L %.1f dBFS R %.1f dBFS peak %d | side/mid %.1f dB | HF1 %.1f dB HF2 %.1f dB | >11k %.1f dB >16k %.1f dB\n",
		       i, s->n / out_rate, db(s->l2 / fs), db(s->r2 / fs), s->peak, db(s->s2 / s->m2),
		       db(s->d2 / s->m2), db(s->dd2 / s->m2), db(s->h11 / s->m2), db(s->h16 / s->m2));
	}
	printf("(HF1/HF2 = 1st/2nd difference energy of mid relative to mid energy: rises with treble / DSEE;\n"
	       " >11k / >16k = energy of mid above 11.5 / 16 kHz relative to mid energy)\n");
	fflush(stdout);
}

/* ---------- component ---------- */
static OMX_HANDLETYPE h;
static OMX_COMPONENTTYPE *C;
static BUFHDR *inb[NBUF], *outb[NBUF];
static int nin, nout, insz;
static BUFHDR *freein[NBUF]; static int nfreein;
static uint32_t state, n_err, n_fbd;
static int input_done, got_eos, is_pcm;
static double last_ev;

static void handle(struct ev *e);
static int pump(int ms)
{
	struct pollfd pf = { evpipe[0], POLLIN, 0 };
	struct ev e;
	if (poll(&pf, 1, ms) <= 0) return 0;
	if (read(evpipe[0], &e, sizeof e) != sizeof e) return 0;
	last_ev = now();
	handle(&e);
	return 1;
}
static int wait_state(uint32_t st, int ms)
{
	double end = now() + ms / 1000.0;
	while (state != st && now() < end) pump(50);
	if (state != st) LOG("timeout waiting for state %u (have %u)", st, state);
	return state == st;
}
static int set_state(uint32_t st)
{
	OMX_ERRORTYPE r = C->SendCommand(h, 0, st, 0);
	if (r) { LOG("SendCommand(%u) = %#x", st, r); return 0; }
	return 1;
}

static int stop_at_limit;
static struct fx tfx; static double t_toggle = -1; static const char *t_spec; static int toggled;
static struct fx afx, xfx_used; static double a_period, a_next; static const char *a_spec, *x_spec_used = "bypass";
static int a_state;
/* A/B alternation step at time secs (output seconds, or wall seconds in mp3r) */
static void alt_step(double secs)
{
	if (a_period <= 0 || secs < a_next) return;
	a_state = !a_state;
	a_next += a_period;
	if (a_state) fx_commit(&afx, a_spec); else fx_commit(&xfx_used, x_spec_used);
	cur_seg = a_state;
	seg_skip = (int)(0.3 * out_rate);
	printf("[%7.3f] AB %s at %.2f s\n", now() - t0, a_state ? "B" : "A", secs);
	fflush(stdout);
}

static void handle(struct ev *e)
{
	if (e->type == EV_EVENT) {
		if (e->a == 0 && e->b == 0) { state = e->c; LOG("state -> %u", state); }
		else if (e->a == 1) { n_err++; LOG("error event %#x", e->b); }
		else LOG("event %u %#x %#x", e->a, e->b, e->c);
	} else if (e->type == EV_EBD) {
		if (nfreein < NBUF) freein[nfreein++] = e->p;
	} else if (e->type == EV_FBD) {
		BUFHDR *b = e->p;
		n_fbd++;
		if (state != ST_EXEC || stop_at_limit) return;	/* after stop: just keep it */
		uint32_t n = b->nFilledLen > b->nAllocLen ? b->nAllocLen : b->nFilledLen;
		int eos = b->nFlags & FLAG_EOS;
		if (n_fbd == 1) LOG("first FBD: %u bytes flags %#x", n, b->nFlags);
		if (out_limit && out_bytes + n > out_limit) n = out_limit - out_bytes;
		n &= ~3u;
		if (n) {
			stats_add((const int16_t *)(b->pBuffer + b->nOffset), n / 4);
			if (write(out_fd, b->pBuffer + b->nOffset, n) != (ssize_t)n) LOG("wav write failed");
			out_bytes += n;
		}
		double secs = (double)out_bytes / (out_rate * 4);
		if (t_toggle >= 0 && !toggled && secs >= t_toggle) { toggled = 1; cur_seg = 1; fx_commit(&tfx, t_spec); }
		alt_step(secs);
		if (eos) { got_eos = 1; LOG("eos after %.2f s of output", secs); return; }
		if (out_limit && out_bytes >= out_limit) { stop_at_limit = 1; LOG("output limit reached (%.2f s)", secs); return; }
		b->nFilledLen = 0; b->nFlags = 0;
		C->FillThisBuffer(h, b);
	}
}

static void feed(void)
{
	while (!input_done && nfreein && !stop_at_limit) {
		BUFHDR *b = freein[--nfreein];
		int n = 0, k = 0;
		if (is_pcm) {
			n = pcm_read(b->pBuffer, insz);
			if (n < insz) input_done = 1;
			if (n == 0) { memset(b->pBuffer, 0, 2 * pcm_ch); n = 2 * pcm_ch; }
		} else {
			while ((k = next_frame(b->pBuffer + n, insz - n)) > 0) n += k;
			if (k == 0) input_done = 1;
			if (n == 0 && !input_done) { freein[nfreein++] = b; break; }
			if (!n) { b->pBuffer[0] = 0; n = 1; }
		}
		b->nFilledLen = n; b->nOffset = 0; b->nTickCount = 0; b->nTimeStamp = 0;
		b->nFlags = input_done ? FLAG_EOS : 0;
		OMX_ERRORTYPE r = C->EmptyThisBuffer(h, b);
		if (r) { LOG("ETB(%d) = %#x", n, r); freein[nfreein++] = b; input_done = 1; break; }
	}
}

static int teardown_started;
static void teardown(void)
{
	if (teardown_started++) return;
	if (C) {
		if (state == ST_EXEC) { set_state(ST_IDLE); wait_state(ST_IDLE, 4000); }
		if (state == ST_IDLE) {
			set_state(ST_LOADED);
			for (int i = 0; i < nin; i++) C->FreeBuffer(h, 0, inb[i]);
			for (int i = 0; i < nout; i++) C->FreeBuffer(h, 1, outb[i]);
			wait_state(ST_LOADED, 4000);
		}
		OMX_FreeHandle(h);
	}
	OMX_Deinit();
}
static void on_alarm(int s)
{
	(void)s;
	LOG("watchdog");
	if (out_fd >= 0) wav_hdr(out_bytes);
	fx_dump("watchdog");
	_exit(3);	/* no OMX calls from a signal handler; the next OMX_Init/gemini reloads the DSP */
}

static void usage(void)
{
	fprintf(stderr, "usage: dspfx page | dspfx mp3|pcm|mp3r [-x SPEC] [-t SEC:SPEC] [-a SEC:SPEC] [-s SECS] [-P off=val] [-r 3|4] [-T SECS] [-V VOL] OUT.wav|- IN\n");
	exit(1);
}

int main(int argc, char **argv)
{
	t0 = now();
	setvbuf(stdout, 0, _IOLBF, 0);
	if (argc < 2) usage();
	if (!strcmp(argv[1], "page")) { if (!fx_map(0)) return 1; fx_dump("now"); return 0; }
	is_pcm = !strcmp(argv[1], "pcm");
	int render = !strcmp(argv[1], "mp3r"), vol = 60;
	if (!is_pcm && !render && strcmp(argv[1], "mp3")) usage();
	const char *xspec = 0;
	struct fx xfx;
	int npar = 0, poff[16], pval[16], outcode = 4, wd = 50;
	double secs = 20;
	int o;
	optind = 2;
	while ((o = getopt(argc, argv, "x:t:a:s:P:r:T:V:")) != -1) {
		switch (o) {
		case 'x': xspec = optarg; if (!fx_parse(xspec, &xfx)) return 1; break;
		case 't': {
			char *c = strchr(optarg, ':');
			if (!c) usage();
			t_toggle = atof(optarg); t_spec = c + 1;
			if (!fx_parse(t_spec, &tfx)) return 1;
			break;
		}
		case 'a': {
			char *c = strchr(optarg, ':');
			if (!c) usage();
			a_period = atof(optarg); a_spec = c + 1; a_next = a_period;
			if (a_period <= 0 || !fx_parse(a_spec, &afx)) return 1;
			break;
		}
		case 'V': vol = atoi(optarg); break;
		case 's': secs = atof(optarg); break;
		case 'P': if (npar < 16 && sscanf(optarg, "%i=%i", &poff[npar], &pval[npar]) == 2) npar++; break;
		case 'r': outcode = atoi(optarg); break;
		case 'T': wd = atoi(optarg); break;
		default: usage();
		}
	}
	if (argc - optind != 2) usage();
	const char *outp = argv[optind], *inp = argv[optind + 1];
	out_rate = outcode == 3 ? 48000 : 44100;
	out_limit = secs > 0 ? (uint32_t)(secs * out_rate) * 4 : 0;
	signal(SIGALRM, on_alarm);
	alarm(wd);

	/* input */
	if (is_pcm) {
		if (!strncmp(inp, "gen:", 4)) {
			char lr[8] = "";
			if (sscanf(inp + 4, "%d:%d:%lf:%7s", &pcm_rate, &pcm_ch, &gen_secs, lr) < 3 || rate_code(pcm_rate) < 0 ||
			    pcm_ch < 1 || pcm_ch > 2) { LOG("bad generator %s", inp); return 1; }
			gen_mode = 1; gen_lr = !strcmp(lr, "lr"); gen_total = (long)(gen_secs * pcm_rate);
			LOG("generator: log sweep %d Hz %d ch %.1f s%s", pcm_rate, pcm_ch, gen_secs, gen_lr ? " (left only)" : "");
		} else if (!wav_open(inp)) return 1;
		insz = 0x3000;
	} else {
		in_fd = open(inp, O_RDONLY);
		if (in_fd < 0) { LOG("open %s failed", inp); return 1; }
		if (next_frame(0, 1 << 20) <= 0) { LOG("no MP3 frame"); return 1; }
		lseek(in_fd, 0, SEEK_SET); rlen = rpos = in_eof = 0;
		LOG("MP3 in: MPEG ver %d layer %d %d Hz", stream.ver, stream.layer, stream.sr);
		insz = 4096;
	}
	if (!render) {
		out_fd = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out_fd < 0) { LOG("open %s failed", outp); return 1; }
		wav_hdr(0);
	}
	hp_init(out_rate);
	if (xspec) { xfx_used = xfx; x_spec_used = xspec; } else fx_parse("bypass", &xfx_used);

	/* effect page */
	if (xspec || t_spec || a_spec) { if (!fx_map(1)) return 1; }
	else fx_map(0);
	fx_dump("start");

	if (pipe(evpipe) < 0) return 1;
	int mode = render ? 0 : 1;	/* 0: the OMF core owns /dev/sound/dsp and the DSP renders by itself */
	OMX_DriverMode(&mode);
	OMX_ERRORTYPE r = OMX_Init(render ? "/usr/lib/omf/omx_nw_walkman_audio.cfg" : "/usr/lib/omf/omx_nw_walkman_audio_ftb.cfg");
	LOG("OMX_Init = %#x (driver mode %d)", r, mode);
	if (r) return 1;
	fx_dump("init");
	if (render) {	/* the core opened the codec; set a moderate volume for the listening test */
		int mfd = open("/dev/sound/mixer", O_RDWR);
		int lr = vol | vol << 8;
		if (mfd >= 0) { LOG("mixer volume %d: %s", vol, ioctl(mfd, SOUND_MIXER_WRITE_VOLUME, &lr) < 0 ? "failed" : "ok"); close(mfd); }
	}
	const char *name = is_pcm ? "OMX.NEC.EL.LPCMD.FTB" : render ? "OMX.NEC.EL.MP3D" : "OMX.NEC.EL.MP3D.FTB";
	OMX_CALLBACKTYPE cb = { cb_event, cb_ebd, cb_fbd };
	r = OMX_GetHandle(&h, name, 0, &cb);
	LOG("GetHandle(%s) = %#x", name, r);
	if (r || !h) { OMX_Deinit(); return 1; }
	C = h;
	C->GetState(h, &state);

	uint16_t p[16] = { 0 };
	r = C->GetParameter(h, IDX_PARAM, p);
	printf("PARAM default (r=%#x):", r); for (int i = 0; i < 11; i++) printf(" +%x=%u", 2 * i, p[i]); putchar('\n');
	if (is_pcm) {
		p[3] = outcode;			/* +6 output rate 3 = 48k / 4 = 44.1k */
		p[4] = 6;			/* +8 6 = little-endian input (7 = big-endian, component swaps) */
		p[5] = pcm_ch;			/* +a input channels */
		p[6] = rate_code(pcm_rate);	/* +c input rate code 3..11 */
	} else {
		p[2] = stream.ver == 3 ? 1 : stream.ver == 2 ? 2 : 3;
		p[3] = outcode;
		p[9] = 0x27;
	}
	for (int i = 0; i < npar; i++) if (poff[i] >= 0 && poff[i] < 32) p[poff[i] / 2] = pval[i];
	r = C->SetParameter(h, IDX_PARAM, p);
	C->GetParameter(h, IDX_PARAM, p);
	printf("PARAM set (r=%#x):", r); for (int i = 0; i < 11; i++) printf(" +%x=%u", 2 * i, p[i]); putchar('\n');
	{
		uint16_t c[16] = { 0 };
		r = C->GetConfig(h, 0xFF000002u, c);
		printf("CONFIG 0xFF000002 (r=%#x):", r); for (int i = 0; i < 6; i++) printf(" %u", c[i]); putchar('\n');
	}

	int ok = 1;
	if (!set_state(ST_IDLE)) ok = 0;
	for (int i = 0; ok && i < NBUF; i++) if (!C->AllocateBuffer(h, &inb[i], 0, 0, insz)) nin++;
	for (int i = 0; ok && !render && i < NBUF; i++) if (!C->AllocateBuffer(h, &outb[i], 1, 0, OUTSZ)) nout++;	/* render: no output port */
	LOG("buffers %d x %d in, %d x %d out", nin, insz, nout, OUTSZ);
	if (!ok || !wait_state(ST_IDLE, 5000)) { teardown(); return 1; }
	if (xspec) fx_commit(&xfx, xspec);
	if (!set_state(ST_EXEC) || !wait_state(ST_EXEC, 5000)) { teardown(); return 1; }
	fx_dump("exec");
	printf("FORMAT %u 2 S16LE (%s)\n", out_rate, name);
	for (int i = 0; i < nout; i++) { outb[i]->nFilledLen = 0; outb[i]->nFlags = 0; C->FillThisBuffer(h, outb[i]); }
	for (int i = 0; i < nin; i++) freein[nfreein++] = inb[i];

	double t_start = now(), next_dump = t_start + 2;
	last_ev = t_start;
	int rc = 0;
	if (render) printf("RENDER: listen now; AB lines below give the switch times (A = -x block, B = -a block)\n");
	while (!got_eos && !stop_at_limit) {
		feed();
		pump(100);
		while (pump(0)) ;
		if (render) {	/* nothing comes back: time is wall time */
			double w = now() - t_start;
			alt_step(w);
			if (secs > 0 && w >= secs) { LOG("render: %.1f s played", w); break; }
			if (input_done && nfreein == nin && now() - last_ev > 2) { LOG("render: input finished"); break; }
		}
		if (n_err > 5) { LOG("too many DSP errors"); rc = 2; break; }
		if (!render && now() - last_ev > 5) { LOG("stalled: no callbacks for 5 s"); rc = 2; break; }
		if (render && !input_done && now() - last_ev > 5) { LOG("render stalled: no input consumed for 5 s"); rc = 2; break; }
		if (now() > next_dump) { fx_dump("run"); next_dump += 2; }
	}
	double el = now() - t_start;
	LOG("decoded %.2f s of audio in %.2f s wall (%.1fx realtime), FBDs %u, errors %u",
	    (double)out_bytes / (out_rate * 4), el, (double)out_bytes / (out_rate * 4) / (el > 0 ? el : 1), n_fbd, n_err);
	fx_dump("end");
	if (!render) {
		wav_hdr(out_bytes);
		close(out_fd); out_fd = -1;
		stats_print();
	}
	teardown();
	LOG("bye rc=%d", rc);
	return rc;
}
