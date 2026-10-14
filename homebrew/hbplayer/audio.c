/* audio.c: PCM output. The wired codec only runs at 11025/22050/44100 Hz; Bluetooth gets 44100 Hz.
 * Anything else is converted with a linear-interpolation resampler. Bluetooth writes are guarded by a
 * timeout so a dropped headset can never freeze the UI. */
#include "hb.h"
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <unistd.h>

static int fd = -1, in_rate, out_rate, is_bt, is32, total_bytes;
static double opened_at;	/* right after a reset + reopen the driver can refuse the first writes for a moment */
static unsigned int phase, step;	/* 16.16 fixed point position in the input */
static short prev_l, prev_r;
static short obuf[4096 * 2];

int aout_is_bt(void) { return fd >= 0 && is_bt; }
static int force_wired;
void aout_force_wired(int on) { force_wired = on; }

/* Volume: on the wired output the CS42L51's own volume (via the OSS mixer, 0..100 where 76 = 0 dB and
 * ~0.65 dB per unit) is used and the PCM stays bit-exact; Bluetooth has no hardware volume, so there the
 * samples are scaled in software. */
static int mixer_fd = -1, hw_applied = -1;
static void apply_hw_volume(void);
static int rg_cdb;	/* this track's ReplayGain (centi-dB), 0 = none */

/* ReplayGain of the track that starts: on the wired output as codec volume units (0.65 dB each; boosts limited to
 * +3 dB so the codec's digital gain can't clip), on Bluetooth in the software gain */
void aout_set_track_gain(int cdb)
{
	if (cdb > 300) cdb = 300;
	if (cdb < -2400) cdb = -2400;
	if (cdb == rg_cdb) return;
	rg_cdb = cdb;
	hw_applied = -1;
	apply_hw_volume();
}

static void apply_hw_volume(void)
{
	if (fd >= 0 ? is_bt : !eng_direct()) return;	/* direct mode: the DSP owns the device, the mixer is still ours */
	int v = volume <= 0 ? 0 : 76 - ((VOLMAX - volume) * 23 + 5) / 10 + (rg_cdb >= 0 ? rg_cdb + 32 : rg_cdb - 32) / 65;	/* 1.5 dB a step = 2.3 codec units of 0.65 dB */
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	if (volume <= 0) v = 0;
	if (v == hw_applied) return;
	if (mixer_fd < 0) { mixer_fd = open("/dev/sound/mixer", O_RDWR); set_cloexec(mixer_fd); }
	int lr = v | v << 8;
	if (mixer_fd >= 0 && ioctl(mixer_fd, SOUND_MIXER_WRITE_VOLUME, &lr) >= 0) { hw_applied = v; lg("aout: codec volume %d", v); }
}

void aout_set_volume(void) { apply_hw_volume(); }

void aout_hw_unity(void)
{
	if (mixer_fd < 0) { mixer_fd = open("/dev/sound/mixer", O_RDWR); set_cloexec(mixer_fd); }
	int lr = 76 | 76 << 8;
	if (mixer_fd >= 0) ioctl(mixer_fd, SOUND_MIXER_WRITE_VOLUME, &lr);
	hw_applied = -1;
}

int aout_gain(void)
{
	if (fd >= 0 && !is_bt && hw_applied >= 0) return 4096;
	int g = gain_q12(volume);
	if (rg_cdb) g = (int)(g * pow(10.0, rg_cdb / 2000.0));
	return g;
}

int aout_start(int rate)
{
	int want_bt = bt_connected() && !force_wired;
	if (fd >= 0 && rate == in_rate && want_bt == is_bt && !is32) return 0;
	eng_park_output();	/* a paused "Máxima batería" session holds the device */
	aout_stop();
	is_bt = want_bt;
	in_rate = rate;
	out_rate = is_bt ? 44100 : (rate == 11025 || rate == 22050 || rate == 44100) ? rate : 44100;
	fd = open(is_bt ? "/dev/sound/adsp" : "/dev/sound/dsp", O_WRONLY | (is_bt ? O_NONBLOCK : 0));
	if (fd < 0) { lg("aout: open %s failed", is_bt ? "adsp" : "dsp"); return -1; }
	set_cloexec(fd);
	opened_at = now();
	/* 8 x 16 KB (~740 ms at 44.1 kHz; the driver's maximum total is 128 KB): the UI loop only has to come back every
	 * ~250 ms with the screen off, and a CPU spike never clicks. Pause / skip drop the queue (aout_stop_now). */
	int frag = 0x0008000E;
	ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
	int fmt = AFMT_S16_LE, ch = 2, r = out_rate;
	ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
	ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
	ioctl(fd, SNDCTL_DSP_SPEED, &r);
	if (r > 8000 && r < 50000) out_rate = r;
	step = (unsigned int)(((unsigned long long)in_rate << 16) / out_rate);
	phase = 0; prev_l = prev_r = 0;
	audio_buf_info bi;
	total_bytes = 0;
	if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) == 0) { total_bytes = bi.fragstotal * bi.fragsize; lg("aout: buffer %d x %d bytes", bi.fragstotal, bi.fragsize); }
	lg("aout: %s in %d out %d", is_bt ? "bluetooth" : "wired", in_rate, out_rate);
	hw_applied = -1;
	apply_hw_volume();
	return 0;
}

/* 24-bit wired output at 44.1 kHz: the driver takes Sony's format code 0x800 (S32 LE container, the codec uses
 * the top 24 bits). Input is already at 44.1 kHz (rsmp.c) and in the hardware volume domain. */
int aout_is32(void) { return fd >= 0 && is32; }
int aout_start32(void)
{
	if (fd >= 0 && is32) return 0;
	eng_park_output();
	aout_stop();
	fd = open("/dev/sound/dsp", O_WRONLY);
	if (fd < 0) { lg("aout: open dsp failed"); return -1; }
	set_cloexec(fd);
	opened_at = now();	/* the same start-up grace as the 16-bit path (put) */
	int frag = 0x0010000D;	/* 16 x 8 KB: same ~370 ms as the 16-bit path */
	ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
	int fmt = 0x800, ch = 2, r = 44100;
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) || fmt != 0x800) { lg("aout: 32-bit format refused (%#x)", fmt); close(fd); fd = -1; return -1; }
	ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
	ioctl(fd, SNDCTL_DSP_SPEED, &r);
	is_bt = 0; is32 = 1; in_rate = out_rate = 44100;
	{ audio_buf_info bi; total_bytes = ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) == 0 ? bi.fragstotal * bi.fragsize : 0; }
	lg("aout: wired 24-bit (S32) 44100");
	hw_applied = -1;
	apply_hw_volume();
	return 0;
}

static int put(const void *p, int bytes);
int aout_write32(const int *s, int frames)
{
	if (fd < 0 || !is32) return -1;
	return frames > 0 ? put(s, frames * 8) : 0;
}

/* write all bytes; on Bluetooth wait at most 2 s for the device to accept data. ENOMEM: the OSS driver could not get
 * its DMA buffer (high-order allocation while the page cache fills RAM, seen on 16 <-> 24-bit switches and 48k/24
 * FLAC starts): the kernel frees pages soon, so the same buffer is retried for up to 1 s instead of failing the
 * track and reopening in a loop. */
static int put(const void *p, int bytes)
{
	const char *b = p;
	int nomem = 0;	/* 20 ms retries in a row (a count, not now(): the wall clock may be set meanwhile) */
	while (bytes > 0) {
		if (is_bt) {
			fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
			struct timeval tv = { 2, 0 };
			if (select(fd + 1, 0, &ws, 0, &tv) <= 0) { lg("aout: bluetooth write timeout"); return -1; }
		}
		ssize_t n = write(fd, b, bytes);
		if (n < 0) {
			if (errno == EINTR || (errno == EAGAIN && is_bt)) continue;	/* Bluetooth: select above waits */
			if (errno == ENOMEM || errno == EAGAIN) {
				if (++nomem <= 50) { hb_alive(); usleep(20000); continue; }
				lg("aout: write error %d for 1 s", errno);
				return -1;
			}
			if (!is_bt && now() - opened_at < 0.3) { usleep(5000); continue; }	/* still stopping the old stream */
			lg("aout: write error %d", errno);
			return -1;
		}
		nomem = 0;
		b += n; bytes -= n;
	}
	return 0;
}

int aout_write(const short *s, int frames)
{
	if (fd < 0 || frames <= 0) return fd < 0 ? -1 : 0;
	if (in_rate == out_rate) return put(s, frames * 4);
	/* linear interpolation; phase counts input frames (16.16), prev_* is the frame before s[0] */
	int n = 0;
	for (;;) {
		unsigned int i = phase >> 16;
		int f = (int)(phase & 0xffff);
		if ((int)i >= frames) break;
		int l0 = i ? s[2 * (i - 1)] : prev_l, r0 = i ? s[2 * (i - 1) + 1] : prev_r;
		int l1 = s[2 * i], r1 = s[2 * i + 1];
		obuf[2 * n] = (short)(l0 + (int)(((long long)(l1 - l0) * f) >> 16));
		obuf[2 * n + 1] = (short)(r0 + (int)(((long long)(r1 - r0) * f) >> 16));
		if (++n == 4096) { if (put(obuf, n * 4)) return -1; n = 0; }
		phase += step;
	}
	phase -= (unsigned int)frames << 16;
	prev_l = s[2 * (frames - 1)]; prev_r = s[2 * (frames - 1) + 1];
	if (n && put(obuf, n * 4)) return -1;
	return 0;
}

int aout_space_known(void) { return fd >= 0; }

/* seconds of audio in the driver's queue, in input time (what a pause drops) */
double aout_queued_sec(void)
{
	if (fd < 0 || !total_bytes || !out_rate) return 0;
	audio_buf_info bi;
	if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) < 0) return 0;
	int q = total_bytes - bi.bytes;
	return q > 0 ? (double)q / ((is32 ? 8 : 4) * out_rate) : 0;
}

/* the output queue ran completely dry (an audible gap): for the self-test counters */
int aout_empty(void)
{
	if (fd < 0 || !total_bytes) return 0;
	audio_buf_info bi;
	return ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) == 0 && bi.bytes >= total_bytes;
}

/* bytes that can be written without blocking, or -1 if the driver can't tell */
int aout_space(void)
{
	if (fd < 0) return -1;
	audio_buf_info bi;
	if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) < 0) return -1;
	/* express it in input frames' worth of bytes (the resampler may write more than it reads) */
	if (is32) return bi.bytes / 2;	/* in 16-bit stereo frame bytes, like the caller expects */
	return (int)((long long)bi.bytes * in_rate / out_rate);
}

void aout_stop(void)
{
	if (fd >= 0) close(fd);	/* the driver plays out what is queued first */
	fd = -1; is32 = 0;
}

/* pause / skip / seek: stop right away instead of playing out the ~0.4 s already queued. On the wired output the
 * codec volume ramps down over ~15 ms first (an instant cut clicks), then the queue is dropped (SNDCTL_DSP_RESET)
 * and the volume goes back for the next start. */
void aout_stop_now(void)
{
	if (fd < 0) return;
	if (!is_bt && hw_applied > 0 && mixer_fd >= 0) {
		for (int k = 3; k >= 0; k--) {
			int v = hw_applied * k / 4, lr = v | v << 8;
			ioctl(mixer_fd, SOUND_MIXER_WRITE_VOLUME, &lr);
			usleep(4000);
		}
	}
	ioctl(fd, SNDCTL_DSP_RESET, 0);
	close(fd);
	fd = -1; is32 = 0;
	if (hw_applied > 0 && mixer_fd >= 0) { int lr = hw_applied | hw_applied << 8; ioctl(mixer_fd, SOUND_MIXER_WRITE_VOLUME, &lr); }
}
