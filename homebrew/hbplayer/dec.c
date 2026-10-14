/* dec.c: audio decoders behind one interface. One file open at a time.
 *   MP3  - libmad (fixed point, ARM asm IMDCT), byte-exact resume, optional half-rate synthesis
 *   FLAC - dr_flac (integer), 16/24-bit -> S16
 *   AAC  - .m4a/.mp4 through the minimp4 demuxer, .aac as ADTS: on the DSP (OMX.NEC.EL.AACD.FTB, access units
 *          sent by dspclient.c) when the stream suits it (LC, <= 2 ch, 8..48 kHz), FAAD2 fixed point otherwise
 *          and whenever the DSP refuses / fails (it takes over at the same position)
 *   MP3 on the DSP - dspclient.c / homebrew/dspdec (NEC OpenMAX on the MP201 audio DSP), 44.1 kHz out;
 *          libmad takes over at the same position if the DSP fails
 *   ATRAC3 / ATRAC3plus / ATRAC Advanced Lossless in Sony's OpenMG container (.oma / .aa3 / .omg) - DSP only
 *          (Sony's AT3D / ATXD / AALD components); unencrypted files only: OpenMG DRM is detected and refused
 *   WMA 2 in ASF (.wma) - DSP only (OMX.NEC.EL.WMAD); DRM-protected ASF, WMA 1 / Pro / Lossless are refused
 *   Direct playback (dec_set_direct before dec_open, "Máxima batería"): MP3 / ATRAC / WMA / AAC decoded AND played
 *          by the DSP (dec_direct_step instead of dec_read; positions from the helper); falls back to the paths
 *          above at the same position if the DSP fails
 * Everything is delivered as interleaved S16 stereo at the file's sample rate; the engine resamples if needed. */
#include "hb.h"
#include "mad.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include "neaacdec.h"
#include "../dspdec/oma.h"
#include "../dspdec/aacinfo.h"
#include "../dspdec/asf.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { K_NONE, K_MP3, K_FLAC, K_M4A, K_ADTS, K_OMA, K_WMA };
static int on_dsp;	/* decoded by the DSP helper (FTB: PCM comes back to us) */
static char err_msg[64];	/* why the last dec_open / dec_read failed, for the UI (dec_error) */
static struct oma_info oinfo;	/* K_OMA */
static char mp3_path[PATHLEN];	/* the open file (for the DSP helper; every kind) */
static int kind, rate, channels, total_sec;
static double pos_sec;
static int gain_track_cdb, gain_album_cdb, has_gain;	/* ReplayGain in centi-dB (e.g. -654 = -6.54 dB) */
static long long skip_start, samples_left;		/* MP3 gapless: LAME encoder delay / padding */
static int mp3_sr;	/* the MP3's own sample rate (the DSP always hands back 44.1 kHz) */

int dec_gain_cdb(int album) { return !has_gain ? 0 : album && gain_album_cdb ? gain_album_cdb : gain_track_cdb; }

static void parse_gain_text(const char *key, const char *val)
{
	double v;
	if (!val || sscanf(val, "%lf", &v) != 1 || v < -60 || v > 60) return;
	if (!strcasecmp(key, "REPLAYGAIN_TRACK_GAIN")) { gain_track_cdb = (int)(v * 100); has_gain = 1; }
	else if (!strcasecmp(key, "REPLAYGAIN_ALBUM_GAIN")) { gain_album_cdb = (int)(v * 100); has_gain = 1; }
}

/* ID3v2 TXXX frames "replaygain_*_gain" in the first 256 KB of the tag */
static void mp3_read_gain(int f, long tagsize)
{
	unsigned char h[10];
	lseek(f, 0, SEEK_SET);
	if (read(f, h, 10) != 10 || h[3] < 3) return;
	int v = h[3];
	long size = tagsize - 10;
	if (size > 262144) size = 262144;
	unsigned char *t = malloc(size);
	if (!t || read(f, t, size) != size) { free(t); return; }
	long pos = 0;
	while (pos + 10 <= size) {
		unsigned char *fr = t + pos;
		if (!fr[0]) break;
		unsigned fsz = v == 4 ? ((fr[4] & 127) << 21 | (fr[5] & 127) << 14 | (fr[6] & 127) << 7 | (fr[7] & 127)) : (unsigned)(fr[4] << 24 | fr[5] << 16 | fr[6] << 8 | fr[7]);
		if (fsz > (unsigned)(size - pos - 10)) break;
		if (!memcmp(fr, "TXXX", 4) && fsz > 3 && fr[10] == 0) {	/* ISO-8859-1: key NUL value */
			char key[64], val[32];
			unsigned char *p = fr + 11, *e = fr + 10 + fsz;
			int k = 0; while (p < e && *p && k < 63) key[k++] = *p++; key[k] = 0; p++;
			int m = 0; while (p < e && *p && m < 31) val[m++] = *p++; val[m] = 0;
			parse_gain_text(key, val);
		}
		pos += 10 + fsz;
	}
	free(t);
}

/* ---------- MP3 ---------- */
static int fd = -1, eof, lowpower, bps = 8000;
static long fsize, base, bufpos;
static unsigned char buf[16384 + MAD_BUFFER_GUARD] __attribute__((aligned(8)));
static struct mad_stream st;
static struct mad_frame fr;
static struct mad_synth sy;
#define CUR() (bufpos + (st.this_frame ? (long)(st.this_frame - buf) : 0))

/* Xing/Info + LAME tag in the first MPEG frame: frame count (-> duration and average bitrate, which the
 * first frame's bitrate gets wrong for VBR), seek TOC, encoder delay and padding for gapless */
static long xing_frames;
static double xing_secs;
static unsigned char xing_toc[100];
static int has_toc;
static void mp3_read_lame(int f, long start)
{
	static const int srt[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };
	unsigned char b[4096];
	lseek(f, start, SEEK_SET);
	int n = read(f, b, sizeof b);
	if (n < 200) return;
	int h = -1;	/* the first layer III header: version and rate give the samples per frame */
	for (int j = 0; j + 4 < n && j < 2000; j++)
		if (b[j] == 0xff && (b[j + 1] & 0xe6) == 0xe2 && ((b[j + 1] >> 3) & 3) != 1 && (b[j + 2] >> 4) != 15 && ((b[j + 2] >> 2) & 3) != 3) { h = j; break; }
	if (h < 0) return;
	int ver = (b[h + 1] >> 3) & 3, sr = srt[ver][(b[h + 2] >> 2) & 3], spf = ver == 3 ? 1152 : 576;
	mp3_sr = sr;
	for (int i = h + 4; i + 160 < n && i < h + 200; i++) {
		if (memcmp(b + i, "Xing", 4) && memcmp(b + i, "Info", 4)) continue;
		unsigned flags = b[i + 4] << 24 | b[i + 5] << 16 | b[i + 6] << 8 | b[i + 7];
		int p = i + 8;
		long frames = 0;
		if (flags & 1) { frames = (long)b[p] << 24 | b[p + 1] << 16 | b[p + 2] << 8 | b[p + 3]; p += 4; }
		if (flags & 2) p += 4;
		if (flags & 4) { memcpy(xing_toc, b + p, 100); has_toc = 1; p += 100; }
		if (flags & 8) p += 4;
		if (frames > 0 && sr) {
			xing_frames = frames;
			xing_secs = (double)frames * spf / sr;
			total_sec = (int)xing_secs;
			if (xing_secs > 1) bps = (int)((fsize - start) / xing_secs);
		}
		if (memcmp(b + p, "LAME", 4) && memcmp(b + p, "Lavc", 4) && memcmp(b + p, "Lavf", 4)) return;
		int delay = b[p + 21] << 4 | b[p + 22] >> 4, padding = (b[p + 22] & 15) << 8 | b[p + 23];
		if (frames > 0 && delay >= 0 && delay < 3000 && padding >= 0 && padding < 3000) {
			skip_start = delay + 529;
			samples_left = frames * spf - delay - padding;	/* the Xing frame itself is silent and is skipped below */
		}
		return;
	}
}

/* time <-> file position: the Xing TOC (percent of the duration -> 1/256 of the file) when there is one,
 * else the (average) bitrate */
static long mp3_sec_to_byte(double sec)
{
	if (has_toc && xing_secs > 1) {
		double pct = sec * 100.0 / xing_secs;
		if (pct < 0) pct = 0;
		if (pct > 99.999) pct = 99.999;
		int a = (int)pct;
		double fa = xing_toc[a], fb = a < 99 ? xing_toc[a + 1] : 256;
		return base + (long)((fa + (fb - fa) * (pct - a)) / 256.0 * (fsize - base));
	}
	return base + (long)(sec * bps);
}
static double mp3_byte_to_sec(long bytes)
{
	if (has_toc && xing_secs > 1 && fsize > base) {
		double x = (double)(bytes - base) * 256.0 / (fsize - base);
		for (int a = 0; a < 100; a++) {
			double fa = xing_toc[a], fb = a < 99 ? xing_toc[a + 1] : 256;
			if (x < fb || a == 99) return (a + (fb > fa ? (x - fa) / (fb - fa) : 0)) * xing_secs / 100.0;
		}
	}
	return bps ? (double)(bytes - base) / bps : 0;
}

static long id3_size(int f)
{
	unsigned char h[10];
	lseek(f, 0, SEEK_SET);
	if (read(f, h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
		return 10 + ((h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127));
	return 0;
}

/* bytes per second from the first frame header, for seeking / positions while the DSP decodes */
static void mp3_probe_bps(void)
{
	static const int br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
	static const int br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
	unsigned char h[4096];
	lseek(fd, base, SEEK_SET);
	int n = read(fd, h, sizeof h);
	for (int i = 0; i + 4 <= n; i++)
		if (h[i] == 0xff && (h[i + 1] & 0xe6) == 0xe2) {	/* layer III sync */
			int v = (h[i + 1] >> 3) & 3, bri = h[i + 2] >> 4;
			if (v == 1 || bri == 0 || bri == 15) continue;
			int kbps = v == 3 ? br1[bri] : br2[bri];
			if (kbps && !xing_frames) { bps = kbps * 125; if (!total_sec) total_sec = (int)((fsize - base) / bps); }
			break;
		}
	lseek(fd, base, SEEK_SET);
}

/* where the last frames started (file offset, time): a pause gives back the audio still queued in the driver by
 * restarting at the right frame, exactly, even in a VBR file (dec_rewind) */
#define RING 96	/* ~2.5 s of 26 ms frames */
static struct { long byte; double sec; } ring[RING];
static int ring_n, ring_w;

static void mp3_restart(long at)
{
	ring_n = 0;
	if (at != base) { skip_start = 0; samples_left = 0; }
	mad_stream_finish(&st); mad_stream_init(&st); mad_frame_mute(&fr);
	if (lowpower) mad_stream_options(&st, MAD_OPTION_HALFSAMPLERATE);
	lseek(fd, at, SEEK_SET); bufpos = at; st.buffer = 0; eof = 0;
}

static inline short mad_s16(mad_fixed_t s)
{
	if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
	else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
	return (short)(s >> (MAD_F_FRACBITS + 1 - 16));
}

static int mp3_read(short *out, int max)
{
	for (int tries = 0; tries < 16; tries++) {
		if (st.buffer == 0 || st.error == MAD_ERROR_BUFLEN) {
			size_t keep = st.next_frame ? (size_t)(st.bufend - st.next_frame) : 0;
			if (keep) memmove(buf, st.next_frame, keep);
			if (st.next_frame) bufpos += st.next_frame - buf;
			ssize_t n = eof ? 0 : read(fd, buf + keep, 16384 - keep);
			if (n <= 0) {
				if (eof) return 0;
				eof = 1; memset(buf + keep, 0, MAD_BUFFER_GUARD); n = MAD_BUFFER_GUARD;
			}
			mad_stream_buffer(&st, buf, keep + n);
			st.error = 0;
		}
		if (mad_frame_decode(&fr, &st)) {
			if (MAD_RECOVERABLE(st.error) || st.error == MAD_ERROR_BUFLEN) continue;
			lg("dec: mp3 error %s", mad_stream_errorstr(&st));
			return -1;
		}
		mad_synth_frame(&sy, &fr);
		ring[ring_w].byte = CUR(); ring[ring_w].sec = pos_sec; ring_w = (ring_w + 1) % RING; if (ring_n < RING) ring_n++;
		struct mad_pcm *pc = &sy.pcm;
		if (fr.header.bitrate && !xing_frames) bps = fr.header.bitrate / 8;	/* VBR: keep the Xing average */
		if (!total_sec && bps) total_sec = (int)((fsize - base) / bps);
		rate = pc->samplerate; channels = pc->channels;
		int n = pc->length < max ? pc->length : max, from = 0;
		if (skip_start > 0) {	/* gapless: drop the encoder delay at the very start */
			if (skip_start >= n) { skip_start -= n; continue; }
			from = (int)skip_start; skip_start = 0;
		}
		if (samples_left > 0) {	/* gapless: stop at the real end (before the padding) */
			if (samples_left <= from) return 0;
			if (samples_left < n) n = (int)samples_left;
			samples_left -= n;
		}
		int k = 0;
		for (int i = from; i < n; i++, k++) {
			out[2 * k] = mad_s16(pc->samples[0][i]);
			out[2 * k + 1] = mad_s16(pc->samples[pc->channels > 1 ? 1 : 0][i]);
		}
		pos_sec += (double)k / rate;
		return k ? k : mp3_read(out, max);
	}
	return -1;
}

/* ---------- FLAC ---------- */
static drflac *flac;
static short fbuf[1152 * 8];

static void flac_meta(void *user, drflac_metadata *m)
{
	(void)user;
	if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
	drflac_vorbis_comment_iterator it;
	drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
	drflac_uint32 len;
	const char *c;
	while ((c = drflac_next_vorbis_comment(&it, &len))) {
		char kv[128];
		if (len >= sizeof kv) continue;
		memcpy(kv, c, len); kv[len] = 0;
		char *eq = strchr(kv, '=');
		if (!eq) continue;
		*eq = 0;
		parse_gain_text(kv, eq + 1);
	}
}

/* high resolution: FLAC with more than 16 bits or a rate the wired output can't run at natively (it only
 * does 11.025 / 22.05 / 44.1 kHz): the engine then reads 32-bit samples and converts with rsmp.c */
int dec_hires(void)
{
	if (kind != K_FLAC || !flac) return 0;
	int r = flac->sampleRate;
	/* 16-bit below 44.1 kHz (8 / 16 / 32 kHz ...) stays on the 16-bit path: upsampling gains nothing there */
	return flac->bitsPerSample > 16 || (r != 44100 && r != 22050 && r != 11025 && r > 44100);
}
int dec_bits(void) { return kind == K_FLAC && flac ? flac->bitsPerSample : 16; }

/* stereo frames as left-justified 32-bit samples (FLAC only) */
int dec_read32(int *out, int max)
{
	static drflac_int32 f32[1152 * 8];
	if (kind != K_FLAC || !flac) return -1;
	if (max > 1152) max = 1152;
	drflac_uint64 n = drflac_read_pcm_frames_s32(flac, max, f32);
	if (!n) return 0;
	int ch = flac->channels;
	for (unsigned i = 0; i < n; i++) {
		out[2 * i] = f32[i * ch];
		out[2 * i + 1] = f32[i * ch + (ch > 1 ? 1 : 0)];
	}
	pos_sec = (double)flac->currentPCMFrame / flac->sampleRate;
	return (int)n;
}

static int flac_read(short *out, int max)
{
	if (max > 1152) max = 1152;
	drflac_uint64 n = drflac_read_pcm_frames_s16(flac, max, fbuf);
	if (!n) return 0;
	int ch = flac->channels;
	for (unsigned i = 0; i < n; i++) {
		out[2 * i] = fbuf[i * ch];
		out[2 * i + 1] = fbuf[i * ch + (ch > 1 ? 1 : 0)];
	}
	pos_sec = (double)flac->currentPCMFrame / flac->sampleRate;
	return (int)n;
}

/* ---------- AAC ---------- */
static NeAACDecHandle aac;
static FILE *afile;
static MP4D_demux_t mp4;
static int mp4_track = -1, mp4_sample, mp4_count;
static unsigned char abuf[65536] __attribute__((aligned(8)));
static int alen, apos;	/* ADTS stream buffer */
static short *pend; static int pend_n, pend_ch;	/* decoded samples not yet delivered */
static struct aac_info ainfo;	/* the stream as the DSP sees it (MP4: AudioSpecificConfig, ADTS: first header) */
static int ainfo_ok, aac_kbps, faad_rate;

static int mp4_read_cb(int64_t offset, void *buffer, size_t size, void *token)
{
	FILE *f = token;
	if (fseek(f, (long)offset, SEEK_SET)) return 1;
	return fread(buffer, 1, size, f) != size;
}

static int aac_setup(void)
{
	aac = NeAACDecOpen();
	if (!aac) return -1;
	NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(aac);
	c->outputFormat = FAAD_FMT_16BIT;
	c->downMatrix = 1;
	NeAACDecSetConfiguration(aac, c);
	return 0;
}

/* MP4 sample index at or before sec (binary search on the timestamps) */
static int m4a_sample_at(double sec, double *snapped)
{
	MP4D_track_t *t = &mp4.track[mp4_track];
	unsigned target = (unsigned)(sec * t->timescale);
	int lo = 0, hi = mp4_count - 1, best = 0;
	unsigned best_ts = 0;
	while (lo <= hi) {
		int mid = (lo + hi) / 2; unsigned bytes, ts, dur;
		MP4D_frame_offset(&mp4, mp4_track, mid, &bytes, &ts, &dur);
		if (ts <= target) { best = mid; best_ts = ts; lo = mid + 1; } else hi = mid - 1;
	}
	if (snapped) *snapped = t->timescale ? (double)best_ts / t->timescale : sec;
	return best;
}

/* the next access unit for the DSP helper (dspclient pulls them): length, 0 = skip, -1 = end */
static int m4a_next_au(unsigned char *buf, int max)
{
	if (mp4_sample >= mp4_count) return -1;
	unsigned bytes, ts, dur;
	MP4D_file_offset_t off = MP4D_frame_offset(&mp4, mp4_track, mp4_sample++, &bytes, &ts, &dur);
	if (!bytes || bytes > (unsigned)max) return 0;
	if (fseek(afile, (long)off, SEEK_SET) || fread(buf, 1, bytes, afile) != bytes) return -1;
	return (int)bytes;
}

/* AAC on the DSP (FTB: PCM comes back at 44.1 kHz) from MP4 sample s; 0 ok */
static int m4a_dsp_start(int direct, int s)
{
	if (!ainfo_ok) return -1;
	mp4_sample = s;
	return dsp_au_open(direct, ainfo.rate, ainfo.channels, ainfo.aot, ainfo.sbr | ainfo.ps << 1, aac_kbps, m4a_next_au);
}

static int m4a_open(const char *path, int want_direct)
{
	afile = fopen(path, "rb");
	if (!afile) return -1;
	fseek(afile, 0, SEEK_END); long sz = ftell(afile); fseek(afile, 0, SEEK_SET);
	memset(&mp4, 0, sizeof mp4);
	if (!MP4D_open(&mp4, mp4_read_cb, afile, sz)) { lg("dec: not an mp4"); return -1; }
	mp4_track = -1;
	for (unsigned i = 0; i < mp4.track_count; i++)
		if (mp4.track[i].handler_type == MP4D_HANDLER_TYPE_SOUN && mp4.track[i].dsi_bytes) { mp4_track = i; break; }
	if (mp4_track < 0) { lg("dec: no aac track"); return -1; }
	MP4D_track_t *t = &mp4.track[mp4_track];
	if (aac_setup()) return -1;
	unsigned long sr = 0; unsigned char ch = 0;
	if (NeAACDecInit2(aac, t->dsi, t->dsi_bytes, &sr, &ch) < 0) { lg("dec: aac init failed"); return -1; }
	rate = faad_rate = (int)sr; channels = ch;
	mp4_count = t->sample_count; mp4_sample = 0;
	total_sec = t->timescale ? (int)(((unsigned long long)t->duration_hi << 32 | t->duration_lo) / t->timescale) : 0;
	ainfo_ok = !aac_parse_asc(t->dsi, t->dsi_bytes, &ainfo) && aac_dsp_ok(&ainfo);
	aac_kbps = t->avg_bitrate_bps / 1000;
	if (!aac_kbps && total_sec > 0) aac_kbps = (int)(sz * 8 / 1000 / total_sec);
	lg("dec: aac aot %d %d Hz (out %d) %d ch sbr %d ps %d %d kbps: %s", ainfo.aot, ainfo.rate, ainfo.out_rate, ainfo.channels, ainfo.sbr, ainfo.ps, aac_kbps,
	   ainfo_ok ? "DSP" : "FAAD2 only");
	if (ainfo_ok && !want_direct && !m4a_dsp_start(0, 0)) { on_dsp = 1; rate = 44100; }
	return 0;
}

static int adts_open(const char *path, int want_direct)
{
	afile = fopen(path, "rb");
	if (!afile) return -1;
	fseek(afile, 0, SEEK_END); fsize = ftell(afile); fseek(afile, 0, SEEK_SET);
	alen = fread(abuf, 1, sizeof abuf, afile); apos = 0;
	if (aac_setup()) return -1;
	unsigned long sr = 0; unsigned char ch = 0;
	long used = NeAACDecInit(aac, abuf, alen, &sr, &ch);
	if (used < 0) { lg("dec: adts init failed"); return -1; }
	apos = (int)used; rate = faad_rate = (int)sr; channels = ch;
	total_sec = 0;	/* FAAD path: estimated from the first frame in aac_read */
	int hl, len = 0;
	for (int i = 0; i + 9 <= alen && !len; i++) len = aac_parse_adts(abuf + i, &ainfo, &hl);
	ainfo_ok = len > 0 && aac_dsp_ok(&ainfo);
	if (len > 0 && ainfo.rate) {
		total_sec = (int)((double)fsize / len * 1024 / ainfo.rate);
		aac_kbps = (int)((double)len * 8 * ainfo.rate / 1024 / 1000);
	}
	if (ainfo_ok && !want_direct && !dsp_open(mp3_path, 0, "aac")) { on_dsp = 1; rate = 44100; }
	return 0;
}

static int adts_fill(void)
{
	if (apos > 0) { memmove(abuf, abuf + apos, alen - apos); alen -= apos; apos = 0; }
	if (alen < (int)sizeof abuf) {
		int n = fread(abuf + alen, 1, sizeof abuf - alen, afile);
		if (n > 0) alen += n;
	}
	return alen;
}

static int deliver(short *out, int max)
{
	int n = pend_n < max ? pend_n : max;
	for (int i = 0; i < n; i++) { out[2 * i] = pend[i * pend_ch]; out[2 * i + 1] = pend[i * pend_ch + (pend_ch > 1 ? 1 : 0)]; }
	pend += n * pend_ch; pend_n -= n;
	pos_sec += (double)n / (rate ? rate : 44100);
	return n;
}

static int aac_read(short *out, int max)
{
	if (pend_n > 0) return deliver(out, max);
	NeAACDecFrameInfo info;
	void *s = 0;
	for (int tries = 0; tries < 8 && !s; tries++) {
		if (kind == K_M4A) {
			if (mp4_sample >= mp4_count) return 0;
			unsigned bytes, ts, dur;
			MP4D_file_offset_t off = MP4D_frame_offset(&mp4, mp4_track, mp4_sample++, &bytes, &ts, &dur);
			if (bytes > sizeof abuf) continue;
			if (fseek(afile, (long)off, SEEK_SET) || fread(abuf, 1, bytes, afile) != bytes) return 0;
			s = NeAACDecDecode(aac, &info, abuf, bytes);
		} else {
			if (adts_fill() < 8) return 0;
			s = NeAACDecDecode(aac, &info, abuf + apos, alen - apos);
			apos += info.bytesconsumed ? (int)info.bytesconsumed : 1;
			if (!total_sec && info.bytesconsumed && info.samples && info.channels && rate)
				total_sec = (int)((double)fsize / info.bytesconsumed * ((double)info.samples / info.channels) / rate);
		}
		if (info.error) { lg("dec: aac %s", NeAACDecGetErrorMessage(info.error)); s = 0; continue; }
		if (!info.samples) s = 0;
	}
	if (!s) return -1;
	if (info.samplerate) rate = (int)info.samplerate;
	pend = s; pend_ch = info.channels ? info.channels : 1; pend_n = (int)(info.samples / pend_ch);
	return deliver(out, max);
}

/* FAAD2 position for sec (after the DSP gave up, or a seek without the DSP) */
static void faad_seek(double sec)
{
	pend_n = 0;
	if (kind == K_M4A) {
		double snapped;
		mp4_sample = m4a_sample_at(sec, &snapped);
		NeAACDecPostSeekReset(aac, mp4_sample);
	} else {
		long to = total_sec ? (long)((double)fsize * sec / total_sec) : 0;
		fseek(afile, to, SEEK_SET); alen = apos = 0;
		NeAACDecPostSeekReset(aac, 0);
	}
}

/* ---------- WMA (ASF): DSP only ---------- */
static struct asf_info winfo;

static int wma_open(const char *path, int want_direct)
{
	fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	set_cloexec(fd);
	int r = asf_parse(fd, &winfo);
	fsize = lseek(fd, 0, SEEK_END);
	if (r == ASF_EDRM) { snprintf(err_msg, sizeof err_msg, "Archivo protegido (DRM)"); lg("dec: %s: ASF DRM", path); return -1; }
	if (r) {
		snprintf(err_msg, sizeof err_msg, "Formato WMA no soportado");
		lg("dec: %s: asf %d tag %#x rate %d ch %d bits %d", path, r, winfo.tag, winfo.rate, winfo.channels, winfo.bits);
		return -1;
	}
	rate = 44100; channels = 2;
	total_sec = (int)winfo.duration;
	lg("dec: wma %d Hz %d ch %u kbps, %.1f s", winfo.rate, winfo.channels, winfo.avgbytes * 8 / 1000, winfo.duration);
	if (!want_direct) {
		if (dsp_open(path, 0, "wma")) { snprintf(err_msg, sizeof err_msg, "WMA necesita el DSP"); return -1; }
		on_dsp = 1;
	}
	return 0;
}

static long wma_sec_to_byte(double sec)
{
	if (winfo.duration <= 0) return winfo.data_off;
	return winfo.data_off + (long)((double)(winfo.data_end - winfo.data_off) * sec / winfo.duration);
}

/* ---------- common ---------- */

int dec_is_audio(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".aac") ||
	       !strcasecmp(dot, ".oma") || !strcasecmp(dot, ".aa3") || !strcasecmp(dot, ".omg") || !strcasecmp(dot, ".wma");
}

const char *dec_error(void) { return err_msg[0] ? err_msg : 0; }

/* ---------- direct playback: the DSP decodes and plays to the codec (dspclient.c dsp_direct_*) ---------- */
static int want_dir;	/* dec_set_direct(): allowed for the next dec_open */
static int dir_on;	/* this file plays directly */
static int dir_started;	/* the helper's session runs (started lazily, so the resume seek doesn't start it twice) */
static double dir_base;	/* position (s) where the session (re)started */
static long dir_off;	/* where: byte offset (files) / MP4 sample index */

void dec_set_direct(int on) { want_dir = on; }
int dec_direct(void) { return dir_on; }

static const char *dsp_codec(void)
{
	switch (kind) {
	case K_MP3: return "mp3";
	case K_OMA: return oma_codec_name(oinfo.codec);
	case K_WMA: return "wma";
	case K_ADTS: return "aac";
	}
	return "?";
}

/* start offset for second sec in the file; *snapped = the second it really starts at */
static long offset_for(double sec, double *snapped)
{
	*snapped = sec;
	switch (kind) {
	case K_MP3: return mp3_sec_to_byte(sec);
	case K_OMA:
		if (oinfo.codec <= OMA_ATX) {
			long fr = (long)(sec * 44100 / oinfo.spf);
			*snapped = (double)fr * oinfo.spf / 44100;
			return oinfo.data + fr * oinfo.frame;
		}
		return total_sec ? oinfo.data + (long)((double)(fsize - oinfo.data) * sec / total_sec) : oinfo.data;
	case K_WMA: return wma_sec_to_byte(sec);
	case K_ADTS: return total_sec ? (long)((double)fsize * sec / total_sec) : 0;
	case K_M4A: return m4a_sample_at(sec, snapped);
	}
	return 0;
}

static int dir_start(void)
{
	if (kind == K_M4A) return m4a_dsp_start(1, (int)dir_off);
	return dsp_direct_open(mp3_path, dir_off, dsp_codec());
}

/* the direct session failed: continue the file on the normal path at the same position (the engine opens
 * /dev/sound/dsp again after dsp_direct_release) */
static void dir_to_normal(void)
{
	double snapped;
	long off = offset_for(pos_sec, &snapped);
	dir_on = dir_started = 0;
	dsp_stop();
	pos_sec = snapped;
	switch (kind) {
	case K_MP3:
		mp3_restart(off);
		rate = 44100;
		on_dsp = !dsp_open(mp3_path, off, "mp3");
		break;
	case K_OMA: case K_WMA:
		on_dsp = !dsp_open(mp3_path, off, dsp_codec());
		if (!on_dsp) snprintf(err_msg, sizeof err_msg, kind == K_WMA ? "WMA necesita el DSP" : "ATRAC necesita el DSP");
		break;
	case K_ADTS:
		on_dsp = ainfo_ok && !dsp_open(mp3_path, off, "aac");
		if (on_dsp) rate = 44100; else { rate = faad_rate; faad_seek(snapped); }
		break;
	case K_M4A:
		on_dsp = !m4a_dsp_start(0, (int)off);
		if (on_dsp) rate = 44100; else { rate = faad_rate; faad_seek(snapped); }
		break;
	}
	lg("dec: direct playback off at %.1f s (%s)", pos_sec, on_dsp ? "DSP decoding" : "ARM decoding");
}

/* one engine step of a direct session: 1 playing, 0 the file ended, -1 = no longer direct (the file continues
 * on the normal path from dec_read) */
int dec_direct_step(void)
{
	if (!dir_on) return -1;
	if (!dir_started) {
		if (dir_start()) { lg("dec: direct start failed"); dir_to_normal(); return -1; }
		dir_started = 1;
	}
	unsigned ms = 0; unsigned long units = 0;
	int r = dsp_direct_poll(&ms, &units);
	if (r >= 0) pos_sec = dir_base + ms / 1000.0;
	if (r > 0) return 1;
	if (r == 0) { dir_started = 0; return 0; }
	dir_to_normal();
	return -1;
}

/* short pause: the DSP holds its place (helper command Z), resume is instant. dec_direct_park frees the device. */
void dec_direct_pause(int p)
{
	if (dir_on && dir_started) dsp_direct_pause(p);
}

/* pause: the session is stopped and /dev/sound/dsp released (bells, games, focus mode or a Bluetooth switch can
 * use the output); the next dec_direct_step restarts it a little before where it stopped */
void dec_direct_park(void)
{
	if (dir_on && dir_started) {
		double back = pos_sec - 0.5, snapped;	/* the DSP had consumed a bit more than what was heard */
		if (back < 0) back = 0;
		dir_off = offset_for(back, &snapped);
		dir_base = pos_sec = snapped;
		dsp_stop();
		dir_started = 0;
	}
	dsp_direct_release();
}

/* ---------- open / read / seek ---------- */

void dec_close(void)
{
	if (on_dsp || dir_started) dsp_stop();
	on_dsp = 0;
	dir_on = dir_started = 0;
	if (kind == K_MP3) { mad_synth_finish(&sy); mad_frame_finish(&fr); mad_stream_finish(&st); }
	if (fd >= 0) close(fd);
	fd = -1;
	if (flac) { drflac_close(flac); flac = 0; }
	if (aac) { NeAACDecClose(aac); aac = 0; }
	if (kind == K_M4A) MP4D_close(&mp4);
	if (afile) { fclose(afile); afile = 0; }
	pend_n = 0; kind = K_NONE; rate = channels = total_sec = 0; pos_sec = 0;
	gain_track_cdb = gain_album_cdb = has_gain = 0; skip_start = samples_left = 0;
	xing_frames = 0; xing_secs = 0; has_toc = 0;
	ainfo_ok = 0;
}

/* ATRAC in OpenMG: header checks here (DRM, format), decoding on the DSP */
static int oma_open(const char *path, int want_direct)
{
	fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	set_cloexec(fd);
	int r = oma_parse(fd, &oinfo);
	fsize = lseek(fd, 0, SEEK_END);
	if (r == OMA_EDRM) { snprintf(err_msg, sizeof err_msg, "Archivo protegido (DRM)"); lg("dec: %s: OpenMG DRM, e_id %#x", path, oinfo.eid); return -1; }
	if (r || !oma_dsp_ok(&oinfo)) {
		snprintf(err_msg, sizeof err_msg, "Formato ATRAC no soportado");
		lg("dec: %s: oma %d codec %#x rate %d ch %d frame %d bits %d", path, r, oinfo.codec, oinfo.rate, oinfo.channels, oinfo.frame, oinfo.bits);
		return -1;
	}
	rate = 44100; channels = 2;
	if (oinfo.codec <= OMA_ATX && oinfo.frame > 0)
		total_sec = (int)((double)(fsize - oinfo.data) / oinfo.frame * oinfo.spf / 44100);
	if (!want_direct) {
		if (dsp_open(path, 0, oma_codec_name(oinfo.codec))) { snprintf(err_msg, sizeof err_msg, "ATRAC necesita el DSP"); return -1; }
		on_dsp = 1;
	}
	return 0;
}

int dec_open(const char *path)
{
	dec_close();
	err_msg[0] = 0;
	const char *dot = strrchr(path, '.');
	if (!dot) return -1;
	snprintf(mp3_path, sizeof mp3_path, "%s", path);	/* the path the DSP helper opens (every kind) */
	/* direct ("Máxima batería") for the formats tested end to end: MP3 and ATRAC; the others play via the DSP or ARM */
	int wd = want_dir && dsp_enabled && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".oma") || !strcasecmp(dot, ".aa3") || !strcasecmp(dot, ".omg"));
	int r = -1;
	if (!strcasecmp(dot, ".mp3")) {
		fd = open(path, O_RDONLY);
		if (fd < 0) return -1;
		set_cloexec(fd);
		kind = K_MP3; bps = 8000;
		fsize = lseek(fd, 0, SEEK_END); base = id3_size(fd);
		if (base > 10) mp3_read_gain(fd, base);
		mp3_read_lame(fd, base);
		mad_stream_init(&st); mad_frame_init(&fr); mad_synth_init(&sy);
		mp3_restart(base);
		rate = 44100; channels = 2;
		r = 0;
		mp3_probe_bps();
		if (!wd) on_dsp = !dsp_open(path, base, "mp3");
	} else if (!strcasecmp(dot, ".flac")) {
		flac = drflac_open_file_with_metadata(path, flac_meta, 0, 0);
		if (!flac) return -1;
		kind = K_FLAC; rate = flac->sampleRate; channels = flac->channels;
		total_sec = rate ? (int)(flac->totalPCMFrameCount / rate) : 0;
		r = 0;
	} else if (!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".mp4")) {
		kind = K_M4A;
		r = m4a_open(path, wd);
		if (!r && wd && !ainfo_ok) wd = 0;
	} else if (!strcasecmp(dot, ".aac")) {
		kind = K_ADTS;
		r = adts_open(path, wd);
		if (!r && wd && !ainfo_ok) wd = 0;
	} else if (!strcasecmp(dot, ".oma") || !strcasecmp(dot, ".aa3") || !strcasecmp(dot, ".omg")) {
		kind = K_OMA;
		r = oma_open(path, wd);
	} else if (!strcasecmp(dot, ".wma")) {
		kind = K_WMA;
		r = wma_open(path, wd);
	}
	if (r) { char keep[sizeof err_msg]; memcpy(keep, err_msg, sizeof keep); dec_close(); memcpy(err_msg, keep, sizeof keep); return -1; }
	if (wd && kind != K_FLAC) {	/* direct: starts at the first dec_direct_step (after the resume seek) */
		double snapped;
		dir_on = 1; dir_started = 0; dir_base = 0;
		dir_off = kind == K_MP3 ? base : offset_for(0, &snapped);
		rate = 44100;
	}
	lg("dec: %s rate %d ch %d total %d s%s%s", dot, rate, channels, total_sec, xing_frames ? " (Xing)" : "", dir_on ? " DIRECT" : on_dsp ? " DSP" : "");
	return 0;
}

/* AAC decoded by FAAD2 on the ARM (not the DSP): HE-AAC needs more than 83 MHz */
int dec_soft_aac(void) { return (kind == K_M4A || kind == K_ADTS) && !on_dsp && !dir_on; }

int dec_read(short *out, int max)
{
	if (dir_on) return -1;	/* the engine drives direct sessions with dec_direct_step */
	switch (kind) {
	case K_MP3:
		if (on_dsp) {
			int n;
			while ((n = dsp_read(out, max)) > 0) {
				/* gapless, as on the libmad path: drop the encoder delay and stop before the padding (only when
				 * the DSP's 44.1 kHz output is the file's own rate, so the LAME sample counts apply as they are) */
				int from = 0;
				if (mp3_sr == 44100 && skip_start > 0) {
					if (skip_start >= n) { skip_start -= n; continue; }
					from = (int)skip_start; skip_start = 0;
				}
				if (mp3_sr == 44100 && samples_left > 0) {
					if (samples_left <= from) return 0;
					if (samples_left < n) n = (int)samples_left;
					samples_left -= n;
				}
				if (from) memmove(out, out + 2 * from, (size_t)(n - from) * 4);
				n -= from;
				if (n <= 0) continue;
				pos_sec += (double)n / 44100; return n;
			}
			if (n == 0) return 0;
			on_dsp = 0;	/* DSP failed: continue with libmad from here */
			rate = 44100;
			mp3_restart(mp3_sec_to_byte(pos_sec));
			return mp3_read(out, max);
		}
		return mp3_read(out, max);
	case K_OMA: case K_WMA: {	/* no fallback: an error makes the engine skip the track */
		if (!on_dsp) return -1;
		int n = dsp_read(out, max);
		if (n > 0) { pos_sec += (double)n / 44100; return n; }
		if (n < 0) {
			on_dsp = 0;
			if (kind == K_WMA) snprintf(err_msg, sizeof err_msg, dsp_protected() ? "Formato WMA no soportado" : "Error del DSP (WMA)");
			else snprintf(err_msg, sizeof err_msg, dsp_protected() ? "Formato ATRAC no soportado" : "Error del DSP (ATRAC)");
		}
		return n;
	}
	case K_FLAC: return flac_read(out, max);
	case K_M4A: case K_ADTS:
		if (on_dsp) {
			int n = dsp_read(out, max);
			if (n > 0) { pos_sec += (double)n / 44100; return n; }
			if (n == 0) return 0;
			on_dsp = 0;	/* DSP refused / failed: FAAD2 from here */
			rate = faad_rate;
			lg("dec: aac on the DSP failed at %.1f s, FAAD2", pos_sec);
			faad_seek(pos_sec);
		}
		return aac_read(out, max);
	}
	return -1;
}

int dec_rate(void) { return rate; }
int dec_total_sec(void) { return total_sec; }
double dec_pos_sec(void) { return pos_sec; }
long dec_pos_bytes(void) { return kind != K_MP3 ? 0 : (on_dsp || dir_on) ? mp3_sec_to_byte(pos_sec) : CUR(); }
int dec_on_dsp(void) { return kind != K_FLAC && kind != K_NONE && (on_dsp || dir_on); }

/* a seek of a direct session: restart it there (or remember it until it starts) */
static int dir_seek(double sec)
{
	double snapped;
	long off = offset_for(sec, &snapped);
	if (kind != K_M4A && off >= fsize) return -1;
	if (kind == K_M4A && off >= mp4_count) return -1;
	dir_off = off; dir_base = pos_sec = snapped;
	if (dir_started && dir_start()) { dir_started = 0; lg("dec: direct restart failed"); dir_to_normal(); }
	return 0;
}

int dec_seek_bytes(long bytes)
{
	if (kind != K_MP3 || bytes < base || bytes >= fsize) return -1;
	if (dir_on) { dir_off = bytes; dir_base = pos_sec = mp3_byte_to_sec(bytes); if (dir_started && dir_start()) { dir_started = 0; dir_to_normal(); } return 0; }
	mp3_restart(bytes);
	if (on_dsp && dsp_open(mp3_path, bytes, "mp3")) on_dsp = 0;
	pos_sec = mp3_byte_to_sec(bytes);
	return 0;
}

/* back by secs (the audio a pause dropped from the driver's queue, not heard yet) */
void dec_rewind(double secs)
{
	if (secs < 0.03 || dir_on || kind == K_NONE) return;
	double to = pos_sec - secs;
	if (to < 0) to = 0;
	if (kind == K_MP3 && !on_dsp) {
		for (int i = 1; i <= ring_n; i++) {	/* newest first: the last frame that started at or before "to" */
			int k = (ring_w - i + RING) % RING;
			if (ring[k].sec <= to + 0.001) { long b = ring[k].byte; double s = ring[k].sec; mp3_restart(b); pos_sec = s; return; }
		}
	}
	dec_seek_sec(to);
}

int dec_seek_sec(double sec)
{
	if (sec < 0) sec = 0;
	if (dir_on) return dir_seek(sec);
	switch (kind) {
	case K_MP3: {
		long to = mp3_sec_to_byte(sec);
		if (to >= fsize) return -1;
		mp3_restart(to); pos_sec = sec;
		if (on_dsp && dsp_open(mp3_path, to, "mp3")) on_dsp = 0;
		return 0;
	}
	case K_OMA: case K_WMA: {	/* ATRAC3 / 3plus: whole frames of a fixed size; AAL / WMA: proportional (the helper resyncs) */
		if (!on_dsp) return -1;
		double snapped;
		long to = offset_for(sec, &snapped);
		if (to >= fsize) return -1;
		if (dsp_open(mp3_path, to, dsp_codec())) { on_dsp = 0; return -1; }
		pos_sec = snapped; return 0;
	}
	case K_FLAC:
		if (!drflac_seek_to_pcm_frame(flac, (drflac_uint64)(sec * flac->sampleRate))) return -1;
		pos_sec = sec; return 0;
	case K_M4A: {
		double snapped;
		int s = m4a_sample_at(sec, &snapped);
		if (s >= mp4_count) return -1;
		if (on_dsp) { if (m4a_dsp_start(0, s)) { on_dsp = 0; rate = faad_rate; faad_seek(snapped); } }
		else faad_seek(snapped);
		pos_sec = snapped; return 0;
	}
	case K_ADTS: {
		if (!total_sec) return -1;
		long to = (long)((double)fsize * sec / total_sec);
		if (to >= fsize) return -1;
		if (on_dsp) { if (dsp_open(mp3_path, to, "aac")) { on_dsp = 0; rate = faad_rate; faad_seek(sec); } }
		else faad_seek(sec);
		pos_sec = sec; return 0;
	}
	}
	return -1;
}

void dec_lowpower(int on)
{
	lowpower = on;
	if (kind == K_MP3) st.options = on ? (st.options | MAD_OPTION_HALFSAMPLERATE) : (st.options & ~MAD_OPTION_HALFSAMPLERATE);
}
