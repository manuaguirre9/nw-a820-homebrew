/* oma.h: Sony OpenMG audio container (.oma / .aa3 / .omg) header parser, shared by homebrew/dspdec (DSP helper)
 * and homebrew/hbplayer/dec.c. Reversed from gemini's omg_oma_parser.c (research/dsp/notes/atrac_notes.txt).
 *
 * Layout: optional ID3v2-style tag with the magic "ea3" instead of "ID3" (10-byte header, syncsafe size, +10
 * if the footer flag is set), then the 96-byte "EA3" audio data header, then the codec frames:
 *   [0..2] "EA3"  [3] version (SonicStage 1..3, ffmpeg's muxer writes 0)  [4..5] header size, BE (0x0060)
 *   [6..7] encryption id, BE: 0xFFFF / 0xFFFE / 0xFF80 = not encrypted; anything else (0, 1, 2, 0x11, 0x80,
 *          0x8002 ...) = OpenMG DRM (key material follows at +0x40 / +0x50 / +0x58) -> cannot be decoded
 *   [0x20] codec id: 0 ATRAC3, 1 ATRAC3plus, 3 MP3, 4 LPCM, 5 WMA, 0x21 / 0x22 ATRAC Advanced Lossless
 *   [0x21..0x23] codec parameters:
 *     ATRAC3:     [0x21] bit1 joint stereo; [0x22] bits7-5 rate index (1 = 44.1 kHz);
 *                 frame bytes = (([0x22]&3)<<8 | [0x23]) * 8            (1024 samples per frame)
 *     ATRAC3plus: [0x22] bits7-5 rate index (0 32k, 1 44.1k, 2 48k), bits4-2 channel id (1 mono, 2 stereo);
 *                 frame bytes = (([0x22]&3)<<8 | [0x23]) * 8 + 8        (2048 samples per frame)
 *     AAL:        [0x21] bits7-6 block samples (0x200 / 0x400 / 0x800), [0x22] bits7-6 rate (0 44.1k, 1 48k,
 *                 2 96k), bits5-4 word length (0 16-bit, 1 24-bit); the lossy base layer is described at
 *                 [0x31..0x33] like ATRAC3plus (id 0x21) or ATRAC3 (id 0x22); data = "BLK" blocks.
 */
#ifndef OMA_H
#define OMA_H
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum { OMA_AT3 = 0, OMA_ATX = 1, OMA_MP3 = 3, OMA_LPCM = 4, OMA_WMA = 5, OMA_AAL_X = 0x21, OMA_AAL_3 = 0x22 };
enum { OMA_OK = 0, OMA_ENOTOMA = -1, OMA_EDRM = -2, OMA_EUNSUP = -3, OMA_EIO = -4 };

struct oma_info {
	int codec;		/* OMA_* codec id */
	int version;		/* EA3 header [3] */
	unsigned eid;		/* encryption id */
	int rate, channels;
	int js;			/* ATRAC3 joint stereo */
	int frame;		/* bytes per frame (ATRAC3 / 3plus); AAL: base layer frame bytes */
	int spf;		/* samples per frame (1024 / 2048; AAL: block samples) */
	long data;		/* file offset of the first frame / block */
	/* AAL */
	int base;		/* 0 none, 1 ATRAC3plus, 2 ATRAC3 (the AALD parameter +6) */
	int blkcode;		/* 0 / 1 / 2 for 0x200 / 0x400 / 0x800 samples (AALD +0xc) */
	int bits;
};

static inline int oma_rate(int idx)
{
	static const int t[8] = { 32000, 44100, 48000, 88200, 96000, 0, 0, 0 };
	return t[idx & 7];
}

/* 0 = playable (unencrypted ATRAC3 / ATRAC3plus / AAL); <0 = OMA_E*. info is filled as far as it could be read */
static inline int oma_parse(int fd, struct oma_info *o)
{
	uint8_t h[96];
	long pos = 0;
	memset(o, 0, sizeof *o);
	if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, h, 10) != 10) return OMA_EIO;
	if (!memcmp(h, "ea3", 3) || !memcmp(h, "ID3", 3)) {
		pos = 10 + ((h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127));
		if (h[5] & 0x10) pos += 10;
	} else if (memcmp(h, "EA3", 3)) return OMA_ENOTOMA;
	if (lseek(fd, pos, SEEK_SET) != pos || read(fd, h, 96) != 96) return OMA_EIO;
	if (memcmp(h, "EA3", 3)) return OMA_ENOTOMA;
	int hsz = h[4] << 8 | h[5];
	if (hsz < 96 || hsz > 4096) hsz = 96;
	o->version = h[3];
	o->eid = (unsigned)(h[6] << 8 | h[7]);
	o->codec = h[0x20];
	o->data = pos + hsz;
	if (o->eid != 0xffff && o->eid != 0xfffe && o->eid != 0xff80) return OMA_EDRM;
	const uint8_t *p = h + 0x20;
	switch (o->codec) {
	case OMA_AT3:
		o->js = (p[1] >> 1) & 1;
		o->rate = oma_rate(p[2] >> 5);
		o->channels = 2;
		o->frame = ((p[2] & 3) << 8 | p[3]) * 8;
		o->spf = 1024;
		break;
	case OMA_ATX:
		o->rate = oma_rate(p[2] >> 5);
		o->channels = (p[2] >> 2) & 7;
		o->frame = ((p[2] & 3) << 8 | p[3]) * 8 + 8;
		o->spf = 2048;
		break;
	case 0x20: case OMA_AAL_X: case OMA_AAL_3: {
		static const int bs[4] = { 0x200, 0x400, 0x800, 0 };
		static const int ar[4] = { 44100, 48000, 96000, 0 };
		o->spf = bs[p[1] >> 6];
		o->blkcode = p[1] >> 6;
		o->rate = ar[p[2] >> 6];
		o->bits = ((p[2] >> 4) & 3) == 0 ? 16 : ((p[2] >> 4) & 3) == 1 ? 24 : 0;
		o->channels = 2;
		const uint8_t *b = h + 0x30;	/* base layer parameters, ATRAC3plus / ATRAC3 layout */
		if (o->codec == OMA_AAL_X) { o->base = 1; o->frame = ((b[2] & 3) << 8 | b[3]) * 8 + 8; }
		else if (o->codec == OMA_AAL_3) { o->base = 2; o->frame = ((b[2] & 3) << 8 | b[3]) * 8; }
		if (!o->spf || !o->rate) return OMA_EUNSUP;
		break;
	}
	default:
		return OMA_EUNSUP;
	}
	if (o->frame <= 0 && o->codec <= OMA_ATX) return OMA_EUNSUP;
	return OMA_OK;
}

/* what the DSP components accept (gemini only ever configures 44.1 kHz stereo; see atrac_notes.txt) */
static inline int oma_dsp_ok(const struct oma_info *o)
{
	if (o->rate != 44100 || o->channels != 2) return 0;
	if (o->codec == OMA_AT3) return (o->js && o->frame == 0xc0) || (!o->js && (o->frame == 0x130 || o->frame == 0x180));
	if (o->codec == OMA_ATX) {
		static const int ok[] = { 0xc0, 0x118, 0x178, 0x230, 0x2e8, 0x3a8, 0x460, 0x5d0, 0x748, 0x800 };
		for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) if (o->frame == ok[i]) return 1;
		return 0;
	}
	if (o->codec == OMA_AAL_X || o->codec == OMA_AAL_3) return o->bits == 16;
	return 0;
}

static inline const char *oma_codec_name(int codec)
{
	switch (codec) {
	case OMA_AT3: return "at3";
	case OMA_ATX: return "atx";
	case 0x20: case OMA_AAL_X: case OMA_AAL_3: return "aal";
	}
	return "?";
}
#endif
