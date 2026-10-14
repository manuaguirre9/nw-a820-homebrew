/* aacinfo.h: AAC stream descriptions for the DSP decoder (OMX.NEC.EL.AACD[.FTB]), shared by homebrew/dspdec and
 * homebrew/hbplayer/dec.c. See research/dsp/notes/aac_wma_notes.txt for the component's parameter block.
 *   - AudioSpecificConfig (MP4 'esds' decoder specific info) -> object type, rates, channels, SBR / PS
 *   - ADTS frame headers
 *   - the AACD parameter block as gemini builds it (GapOmfUtil 0x2dc1c4 + GapAACDecoderCmp 0x2dd370)
 */
#ifndef AACINFO_H
#define AACINFO_H
#include <stdint.h>
#include <string.h>

struct aac_info {
	int aot;		/* core audio object type: 1 Main, 2 LC, 3 SSR, 4 LTP ... */
	int rate;		/* core sampling rate (what one access unit of 1024 samples is counted in) */
	int out_rate;		/* rate after SBR (2 x rate with SBR) */
	int channels;		/* channel configuration 1..7 (0 = PCE, not supported) */
	int sbr, ps;		/* HE-AAC (SBR) / HE-AAC v2 (parametric stereo) signalled */
	int frame960;		/* frameLengthFlag: 960-sample frames (not supported) */
};

static const int aac_rates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };

struct aac_bits { const uint8_t *p; int n, pos; };
static inline unsigned aac_get(struct aac_bits *b, int k)
{
	unsigned v = 0;
	while (k-- > 0) {
		int bit = b->pos < b->n * 8 ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1 : 0;
		v = v << 1 | bit;
		b->pos++;
	}
	return v;
}
static inline int aac_get_rate(struct aac_bits *b)
{
	unsigned i = aac_get(b, 4);
	return i == 15 ? (int)aac_get(b, 24) : aac_rates[i];
}

/* AudioSpecificConfig; 0 ok, -1 garbage */
static inline int aac_parse_asc(const uint8_t *d, int n, struct aac_info *a)
{
	struct aac_bits b = { d, n, 0 };
	memset(a, 0, sizeof *a);
	if (n < 2) return -1;
	int aot = aac_get(&b, 5);
	if (aot == 31) aot = 32 + aac_get(&b, 6);
	a->rate = aac_get_rate(&b);
	a->channels = aac_get(&b, 4);
	a->out_rate = a->rate;
	if (aot == 5 || aot == 29) {	/* explicit hierarchical signalling */
		a->sbr = 1; a->ps = aot == 29;
		a->out_rate = aac_get_rate(&b);
		aot = aac_get(&b, 5);
		if (aot == 31) aot = 32 + aac_get(&b, 6);
	}
	a->aot = aot;
	if (aot >= 1 && aot <= 4) {	/* GASpecificConfig */
		a->frame960 = aac_get(&b, 1);
		if (aac_get(&b, 1)) aac_get(&b, 14);	/* dependsOnCoreCoder: coreCoderDelay */
		aac_get(&b, 1);	/* extensionFlag */
		if (!a->channels) return 0;	/* program_config_element: caller refuses */
		/* backward compatible SBR / PS signalling after the config */
		if (b.pos + 16 <= n * 8 && aac_get(&b, 11) == 0x2b7) {
			int ext = aac_get(&b, 5);
			if (ext == 5) {
				a->sbr = aac_get(&b, 1);
				if (a->sbr) {
					a->out_rate = aac_get_rate(&b);
					if (b.pos + 12 <= n * 8 && aac_get(&b, 11) == 0x548) a->ps = aac_get(&b, 1);
				}
			}
		}
	}
	if (!a->rate) return -1;
	return 0;
}

/* ADTS header at p (at least 9 bytes available); 0 = no sync. Fills a (core values), *hdr = header bytes
 * (7, or 9 with CRC), returns the whole frame length */
static inline int aac_parse_adts(const uint8_t *p, struct aac_info *a, int *hdr)
{
	if (p[0] != 0xff || (p[1] & 0xf6) != 0xf0) return 0;	/* sync, layer 0 */
	int prof = p[2] >> 6, sfi = (p[2] >> 2) & 15, ch = (p[2] & 1) << 2 | p[3] >> 6;
	int len = (p[3] & 3) << 11 | p[4] << 3 | p[5] >> 5;
	if (sfi > 12 || len < 7) return 0;
	memset(a, 0, sizeof *a);
	a->aot = prof + 1; a->rate = a->out_rate = aac_rates[sfi]; a->channels = ch;
	*hdr = (p[1] & 1) ? 7 : 9;
	if (len < *hdr) return 0;
	return len;
}

/* DSP sample-rate code for the parameter block (3 = 48k .. 11 = 8k); 0 = not supported */
static inline int aac_rate_code(int rate)
{
	static const int t[9] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
	for (int i = 0; i < 9; i++) if (t[i] == rate) return i + 3;
	return 0;
}

/* can the DSP take this stream at all (1) - else the player keeps FAAD2 */
static inline int aac_dsp_ok(const struct aac_info *a)
{
	return a->aot >= 1 && a->aot <= 3 && !a->frame960 && a->channels >= 1 && a->channels <= 2 && aac_rate_code(a->rate);
}

/* AACD parameter block (u16 words, index = byte offset / 2), gemini's layout:
 *   +0 6  +2 2 output channels  +4 object type (1 Main 2 LC 3 SSR)  +6 4 = 44.1 kHz out
 *   +8 0 raw access units / 2 ADTS frames (see aac_wma_notes.txt: likely, not confirmed)
 *   +a channels  +c rate code (ADTS: 2 / 3 like gemini's format 6)  +e 0  +10 SBR  +12 bitrate kbps (0x17 =
 *   unknown)  +14 0 */
static inline void aac_params(uint16_t *p, const struct aac_info *a, int adts, int kbps)
{
	p[0] = 6; p[1] = 2; p[2] = a->aot; p[3] = 4;
	p[4] = adts ? 2 : 0;
	p[5] = adts ? 2 : a->channels;	/* gemini's ADTS variant (format 6) always sends 2 / 3 */
	p[6] = adts ? 3 : aac_rate_code(a->rate);
	if (!p[6]) p[6] = 3;
	p[7] = 0;
	p[8] = a->sbr ? 1 : 0;
	p[9] = kbps >= 1 && kbps <= 0x3fc ? kbps : 0x17;
	p[10] = 0;
}
#endif
