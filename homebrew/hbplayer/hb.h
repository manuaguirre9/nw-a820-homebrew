/* hb.h: shared declarations for the homebrew player modules. */
#ifndef HB_H
#define HB_H
#include "../common/gfx.h"
#include "../common/keys.h"
#include "screens.h"
#include <stdio.h>
#include <time.h>

#define PATHLEN 512
/* our programs run straight from the FAT partition (fmask 0022: executable), no longer copied to /tmp: ramfs pages
 * can never be evicted, so every copied binary (3.6 MB in all) was RAM taken for good */
#define HB_BIN "/contents/HB/bin"
#define K_REDRAW (-2)	/* getkey(): nothing to act on, but the screen must be redrawn */

/* hbplayer.c */
void lg(const char *fmt, ...);
double now(void);
int getkey(int timeout_ms);
void wake(void);
void idle_tick(int playing);		/* backlight, auto-suspend, low battery */
int popup_active(void);			/* volume / hold / note overlay on screen */
int popup_expired(void);		/* 1 once when the overlay should be cleared */
void show_volume(void);			/* draw + flip the volume overlay */
int list_run(struct list_view *v);	/* scrolling list with marquee; returns index or -1 (BACK) */
extern int (*list_idle_hook)(void);
extern int list_vis_top;	/* background work between key polls (e.g. thumbnails); 1 = redraw */
void message(const char *title, const char *l1, const char *l2);	/* draw + flip */
FILE *safe_open(const char *fn, char *tmp, size_t tmpmax);	/* atomic settings save: safe_open ... safe_close */
void safe_close(FILE *f, const char *tmp, const char *fn);
void clock_from_http(const char *date);	/* drift correction from an HTTP Date header (alarm.c) */
extern int quick_open;
int sound_quick_menu(int with_np);	/* OPTION: quick sound settings (settings.c); 1 = open the player */
int ui_volume_key(int k);	/* VOL+/VOL- -> volume change + popup; 1 if it was one */
void msg_wait(double secs);	/* message on screen for up to secs, any key dismisses; audio keeps playing */
void flip(void);			/* gfx_flip, or the pending slide transition */
void nowplaying_run(void);
extern int eng_resume_music;	/* engine.c: resume the next song opened at its saved position (only "Reproduciendo") */		/* the "now playing" screen for whatever the engine plays */
extern int backlight_on;
extern int ui_brightness, ui_dim_secs;	/* settings (saved in pos.txt) */
void settings_save(void);
extern int ui_beep;		/* Ajustes > Pitido: Sony's key beep */
enum { BEEP_ONE, BEEP_TWO, BEEP_THREE, BEEP_LONG };
void beep(int kind);
extern char statedir[PATHLEN];
extern int volume;	/* 0..VOLMAX, 30 = 0 dB, 1.5 dB per step */
#define VOLMAX 30	/* like Sony's 0..30; 30 = 0 dB, 1.5 dB per step */
int gain_q12(int vol);
void set_cloexec(int fd);
void hb_alive(void);			/* watchdog keep-alive; call inside any loop that may run > 10 s */
void ui_tick(void);			/* hb_alive + keep the music playing: call inside any UI wait loop */
static inline short hb_clip(int v)
{
	if (v > 32767) return 32767;
	if (v < -32768) return -32768;
	return (short)v;
}
/* saved positions (books / podcasts) */
int pos_get(const char *path, long *bytes, int *sec);
void pos_set(const char *path, long bytes, int sec);
void pos_save(void);
void set_lastfile(const char *path);

/* engine.c: background playback */
struct track { char path[PATHLEN]; char title[96]; char sub[96]; char genre[32]; char year[8]; };
enum { ENG_BOOK, ENG_MUSIC };
struct eng_info { const char *title, *sub, *genre, *year, *context, *path; int idx, n, sec, total, paused, mode; };
void eng_play(const struct track *list, int n, int idx, int mode, const char *context);
void eng_play2(const struct track *list, int n, int idx, int mode, const char *context, int shuffle);
void eng_tick(void);		/* sleep timer etc.; runs even while paused */
void eng_step(void);		/* decode + output one frame if playing */
void eng_stop(void);
void eng_pause(int p);
void eng_toggle(void);
void eng_next(void);
void eng_prev(void);
void eng_seek(int secs);
void eng_seek_to(double sec);
/* chapters.c: Podcasting 2.0 chapters ("<episode>.chapters.json" next to the audio) */
int chap_load(const char *audio_path);
int chap_count(void);
int chap_index(double sec);
double chap_start(int i);
const char *chap_title(int i);
int eng_active(void);
int eng_playing(void);
int eng_changed(void);		/* 1 once after the track / state changed */
int eng_take_sleep(void);	/* 1 once when the sleep timer fired */
void eng_set_sleep(int minutes);
int eng_sleep_left(void);
void eng_info(struct eng_info *e);
void eng_note(const char *msg);
void eng_lowpower(int on);	/* decode at half sample rate (while a game runs) */	/* short on-screen notice (hbplayer.c) */
extern int eng_shuffle, eng_repeat, eng_sleep_min;

/* audio.c: output to the wired codec (/dev/sound/dsp) or Bluetooth (/dev/sound/adsp), with resampling */
int aout_start(int rate);		/* (re)open for this input rate; 0 on success */
int aout_write(const short *stereo, int frames);	/* interleaved S16 stereo; -1 on error / timeout */
void aout_stop(void);
void aout_stop_now(void);	/* drop what is queued (pause / skip / seek), with a short volume ramp */
int aout_is_bt(void);
int aout_gain(void);			/* Q12 software gain to apply (4096 when the codec does the volume) */
void aout_set_volume(void);		/* apply `volume` to the codec (wired) */
void aout_hw_unity(void);		/* codec at 0 dB (for gnuboy, which scales its own samples) */
int aout_space(void);			/* bytes writable without blocking (input-rate terms), -1 unknown */

/* bt.c: Bluetooth A2DP via Sony/CSR's factory demo app */
int bt_is_on(void);
void set_cpu(int mhz);
/* fsname.c: FAT names are ISO-8859-1 on this mount; our text is UTF-8 */
void fs_to_utf8(const char *in, char *out, size_t max);
void utf8_to_fs(const char *in, char *out, size_t max);
int fs_match(const char *fsname, const char *utf8name);
void remote_mode(void);		/* the Walkman as a Bluetooth remote / keyboard for a phone or PC (homebrew/hid) */
/* podsync.c: podcasts over the phone's Bluetooth tethering: subscriptions, the shared link, background downloads */
struct rss_item;
void podsync_update(int only);	/* sync every show (-1) or one, then the downloads screen */
int dl_active(void);		/* podcast downloads running in the background */
void dl_tick(void);		/* advance them (called from the key wait) */
void dl_cancel(void);
void dl_cancel_quiet(void);	/* same, without restarting Bluetooth audio (suspend / power off) */
int podcast_forget(const char *path);	/* delete an episode + sidecars, never download it again */
int dl_pending(void);		/* episodes found by the last search and not downloaded yet (home badge) */
int dl_percent(void);		/* current episode 0..100, -1 unknown */
const char *dl_row_value(void);
int dl_take_dirty(void);
int dl_add_episode(int fidx, const struct rss_item *item);	/* 0 queued, 1 already there, -1 failed */
int pod_feeds(void);		/* (re)reads feeds.txt: how many shows */
const char *pod_feed_name(int i);
const char *pod_feed_url(int i);
int pod_feed_keep(int i);
void pod_feed_path(int i, char *out, size_t max);
int pod_feed_for_folder(const char *path);	/* -1 = a folder copied by hand */
int pod_feed_find_url(const char *url);
int pod_subscribe(const char *name, const char *url, int keep);	/* index, -1 failed */
int pod_set_keep(int i, int keep);
int pod_unsubscribe(int i, int delete_files);
int pod_net_up(const char *title);	/* the phone link (modal connect when needed); pair with pod_net_down */
void pod_net_down(void);
int pod_net_busy(void);		/* someone holds the link (getkey keeps it serviced) */
int pod_fetch(const char *url, const char *dest, int (*cb)(long got, long total));	/* 0 ok, -1 failed, -2 cancelled */
int pod_count_items(const char *path, long *scan_off, int *items);
const char *pod_phone_name(void);	/* 0 = none chosen */
int pod_choose_phone(void);
/* podui.c: the podcast screens */
void pod_add(void);			/* "Agregar podcast": keyboard, search, subscribe */
int pod_show_option(const char *folder_path, const char *folder_name);	/* OPTION on a show; 1 = list changed */
void pod_menu_option(void);		/* OPTION on "Buscar nuevos" / "Agregar podcast": phone, country */
/* kbd.c: on-screen keyboard. 1 = accepted (text in buf), 0 = cancelled */
int kbd_input(const char *title, const char *hint, char *buf, size_t max);
/* i18n.c: the interface in English (every drawn text passes through gfx_tr) */
extern int ui_lang;		/* 0 español, 1 English */
void ui_lang_set(int lang);
const char *ui_tr(const char *s);
void factory_reset(void);
int batt_pct(double adc);	/* batt.c: % from the (averaged) ADC reading */
void focus_load(const char *v);
void focus_save(char *out, size_t max);	/* hbplayer.c: clear the player's state on /var and restart */
int bt_pick_device(const char *title, const char *hint, char *addr, size_t amax, char *name, size_t nmax);	/* 0 = picked */
int bt_connected(void);
const char *bt_device_name(void);
void bt_menu(void);
void bt_poll(void);
int bt_take_dropped(void);
void bt_link_lost(void);
int bt_avrcp_key(void);
void bt_shutdown(void);
void bt_auto_start(void);		/* remembered device: bring the stack up in the background and connect */

/* cover.c */
int cover_load(const char *path, uint16_t *dst, int size);	/* cached on /contents/HB/cache */
int cover_decode_file(const char *jpg, uint16_t *dst, int size);	/* a JPEG file, not cached (downloaded thumbnails) */
int cover_cached(const char *path, uint16_t *dst, int size);	/* cache only, instant */
extern void (*cover_tick)(void);	/* called while decoding, to keep the audio fed */
extern int (*cover_cancel)(void);	/* optional: returns 1 to give up (a key came); then cover_aborted() is 1 */
int cover_aborted(void);	/* the last cover_load gave up: nothing remembered, try again later */
int key_waiting(void);	/* a key event is queued (not consumed) */

/* dec.c: one open file at a time; MP3 (libmad), FLAC (dr_flac), AAC in .m4a/.mp4/.aac (FAAD2 + minimp4) */
int dec_open(const char *path);		/* 0 ok */
void dec_close(void);
int dec_read(short *stereo, int maxframes);	/* S16 stereo frames returned; 0 = end, -1 = error */
int dec_rate(void);
int dec_total_sec(void);
double dec_pos_sec(void);
void dec_rewind(double secs);	/* back by secs (exact frame on the libmad path) */
double aout_queued_sec(void);	/* audio written to the driver and not played yet */
long dec_pos_bytes(void);		/* MP3: exact byte offset for resume; other formats 0 */
int dec_seek_bytes(long bytes);		/* MP3 only; 0 ok */
int dec_seek_sec(double sec);		/* 0 ok */
void dec_lowpower(int on);		/* MP3 half-rate synthesis while a game runs */
int dec_is_audio(const char *name);	/* by extension: mp3 flac m4a mp4 aac oma aa3 omg wma */
/* direct playback ("Máxima batería"): the DSP decodes and plays to the codec itself */
void dec_set_direct(int on);		/* allow it for the next dec_open (MP3 / ATRAC / WMA / AAC-LC) */
int dec_direct(void);			/* the open file plays directly (drive it with dec_direct_step, not dec_read) */
int dec_direct_step(void);		/* 1 playing, 0 file ended, -1 no longer direct (continue with dec_read) */
void dec_direct_park(void);		/* pause: stop the session, release /dev/sound/dsp (restarts on the next step) */
const char *dec_error(void);		/* why the last dec_open / dec_read failed (for a notice), 0 if unknown */
int dec_gain_cdb(int album);
int dec_on_dsp(void);
int dec_hires(void);			/* FLAC > 16 bit or at a rate the wired output doesn't run at */
int dec_bits(void);
int dec_read32(int *stereo32, int maxframes);	/* left-justified 32-bit stereo frames */
/* rsmp.c: polyphase resampler to 44.1 kHz, 32-bit */
int rs_init(int in_rate);
int rs_process(const int *in, int n, int *out);
/* audio.c: 24-bit wired output (S32 container, Sony's format code 0x800) */
int aout_start32(void);
int aout_write32(const int *s, int frames);
int aout_is32(void);
int eng_hires(void);		/* engine.c: playing through the 24-bit path */
/* voice.c: silence trimming and voice boost for podcasts / audiobooks */
extern int voice_trim_on, voice_boost_on;
int voice_trim(const short *in, int n, int rate, short *out);	/* out: room for n + 480 frames */
void voice_trim_reset(void);
void voice_boost(short *pcm, int n, int rate);
void voice_boost_reset(void);
double voice_saved_sec(void);
/* eq.c headphone effects */
extern int hp_xfeed, hp_loud, hp_autoeq;
int autoeq_load(void);
void autoeq_reload(void);
void aout_set_track_gain(int cdb);	/* ReplayGain of the track (centi-dB) */
int aout_space_known(void);
int dspfx_running(void);
int aout_empty(void);	/* the output queue is completely empty (underrun) */
void eng_park_output(void);	/* a paused direct session releases the audio device */
int eng_underruns(void);
void eng_prof_report(void);
void dec_direct_pause(int p);	/* direct session: pause in place (helper Z) */
int eng_resampling(void);	/* hi-res file being resampled to 44.1 kHz (needs 248 MHz) */
int eng_load_pct(void);	/* estimated CPU of the chain, % at 124 MHz */
int eq_load_pct(void);
void eq_effects_desc(char *out, int max);
const char *eng_not_direct(int book);	/* why not direct ("Máxima batería"), 0 = direct */	/* time-stretch or FAAD2 running on the ARM (not below 124 MHz) */
int dec_soft_aac(void);
int eng_direct(void);		/* engine.c: the current track plays in the DSP's direct mode */			/* the open MP3 is being decoded by the audio DSP */
/* dspclient.c: the MP201 audio DSP through the dspdec helper: decoding (MP3, ATRAC) and Sony's effects */
extern int dsp_enabled;
extern int dsp_direct;			/* setting "Máxima batería" (0/1) */
int dsp_open(const char *path, long offset, const char *codec);	/* codec "mp3" / "at3" / "atx" / "aal" / "aac" / "wma" */
int dsp_au_open(int direct, int rate, int channels, int aot, int sbr_ps, int kbps, int (*src)(unsigned char *buf, int max));
int dsp_direct_open(const char *path, long offset, const char *codec);	/* the DSP plays it (0 ok) */
int dsp_direct_poll(unsigned *ms, unsigned long *units);	/* 1 playing, 0 ended, -1 failed */
void dsp_direct_pause(int p);
void dsp_direct_release(void);		/* free /dev/sound/dsp (the helper leaves direct mode); before aout_start */
int dsp_read(short *stereo, int maxframes);
int dsp_protected(void);		/* the last dsp_read error was a protected / unsupported file */
void dsp_stop(void);
void dsp_stop_helper(void);
extern int sony_cb, sony_band[5], sony_cs;	/* Sony EQ Personalizado (Clear Bass 0..3, 400..16k -3..+3), Clear Stereo 0..2 */
unsigned sony_eq_word(void);
void sony_eq_to_custom(void);
void sony_eq_preset_values(int preset, int *cb, int *band);
void dsp_fx_update(void);
/* effect stream: our 16-bit PCM through the DSP (LPCMD) so Sony's VPT / EQ / normalizer apply to every format */
int dspfx_active(int rate);		/* a Sony effect is on and this PCM is not already DSP-decoded */
int dspfx_process(const short *in, int frames, int rate, short *out, int max_out);	/* -> 44.1k frames, -1 n/a */
void dspfx_reset(void);			/* drop what is in flight (seek / stop) */
extern int sony_vpt, sony_eq, sony_dn, sony_dsee;	/* eq.c: Sony DSP effects (saved in eq.txt) */		/* ReplayGain of the open file in centi-dB, 0 if none */

/* stretch.c: time stretching for speech (speed x100, pitch unchanged) */
void stretch_set_rate(int rate);
void stretch_set_speed(int factor_x100);
void stretch_reset(void);
int stretch_push(const short *pcm, int frames);
int stretch_pull(short *out);		/* one hop of frames, or 0 */

/* lyrics.c */
int lrc_load(const char *path);
void lrc_unload(void);
int lrc_count(void);
int lrc_index(double sec);
const char *lrc_text(int idx);

/* eq.c: sound effects (5-band EQ, bass boost, stereo widening), settings in STATEDIR/eq.txt */
struct eq_settings { int on; int band[5]; int bass; int wide; };	/* band -6..6 dB, bass 0..2 (0/3/6 dB), wide 0/1 */
struct eq_preset { const char *name; int band[5]; };
extern struct eq_settings eq;
extern const struct eq_preset eq_presets[];
extern const int eq_npresets;
void eq_apply(int rate);
void eq_process(short *pcm, int frames, int rate);
int eq_active(void);
void eq_load(const char *statedir);
void eq_save(const char *statedir);

/* alarm.c */
struct alarm_settings { int on, hour, min, src; };	/* src 0 = resume last played, 1 = bells */
extern struct alarm_settings alarm_cfg;
void alarm_program_rtc(void);
int alarm_due(void);
time_t alarm_next(void);
void alarm_load(const char *line);
void alarm_menu(void);
void alarm_ring(void);			/* hbplayer.c */
extern int eng_speed;			/* books/podcasts speed x100 (100, 125, 150, 200) */
extern int replaygain_mode;		/* 0 off, 1 track, 2 album */
void bt_receiver(void);			/* bt.c: the Walkman as a Bluetooth speaker/DAC for the phone */
void aout_force_wired(int on);		/* audio.c: ignore the Bluetooth link for output (receiver mode) */
void focus_bells(void);			/* focus.c: ring bells until a key (alarm) */

/* settings.c: the "Ajustes" screen */
void settings_menu(void);

/* music.c / focus.c */
void music_menu(void);
void music_preload(void);	/* the library from its cache, silently; gives up at the first new / changed file */
/* warm.c: start-up warm-up of resources, code, music folders and cover thumbnails in a low-priority child */
void warm_start(void);
int warm_poll(void);	/* 1 when it has just finished */
void warm_stop(void);	/* before /contents is let go */
void focus_menu(void);
#endif
