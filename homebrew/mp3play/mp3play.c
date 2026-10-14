/* mp3play: MP3 -> OSS PCM player for the NW-A829, using libmad (fixed point).
 * usage: mp3play [-d /dev/sound/dsp] [-t max_seconds] [-b] file.mp3
 *   -b  benchmark: decode only (no audio), report decode speed vs real time */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/soundcard.h>
#include "mad.h"

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static inline short scale(mad_fixed_t s)
{
	s += 1L << (MAD_F_FRACBITS - 16);
	if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
	else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
	return (short)(s >> (MAD_F_FRACBITS + 1 - 16));
}

static int open_dsp(const char *dev, int rate)
{
	int fd = open(dev, O_WRONLY);
	if (fd < 0) { perror(dev); return -1; }
	int fmt = AFMT_S16_LE, ch = 2, r = rate;
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0) perror("SETFMT");
	if (ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) < 0) perror("CHANNELS");
	if (ioctl(fd, SNDCTL_DSP_SPEED, &r) < 0) perror("SPEED");
	printf("dsp %s: fmt=%d ch=%d rate=%d (asked %d)\n", dev, fmt, ch, r, rate);
	return fd;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/sound/dsp";
	double maxsec = 1e9;
	int bench = 0, c;
	while ((c = getopt(argc, argv, "d:t:b")) != -1) {
		if (c == 'd') dev = optarg;
		else if (c == 't') maxsec = atof(optarg);
		else if (c == 'b') bench = 1;
	}
	if (optind >= argc) { fprintf(stderr, "usage: mp3play [-d dev] [-t sec] [-b] file.mp3\n"); return 1; }
	int in = open(argv[optind], O_RDONLY);
	if (in < 0) { perror(argv[optind]); return 1; }

	static unsigned char buf[16384 + MAD_BUFFER_GUARD];
	static short pcm[1152 * 2];
	struct mad_stream st; struct mad_frame fr; struct mad_synth sy;
	mad_stream_init(&st); mad_frame_init(&fr); mad_synth_init(&sy);
	int out = -1, rate = 0, eof = 0;
	double t0 = now(), audio = 0;
	long frames = 0;
	for (;;) {
		if (st.buffer == 0 || st.error == MAD_ERROR_BUFLEN) {
			size_t keep = st.next_frame ? (size_t)(st.bufend - st.next_frame) : 0;
			if (keep) memmove(buf, st.next_frame, keep);
			ssize_t n = eof ? 0 : read(in, buf + keep, 16384 - keep);
			if (n <= 0) {
				if (eof) break;
				eof = 1; memset(buf + keep, 0, MAD_BUFFER_GUARD); n = MAD_BUFFER_GUARD;
			}
			mad_stream_buffer(&st, buf, keep + n);
			st.error = 0;
		}
		if (mad_frame_decode(&fr, &st)) {
			if (MAD_RECOVERABLE(st.error) || st.error == MAD_ERROR_BUFLEN) continue;
			fprintf(stderr, "decode error %s\n", mad_stream_errorstr(&st));
			break;
		}
		mad_synth_frame(&sy, &fr);
		struct mad_pcm *p = &sy.pcm;
		if (!rate) {
			rate = p->samplerate;
			printf("mp3: %d Hz, %d ch, %lu kbps\n", rate, p->channels, fr.header.bitrate / 1000);
			if (!bench && (out = open_dsp(dev, rate)) < 0) return 2;
		}
		for (int i = 0; i < p->length; i++) {
			pcm[2 * i] = scale(p->samples[0][i]);
			pcm[2 * i + 1] = scale(p->samples[p->channels > 1 ? 1 : 0][i]);
		}
		if (!bench && write(out, pcm, p->length * 4) < 0) { perror("write"); break; }
		frames++;
		audio += (double)p->length / p->samplerate;
		if (audio >= maxsec) break;
	}
	double dt = now() - t0;
	printf("frames=%ld audio=%.1fs wall=%.1fs speed=%.2fx realtime\n", frames, audio, dt, dt > 0 ? audio / dt : 0);
	mad_synth_finish(&sy); mad_frame_finish(&fr); mad_stream_finish(&st);
	if (out >= 0) close(out);
	return 0;
}
