/* dspclient.c: the player's side of homebrew/dspdec (decoding and Sony effects on the MP201's audio DSP).
 * dspdec is a helper process (it links NEC's OpenMAX libraries, which bring LinuxThreads along; the player
 * stays thread-free). Protocol: see the header of homebrew/dspdec/dspdec.c and research/dsp/notes/
 * player_dsp_notes.txt / direct_notes.txt. Uses, one helper, one component at a time:
 *   - decoding (dsp_open / dsp_read): MP3 (libmad takes over if the DSP fails), ATRAC in .oma and WMA (no
 *     fallback), AAC in ADTS (FAAD2 fallback); dsp_au_open / dsp_read: AAC access units demuxed by dec.c (MP4)
 *   - direct sessions (dsp_direct_open / dsp_au_open(direct) / dsp_direct_poll): the DSP decodes AND plays to
 *     the codec ("maximum battery"); we only get positions and the end. While the helper is in direct mode it
 *     owns /dev/sound/dsp: dsp_direct_release() must run before anything else opens it (the engine does this
 *     before aout_start; pausing parks the session and releases it)
 *   - effect stream (dspfx_*): our own 16-bit PCM (FLAC / AAC / libmad) through OMX.NEC.EL.LPCMD.FTB so Sony's
 *     VPT / EQ / Dynamic Normalizer from the DSP effect page apply to every format; 44.1 kHz comes back.
 * Packets tagged with an older seq (before a seek / track change / reset) are dropped. Pipes on the 2.6.10
 * kernel hold only 4 KB, so writes to the helper are O_NONBLOCK and keep reading the helper's output while they
 * wait (otherwise both sides could block on a full pipe). */
#include "hb.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DSPDEC HB_BIN "/dspdec"
#define PKT_MAGIC 0x43505344u
enum { PF_EOS = 1, PF_ERR = 2, PF_READY = 4, PF_PROT = 8, PF_POS = 16, PF_ACK = 32, PF_REL = 64 };
struct pkt { uint32_t magic, seq, len, flags; };
struct dpos { uint32_t ms, off, ebds, state; };
enum { M_NONE, M_DEC, M_FX, M_DIR };

static pid_t pid = -1;
static int to_fd = -1, from_fd = -1, broken, mode, last_prot;
static double broken_at;	/* last failure: after 3 in a row the DSP rests for a while, then gets another chance */
static uint32_t cur_seq;
static unsigned char pcm[8192 + 16] __attribute__((aligned(8)));	/* decode mode: the current packet's PCM */
static int pcm_len, pcm_pos, at_eos;

int dsp_enabled = 1;	/* setting; off = always libmad / FAAD2, no ATRAC / WMA, no DSP effects on other formats */
int dsp_direct = 1;	/* setting "Máxima batería": wired playback decoded and played by the DSP itself */

/* direct sessions */
static int holding;		/* the helper is (or may be) in direct mode: it has /dev/sound/dsp open */
static struct dpos dpos;
static int dir_state;		/* 1 running, 0 ended, -1 failed */
static double dir_heard;	/* last position report: a session that goes quiet has failed */
/* access-unit streams (AAC from MP4) */
static int au_on, au_ended;
static uint32_t au_sent, au_done;
static int (*au_src)(unsigned char *buf, int max);

/* ---------- receive side: bytes from the helper, split into packets ---------- */
static unsigned char rx[32768] __attribute__((aligned(8)));
static int rx_len;

/* read whatever is available (waiting up to ms for the first byte); -1 on EOF/error, else bytes read */
static int rx_pump(int ms)
{
	if (rx_len >= (int)sizeof rx) return 0;
	struct pollfd pf = { from_fd, POLLIN, 0 };
	int r = poll(&pf, 1, ms);
	if (r <= 0) return 0;
	ssize_t n = read(from_fd, rx + rx_len, sizeof rx - rx_len);
	if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
	if (n <= 0) return -1;
	rx_len += n;
	return (int)n;
}

/* the next complete packet in rx (data points into rx, valid until rx_drop); 1 = got one, 0 = need more,
 * -1 = garbage */
static int rx_peek(struct pkt *p, unsigned char **data)
{
	if (rx_len < (int)sizeof *p) return 0;
	memcpy(p, rx, sizeof *p);
	if (p->magic != PKT_MAGIC || p->len > 8192) return -1;
	if (rx_len < (int)(sizeof *p + p->len)) return 0;
	*data = rx + sizeof *p;
	return 1;
}
static void rx_drop(const struct pkt *p)
{
	int n = sizeof *p + p->len;
	memmove(rx, rx + n, rx_len - n);
	rx_len -= n;
}

/* control packets of the current stream (ACK / POS); 1 = it was one (dropped by the caller) */
static int ctl_packet(const struct pkt *p, const unsigned char *d)
{
	if (p->flags & PF_ACK) { if (p->len >= 4) memcpy(&au_done, d, 4); return 1; }
	if (p->flags & PF_POS) {
		if (p->len >= sizeof dpos) memcpy(&dpos, d, sizeof dpos);
		dir_heard = now();
		if (p->flags & PF_EOS) dir_state = 0;
		return 1;
	}
	return (p->flags & PF_REL) != 0;
}

/* ---------- effect stream output FIFO (44.1 kHz stereo S16) ---------- */
#define FIFO_FRAMES 32768	/* 0.74 s at 44.1k: room for one call's output even from 8 kHz input */
static short fifo[FIFO_FRAMES * 2];
static int fifo_n;	/* frames */

static void fail(const char *why);

/* three failures in a row: no helper for 2 minutes (each attempt costs seconds of silence), then retry */
static int dsp_usable(void) { return broken < 3 || now() - broken_at > 120; }

/* move complete packets of the running effect stream from rx into the FIFO; -1 on a helper error */
static int fx_collect(void)
{
	struct pkt p; unsigned char *d; int r;
	while ((r = rx_peek(&p, &d)) == 1) {
		if (p.seq == cur_seq && mode == M_FX && !(p.flags & (PF_POS | PF_ACK | PF_REL))) {
			if (p.flags & PF_ERR) { rx_drop(&p); return -1; }
			int f = p.len / 4;
			if (fifo_n + f > FIFO_FRAMES) return 0;	/* full: leave it in rx (backpressure) */
			memcpy(fifo + 2 * fifo_n, d, f * 4);
			fifo_n += f;
		}
		rx_drop(&p);
	}
	return r < 0 ? -1 : 0;
}

/* ---------- process management ---------- */
static int read_full(int fd, void *p, int n, int ms)
{
	unsigned char *c = p;
	double end = now() + ms / 1000.0;
	while (n > 0) {
		struct pollfd pf = { fd, POLLIN, 0 };
		int left = (int)((end - now()) * 1000);
		if (left <= 0 || poll(&pf, 1, left) <= 0) return -1;
		ssize_t r = read(fd, c, n);
		if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (r <= 0) return -1;
		c += r; n -= r;
	}
	return 0;
}

/* wait up to ms for the helper to exit, discarding its output meanwhile (a paused player leaves it blocked
 * in a write to us, where it can't see "Q") */
static int wait_exit(int ms)
{
	double end = now() + ms / 1000.0;
	while (now() < end) {
		if (waitpid(pid, 0, WNOHANG) != 0) return 1;
		if (from_fd >= 0) {
			struct pollfd pf = { from_fd, POLLIN, 0 };
			if (poll(&pf, 1, 50) > 0) { unsigned char junk[4096]; if (read(from_fd, junk, sizeof junk) <= 0) usleep(20000); }
		} else usleep(50000);
		hb_alive();
	}
	return waitpid(pid, 0, WNOHANG) != 0;
}

void dsp_stop_helper(void)
{
	if (pid > 0) {
		if (to_fd >= 0) { ssize_t w = write(to_fd, "Q\n", 2); (void)w; }
		if (!wait_exit(3000)) {
			kill(pid, SIGTERM);	/* dspdec tears down cleanly from its main loop */
			if (!wait_exit(1500)) {
				kill(pid, SIGKILL);
				if (!wait_exit(3000)) lg("dsp: helper %d does not die (stuck in the driver?)", (int)pid);	/* never block forever */
			}
		}
	}
	if (to_fd >= 0) close(to_fd);
	if (from_fd >= 0) close(from_fd);
	pid = -1; to_fd = from_fd = -1; pcm_len = pcm_pos = 0; rx_len = 0; fifo_n = 0; mode = M_NONE;
	holding = 0; au_on = 0;	/* the process is gone: so is its hold on /dev/sound/dsp */
}

static void fail(const char *why)
{
	lg("dsp: %s", why);
	broken++; broken_at = now();
	dsp_stop_helper();
}

static int spawn(void)
{
	if (pid > 0) return 0;
	if (!dsp_usable() || access(DSPDEC, X_OK)) return -1;
	if (broken >= 3) { broken = 2; lg("dsp: trying the helper again"); }	/* the rest is over: one more chance */
	int a[2], b[2];
	if (pipe(a)) return -1;
	if (pipe(b)) { close(a[0]); close(a[1]); return -1; }
	pid = fork();
	if (pid < 0) { close(a[0]); close(a[1]); close(b[0]); close(b[1]); pid = -1; return -1; }
	if (pid == 0) {
		dup2(a[0], 0); dup2(b[1], 1);
		for (int fd = 3; fd < 1024; fd++) close(fd);	/* no inherited sockets / devices / files */
		char logfn[PATHLEN + 16]; snprintf(logfn, sizeof logfn, "%s/dspdec.log", statedir);
		execl(DSPDEC, "dspdec", logfn, (char *)0);
		_exit(127);
	}
	close(a[0]); close(b[1]);
	to_fd = a[1]; from_fd = b[0];
	set_cloexec(to_fd); set_cloexec(from_fd);
	fcntl(to_fd, F_SETFL, fcntl(to_fd, F_GETFL) | O_NONBLOCK);
	signal(SIGPIPE, SIG_IGN);
	rx_len = 0; fifo_n = 0; mode = M_NONE;
	struct pkt p;
	hb_alive();
	if (read_full(from_fd, &p, sizeof p, 5000) || p.magic != PKT_MAGIC || !(p.flags & PF_READY)) { fail("helper did not start"); return -1; }
	lg("dsp: helper ready (pid %d)", (int)pid);
	dsp_fx_update();
	return 0;
}

/* write all of buf to the helper within ms, reading its output meanwhile; 0 ok */
static int send_all(const void *buf, int n, int ms)
{
	const unsigned char *c = buf;
	double end = now() + ms / 1000.0;
	while (n > 0) {
		ssize_t w = write(to_fd, c, n);
		if (w > 0) { c += w; n -= w; end = now() + ms / 1000.0; continue; }	/* progress: restart the clock */
		if (w < 0 && errno != EAGAIN && errno != EINTR) return -1;
		if (now() > end) return -1;
		struct pollfd pf[2] = { { to_fd, POLLOUT, 0 }, { from_fd, rx_len < (int)sizeof rx ? POLLIN : 0, 0 } };
		if (poll(pf, 2, 50) < 0 && errno != EINTR) return -1;
		if (pf[1].revents & (POLLIN | POLLHUP)) {
			if (rx_pump(0) < 0) return -1;
			if (mode == M_FX && fx_collect() < 0) return -1;
		}
		hb_alive();
	}
	return 0;
}
static int send_cmd(const char *s) { return send_all(s, (int)strlen(s), 2000); }

/* wait up to ms for a packet of seq s with one of the flags in mask (control packets are processed, older
 * seqs dropped); returns its flags, -1 on timeout / helper failure */
static int wait_flags(uint32_t s, uint32_t mask, int ms)
{
	double end = now() + ms / 1000.0;
	for (;;) {
		struct pkt p; unsigned char *d;
		int r = rx_peek(&p, &d);
		if (r < 0) { fail("garbage from helper"); return -1; }
		if (r == 1) {
			int f = p.seq == s ? (int)p.flags : 0;
			if (p.seq == s) ctl_packet(&p, d);
			rx_drop(&p);
			if (f & mask) return f;
			continue;
		}
		hb_alive();
		if (now() > end) return -1;
		if (rx_pump(100) < 0) { fail("helper gone"); return -1; }
	}
}

/* Sony's effects on the DSP page (they act on everything the DSP decodes or processes) */
/* Sony's 6-band EQ (the DSP's own, so it also works in "Máxima batería"). Word = b1 | b1<<4 | b2<<8 | .. | b6<<24,
 * each band 0..6 with 3 = flat and LOWER = MORE level (measured 2026-09-29, 2 dB per step): b1 = Clear Bass
 * (~55 Hz, boost only in Sony's UI), b2..b6 peaks at 400 Hz, 1 k, 2.5 k, 6.3 k, 16 kHz. */
static const unsigned sony_eq_words[5] = { 0xfffffffe, 0x03224322, 0x04332233, 0x02343322, 0x01432311 };	/* off, Heavy, Pop, Jazz, Unique */
unsigned sony_eq_word(void)
{
	if (sony_eq == 5) {	/* Personalizado: Clear Bass 0..3, bands -3..+3 */
		unsigned b1 = 3 - sony_cb, w = b1 | b1 << 4;
		for (int i = 0; i < 5; i++) w |= (unsigned)(3 - sony_band[i]) << (8 + 4 * i);
		return w;
	}
	return sony_eq_words[sony_eq < 0 || sony_eq > 4 ? 0 : sony_eq];
}
/* the values of preset (1..4) in Personalizado terms: Clear Bass 0..3, bands -3..+3 */
void sony_eq_preset_values(int preset, int *cb, int *band)
{
	if (preset < 1 || preset > 4) { *cb = 0; for (int i = 0; i < 5; i++) band[i] = 0; return; }
	unsigned w = sony_eq_words[preset];
	*cb = 3 - (int)(w & 15);
	if (*cb < 0) *cb = 0;
	for (int i = 0; i < 5; i++) band[i] = 3 - (int)((w >> (8 + 4 * i)) & 15);
}
/* start editing from the current preset: Personalizado takes its values */
void sony_eq_to_custom(void) { sony_eq_preset_values(sony_eq, &sony_cb, sony_band); }

void dsp_fx_update(void)
{
	if (pid <= 0 || to_fd < 0) return;
	char cmd[64];
	snprintf(cmd, sizeof cmd, "F %x %x %d\n", sony_vpt < 0 ? 0xffff : sony_vpt, sony_eq_word(), sony_dn);
	if (send_cmd(cmd)) { fail("write"); return; }
	/* DSEE (works on DSP-decoded MP3 when the bitrate is known); level -727 = gemini's headphone value at
	 * volume steps 0..14 (no effect in FTB mode; the direct-mode firmware may use it) */
	/* Clear Stereo: 0x8001 off, -38 / -34 (gemini's two levels); no effect measured on LPCM, maybe in direct mode */
	static const unsigned cs_words[3] = { 0x8001, 0xffffffda, 0xffffffde };
	snprintf(cmd, sizeof cmd, "G %x fd29 %x\n", sony_dsee ? 0x0f : 0, cs_words[sony_cs < 0 || sony_cs > 2 ? 0 : sony_cs]);
	if (send_cmd(cmd)) fail("write");
}

/* ---------- decoding ---------- */

/* starts decoding path from byte offset with codec "mp3" / "at3" / "atx" / "aal" / "aac" (ADTS) / "wma";
 * 0 ok (errors show up in dsp_read) */
int dsp_open(const char *path, long offset, const char *codec)
{
	if (!dsp_enabled || spawn()) return -1;
	char cmd[PATHLEN + 64];
	snprintf(cmd, sizeof cmd, "O %u %ld %s %s\n", ++cur_seq, offset, codec ? codec : "mp3", path);
	mode = M_DEC; fifo_n = 0; last_prot = 0; au_on = 0;
	holding = 0;	/* the helper switches to FTB mode (closing /dev/sound/dsp) before it sends any PCM */
	if (send_cmd(cmd)) { fail("write"); return -1; }
	pcm_len = pcm_pos = 0; at_eos = 0;
	return 0;
}

/* one access unit from src to the helper; 0 = sent (or skipped), 1 = the source ended ("E" sent), -1 = failure */
static int au_push(void)
{
	unsigned char b[2048 + 16];
	int n = au_src(b + 16, 2048);
	if (n < 0) { au_ended = 1; return send_cmd("E\n") ? -1 : 1; }
	if (n == 0) return 0;
	char h[16];
	int hl = snprintf(h, sizeof h, "A %d\n", n);
	memcpy(b + 16 - hl, h, hl);
	if (send_all(b + 16 - hl, hl + n, 1500)) return -1;
	au_sent++;
	return 0;
}

/* AAC access-unit stream (MP4 demuxed by dec.c): src(buf, max) returns the next AU's length, 0 to skip one,
 * -1 at the end. direct 0: PCM through dsp_read; direct 1: the DSP plays (dsp_direct_poll). 0 ok */
int dsp_au_open(int direct, int rate, int channels, int aot, int sbr_ps, int kbps, int (*src)(unsigned char *, int))
{
	if (!dsp_enabled || spawn()) return -1;
	char cmd[96];
	uint32_t s = ++cur_seq;
	snprintf(cmd, sizeof cmd, "M %u %d %d %d %d %d %d 0\n", s, direct, rate, channels, aot, sbr_ps, kbps);
	mode = direct ? M_DIR : M_DEC; fifo_n = 0; last_prot = 0;
	au_on = 1; au_ended = 0; au_sent = au_done = 0; au_src = src;
	pcm_len = pcm_pos = 0; at_eos = 0;
	memset(&dpos, 0, sizeof dpos); dir_state = 1;
	holding = direct;
	if (send_cmd(cmd)) { fail("write"); return -1; }
	if (direct) {	/* the helper answers with a POS packet once the DSP runs (after a core switch: ~0.5 s) */
		int f = wait_flags(s, PF_POS | PF_ERR, 4000);
		if (f < 0 || (f & PF_ERR)) { last_prot = f > 0 && (f & PF_PROT); lg("dsp: direct aac start failed"); mode = M_NONE; au_on = 0; return -1; }
	}
	return 0;
}

void dsp_stop(void)
{
	if (pid > 0 && to_fd >= 0 && (mode == M_DEC || mode == M_DIR)) { cur_seq++; if (send_cmd("S\n")) fail("write"); }
	if (mode == M_DEC || mode == M_DIR) mode = M_NONE;
	pcm_len = pcm_pos = 0; au_on = 0;
}

int dsp_protected(void) { return last_prot; }

/* up to max stereo frames; 0 = end of stream, -1 = failure (caller falls back / skips) */
int dsp_read(short *out, int max)
{
	if (pid <= 0 || mode != M_DEC) return -1;
	if (at_eos) return 0;
	double deadline = now() + 3;
	while (pcm_pos >= pcm_len) {
		struct pkt p; unsigned char *d;
		int r = rx_peek(&p, &d);
		if (r < 0) { fail("garbage from helper"); return -1; }
		if (r == 0) {
			hb_alive();
			if (now() > deadline) { fail("no data"); return -1; }
			/* access units: keep a few with the DSP (only while no packet is waiting in rx, so our writes
			 * can never meet a helper blocked on a full pipe with rx full) */
			if (au_on) {
				int k = 0;
				while (!au_ended && au_sent - au_done < 12 && k++ < 12) {
					int a = au_push();
					if (a < 0) { fail("write"); return -1; }
					if (a > 0) break;
				}
			}
			if (rx_pump(200) < 0) { fail("helper gone"); return -1; }
			continue;
		}
		if (p.seq != cur_seq) { rx_drop(&p); continue; }	/* before the last seek */
		if (ctl_packet(&p, d)) { rx_drop(&p); deadline = now() + 3; continue; }
		if (p.flags & PF_ERR) {
			rx_drop(&p);
			last_prot = !!(p.flags & PF_PROT);
			lg("dsp: decoder error%s", last_prot ? " (protected / unsupported file)" : "");
			mode = M_NONE;	/* the helper itself is fine */
			return -1;
		}
		if (p.flags & PF_EOS) { rx_drop(&p); at_eos = 1; broken = 0; return 0; }
		memcpy(pcm, d, p.len);
		pcm_len = p.len; pcm_pos = 0;
		rx_drop(&p);
	}
	int frames = (pcm_len - pcm_pos) / 4;
	if (frames > max) frames = max;
	memcpy(out, pcm + pcm_pos, frames * 4);
	pcm_pos += frames * 4;
	return frames;
}

/* ---------- direct sessions (the DSP plays to the codec) ---------- */

/* play path from byte offset with codec (mp3 / at3 / atx / aal / aac (ADTS) / wma) on the DSP itself. Waits
 * for the start (a core switch reloads the DSP: up to ~0.5 s). 0 ok */
int dsp_direct_open(const char *path, long offset, const char *codec)
{
	if (!dsp_enabled || spawn()) return -1;
	char cmd[PATHLEN + 64];
	uint32_t s = ++cur_seq;
	snprintf(cmd, sizeof cmd, "R %u %ld %s %s\n", s, offset, codec, path);
	mode = M_DIR; last_prot = 0; au_on = 0; holding = 1;
	memset(&dpos, 0, sizeof dpos); dir_state = 1;
	if (send_cmd(cmd)) { fail("write"); return -1; }
	int f = wait_flags(s, PF_POS | PF_ERR | PF_EOS, 4000);
	if (f < 0 || (f & PF_ERR)) {
		last_prot = f > 0 && (f & PF_PROT);
		lg("dsp: direct start failed (%s)%s", codec, last_prot ? ": protected / unsupported" : "");
		if (mode == M_DIR) mode = M_NONE;
		return -1;
	}
	if (f & PF_EOS) dir_state = 0;	/* nothing to play from there */
	broken = 0; dir_heard = now();
	lg("dsp: direct %s @%ld", codec, offset);
	return 0;
}

/* progress of the direct session: 1 playing, 0 ended, -1 failed. *ms = played time since the (re)start (by the
 * input the DSP consumed), *units = file offset of the consumed input (AU streams: AUs consumed). Non-blocking;
 * also keeps an AU stream fed. */
int dsp_direct_poll(unsigned *ms, unsigned long *units)
{
	if (pid <= 0 || mode != M_DIR) return -1;
	for (int i = 0; i < 16; i++) {
		int r = rx_pump(0);
		if (r < 0) { fail("helper gone"); return -1; }
		struct pkt p; unsigned char *d;
		int k;
		while ((k = rx_peek(&p, &d)) == 1) {
			if (p.seq == cur_seq) {
				if (!ctl_packet(&p, d)) {
					if (p.flags & PF_ERR) { dir_state = -1; last_prot = !!(p.flags & PF_PROT); lg("dsp: direct session error"); }
					else if (p.flags & PF_EOS) dir_state = 0;
				}
			}
			rx_drop(&p);
		}
		if (k < 0) { fail("garbage from helper"); return -1; }
		if (r == 0) break;
	}
	if (au_on && dir_state == 1) {	/* ~3.7 s of access units ahead of the DSP (it holds 24 of them itself) */
		int k = 0;
		while (!au_ended && au_sent - dpos.off < 160 && k++ < 24) {
			int a = au_push();
			if (a < 0) { fail("write"); return -1; }
			if (a > 0) break;
		}
	}
	if (dir_state == 1 && now() - dir_heard > 5) { dir_state = -1; lg("dsp: direct session went quiet"); }	/* reports come 4x a second */
	if (ms) *ms = dpos.ms;
	if (units) *units = dpos.off;
	if (dir_state <= 0) mode = M_NONE;
	return dir_state;
}

void dsp_direct_pause(int p)
{
	if (pid > 0 && mode == M_DIR) { char c[8]; snprintf(c, sizeof c, "Z %d\n", p ? 1 : 0); if (send_cmd(c)) fail("write"); }
	dir_heard = now();	/* a pause is not silence */
}

/* the helper leaves direct mode (closes /dev/sound/dsp); call before opening the audio device. Blocks until it
 * confirmed (normally < 0.3 s); a no-op when it isn't holding the device */
void dsp_direct_release(void)
{
	if (!holding) return;
	holding = 0;
	if (pid <= 0 || to_fd < 0) return;
	char c[24];
	uint32_t s = ++cur_seq;
	snprintf(c, sizeof c, "X %u\n", s);
	if (mode == M_DIR || mode == M_DEC) mode = M_NONE;
	au_on = 0;
	if (send_cmd(c)) { fail("write"); return; }
	if (wait_flags(s, PF_REL, 4000) < 0 && pid > 0) fail("release timeout");	/* killing it frees the device too */
	else lg("dsp: direct mode released");
}


/* ---------- effect stream (Sony effects for FLAC / AAC / libmad output) ---------- */
static int fx_rate;

static int lpcm_rate_ok(int r)
{
	return r == 48000 || r == 44100 || r == 32000 || r == 24000 || r == 22050 || r == 16000 || r == 12000 || r == 11025 || r == 8000;
}

/* worth routing this PCM through the DSP: a Sony effect is selected and the DSP is not already decoding it */
int dspfx_running(void) { return pid > 0 && mode == M_FX; }	/* an LPCMD effect stream is active */

int dspfx_active(int rate)
{
	if (!dsp_enabled || !dsp_usable() || dec_on_dsp() || !lpcm_rate_ok(rate)) return 0;
	return sony_vpt >= 0 || sony_eq > 0 || sony_dn;
}

/* drop everything in flight (seek, stop, effects switched off); the next dspfx_process restarts the stream */
void dspfx_reset(void)
{
	if (mode == M_FX && pid > 0) { cur_seq++; if (send_cmd("S\n")) fail("write"); }
	if (mode == M_FX) mode = M_NONE;
	fifo_n = 0;
}

/* push frames of stereo S16 at rate, return up to max_out processed frames at 44.1 kHz in out (0 while the
 * pipeline fills), -1 = not available (the caller plays the unprocessed PCM) */
int dspfx_process(const short *in, int frames, int rate, short *out, int max_out)
{
	if (!dspfx_active(rate)) { if (mode == M_FX) dspfx_reset(); return -1; }
	if (mode != M_FX || fx_rate != rate) {
		if (spawn()) return -1;
		char cmd[48];
		snprintf(cmd, sizeof cmd, "P %u %d\n", ++cur_seq, rate);
		mode = M_FX; fx_rate = rate; fifo_n = 0;
		holding = 0;	/* P switches the helper to FTB mode first */
		if (send_cmd(cmd)) { fail("fx start"); return -1; }
		lg("dsp: effect stream %d Hz", rate);
	}
	while (frames > 0) {
		int n = frames > 2048 ? 2048 : frames;	/* D payload <= 8 KB */
		char hdr[24];
		int hl = snprintf(hdr, sizeof hdr, "D %d\n", n * 4);
		if (send_all(hdr, hl, 1500) || send_all(in, n * 4, 1500)) { fail("fx stream stalled"); return -1; }
		in += 2 * n; frames -= n;
	}
	for (;;) {	/* everything the helper has sent so far */
		int r = rx_pump(0);
		if (r < 0 || fx_collect() < 0) { fail("fx stream error"); return -1; }
		if (r == 0) break;
	}
	int m = fifo_n < max_out ? fifo_n : max_out;
	memcpy(out, fifo, m * 4);
	memmove(fifo, fifo + 2 * m, (fifo_n - m) * 4);
	fifo_n -= m;
	return m;
}
