/* dspdec: decoding and Sony effects on the MP201's audio DSP for hbplayer (research/dsp: FINDINGS.md,
 * notes/mp3d_notes.txt, notes/atrac_notes.txt, notes/aac_wma_notes.txt, notes/direct_notes.txt,
 * effects/effects_notes.txt, notes/player_dsp_notes.txt).
 *
 * A helper process: the player writes commands on our stdin and reads packets from our stdout. We drive NEC's
 * OpenMAX IL components in one of two core modes (OMX_DriverMode + OMX_Init with one of Sony's cfgs; switching
 * = OMX_Deinit + OMX_Init, ~0.3-0.5 s, the DSP firmware is reloaded):
 *   FTB    (driver mode 1, omx_nw_walkman_audio_ftb.cfg): "*.FTB" components hand the decoded PCM back to us
 *          (24 x 8192 output buffers), we pass it to the player, which plays it itself.
 *   direct (driver mode 0, omx_nw_walkman_audio.cfg): the plain components (only an input port). The OMF core
 *          opens /dev/sound/dsp itself at OMX_Init; at Idle->Executing it switches the PCM DMA interrupt to the
 *          DSP (PCM_DMA_INT_SET 1, S32 format) and the DSP decodes AND plays to the codec: the ARM only feeds
 *          compressed data ("maximum battery"). No PCM comes back; positions come from the input buffers the
 *          DSP has consumed (EmptyBufferDone), the end from the component's EventBufferFlag.
 *          While we are in this mode the player must not open /dev/sound/dsp (X releases it).
 *
 *   codec  FTB component            direct component     input buffers  framing
 *   mp3    OMX.NEC.EL.MP3D.FTB      OMX.NEC.EL.MP3D      24 x 4096      whole MPEG frames per buffer (byte-swapped)
 *   at3    OMX.SONY.AT3D.FTB        OMX.SONY.AT3D        24 x 2048      one ATRAC3 frame per buffer
 *   atx    OMX.SONY.ATXD.FTB        OMX.SONY.ATXD        24 x 2048      one ATRAC3plus frame per buffer
 *   aal    OMX.SONY.AALD.FTB        OMX.SONY.AALD        24 x 12288     one "BLK" block per buffer - UNTESTED
 *   pcm    OMX.NEC.EL.LPCMD.FTB     -                    24 x 12288     our S16 stereo (Sony effects on anything)
 *   aac    OMX.NEC.EL.AACD.FTB      OMX.NEC.EL.AACD      24 x 2048      one access unit per buffer (raw, +8 = 0) or
 *                                                                       one ADTS frame (+8 = 2) - UNTESTED
 *   wma    OMX.NEC.EL.WMAD.FTB      OMX.NEC.EL.WMAD      24 x 4096      ASF payloads (asf.h) - UNTESTED
 *
 * Commands (text lines, except the payloads of D and A):
 *   O <seq> <offset> [<codec>] <path>   FTB decode of <path> (codec mp3|at3|atx|aal|aac|wma; omitted = mp3) from
 *                             byte <offset> (mp3: frame search from there; ATRAC: rounded to a frame; aac = ADTS
 *                             file: frame search; wma: rounded to an ASF packet). PCM packets carry <seq>.
 *   R <seq> <offset> <codec> <path>     the same as a DIRECT session (the DSP plays it). POS packets report the
 *                             progress; EOS at the end. Pause with Z, stop with S (stays in direct mode), X to
 *                             leave direct mode.
 *   M <seq> <direct> <rate> <ch> <aot> <sbr> <kbps> <adts>   AAC access-unit stream (MP4 demuxed by the player):
 *                             then "A <len>\n<AU>" per access unit and "E" after the last one. FTB (direct 0): PCM
 *                             packets + ACK packets (AUs the DSP has taken); direct 1: POS packets (off = AUs).
 *   P <seq> <rate>            start a PCM effect stream (LPCMD.FTB): 16-bit stereo at <rate> in, 44.1 kHz out
 *   D <len>\n<len bytes>      PCM for the running P stream (multiple of 4 bytes, <= 65536)
 *   Z <1|0>                   pause / resume the direct session
 *   S                         stop (component back to Idle; buffers held by the DSP are dropped)
 *   X <seq>                   leave direct mode (OMX_Deinit: /dev/sound/dsp is closed); answered by a REL packet
 *   Q                         quit (full teardown, OMX_Deinit)
 *   F <vpt> <eqhex> <dn>      Sony effects through the DSP effect page (phys 0x33EA0000, gemini's AudioC
 *                             handshake): vpt 0xffff off / 0..5, eq 0xfffffffe off or packed nibbles, dn 0/1
 *   G <dsee> <gain> <cs>      the other page fields (hex): DSEE 0 / 0xf, level s16, Clear Stereo u32
 * Packets on stdout: struct pkt { u32 magic 'DSPC', u32 seq, u32 len, u32 flags } + len bytes.
 *   flags 1 EOS, 2 error, 4 ready (once after start), 8 protected / unsupported (with 2), 16 POS (payload struct
 *   dpos: ms / off / ebds / state of a direct session), 32 ACK (payload u32: AUs consumed, AU streams in FTB
 *   mode), 64 REL (X done: /dev/sound/dsp is free). Anything without POS/ACK/REL and len > 0 is S16 PCM.
 *
 * Test modes (no player; LOG may be "-" = stderr):
 *   dspdec LOG -d CODEC IN OUT.raw [-F firstflags] [-s secs] [-A 0|2] [-P off=val]...   FTB decode to raw S16LE
 *          44.1 kHz stereo, prints FORMAT / RESULT / STATS. CODEC mp3 at3 atx aal aac wma (aac: .m4a/.mp4 through
 *          the built-in MP4 demuxer as an access-unit stream, anything else as ADTS; -A 2 = ADTS frames whole with
 *          +8 = 2, -A 0 = headers stripped, +8 = 0 (default)).
 *   dspdec LOG -r CODEC IN [-s secs] [-V vol] [-G dsee,gain,cs] [-a secs] [-p t] [-j t] [-A 0|2] [-P off=val]...
 *          DIRECT playback test: plays IN on the headphones for secs (default 10) and logs once a second the
 *          position from EmptyBufferDones (pace = played / wall: ~1.0 means the DSP is paced by the DAC), the
 *          PCM DMA channel (M2P LCH4 source pointer: moving through DSP memory = the DSP feeds the codec), PCM0's
 *          TX enable and the effect page (+0x1c = 1: DSEE active). -V codec volume 0..100 (76 = 0 dB); -G page
 *          values at the start; -a toggles DSEE every secs; -p pauses 2 s at t; -j restarts at 50 % at t (seek).
 */
#include "oma.h"
#include "aacinfo.h"
#include "asf.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/time.h>
#include <unistd.h>
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

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
	void *GetConfig, *SetConfig, *GetExtensionIndex;
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
#define MAXIN 0x3000
#define OUTSZ 8192
#define ST_LOADED 1
#define ST_IDLE 2
#define ST_EXEC 3
#define ST_PAUSE 4
#define EV_CMDCOMPLETE 0
#define EV_ERROR 1
#define EV_BUFFERFLAG 4
#define FLAG_EOS 1
#define FLAG_DISCONT 0x10000u	/* gemini: first buffer after start / seek (ATRAC separators) */
#define ERR_UNDERFLOW 0x80001007u
#define PKT_MAGIC 0x43505344u	/* "DSPC" */
enum { PF_EOS = 1, PF_ERR = 2, PF_READY = 4, PF_PROT = 8, PF_POS = 16, PF_ACK = 32, PF_REL = 64 };
enum { C_MP3, C_AT3, C_ATX, C_AAL, C_PCM, C_AAC, C_WMA, C_N };
static const struct { const char *name, *comp, *dcomp; int insz; } codecs[C_N] = {
	{ "mp3", "OMX.NEC.EL.MP3D.FTB", "OMX.NEC.EL.MP3D", 4096 },
	{ "at3", "OMX.SONY.AT3D.FTB", "OMX.SONY.AT3D", 0x800 },
	{ "atx", "OMX.SONY.ATXD.FTB", "OMX.SONY.ATXD", 0x800 },
	{ "aal", "OMX.SONY.AALD.FTB", "OMX.SONY.AALD", 0x3000 },
	{ "pcm", "OMX.NEC.EL.LPCMD.FTB", "OMX.NEC.EL.LPCMD", 0x3000 },
	{ "aac", "OMX.NEC.EL.AACD.FTB", "OMX.NEC.EL.AACD", 0x800 },
	{ "wma", "OMX.NEC.EL.WMAD.FTB", "OMX.NEC.EL.WMAD", 0x1000 },
};
#define CFG_FTB "/usr/lib/omf/omx_nw_walkman_audio_ftb.cfg"
#define CFG_DIRECT "/usr/lib/omf/omx_nw_walkman_audio.cfg"

static FILE *logfp;
static double t0;
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
#define LOG(...) do { if (logfp) { fprintf(logfp, "[%8.3f] ", now() - t0); fprintf(logfp, __VA_ARGS__); fputc('\n', logfp); fflush(logfp); } } while (0)

static volatile sig_atomic_t term_req;
static void on_term(int s) { (void)s; term_req = 1; }	/* only a flag: no OMX calls in a signal handler */
static void teardown_exit(int rc);

/* ---------- callbacks -> pipe (they run on the component / receiver threads) ---------- */
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

/* ---------- output packets ---------- */
struct pkt { uint32_t magic, seq, len, flags; };
struct dpos { uint32_t ms, off, ebds, state; };	/* state: 1 paused, 2 all input sent */
static int out_fd = 1, test_mode;
static int write_all(int fd, const void *p, size_t n)
{
	const char *c = p;
	while (n) {
		ssize_t w = write(fd, c, n);
		if (w < 0) { if (errno == EINTR && !term_req) continue; return -1; }
		c += w; n -= w;
	}
	return 0;
}
static void send_pkt(uint32_t seq, uint32_t flags, const void *data, uint32_t len)
{
	if (test_mode) { if (len && !(flags & (PF_POS | PF_ACK)) && write_all(out_fd, data, len)) teardown_exit(1); return; }
	struct pkt p = { PKT_MAGIC, seq, len, flags };
	if (write_all(1, &p, sizeof p) || (len && write_all(1, data, len))) { LOG("stdout closed / terminated, quitting"); teardown_exit(0); }
}
/* a small packet without blocking (positions of a direct session: the player may not read for a while, and a
 * blocked helper would starve the DSP). 0 = sent */
static int try_send_pkt(uint32_t seq, uint32_t flags, const void *data, uint32_t len)
{
	if (test_mode) return 0;
	unsigned char b[64];
	struct pkt p = { PKT_MAGIC, seq, len, flags };
	if (len > sizeof b - sizeof p) return -1;
	memcpy(b, &p, sizeof p); memcpy(b + sizeof p, data, len);
	int fl = fcntl(1, F_GETFL);
	fcntl(1, F_SETFL, fl | O_NONBLOCK);
	ssize_t w = write(1, b, sizeof p + len);	/* <= PIPE_BUF: all or nothing */
	fcntl(1, F_SETFL, fl);
	return w == (ssize_t)(sizeof p + len) ? 0 : -1;
}

/* ---------- input file ---------- */
static int in_fd = -1;
static uint8_t rbuf[65536];
static int rlen, rpos, in_eof;
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
/* file offset of rbuf[rpos] */
static long rd_off(void) { return in_fd >= 0 ? (long)lseek(in_fd, 0, SEEK_CUR) - (rlen - rpos) : 0; }

/* MP3 frames */
static const int br_tab[2][3][16] = {
	{ { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
	  { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
	  { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
	{ { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
	  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
	  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
static const int sr_tab[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };
struct hdr { int ver, layer, sr, len, kbps; };
static int parse_hdr(const uint8_t *h, struct hdr *o)
{
	if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0) return 0;
	int ver = (h[1] >> 3) & 3, lay = 4 - ((h[1] >> 1) & 3), bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
	if (ver == 1 || lay == 4 || bri == 0 || bri == 15 || sri == 3) return 0;
	int lsf = ver != 3, sr = sr_tab[ver][sri], kbps = br_tab[lsf][lay - 1][bri];
	o->ver = ver; o->layer = lay; o->sr = sr; o->kbps = kbps;
	o->len = lay == 1 ? (12000 * kbps / sr + pad) * 4 : (lay == 3 && lsf ? 72000 : 144000) * kbps / sr + pad;
	return o->len > 4;
}
static struct hdr stream;	/* the first frame found: later frames must match its layer / rate */
static int have_stream;
static int mp3_spf(void) { return stream.layer == 1 ? 384 : stream.layer == 3 && stream.ver != 3 ? 576 : 1152; }

/* next whole frame into dst (max room bytes); returns its length, 0 at the end, -1 if it doesn't fit */
static int mp3_next(uint8_t *dst, int room)
{
	for (;;) {
		if (rlen - rpos < 8 && refill() < 4) return 0;
		struct hdr h, h2;
		if (parse_hdr(rbuf + rpos, &h) && (have_stream ? h.layer == stream.layer && h.sr == stream.sr : 1)) {
			if (rlen - rpos < h.len + 4 && refill() < h.len) return 0;	/* truncated last frame */
			/* the very first frame must be followed by another header (avoids locking onto noise) */
			if (!have_stream && rlen - rpos >= h.len + 4 && !parse_hdr(rbuf + rpos + h.len, &h2)) { rpos++; continue; }
			if (!have_stream) { stream = h; have_stream = 1; }
			if (h.len > room) return -1;
			memcpy(dst, rbuf + rpos, h.len);
			rpos += h.len;
			return h.len;
		}
		if (rlen - rpos >= 3 && !memcmp(rbuf + rpos, "TAG", 3) && in_eof && rlen - rpos <= 128) return 0;
		rpos++;	/* resync */
	}
}

/* ATRAC3 / ATRAC3plus: fixed-size frames after the EA3 header */
static struct oma_info oma;
static long frames_left;	/* whole frames still in the file from the read position */
static int at_next(uint8_t *dst, int room)
{
	if (frames_left <= 0) return 0;
	if (oma.frame > room) return -1;
	if (rlen - rpos < oma.frame && refill() < oma.frame) { frames_left = 0; return 0; }
	memcpy(dst, rbuf + rpos, oma.frame);
	rpos += oma.frame;
	frames_left--;
	return oma.frame;
}

/* ATRAC Advanced Lossless: "BLK" blocks, 24-byte header (BE16 at +4 = payload bytes), found at 8-byte steps */
static int aal_next(uint8_t *dst, int room)
{
	for (;;) {
		if (rlen - rpos < 24 && refill() < 24) return 0;
		if (!memcmp(rbuf + rpos, "BLK", 3)) {
			int len = (rbuf[rpos + 4] << 8 | rbuf[rpos + 5]) + 24;
			if (len > room) { LOG("aal: block of %d bytes > %d", len, room); return 0; }
			if (rlen - rpos < len && refill() < len) return 0;
			memcpy(dst, rbuf + rpos, len);
			rpos += len;
			return len;
		}
		rpos += 8;
	}
}

/* AAC in ADTS: one frame per buffer, whole (adts_whole: +8 = 2) or the raw access unit without the header */
static struct aac_info aac;
static int adts_whole;		/* -A 2 in the test modes: ADTS frames with their header, +8 = 2 */
static int aac_kbps;
static int adts_next(uint8_t *dst, int room)
{
	for (int skipped = 0; skipped < 65536; skipped++) {
		if (rlen - rpos < 9 && refill() < 9) return 0;
		struct aac_info a; int hl;
		int len = aac_parse_adts(rbuf + rpos, &a, &hl);
		if (len && a.rate == aac.rate && a.channels == aac.channels) {
			if (rlen - rpos < len && refill() < len) return 0;	/* truncated last frame */
			int n = adts_whole ? len : len - hl;
			if (n > room) { rpos += len; continue; }	/* > 2048: not a real frame */
			memcpy(dst, rbuf + rpos + (adts_whole ? 0 : hl), n);
			rpos += len;
			if (n) return n;	/* an empty frame (header only): the next one, without recursing */
		}
		if (rlen - rpos >= 3 && !memcmp(rbuf + rpos, "TAG", 3) && in_eof && rlen - rpos <= 128) return 0;
		rpos++;
	}
	return 0;
}

/* AAC access units from the player ("A" commands) or from the test demuxer: slots of <= 2048 bytes */
#define AQ_SLOTS 256
static uint8_t (*aq)[2048];
static uint16_t aq_len[AQ_SLOTS];
static int aq_r, aq_n, aq_end;	/* read slot, count, "E" received */
static long aq_taken;		/* AUs put into DSP buffers since the stream started */
static int aq_put(const uint8_t *d, int n)
{
	if (!aq && !(aq = malloc(AQ_SLOTS * sizeof *aq))) return -1;
	if (aq_n >= AQ_SLOTS || n <= 0 || n > 2048) return n > 2048 || n <= 0 ? 0 : -1;	/* oversized: dropped */
	int w = (aq_r + aq_n) % AQ_SLOTS;
	memcpy(aq[w], d, n); aq_len[w] = n; aq_n++;
	return 0;
}
static int aq_next(uint8_t *dst, int room)
{
	if (!aq_n) return 0;
	int n = aq_len[aq_r];
	if (n > room) n = room;
	memcpy(dst, aq[aq_r], n);
	aq_r = (aq_r + 1) % AQ_SLOTS; aq_n--; aq_taken++;
	return n;
}

/* test modes: MP4 demuxed here (the player demuxes itself and sends "A") */
static MP4D_demux_t tmp4;
static FILE *tmp4f;
static int tmp4_track = -1, tmp4_sample, tmp4_count;
static int tmp4_read(int64_t off, void *buf, size_t size, void *tok)
{
	FILE *f = tok;
	if (fseek(f, (long)off, SEEK_SET)) return 1;
	return fread(buf, 1, size, f) != size;
}
static int tmp4_open(const char *path)
{
	tmp4f = fopen(path, "rb");
	if (!tmp4f) return -1;
	fseek(tmp4f, 0, SEEK_END); long sz = ftell(tmp4f); fseek(tmp4f, 0, SEEK_SET);
	if (!MP4D_open(&tmp4, tmp4_read, tmp4f, sz)) return -1;
	for (unsigned i = 0; i < tmp4.track_count; i++)
		if (tmp4.track[i].handler_type == MP4D_HANDLER_TYPE_SOUN && tmp4.track[i].dsi_bytes) { tmp4_track = i; break; }
	if (tmp4_track < 0) return -1;
	MP4D_track_t *t = &tmp4.track[tmp4_track];
	if (aac_parse_asc(t->dsi, t->dsi_bytes, &aac)) return -1;
	tmp4_count = t->sample_count; tmp4_sample = 0;
	aac_kbps = t->avg_bitrate_bps / 1000;
	return 0;
}
static void tmp4_fill(void)	/* keep the queue topped up */
{
	static uint8_t b[4096];
	while (tmp4f && aq_n < 64 && !aq_end) {
		if (tmp4_sample >= tmp4_count) { aq_end = 1; break; }
		unsigned bytes, ts, dur;
		MP4D_file_offset_t off = MP4D_frame_offset(&tmp4, tmp4_track, tmp4_sample++, &bytes, &ts, &dur);
		if (bytes > sizeof b || fseek(tmp4f, (long)off, SEEK_SET) || fread(b, 1, bytes, tmp4f) != bytes) continue;
		aq_put(b, bytes);
	}
}

/* WMA in ASF (asf.h): the audio stream's payload bytes, whole blocks (nBlockAlign) per buffer when they fit */
static struct asf_info asf;
static long asf_packets_left;
static uint8_t wfifo[65536 + 16384];
static int wf_len;
static int wma_next(uint8_t *dst, int room)
{
	int want = asf.align > room ? room : (room / asf.align) * asf.align;
	while (wf_len < want && asf_packets_left > 0 && wf_len + asf.packet_size <= (int)sizeof wfifo) {
		if (rlen - rpos < asf.packet_size && refill() < asf.packet_size) { asf_packets_left = 0; break; }
		asf_packets_left--;
		int n = asf_packet_payload(&asf, rbuf + rpos, wfifo + wf_len, (int)sizeof wfifo - wf_len);
		rpos += asf.packet_size;
		if (n < 0) LOG("wma: bad packet");
		else wf_len += n;
	}
	int k = wf_len < want ? wf_len : want;
	if (asf.align <= room && k >= asf.align) k -= k % asf.align;	/* whole blocks */
	memcpy(dst, wfifo, k);
	memmove(wfifo, wfifo + k, wf_len - k); wf_len -= k;
	return k;
}

/* ---------- the component ---------- */
static OMX_HANDLETYPE h;
static OMX_COMPONENTTYPE *C;
static int core_mode = -1;	/* -1 not initialised, 1 FTB (PCM back to us), 0 direct (the DSP plays) */
static int cur_codec = -1;
static uint16_t cur_par[24];
static BUFHDR *inb[NBUF], *outb[NBUF];
static int nin, nout, insz;
static BUFHDR *freein[NBUF]; static int nfreein;
static BUFHDR *ready[NBUF]; static int nready;	/* filled outputs waiting to be written to the player */
static uint32_t state;
static int running, input_done, seq, codec, first_buf, austream;
static uint32_t n_err, first_flags = FLAG_DISCONT;
static double last_ev;
static OMX_CALLBACKTYPE cb = { cb_event, cb_ebd, cb_fbd };
static struct { int off, val; } povr[8];	/* test modes: -P off=val parameter overrides */
static int npovr;

/* direct sessions */
static int direct, dpaused;
static struct { double t_end; long off_end; } bmeta[NBUF];
static double fed_t, pos_t, last_ebd_t, eosflag_t, last_pos_sent, src_rate;
static long pos_off;
static uint32_t n_ebd, acked;

static int buf_index(BUFHDR *b) { for (int i = 0; i < nin; i++) if (inb[i] == b) return i; return -1; }

static void handle(struct ev *e)
{
	last_ev = now();
	if (e->type == EV_EVENT) {
		if (e->a == EV_CMDCOMPLETE && e->b == 0) state = e->c;	/* CmdComplete(StateSet, state) */
		else if (e->a == EV_ERROR) {
			if (e->b == ERR_UNDERFLOW && (codec == C_PCM || direct)) return;	/* starved stream / render start: normal */
			n_err++; LOG("error event %#x", e->b);
		} else if (e->a == EV_BUFFERFLAG) {
			LOG("buffer flag event (%u, %u)%s", e->b, e->c, direct ? " = end of playback" : "");
			if (direct && running && !eosflag_t) eosflag_t = now();
		}
	} else if (e->type == EV_EBD) {
		BUFHDR *b = e->p;
		if (nfreein < NBUF) freein[nfreein++] = b;
		if (running) {
			int i = buf_index(b);
			if (i >= 0) { pos_t = bmeta[i].t_end; pos_off = bmeta[i].off_end; }
			n_ebd++; last_ebd_t = now();
		}
	} else if (e->type == EV_FBD) {
		if (running && nready < NBUF) ready[nready++] = e->p;
	}
}

static int pump(int ms)
{
	struct pollfd pf = { evpipe[0], POLLIN, 0 };
	struct ev e;
	if (poll(&pf, 1, ms) <= 0) return 0;
	if (read(evpipe[0], &e, sizeof e) != sizeof e) return 0;
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

static void to_loaded(void)
{
	if (!C) return;
	if (state == ST_EXEC || state == ST_PAUSE) { set_state(ST_IDLE); wait_state(ST_IDLE, 4000); }
	if (state == ST_IDLE) {
		set_state(ST_LOADED);
		for (int i = 0; i < nin; i++) C->FreeBuffer(h, 0, inb[i]);
		for (int i = 0; i < nout; i++) C->FreeBuffer(h, 1, outb[i]);
		wait_state(ST_LOADED, 4000);
	}
	nin = nout = 0; nfreein = nready = 0;
	memset(cur_par, 0xff, sizeof cur_par);
	while (pump(0)) ;
}

static void release_handle(void)
{
	if (!C) return;
	to_loaded();
	OMX_FreeHandle(h);
	h = 0; C = 0; cur_codec = -1;
}

/* bring the OMF core up in mode m (1 FTB, 0 direct); switching modes reloads the DSP */
static int core_up(int m)
{
	if (core_mode == m) return 1;
	if (core_mode >= 0) {
		release_handle();
		OMX_Deinit();
		LOG("core down (mode %d)", core_mode);
		core_mode = -1;
		while (pump(0)) ;
	}
	int mode = m;
	OMX_DriverMode(&mode);	/* 1: the core leaves /dev/sound alone; 0: it opens /dev/sound/dsp and the DSP renders */
	OMX_ERRORTYPE r = OMX_Init(m ? CFG_FTB : CFG_DIRECT);
	LOG("OMX_Init(%s) = %#x", m ? "ftb" : "direct", r);
	if (r) { OMX_Deinit(); return 0; }
	core_mode = m;
	return 1;
}

/* p == 0: just make sure we hold a handle of codec c (Loaded or Idle), so its defaults can be read.
 * Otherwise: that component in Idle with parameter block p, buffers allocated (parameters can only be set in
 * Loaded, so a different block means Idle -> Loaded -> Idle). */
static int select_comp(int c, const uint16_t *pin)
{
	if (c != cur_codec) {
		release_handle();
		const char *name = core_mode == 0 ? codecs[c].dcomp : codecs[c].comp;
		OMX_ERRORTYPE r = OMX_GetHandle(&h, name, 0, &cb);
		LOG("GetHandle(%s) = %#x", name, r);
		if (r || !h) { h = 0; return 0; }
		C = h; cur_codec = c;
		C->GetState(h, &state);
		memset(cur_par, 0xff, sizeof cur_par);
	}
	if (!pin) return 1;
	uint16_t p[24];
	memcpy(p, pin, sizeof p);
	for (int i = 0; i < npovr; i++) if (povr[i].off >= 0 && povr[i].off < 48) p[povr[i].off / 2] = povr[i].val;
	if (state == ST_IDLE && !memcmp(p, cur_par, sizeof cur_par)) return 1;
	to_loaded();
	OMX_ERRORTYPE r = C->SetParameter(h, IDX_PARAM, p);
	LOG("SetParameter = %#x: +0=%u +2=%u +4=%u +6=%u +8=%u +a=%u +c=%u +e=%d +10=%u +12=%u +14=%u", r, p[0], p[1], p[2], p[3], p[4], p[5], p[6], (int16_t)p[7], p[8], p[9], p[10]);
	if (r) return 0;
	if (!set_state(ST_IDLE)) return 0;
	insz = codecs[c].insz;
	for (int i = 0; i < NBUF; i++) if (!C->AllocateBuffer(h, &inb[i], 0, 0, insz)) nin++;
	if (core_mode == 1) for (int i = 0; i < NBUF; i++) if (!C->AllocateBuffer(h, &outb[i], 1, 0, OUTSZ)) nout++;
	if (nin != NBUF || (core_mode == 1 && nout != NBUF)) LOG("allocated only %d/%d buffers", nin, nout);
	if (!wait_state(ST_IDLE, 5000)) return 0;
	memcpy(cur_par, p, sizeof cur_par);
	return 1;
}

/* Executing -> Idle: the DSP drops whatever it holds; every buffer is ours again */
static uint8_t pcmq[3 * MAXIN];	/* PCM effect stream: bytes received from the player, not yet sent */
static int pcmq_len;
static void stop(void)
{
	running = 0;
	if (C && (state == ST_EXEC || state == ST_PAUSE)) { set_state(ST_IDLE); wait_state(ST_IDLE, 4000); }
	while (pump(0)) ;
	nready = 0;
	pcmq_len = 0;
	aq_r = aq_n = aq_end = 0; austream = 0;
	direct = dpaused = 0;
	if (in_fd >= 0) { close(in_fd); in_fd = -1; }
	if (tmp4f) { fclose(tmp4f); tmp4f = 0; MP4D_close(&tmp4); tmp4_track = -1; }
}

static int go_exec(uint32_t s)
{
	if (!set_state(ST_EXEC) || !wait_state(ST_EXEC, 5000)) return 0;
	seq = s; running = 1; input_done = 0; n_err = 0; first_buf = 1; last_ev = now();
	for (int i = 0; i < nout; i++) { outb[i]->nFilledLen = 0; outb[i]->nFlags = 0; C->FillThisBuffer(h, outb[i]); }
	nfreein = 0;
	for (int i = 0; i < nin; i++) freein[nfreein++] = inb[i];
	fed_t = pos_t = 0; pos_off = 0; n_ebd = acked = 0; aq_taken = 0;
	last_ebd_t = now(); eosflag_t = 0; last_pos_sent = 0;
	direct = core_mode == 0; dpaused = 0;
	return 1;
}

static void send_pos(int blocking)
{
	struct dpos d = { (uint32_t)(pos_t * 1000 + 0.5), (uint32_t)pos_off, n_ebd, (dpaused ? 1u : 0u) | (input_done ? 2u : 0u) };
	if (blocking) send_pkt(seq, PF_POS, &d, sizeof d);
	else if (try_send_pkt(seq, PF_POS, &d, sizeof d)) return;
	last_pos_sent = now();
}

/* the component's current block (its defaults after GetHandle), to change only the fields we know */
static void base_params(int c, uint16_t *p)
{
	memset(p, 0, 24 * sizeof *p);
	if (cur_codec == c && C) C->GetParameter(h, IDX_PARAM, p);
}

static int prot_flag;	/* the last start failed because the file is protected / unsupported */

/* start decoding (FTB) or playing (direct: core mode 0) a file */
static int start_file(uint32_t s, long offset, int c, const char *path, int dir)
{
	stop();
	prot_flag = 0;
	if (!core_up(dir ? 0 : 1)) return 0;
	in_fd = open(path, O_RDONLY);
	if (in_fd < 0) { LOG("open %s failed", path); return 0; }
	rlen = rpos = in_eof = 0; have_stream = 0;
	uint16_t p[24];
	if (c == C_MP3) {
		lseek(in_fd, offset, SEEK_SET);
		uint8_t tmp[4096];
		int n = mp3_next(tmp, sizeof tmp);	/* the first frame: the parameter depends on the MPEG version */
		if (n <= 0) { LOG("no MP3 frame in %s @%ld", path, offset); return 0; }
		rpos -= n;	/* put it back */
		if (cur_codec != c && !select_comp(c, 0)) return 0;
		base_params(c, p);
		p[0] = 6;
		p[2] = stream.ver == 3 ? 1 : stream.ver == 2 ? 2 : 3;	/* +4: MPEG version 1 / 2 / 2.5, as gemini sets it */
		p[3] = 4;			/* +6: output 44.1 kHz (3 = 48 kHz) */
		p[4] = 4;
		p[9] = stream.kbps >= 1 && stream.kbps <= 0x3fc ? stream.kbps : 0x27;	/* +0x12: the bitrate in kbps, as gemini sends it (DSEE needs it) */
		src_rate = stream.sr;
		LOG("start seq %u %s @%ld: %s mp3 ver %d layer %d %d Hz %d kbps", s, path, offset, dir ? "DIRECT" : "ftb", stream.ver, stream.layer, stream.sr, stream.kbps);
	} else if (c == C_AAC) {
		lseek(in_fd, offset, SEEK_SET);
		refill();
		int hl, len = 0;
		for (; rpos + 9 <= rlen; rpos++) {	/* first ADTS header followed by another one */
			len = aac_parse_adts(rbuf + rpos, &aac, &hl);
			struct aac_info a2; int h2;
			if (len && rpos + len + 9 <= rlen && aac_parse_adts(rbuf + rpos + len, &a2, &h2) && a2.rate == aac.rate) break;
			if (len && in_eof && rpos + len >= rlen) break;
			len = 0;
		}
		if (!len) { LOG("no ADTS frame in %s @%ld", path, offset); prot_flag = 1; return 0; }
		if (!aac_dsp_ok(&aac)) { LOG("aac: aot %d rate %d ch %d not for the DSP", aac.aot, aac.rate, aac.channels); prot_flag = 1; return 0; }
		if (cur_codec != c && !select_comp(c, 0)) return 0;
		base_params(c, p);
		aac_params(p, &aac, adts_whole, 0);
		src_rate = aac.rate;
		LOG("start seq %u %s @%ld: %s aac/adts aot %d %d Hz %d ch, %s", s, path, offset, dir ? "DIRECT" : "ftb", aac.aot, aac.rate, aac.channels, adts_whole ? "whole frames (+8=2)" : "raw AUs (+8=0)");
	} else if (c == C_WMA) {
		int r = asf_parse(in_fd, &asf);
		if (r) {
			LOG("%s: asf_parse %d (tag %#x drm %d)", path, r, asf.tag, asf.drm);
			prot_flag = 1;
			return 0;
		}
		long first = asf.data_off;
		if (offset > first) first += (offset - asf.data_off) / asf.packet_size * asf.packet_size;
		asf_packets_left = (asf.data_end - first) / asf.packet_size;
		asf.synced = first == asf.data_off;	/* after a seek: from the next media object start */
		wf_len = 0;
		lseek(in_fd, first, SEEK_SET);
		if (cur_codec != c && !select_comp(c, 0)) return 0;
		base_params(c, p);
		asf_wma_params(p, &asf);
		src_rate = asf.rate;
		LOG("start seq %u %s @%ld: %s wma tag %#x %d Hz %d ch %u B/s align %d, packet %d, %ld packets", s, path, first, dir ? "DIRECT" : "ftb",
		    asf.tag, asf.rate, asf.channels, asf.avgbytes, asf.align, asf.packet_size, asf_packets_left);
	} else {
		int r = oma_parse(in_fd, &oma);
		if (r || !oma_dsp_ok(&oma)) {
			LOG("%s: oma_parse %d codec %#x eid %#x rate %d ch %d frame %d -> %s", path, r, oma.codec, oma.eid,
			    oma.rate, oma.channels, oma.frame, r == OMA_EDRM ? "protected" : "unsupported");
			prot_flag = 1;
			return 0;
		}
		int want = oma.codec == OMA_AT3 ? C_AT3 : oma.codec == OMA_ATX ? C_ATX : C_AAL;
		if (want != c) LOG("%s: header says %s, not %s", path, codecs[want].name, codecs[c].name);
		c = want;
		long fsize = lseek(in_fd, 0, SEEK_END);
		if (offset < oma.data) offset = oma.data;
		if (c != C_AAL) {
			offset = oma.data + (offset - oma.data) / oma.frame * oma.frame;
			frames_left = offset < fsize ? (fsize - offset) / oma.frame : 0;
		} else offset = oma.data + ((offset - oma.data) & ~7L);
		lseek(in_fd, offset, SEEK_SET);
		if (cur_codec != c && !select_comp(c, 0)) return 0;
		base_params(c, p);
		p[0] = 6; p[1] = 2; p[2] = 4;
		if (c == C_AT3) { p[3] = 0; p[4] = oma.js; p[5] = 4; p[6] = oma.frame; }	/* gemini 0x2dc394 */
		else if (c == C_ATX) { p[3] = 1; p[4] = 4; p[5] = oma.frame; }		/* gemini 0x2dc544 */
		else { p[3] = oma.base; p[4] = 4; p[5] = oma.frame; p[6] = oma.blkcode; }	/* gemini 0x2dc7f0 */
		src_rate = oma.rate;
		LOG("start seq %u %s @%ld: %s %s v%d js %d frame %d, %ld frames", s, path, offset, dir ? "DIRECT" : "ftb", codecs[c].name, oma.version, oma.js, oma.frame, frames_left);
		if (c != C_AAL && frames_left == 0) { codec = c; seq = s; send_pkt(s, PF_EOS, 0, 0); stop(); return 1; }
	}
	codec = c;
	if (!select_comp(c, p)) return 0;
	if (!go_exec(s)) return 0;
	if (direct) send_pos(1);	/* "started" */
	return 1;
}

/* AAC access-unit stream (the player demuxes the MP4 and sends "A" commands) */
static int start_austream(uint32_t s, int dir, const struct aac_info *a, int kbps, int adts)
{
	stop();
	prot_flag = 0;
	if (!aac_dsp_ok(a)) { LOG("aac stream: aot %d rate %d ch %d not for the DSP", a->aot, a->rate, a->channels); prot_flag = 1; return 0; }
	if (!core_up(dir ? 0 : 1)) return 0;
	if (cur_codec != C_AAC && !select_comp(C_AAC, 0)) return 0;
	uint16_t p[24];
	base_params(C_AAC, p);
	aac = *a;
	aac_params(p, &aac, adts, kbps);
	src_rate = aac.rate;
	codec = C_AAC;
	if (!select_comp(C_AAC, p)) return 0;
	LOG("aac stream seq %u %s aot %d %d Hz (out %d) %d ch sbr %d ps %d %d kbps", s, dir ? "DIRECT" : "ftb", aac.aot, aac.rate, aac.out_rate, aac.channels, aac.sbr, aac.ps, kbps);
	if (!go_exec(s)) return 0;
	austream = 1;
	if (direct) send_pos(1);
	return 1;
}

static const int rate_codes[] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };	/* codes 3..11 */
static int start_pcm(uint32_t s, int rate)
{
	stop();
	int code = -1;
	for (int i = 0; i < 9; i++) if (rate_codes[i] == rate) code = i + 3;
	if (code < 0) { LOG("pcm: rate %d not supported", rate); return 0; }
	if (!core_up(1)) return 0;
	if (cur_codec != C_PCM && !select_comp(C_PCM, 0)) return 0;
	uint16_t p[24];
	base_params(C_PCM, p);
	p[3] = 4;	/* +6 output 44.1 kHz */
	p[4] = 6;	/* +8 little-endian input */
	p[5] = 2;	/* +a stereo input */
	p[6] = code;	/* +c input rate */
	codec = C_PCM;
	if (!select_comp(C_PCM, p)) return 0;
	LOG("pcm stream seq %u %d Hz", s, rate);
	return go_exec(s);
}

static void feed(void)
{
	while (running && !input_done && nfreein) {
		BUFHDR *b = freein[nfreein - 1];
		int n = 0, k = 0;
		double dt = 0;
		if (codec == C_PCM) {
			/* send what we have once it is worth a buffer; small chunks are fine when the DSP is idle */
			int busy = nin - nfreein;
			if (pcmq_len < 4 || (pcmq_len < 4096 && busy >= 2)) break;
			n = pcmq_len > insz ? insz : pcmq_len;
			n &= ~3;
			memcpy(b->pBuffer, pcmq, n);
			memmove(pcmq, pcmq + n, pcmq_len - n); pcmq_len -= n;
		} else if (codec == C_MP3) {
			while ((k = mp3_next(b->pBuffer + n, insz - n)) > 0) { n += k; dt += (double)mp3_spf() / stream.sr; }
			if (k == 0) input_done = 1;	/* end of file (k < 0: next frame doesn't fit, stays for the next buffer) */
			if (n == 0 && !input_done) break;
		} else if (codec == C_AAC && austream) {
			if (!aq_n) { if (aq_end) input_done = 1; else break; }
			else { n = aq_next(b->pBuffer, insz); dt = 1024.0 / aac.rate; if (!aq_n && aq_end) input_done = 1; }
			if (!n && !input_done) break;
		} else if (codec == C_AAC) {
			n = adts_next(b->pBuffer, insz);
			if (n <= 0) { input_done = 1; n = 0; }
			else { dt = 1024.0 / aac.rate; if (rlen - rpos < 9 && refill() < 9) input_done = 1; }
		} else if (codec == C_WMA) {
			n = wma_next(b->pBuffer, insz);
			if (n <= 0) { input_done = 1; n = 0; }
			else { dt = (double)n / (asf.avgbytes ? asf.avgbytes : 16000); if (asf_packets_left <= 0 && !wf_len) input_done = 1; }
		} else {
			/* ATRAC: whole frames / blocks only, EOS on the last one (end_fallback() covers files that
			 * end in a truncated frame, where no buffer carried the flag) */
			n = codec == C_AAL ? aal_next(b->pBuffer, insz) : at_next(b->pBuffer, insz);
			if (n <= 0) { input_done = 1; break; }
			dt = (double)oma.spf / (oma.rate ? oma.rate : 44100);
			if (codec != C_AAL) { if (frames_left == 0) input_done = 1; }
			else if (rlen - rpos < 24 && refill() < 24) input_done = 1;
		}
		nfreein--;
		if (!n) { b->pBuffer[0] = 0; n = 1; }	/* an EOS-only buffer (nFilledLen must be >= 1) */
		fed_t += dt;
		int bi = buf_index(b);
		if (bi >= 0) { bmeta[bi].t_end = fed_t; bmeta[bi].off_end = austream ? aq_taken : rd_off(); }
		b->nFilledLen = n; b->nOffset = 0; b->nTickCount = 0; b->nTimeStamp = (int64_t)(fed_t * 1e6);
		b->nFlags = input_done ? FLAG_EOS : 0;
		if (first_buf && (codec == C_AT3 || codec == C_ATX || codec == C_AAL)) b->nFlags |= first_flags;
		first_buf = 0;
		OMX_ERRORTYPE r = C->EmptyThisBuffer(h, b);
		if (r) { LOG("ETB(%d) = %#x", n, r); freein[nfreein++] = b; input_done = 1; n_err += 4; break; }
	}
}

/* hand filled outputs to the player (blocking write: the pipe is our backpressure) and give them back */
static uint64_t out_frames;
static void drain_ready(void)
{
	while (running && nready) {
		BUFHDR *b = ready[0];
		memmove(ready, ready + 1, --nready * sizeof ready[0]);
		uint32_t n = b->nFilledLen > b->nAllocLen ? b->nAllocLen : b->nFilledLen;
		int eos = b->nFlags & FLAG_EOS;
		if (n) { send_pkt(seq, 0, b->pBuffer + b->nOffset, n); out_frames += n / 4; }
		last_ev = now();	/* a paused player blocks us in send_pkt: that is not a DSP stall */
		if (eos) { send_pkt(seq, PF_EOS, 0, 0); LOG("eos seq %u", seq); stop(); return; }
		b->nFilledLen = 0; b->nFlags = 0;
		C->FillThisBuffer(h, b);
	}
	if (running && austream && !direct && n_ebd != acked) { acked = n_ebd; uint32_t a = (uint32_t)aq_taken - (uint32_t)(nin - nfreein); send_pkt(seq, PF_ACK, &a, 4); }
}

/* all input consumed, every input buffer back, nothing from the DSP for a while and no EOS flag seen (the
 * last buffer could not carry it): end the stream ourselves. 0 = still running */
static int end_fallback(void)
{
	if (!running || direct || codec == C_PCM || !input_done || nfreein != nin || nready || now() - last_ev < 1.5) return 0;
	LOG("input done, DSP idle, no EOS: ending seq %u", seq);
	send_pkt(seq, PF_EOS, 0, 0);
	stop();
	return 1;
}

/* direct sessions: positions, the end (EventBufferFlag, or everything consumed and 3 s without progress), stalls.
 * returns 1 when the session ended (EOS sent), -1 on an error */
static int direct_tick(void)
{
	if (!running || !direct) return 0;
	double t = now();
	int end = 0;
	if (eosflag_t && t - eosflag_t >= 0.2) end = 1;
	else if (input_done && nfreein == nin && !dpaused && t - last_ebd_t > 3) { LOG("all input consumed 3 s ago, no buffer flag: ending"); end = 1; }
	if (end) {
		LOG("direct eos seq %u at %.2f s (%u buffers)", seq, pos_t, n_ebd);
		send_pos(1);
		send_pkt(seq, PF_EOS, 0, 0);
		stop();
		return 1;
	}
	if (!dpaused && !input_done && t - last_ev > 10) { LOG("direct session stalled"); send_pkt(seq, PF_ERR, 0, 0); stop(); return -1; }
	if (t - last_pos_sent >= (dpaused ? 5 : 0.25)) send_pos(0);	/* paused: the player does not read, don't fill the pipe */
	return 0;
}

static void direct_pause(int p)
{
	if (!running || !direct || p == dpaused) return;
	if (set_state(p ? ST_PAUSE : ST_EXEC) && wait_state(p ? ST_PAUSE : ST_EXEC, 3000)) { dpaused = p; last_ev = last_ebd_t = now(); LOG("direct %s", p ? "paused" : "resumed"); }
	else LOG("direct pause(%d) failed, state %u", p, state);
	send_pos(0);
}

/* ---------- Sony effects (research/dsp/effects/effects_notes.txt) ---------- */
static volatile uint32_t *fxw;
static volatile uint16_t *fxh;
static unsigned fx_vpt = 0xffff, fx_eq = 0xfffffffe, fx_dn, fx_dsee, fx_gain, fx_cs = 0x8001;
static void fx_map(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { LOG("fx: /dev/mem"); return; }
	void *p = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x33EA0000u);
	close(fd);
	if (p == MAP_FAILED) { LOG("fx: mmap failed"); return; }
	fxw = p; fxh = p;
}
static void fx_commit(void)
{
	if (!fxw) return;
	uint32_t blk[5] = { (fx_dsee & 0xffff) | (uint32_t)(fx_dn ? 1 : 0) << 16, (fx_vpt & 0xffff) | (fx_gain & 0xffff) << 16, fx_cs, fx_eq, 0 };
	fxh[0x08 / 2] = 1;
	double end = now() + 0.5;
	while (fxh[0x0a / 2] != 0 && now() < end) ;
	for (int i = 0; i < 5; i++) fxw[3 + i] = blk[i];
	fxh[0x04 / 2] = 1;
	fxh[0x08 / 2] = 0;
	LOG("fx: vpt %04x eq %08x dn %u dsee %x gain %04x cs %08x", fx_vpt, fx_eq, fx_dn, fx_dsee, fx_gain & 0xffff, fx_cs);
}

/* ---------- commands ---------- */
static int codec_by_name(const char *s, int len)
{
	for (int i = 0; i < C_N; i++) if ((int)strlen(codecs[i].name) == len && !memcmp(s, codecs[i].name, len)) return i;
	return -1;
}

static void cmd_open(char *arg, int dir)
{
	unsigned s; long off; int consumed = 0;
	if (sscanf(arg, "%u %ld %n", &s, &off, &consumed) < 2 || !consumed) return;
	char *rest = arg + consumed;
	int c = C_MP3;
	if (*rest != '/') {	/* "O seq offset codec path" (old form: "O seq offset path") */
		char *sp = strchr(rest, ' ');
		int k = sp ? codec_by_name(rest, (int)(sp - rest)) : -1;
		if (k >= 0 && k != C_PCM) { c = k; rest = sp + 1; }
	}
	if (!start_file(s, off, c, rest, dir)) { stop(); send_pkt(s, PF_ERR | (prot_flag ? PF_PROT : 0), 0, 0); }
}

static void cmd_austream(char *arg)
{
	unsigned s; int dir, rate, ch, aot, sbr, kbps, adts;
	if (sscanf(arg, "%u %d %d %d %d %d %d %d", &s, &dir, &rate, &ch, &aot, &sbr, &kbps, &adts) != 8) return;
	struct aac_info a;
	memset(&a, 0, sizeof a);
	a.rate = rate; a.channels = ch; a.aot = aot; a.sbr = sbr & 1; a.ps = (sbr >> 1) & 1; a.out_rate = a.sbr ? 2 * rate : rate;
	if (!start_austream(s, dir, &a, kbps, adts)) { stop(); send_pkt(s, PF_ERR | (prot_flag ? PF_PROT : 0), 0, 0); }
}

/* leave direct mode: /dev/sound/dsp is closed by OMX_Deinit */
static void cmd_release(unsigned s)
{
	if (core_mode == 0) { stop(); release_handle(); OMX_Deinit(); core_mode = -1; LOG("direct mode released"); while (pump(0)) ; }
	send_pkt(s, PF_REL, 0, 0);
}

/* line commands; returns 0 on Q */
static int handle_cmd(char *line)
{
	if (line[0] == 'Q') return 0;
	if (line[0] == 'S') { stop(); return 1; }
	if (line[0] == 'E') { if (austream) aq_end = 1; return 1; }
	if (line[0] == 'Z') { direct_pause(atoi(line + 1) != 0); return 1; }
	if (line[0] == 'X') { cmd_release((unsigned)strtoul(line + 1, 0, 10)); return 1; }
	if (line[0] == 'F') { unsigned v, e, d; if (sscanf(line + 1, "%x %x %u", &v, &e, &d) == 3) { fx_vpt = v; fx_eq = e; fx_dn = d; fx_commit(); } return 1; }
	if (line[0] == 'G') { unsigned d, g, c; if (sscanf(line + 1, "%x %x %x", &d, &g, &c) == 3) { fx_dsee = d; fx_gain = g; fx_cs = c; fx_commit(); } return 1; }
	if (line[0] == 'O') { cmd_open(line + 1, 0); return 1; }
	if (line[0] == 'R') { cmd_open(line + 1, 1); return 1; }
	if (line[0] == 'M') { cmd_austream(line + 1); return 1; }
	if (line[0] == 'P') {
		unsigned s; int rate;
		if (sscanf(line + 1, "%u %d", &s, &rate) == 2 && !start_pcm(s, rate)) { stop(); send_pkt(s, PF_ERR, 0, 0); }
		return 1;
	}
	return 1;
}

/* stdin: text commands and the binary payloads of D / A; parsing stops (and stdin is not read) while the PCM or
 * access-unit queue has no room for the next payload, which is the player's backpressure */
static uint8_t cmdbuf[65536 + 256];
static int cmdlen;
static int parse_cmds(void)
{
	int pos = 0, quit = 0;
	while (pos < cmdlen && !quit) {
		uint8_t *nl = memchr(cmdbuf + pos, '\n', cmdlen - pos);
		if (!nl) break;
		if (cmdbuf[pos] == 'D' || cmdbuf[pos] == 'A') {
			int len = atoi((char *)cmdbuf + pos + 1);
			int hl = (int)(nl + 1 - (cmdbuf + pos));
			if (len < 0 || len > 65536) { pos += hl; continue; }
			if (cmdlen - pos - hl < len) break;	/* payload not complete yet */
			if (cmdbuf[pos] == 'D') {
				if (running && codec == C_PCM) {
					if (pcmq_len + len > (int)sizeof pcmq) break;	/* no room: wait for the DSP */
					memcpy(pcmq + pcmq_len, cmdbuf + pos + hl, len & ~3);
					pcmq_len += len & ~3;
				}
			} else if (running && austream && aq_put(cmdbuf + pos + hl, len) < 0) break;	/* queue full */
			pos += hl + len;
			continue;
		}
		*nl = 0;
		if (!handle_cmd((char *)cmdbuf + pos)) quit = 1;
		pos = (int)(nl + 1 - cmdbuf);
	}
	memmove(cmdbuf, cmdbuf + pos, cmdlen - pos); cmdlen -= pos;
	if (cmdlen >= (int)sizeof cmdbuf - 1) { LOG("command buffer overflow"); cmdlen = 0; }
	return !quit;
}

static void teardown_exit(int rc)
{
	running = 0;
	if (C) { if (state == ST_EXEC || state == ST_PAUSE) { set_state(ST_IDLE); wait_state(ST_IDLE, 2000); } release_handle(); }
	if (core_mode >= 0) OMX_Deinit();
	LOG("bye rc=%d", rc);
	_exit(rc);
}

static int omx_up(void)
{
	if (pipe(evpipe) < 0) return 0;
	return core_up(1);
}

/* ---------- test modes ---------- */
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int is_mp4(const char *p)
{
	const char *d = strrchr(p, '.');
	return d && (!strcasecmp(d, ".m4a") || !strcasecmp(d, ".mp4") || !strcasecmp(d, ".m4b"));
}
static int test_args(int argc, char **argv, int from, double *secs)
{
	for (int i = from; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-F")) first_flags = strtoul(argv[i + 1], 0, 0);
		else if (!strcmp(argv[i], "-s")) *secs = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "-A")) adts_whole = atoi(argv[i + 1]) == 2;
		else if (!strcmp(argv[i], "-P") && npovr < 8) {
			char *eq = strchr(argv[i + 1], '=');
			if (eq) { povr[npovr].off = strtol(argv[i + 1], 0, 0); povr[npovr].val = strtol(eq + 1, 0, 0); npovr++; }
		}
	}
	return 0;
}
/* start a test session on IN (the player's paths, except MP4, which is demuxed here) */
static int test_start(int c, const char *in, int dir, int pct)
{
	if (c == C_AAC && is_mp4(in)) {
		stop();
		if (tmp4_open(in)) { printf("RESULT not an AAC mp4\n"); return 0; }
		if (pct > 0 && tmp4_count) { tmp4_sample = (int)((long long)tmp4_count * pct / 100); }
		LOG("mp4: %d samples, asc aot %d %d Hz (out %d) %d ch sbr %d ps %d, %d kbps", tmp4_count, aac.aot, aac.rate, aac.out_rate, aac.channels, aac.sbr, aac.ps, aac_kbps);
		FILE *keep = tmp4f; MP4D_demux_t km = tmp4; int kt = tmp4_track, ks = tmp4_sample, kc = tmp4_count;
		tmp4f = 0;	/* start_austream() -> stop() must not close the demuxer */
		struct aac_info a = aac;
		int r = start_austream(1, dir, &a, aac_kbps, 0);
		tmp4f = keep; tmp4 = km; tmp4_track = kt; tmp4_sample = ks; tmp4_count = kc;
		if (r) tmp4_fill();
		return r;
	}
	long off = 0;
	if (pct > 0) {	/* -j: percent of the file -> byte offset */
		int f = open(in, O_RDONLY);
		if (f >= 0) { off = (long)((long long)lseek(f, 0, SEEK_END) * pct / 100); close(f); }
	}
	return start_file(1, off, c, in, dir);
}

static void print_stats(const char *out)
{
	int f = open(out, O_RDONLY);
	if (f < 0) return;
	int16_t s[4096]; double e[2] = { 0, 0 }; long n = 0; int pk = 0; ssize_t k;
	while ((k = read(f, s, sizeof s)) > 0)
		for (int i = 0; i + 1 < k / 2; i += 2, n++) {
			e[0] += (double)s[i] * s[i]; e[1] += (double)s[i + 1] * s[i + 1];
			if (abs(s[i]) > pk) pk = abs(s[i]);
			if (abs(s[i + 1]) > pk) pk = abs(s[i + 1]);
		}
	close(f);
	if (n) printf("STATS L %.1f dBFS R %.1f dBFS peak %d frames %ld\n", 10 * log10(e[0] / n / 1073741824.0 + 1e-12),
		      10 * log10(e[1] / n / 1073741824.0 + 1e-12), pk, n);
}

/* dspdec LOG -d CODEC IN OUT.raw [...] */
static int test_decode(int argc, char **argv)
{
	const char *cn = argv[3], *in = argv[4], *out = argv[5];
	double secs = 0;
	test_args(argc, argv, 6, &secs);
	int c = codec_by_name(cn, strlen(cn));
	if (c < 0 || c == C_PCM) { fprintf(stderr, "codec: mp3|at3|atx|aal|aac|wma\n"); return 1; }
	test_mode = 1;
	out_fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out_fd < 0) { fprintf(stderr, "open %s\n", out); return 1; }
	if (!omx_up()) return 1;
	fx_map();
	double ts = now();
	if (!test_start(c, in, 0, 0)) { printf("RESULT start failed%s\n", prot_flag ? " (protected/unsupported)" : ""); teardown_exit(1); }
	printf("FORMAT %s %s param +0=%u +2=%u +4=%u +6=%u +8=%u +a=%u +c=%u +e=%d +10=%u +12=%u +14=%u\n", codecs[codec].name, codecs[codec].comp,
	       cur_par[0], cur_par[1], cur_par[2], cur_par[3], cur_par[4], cur_par[5], cur_par[6], (int16_t)cur_par[7], cur_par[8], cur_par[9], cur_par[10]);
	int rc = 0;
	while (running) {
		tmp4_fill();
		feed();
		pump(100);
		while (pump(0)) ;
		drain_ready();
		if (end_fallback()) printf("no EOS flag from the DSP (ended after 1.5 s idle)\n");
		if (secs > 0 && out_frames >= secs * 44100) { printf("limit reached\n"); break; }
		if (n_err > 3) { printf("too many DSP errors (%u)\n", n_err); rc = 2; break; }
		if (now() - last_ev > 5) { printf("stalled: no callbacks for 5 s\n"); rc = 2; break; }
	}
	double el = now() - ts, got = out_frames / 44100.0;
	printf("RESULT rc=%d decoded %.2f s in %.2f s (%.1fx realtime), errors %u, input %.2f s\n", rc, got, el, el > 0 ? got / el : 0, n_err, fed_t);
	close(out_fd);
	print_stats(out);
	fflush(stdout);
	teardown_exit(rc);
	return rc;
}

/* read-only views of the PCM DMA channel (M2P LCH4 -> PCM0) and PCM0 (research/dsp/notes/direct_notes.txt) */
static volatile uint32_t *dma_par, *dma_ctl, *pcm0;
static void *map_ro(uint32_t phys)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) return 0;
	void *p = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, phys);
	close(fd);
	return p == MAP_FAILED ? 0 : p;
}
struct regs { uint32_t aadd, aadp, leng, rcount, wcount, mode, m2p_status, pcm_mon, pcm_enmon; };
static void read_regs(struct regs *r)
{
	memset(r, 0xff, sizeof *r);
	if (dma_par) {
		volatile uint32_t *l4 = dma_par + 0x400 / 4;	/* 0x40091400: M2P logical channel 4 parameters */
		r->aadd = l4[0]; r->aadp = l4[1]; r->leng = l4[0x40 / 4]; r->rcount = l4[0x44 / 4]; r->wcount = l4[0x48 / 4]; r->mode = l4[0x50 / 4];
	}
	if (dma_ctl) r->m2p_status = dma_ctl[1];	/* 0x40099004 */
	if (pcm0) { r->pcm_mon = pcm0[1]; r->pcm_enmon = pcm0[0x18 / 4]; }	/* 0x400C0004 TXRX_MON, +0x18 ENMON */
}
static unsigned long irq_count(const char *name)
{
	FILE *f = fopen("/proc/interrupts", "r");
	char l[256]; unsigned long v = 0;
	if (!f) return 0;
	while (fgets(l, sizeof l, f)) if (strstr(l, name)) { char *c = strchr(l, ':'); if (c) v = strtoul(c + 1, 0, 10); }
	fclose(f);
	return v;
}
static void log_regs(const char *tag)
{
	struct regs r;
	read_regs(&r);
	printf("REGS %-6s lch4 aadd %08x aadp %08x leng %06x rcnt %06x wcnt %06x mode %08x | m2p status %08x | pcm0 txrx %x en %x | irq interdsp %lu audio %lu\n",
	       tag, r.aadd, r.aadp, r.leng & 0xffffff, r.rcount & 0xffffff, r.wcount & 0xffffff, r.mode, r.m2p_status, r.pcm_mon, r.pcm_enmon, irq_count("interdsp"), irq_count("audio"));
}

/* dspdec LOG -r CODEC IN [...]: direct playback test */
static int test_render(int argc, char **argv)
{
	const char *cn = argv[3], *in = argv[4];
	double secs = 10, alt = 0, pause_at = -1, jump_at = -1;
	int vol = -1, have_g = 0; unsigned g_dsee = 0, g_gain = 0, g_cs = 0x8001;
	test_args(argc, argv, 5, &secs);
	for (int i = 5; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-V")) vol = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-a")) alt = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "-p")) pause_at = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "-j")) jump_at = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "-G") && sscanf(argv[i + 1], "%x,%x,%x", &g_dsee, &g_gain, &g_cs) == 3) have_g = 1;
	}
	int c = codec_by_name(cn, strlen(cn));
	if (c < 0 || c == C_PCM) { fprintf(stderr, "codec: mp3|at3|atx|aal|aac|wma\n"); return 1; }
	test_mode = 1;
	setvbuf(stdout, 0, _IOLBF, 0);
	dma_par = map_ro(0x40091000u); dma_ctl = map_ro(0x40099000u); pcm0 = map_ro(0x400C0000u);
	fx_map();
	log_regs("boot");
	if (pipe(evpipe) < 0 || !core_up(0)) { printf("RESULT rc=1 OMX_Init(direct) failed\n"); teardown_exit(1); }
	log_regs("init");
	if (vol >= 0) {	/* the OMF core opened the codec; our moderate listening volume */
		int m = open("/dev/sound/mixer", O_RDWR), lr = vol | vol << 8;
		printf("MIXER volume %d: %s\n", vol, m >= 0 && ioctl(m, SOUND_MIXER_WRITE_VOLUME, &lr) >= 0 ? "ok" : "failed");
		if (m >= 0) close(m);
	}
	if (have_g) { fx_dsee = g_dsee; fx_gain = g_gain; fx_cs = g_cs; fx_commit(); }
	double ts = now();
	if (!test_start(c, in, 1, 0)) { printf("RESULT rc=1 start failed%s\n", prot_flag ? " (protected/unsupported)" : ""); teardown_exit(1); }
	printf("FORMAT DIRECT %s %s src %.0f Hz param +0=%u +2=%u +4=%u +6=%u +8=%u +a=%u +c=%u +e=%d +10=%u +12=%u +14=%u\n", codecs[codec].name, codecs[codec].dcomp, src_rate,
	       cur_par[0], cur_par[1], cur_par[2], cur_par[3], cur_par[4], cur_par[5], cur_par[6], (int16_t)cur_par[7], cur_par[8], cur_par[9], cur_par[10]);
	printf("LISTEN: playing %s for %.0f s from now (uptime-relative start in the events file)\n", base_name(in), secs);
	log_regs("exec");
	int rc = 0, ab = 0, paused_done = 0, jumped = 0, moving = 0, samples = 0, in_dsp = 0;
	double next = ts + 1, next_ab = alt > 0 ? ts + alt : 0, played_before_jump = 0, pause_t = 0;
	uint32_t last_aadp = 0;
	while (now() - ts < secs) {
		tmp4_fill();
		feed();
		pump(50);
		while (pump(0)) ;
		double t = now() - ts;
		if (!running) { printf("session ended at %.2f s wall\n", t); break; }
		int d = direct_tick();
		if (d) { printf("%s at %.2f s wall (played %.2f s by EBD)\n", d > 0 ? "EOS" : "ERROR", t, pos_t); if (d < 0) rc = 2; break; }
		if (n_err > 6) { printf("too many DSP errors (%u)\n", n_err); rc = 2; break; }
		if (pause_at >= 0 && !paused_done && t >= pause_at) { direct_pause(1); pause_t = now(); paused_done = 1; printf("PAUSE at %.2f s (pos %.2f)\n", t, pos_t); }
		if (dpaused && now() - pause_t >= 2) { direct_pause(0); printf("RESUME at %.2f s (pos %.2f)\n", t, pos_t); }
		if (jump_at >= 0 && !jumped && t >= jump_at) {
			jumped = 1; played_before_jump = pos_t;
			printf("SEEK at %.2f s: restart at 50 %%\n", t);
			if (!test_start(c, in, 1, 50)) { printf("seek restart failed\n"); rc = 2; break; }
		}
		if (next_ab && now() >= next_ab) {
			ab = !ab; next_ab += alt;
			fx_dsee = ab ? 0xf : 0; fx_commit();
			printf("AB %s at %.2f s\n", ab ? "B (dsee on)" : "A (dsee off)", t);
		}
		if (now() >= next) {
			next += 1;
			struct regs r; read_regs(&r);
			samples++;
			if (r.aadp != last_aadp) moving++;
			if (r.aadp >= 0x33A00000u && r.aadp < 0x34000000u) in_dsp++;
			last_aadp = r.aadp;
			double played = played_before_jump + pos_t;
			printf("T %5.1f fed %6.2f s played %6.2f s (ebds %4u) pace %.2f%s | lch4 aadp %08x rcnt %06x st %08x pcm0 %x | page +04 %x +1c %x\n",
			       t, fed_t, pos_t, n_ebd, t > 0 ? played / t : 0, dpaused ? " PAUSED" : "", r.aadp, r.rcount & 0xffffff, r.m2p_status, r.pcm_mon,
			       fxh ? fxh[2] : 0xffff, fxh ? fxh[0x1c / 2] : 0xffff);
		}
	}
	double el = now() - ts, played = played_before_jump + pos_t;
	log_regs("end");
	printf("RESULT rc=%d DIRECT %s played %.2f s (by EmptyBufferDone) in %.2f s wall, pace %.2f, buffers %u, errors %u, dma pointer moved in %d/%d samples, in DSP memory %d/%d\n",
	       rc, codecs[codec].name, played, el, el > 0 ? played / el : 0, n_ebd, n_err, moving, samples, in_dsp, samples);
	printf("VERDICT %s\n", moving >= samples / 2 && samples ? "the PCM DMA channel runs during direct playback (the DSP feeds the codec)" : "the PCM DMA channel did NOT move - probably silent");
	fflush(stdout);
	teardown_exit(rc);
	return rc;
}

int main(int argc, char **argv)
{
	t0 = now();
	if (argc > 1) logfp = !strcmp(argv[1], "-") ? stderr : fopen(argv[1], "w");
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_term;	/* no SA_RESTART: a blocked write/poll returns EINTR and we tear down */
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	signal(SIGPIPE, SIG_IGN);
	if (argc >= 6 && !strcmp(argv[2], "-d")) return test_decode(argc, argv);
	if (argc >= 5 && !strcmp(argv[2], "-r")) return test_render(argc, argv);
	if (!omx_up()) { send_pkt(0, PF_ERR, 0, 0); return 1; }
	fx_map();
	send_pkt(0, PF_READY, 0, 0);
	int in_open = 1;
	for (;;) {
		if (term_req) teardown_exit(0);
		feed();
		drain_ready();
		int room = cmdlen < (int)sizeof cmdbuf - 1;
		struct pollfd pf[2] = { { in_open && room ? 0 : -1, POLLIN, 0 }, { evpipe[0], POLLIN, 0 } };
		if (poll(pf, 2, running ? (direct ? (dpaused ? 1000 : 100) : 200) : 1000) < 0 && errno != EINTR) break;	/* paused: wake once a second */
		if (pf[1].revents & POLLIN) { while (pump(0)) ; }
		if (in_open && room && (pf[0].revents & (POLLIN | POLLHUP))) {
			ssize_t n = read(0, cmdbuf + cmdlen, sizeof cmdbuf - 1 - cmdlen);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) break;	/* the player went away */
			cmdlen += n;
		}
		if (!parse_cmds()) break;
		if (running && n_err > (direct ? 6u : 3u)) { LOG("too many DSP errors"); send_pkt(seq, PF_ERR, 0, 0); stop(); }
		end_fallback();
		direct_tick();
		if (running && !direct && codec != C_PCM && !austream && now() - last_ev > 5) { LOG("stalled"); send_pkt(seq, PF_ERR, 0, 0); stop(); }
		if (running && austream && !direct && !input_done && nfreein < nin && now() - last_ev > 5) { LOG("stalled"); send_pkt(seq, PF_ERR, 0, 0); stop(); }
	}
	teardown_exit(0);
	return 0;
}
