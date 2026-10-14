/* i_wmsound.c: sound effects for Doom on the NW-A829. Doom's DS lumps are 8-bit unsigned at 11025 Hz; they are
 * mixed in software (8 channels, volume + stereo separation) straight to /dev/sound/dsp at 11025 Hz S16 stereo,
 * a rate the Walkman's codec supports natively, so no resampling is needed. Music is not supported (needs a
 * MIDI/OPL synthesizer). Master volume comes from HB_VOL (0..20, the player's setting).
 * This file may be distributed under the terms of the GNU GPL (like doomgeneric). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define NCH 8
#define RATE 11025

struct chan { const unsigned char *data; int len, pos, lvol, rvol; };
static struct chan ch[NCH];
static int dsp = -1, master = 4096;
static short mix[1024 * 2];

static snddevice_t devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

static boolean WM_Init(boolean use_sfx_prefix)
{
	(void)use_sfx_prefix;
	const char *v = getenv("HB_VOL");
	if (v) {	/* 2 dB per step, 20 = 0 dB (the player put the codec at 0 dB) */
		int vol = atoi(v), g = 4096;
		for (int s = vol; s < 20; s++) g = g * 3254 / 4096;
		master = vol <= 0 ? 0 : g;
	}
	dsp = open("/dev/sound/dsp", O_WRONLY | O_NONBLOCK);
	if (dsp < 0) { fprintf(stderr, "sound: no dsp\n"); return false; }
	int frag = 0x0008000A, fmt = AFMT_S16_LE, chn = 2, rate = RATE;	/* 8 x 1 KB */
	ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
	ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt);
	ioctl(dsp, SNDCTL_DSP_CHANNELS, &chn);
	ioctl(dsp, SNDCTL_DSP_SPEED, &rate);
	fprintf(stderr, "sound: dsp %d Hz, master %d\n", rate, master);
	return true;
}

static void WM_Shutdown(void) { if (dsp >= 0) close(dsp); dsp = -1; }

static int WM_GetSfxLumpNum(sfxinfo_t *sfx)
{
	char name[16];
	if (sfx->link) sfx = sfx->link;
	snprintf(name, sizeof name, "ds%s", sfx->name);
	return W_GetNumForName(name);
}

static void set_params(int c, int vol, int sep)
{
	if (sep < 0) sep = 0; if (sep > 254) sep = 254;
	ch[c].lvol = vol * (254 - sep) / 254;
	ch[c].rvol = vol * sep / 254;
}

static void WM_UpdateSoundParams(int c, int vol, int sep) { if (c >= 0 && c < NCH) set_params(c, vol, sep); }

static int WM_StartSound(sfxinfo_t *sfx, int c, int vol, int sep)
{
	if (dsp < 0 || c < 0 || c >= NCH) return -1;
	if (sfx->link) sfx = sfx->link;
	if (sfx->lumpnum < 0) return -1;
	int len = W_LumpLength(sfx->lumpnum);
	const unsigned char *d = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
	if (len < 48 || d[0] != 3) { W_ReleaseLumpNum(sfx->lumpnum); return -1; }
	int samples = d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24;
	if (samples > len - 8) samples = len - 8;
	samples -= 32;
	if (samples <= 0) { W_ReleaseLumpNum(sfx->lumpnum); return -1; }
	ch[c].data = d + 16; ch[c].len = samples; ch[c].pos = 0;
	set_params(c, vol, sep);
	return c;
}

static void WM_StopSound(int c) { if (c >= 0 && c < NCH) ch[c].data = 0; }
static boolean WM_SoundIsPlaying(int c) { return c >= 0 && c < NCH && ch[c].data != 0; }

/* mix and write as much as the device accepts without blocking */
static void WM_Update(void)
{
	if (dsp < 0) return;
	audio_buf_info bi;
	if (ioctl(dsp, SNDCTL_DSP_GETOSPACE, &bi) < 0) return;
	int frames = bi.bytes / 4;
	while (frames > 0) {
		int n = frames > 1024 ? 1024 : frames;
		for (int i = 0; i < n; i++) {
			int l = 0, r = 0;
			for (int c = 0; c < NCH; c++) {
				if (!ch[c].data) continue;
				int s = ((int)ch[c].data[ch[c].pos] - 128) << 8;
				l += s * ch[c].lvol / 127; r += s * ch[c].rvol / 127;
				if (++ch[c].pos >= ch[c].len) ch[c].data = 0;
			}
			l = (l * master) >> 12; r = (r * master) >> 12;
			mix[2 * i] = (short)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
			mix[2 * i + 1] = (short)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
		}
		if (write(dsp, mix, n * 4) <= 0) break;
		frames -= n;
	}
}

static void WM_CacheSounds(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; }

sound_module_t DG_sound_module = {
	devices, sizeof devices / sizeof devices[0],
	WM_Init, WM_Shutdown, WM_GetSfxLumpNum, WM_Update, WM_UpdateSoundParams,
	WM_StartSound, WM_StopSound, WM_SoundIsPlaying, WM_CacheSounds,
};

/* no music */
static boolean M_Init(void) { return false; }
static void M_Shutdown(void) {}
static void M_SetVolume(int v) { (void)v; }
static void M_Pause(void) {}
static void M_Resume(void) {}
static void *M_Register(void *d, int l) { (void)d; (void)l; return 0; }
static void M_Unregister(void *h) { (void)h; }
static void M_Play(void *h, boolean loop) { (void)h; (void)loop; }
static void M_Stop(void) {}
static boolean M_Playing(void) { return false; }
static void M_Poll(void) {}
static snddevice_t mdevices[] = { SNDDEVICE_NONE };

music_module_t DG_music_module = {
	mdevices, 1, M_Init, M_Shutdown, M_SetVolume, M_Pause, M_Resume, M_Register, M_Unregister, M_Play, M_Stop, M_Playing, M_Poll,
};

/* variables that i_sound.c binds to the config file when FEATURE_SOUND is on (they belong to the SDL module) */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
