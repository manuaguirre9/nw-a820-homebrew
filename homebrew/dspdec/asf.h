/* asf.h: just enough ASF (.wma) for the DSP's WMA decoder (OMX.NEC.EL.WMAD[.FTB]), shared by homebrew/dspdec and
 * homebrew/hbplayer/dec.c. research/dsp/notes/aac_wma_notes.txt has the component's parameter block.
 *   - header object: File Properties (data packet size, duration, preroll), the audio Stream Properties
 *     (WAVEFORMATEX + WMA extradata), and DRM: Content Encryption / Extended Content Encryption / Digital
 *     Signature objects mean a protected file (refused)
 *   - data object: fixed-size data packets; asf_packet_payload() extracts the audio stream's payload bytes
 *     (single / multiple payloads, compressed payloads), skipping fragments until the first media object start
 * Only WMA 2 (format tag 0x161) is taken by the DSP (0x160 WMA 1, 0x162 Pro, 0x163 Lossless are refused).
 */
#ifndef ASF_H
#define ASF_H
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum { ASF_OK = 0, ASF_ENOTASF = -1, ASF_EDRM = -2, ASF_EUNSUP = -3, ASF_EIO = -4 };

struct asf_info {
	int stream;			/* audio stream number */
	int tag, channels, rate, bits, align;
	unsigned avgbytes;
	unsigned spb, sba;		/* WMA 2 extradata: samples per block, super block align */
	int encopt;			/* WMA 2 extradata: encode options */
	int packet_size;
	long data_off, data_end;	/* first data packet, end of the packets */
	long packets;
	double duration;		/* seconds (play duration - preroll) */
	int drm;
	int synced;			/* reader state: a media object start was seen (after open / seek) */
};

static const uint8_t asf_guid_header[16] = { 0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c };
static const uint8_t asf_guid_data[16] = { 0x36, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c };
static const uint8_t asf_guid_fileprop[16] = { 0xa1, 0xdc, 0xab, 0x8c, 0x47, 0xa9, 0xcf, 0x11, 0x8e, 0xe4, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65 };
static const uint8_t asf_guid_streamprop[16] = { 0x91, 0x07, 0xdc, 0xb7, 0xb7, 0xa9, 0xcf, 0x11, 0x8e, 0xe6, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65 };
static const uint8_t asf_guid_audio[16] = { 0x40, 0x9e, 0x69, 0xf8, 0x4d, 0x5b, 0xcf, 0x11, 0xa8, 0xfd, 0x00, 0x80, 0x5f, 0x5c, 0x44, 0x2b };
static const uint8_t asf_guid_encrypt[16] = { 0xfb, 0xb3, 0x11, 0x22, 0x23, 0xbd, 0xd2, 0x11, 0xb4, 0xb7, 0x00, 0xa0, 0xc9, 0x55, 0xfc, 0x6e };
static const uint8_t asf_guid_extencrypt[16] = { 0x14, 0xe6, 0x8a, 0x29, 0x22, 0x26, 0x17, 0x4c, 0xb9, 0x35, 0xda, 0xe0, 0x7e, 0xe9, 0x28, 0x9c };
static const uint8_t asf_guid_signature[16] = { 0xfc, 0xb3, 0x11, 0x22, 0x23, 0xbd, 0xd2, 0x11, 0xb4, 0xb7, 0x00, 0xa0, 0xc9, 0x55, 0xfc, 0x6e };

static inline uint32_t asf_u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline uint32_t asf_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t asf_u64(const uint8_t *p) { return asf_u32(p) | (uint64_t)asf_u32(p + 4) << 32; }

/* 0 = a WMA 2 stream the DSP can take; else ASF_E* (the info is filled as far as it was read) */
static inline int asf_parse(int fd, struct asf_info *a)
{
	uint8_t h[30];
	memset(a, 0, sizeof *a);
	a->stream = -1;
	if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, h, 30) != 30) return ASF_EIO;
	if (memcmp(h, asf_guid_header, 16)) return ASF_ENOTASF;
	uint64_t hsize = asf_u64(h + 16);
	if (hsize < 30 || hsize > (1 << 20)) return ASF_EUNSUP;
	uint32_t nobj = asf_u32(h + 24);
	long pos = 30;
	uint64_t play = 0, preroll = 0;
	uint8_t o[24 + 128];
	for (uint32_t i = 0; i < nobj && pos + 24 <= (long)hsize; i++) {
		if (lseek(fd, pos, SEEK_SET) != pos || read(fd, o, 24) != 24) return ASF_EIO;
		uint64_t osz = asf_u64(o + 16);
		if (osz < 24 || osz > hsize - (uint64_t)pos) break;	/* in 64 bits: a huge size must not wrap */
		if (!memcmp(o, asf_guid_encrypt, 16) || !memcmp(o, asf_guid_extencrypt, 16) || !memcmp(o, asf_guid_signature, 16)) a->drm = 1;
		else if (!memcmp(o, asf_guid_fileprop, 16) && osz >= 104) {
			if (read(fd, o + 24, 80) != 80) return ASF_EIO;
			play = asf_u64(o + 24 + 40); preroll = asf_u64(o + 24 + 56);
			a->packet_size = (int)asf_u32(o + 24 + 68);	/* minimum data packet size (== maximum) */
			a->packets = (long)asf_u64(o + 24 + 32);
		} else if (!memcmp(o, asf_guid_streamprop, 16) && osz >= 78 && a->stream < 0) {
			int n = osz - 24 > 128 ? 128 : (int)(osz - 24);
			if (read(fd, o + 24, n) != n) return ASF_EIO;
			const uint8_t *s = o + 24;
			if (!memcmp(s, asf_guid_audio, 16)) {
				uint32_t tlen = asf_u32(s + 40);
				const uint8_t *w = s + 54;	/* type specific data: WAVEFORMATEX */
				if (tlen >= 18 && 54 + 18 <= n) {
					a->stream = asf_u16(s + 48) & 0x7f;
					a->tag = asf_u16(w); a->channels = asf_u16(w + 2); a->rate = (int)asf_u32(w + 4);
					a->avgbytes = asf_u32(w + 8); a->align = asf_u16(w + 12); a->bits = asf_u16(w + 14);
					int cb = asf_u16(w + 16);
					if (a->tag == 0x161 && cb >= 10 && 54 + 18 + 10 <= n) {
						a->spb = asf_u32(w + 18); a->encopt = asf_u16(w + 22); a->sba = asf_u32(w + 24);
					}
				}
			}
		}
		pos += (long)osz;
	}
	/* the data object follows the header */
	pos = (long)hsize;
	if (lseek(fd, pos, SEEK_SET) != pos || read(fd, o, 50) != 50) return ASF_EIO;
	if (memcmp(o, asf_guid_data, 16)) return ASF_ENOTASF;
	a->data_off = pos + 50;
	uint64_t dsize = asf_u64(o + 16);
	long fend = lseek(fd, 0, SEEK_END);
	a->data_end = dsize > 50 && pos + (long)dsize <= fend ? pos + (long)dsize : fend;
	if (a->packet_size > 0) {
		long np = (a->data_end - a->data_off) / a->packet_size;
		if (!a->packets || a->packets > np) a->packets = np;
	}
	a->duration = play ? (double)play / 1e7 - (double)preroll / 1e3 : 0;
	if (a->duration < 0) a->duration = 0;
	if (a->drm) return ASF_EDRM;
	if (a->stream < 0 || a->packet_size <= 0 || a->packet_size > 65536) return ASF_EUNSUP;
	if (a->tag != 0x161 || a->bits != 16 || a->channels < 1 || a->channels > 2 || !a->align) return ASF_EUNSUP;
	if (a->avgbytes <= 15 || a->avgbytes >= 2u * (unsigned)a->rate) return ASF_EUNSUP;
	switch (a->rate) { case 48000: case 44100: case 32000: case 22050: case 16000: case 11025: case 8000: break; default: return ASF_EUNSUP; }
	return ASF_OK;
}

static inline uint32_t asf_var(const uint8_t **p, const uint8_t *end, int type)
{
	uint32_t v = 0;
	int n = type == 1 ? 1 : type == 2 ? 2 : type == 3 ? 4 : 0;
	if (*p + n > end) { *p = end; return 0; }
	if (n == 1) v = (*p)[0]; else if (n == 2) v = asf_u16(*p); else if (n == 4) v = asf_u32(*p);
	*p += n;
	return v;
}

/* append the audio stream's payload bytes of one data packet (packet_size bytes at pk) to out (room bytes);
 * returns the byte count, -1 on a malformed packet */
static inline int asf_packet_payload(struct asf_info *a, const uint8_t *pk, uint8_t *out, int room)
{
	const uint8_t *p = pk, *end = pk + a->packet_size;
	if (p[0] & 0x80) {	/* error correction data */
		int n = p[0] & 0x0f;
		if (p[0] & 0x60) return -1;
		p += 1 + n;
	}
	if (p + 2 > end) return -1;
	int ltf = p[0], pf = p[1];
	p += 2;
	uint32_t plen = asf_var(&p, end, (ltf >> 5) & 3);
	asf_var(&p, end, (ltf >> 1) & 3);	/* sequence */
	uint32_t pad = asf_var(&p, end, (ltf >> 3) & 3);
	p += 6;	/* send time, duration */
	if (plen && plen <= (uint32_t)a->packet_size) end = pk + plen;
	if (pad < (uint32_t)(end - pk)) end -= pad; else return -1;
	if (p > end) return -1;
	int multi = ltf & 1, npay = 1, plt = 0;
	if (multi) { if (p >= end) return -1; npay = p[0] & 0x3f; plt = (p[0] >> 6) & 3; p++; }
	int got = 0;
	for (int i = 0; i < npay && p < end; i++) {
		int sn = p[0] & 0x7f;
		p++;
		asf_var(&p, end, (pf >> 4) & 3);	/* media object number */
		uint32_t moff = asf_var(&p, end, (pf >> 2) & 3);
		uint32_t rlen = asf_var(&p, end, pf & 3);
		int compressed = rlen == 1;
		if (compressed) p += 1;	/* presentation time delta (moff is the presentation time) */
		else p += rlen;
		uint32_t len = multi ? asf_var(&p, end, plt) : (uint32_t)(end - p);
		if (p > end || len > (uint32_t)(end - p)) return -1;
		if (sn == a->stream) {
			if (compressed) {	/* sub-payloads: [len byte][data] ..., each a whole media object */
				const uint8_t *q = p, *qe = p + len;
				while (q < qe) {
					int sl = *q++;
					if (sl > qe - q) break;
					a->synced = 1;
					if (got + sl <= room) { memcpy(out + got, q, sl); got += sl; }
					q += sl;
				}
			} else {
				if (moff == 0) a->synced = 1;
				if (a->synced && got + (int)len <= room) { memcpy(out + got, p, len); got += len; }
			}
		}
		p += len;
	}
	return got;
}

/* WMAD parameter block (u16 words; user layout from OmfMcWmadSetParameter 0x3864 / gemini 0x2de70c):
 *   +0 6  +2 2 out channels  +4 1  +6 4 = 44.1 kHz out  +8 5  +a channels  +c rate code  +e 0
 *   +12 kbps (0x2f unknown)  +14 0  +16 tag  +18 u32 avg bytes/s  +1c block align  +1e bits (16)
 *   +20 u32 samples per block  +24 encode options  +28 u32 super block align */
static inline void asf_wma_params(uint16_t *p, const struct asf_info *a)
{
	static const int rates[9] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
	int code = 3;
	for (int i = 0; i < 9; i++) if (rates[i] == a->rate) code = i + 3;
	unsigned kbps = a->avgbytes * 8 / 1000;
	memset(p, 0, 22 * sizeof *p);
	p[0] = 6; p[1] = 2; p[2] = 1; p[3] = 4; p[4] = 5; p[5] = a->channels; p[6] = code; p[7] = 0;
	p[9] = kbps >= 1 && kbps <= 0x3fc ? kbps : 0x2f;
	p[10] = 0;
	p[11] = a->tag;
	p[12] = a->avgbytes & 0xffff; p[13] = a->avgbytes >> 16;
	p[14] = a->align; p[15] = a->bits;
	p[16] = a->spb & 0xffff; p[17] = a->spb >> 16;
	p[18] = a->encopt;
	p[20] = a->sba & 0xffff; p[21] = a->sba >> 16;
}
#endif
