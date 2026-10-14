/* hosttest.c: PC (Windows) smoke test of the gpSP core configuration used on the Walkman.
 * Same core options as walkman_gba.c (built-in open BIOS, auto frameskip, 32768 Hz), interpreter instead of the
 * ARM dynarec. Runs a ROM for N frames, then writes out/<name>.ppm (240x160) and out/<name>_rot.ppm (the 240x320
 * portrait framebuffer as walkman_gba.c's mode 0 would fill it), checks save state round trip and prints audio
 * statistics. Usage: hosttest rom.gba [frames] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "libretro.h"

static uint16_t last[240 * 160];
static int got_frame, skipped;
static long audio_frames, audio_peak;
static unsigned pad;

static void logp(enum retro_log_level l, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
static const char *option(const char *k)
{
	if (!strcmp(k, "gpsp_bios")) return "builtin";
	if (!strcmp(k, "gpsp_boot_mode")) return "game";
	if (!strcmp(k, "gpsp_frameskip")) return "auto";
	if (!strcmp(k, "gpsp_sound_rate")) return "32768";
	if (!strcmp(k, "gpsp_color_correction")) return "disabled";
	if (!strcmp(k, "gpsp_frame_mixing")) return "disabled";
	return 0;
}
static struct retro_audio_buffer_status_callback bs;
static bool env(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logp; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: { struct retro_variable *v = data; v->value = option(v->key); return v->value != 0; }
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: if (data) bs = *(struct retro_audio_buffer_status_callback *)data; return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: fprintf(stderr, "msg: %s\n", ((struct retro_message *)data)->msg); return true;
	default: return false;
	}
}
static void video(const void *d, unsigned w, unsigned h, size_t pitch)
{
	if (!d) { skipped++; return; }
	for (unsigned y = 0; y < h && y < 160; y++) memcpy(last + y * 240, (const char *)d + y * pitch, 240 * 2);
	got_frame++;
}
static size_t audio(const int16_t *s, size_t n)
{
	for (size_t i = 0; i < n * 2; i++) { long a = s[i] < 0 ? -s[i] : s[i]; if (a > audio_peak) audio_peak = a; }
	audio_frames += (long)n; return n;
}
static void audio1(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio(s, 1); }
static void poll(void) {}
static int16_t state(unsigned port, unsigned dev, unsigned idx, unsigned id) { return id == RETRO_DEVICE_ID_JOYPAD_MASK ? (int16_t)pad : (pad >> id) & 1; }

static void ppm(const char *path, const uint16_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6 %d %d 255\n", w, h);
	for (int i = 0; i < w * h; i++) { uint16_t c = px[i]; unsigned char rgb[3] = { (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3 }; fwrite(rgb, 1, 3, f); }
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 2) return 1;
	int frames = argc > 2 ? atoi(argv[2]) : 300;
	retro_set_environment(env);
	retro_set_video_refresh(video);
	retro_set_audio_sample(audio1);
	retro_set_audio_sample_batch(audio);
	retro_set_input_poll(poll);
	retro_set_input_state(state);
	retro_init();
	struct retro_game_info gi = { argv[1], 0, 0, 0 };
	if (!retro_load_game(&gi)) { printf("load failed\n"); return 2; }
	for (int i = 0; i < frames; i++) {
		if (bs.callback) bs.callback(true, 100, false);
		if (argc > 3 && i % 60 == 30) pad = 1u << RETRO_DEVICE_ID_JOYPAD_START; else if (i % 60 == 34) pad = 0;
		retro_run();
	}
	/* save state round trip */
	size_t n = retro_serialize_size();
	void *b = malloc(n);
	int ok1 = retro_serialize(b, n), ok2 = retro_unserialize(b, n);
	retro_run();
	const char *name = strrchr(argv[1], '/'); name = name ? name + 1 : argv[1];
	char p[512];
	snprintf(p, sizeof p, "out/%s.ppm", name); ppm(p, last, 240, 160);
	/* mode 0 mapping exactly as walkman_gba.c: screen (x, y) = game (y * 240 / 320, 159 - (x - 13) * 160 / 213) */
	static uint16_t scr[240 * 320];
	for (int y = 0; y < 320; y++)
		for (int x = 0; x < 213; x++) scr[y * 240 + 13 + x] = last[(159 - x * 160 / 213) * 240 + y * 240 / 320];
	snprintf(p, sizeof p, "out/%s_rot.ppm", name); ppm(p, scr, 240, 320);
	printf("%s: frames drawn %d skipped %d, audio %ld frames (%.0f Hz), peak %ld, state %zu bytes ser %d unser %d, sram size %zu\n",
		name, got_frame, skipped, audio_frames, audio_frames * 59.7275 / (frames + 1), audio_peak, n, ok1, ok2,
		retro_get_memory_size(RETRO_MEMORY_SAVE_RAM));
	retro_unload_game();
	retro_deinit();
	return 0;
}
