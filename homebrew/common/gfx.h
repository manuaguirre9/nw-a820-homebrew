/* gfx.h: drawing for the NW-A829 homebrew apps (240x320 RGB565).
 * Everything draws into an off-screen back buffer (fb); gfx_flip() copies it to /dev/fb/0 in one go, so the
 * screen never shows half-drawn frames. Text uses Sony's own 22 px UI font and images come from Sony's UI
 * texture pack, both read at runtime from the Walkman's firmware (res dir), with fallbacks if missing.
 * The same code builds for Windows (no device) to render screenshots. */
#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#define W 240
#define H 320
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

struct img { int w, h; const uint16_t *px; };

int gfx_open(void);			/* back buffer + /dev/fb/0 (device) or just the buffer (PC) */
int gfx_load_res(const char *resdir);	/* Sony font + textures from <resdir>/font, <resdir>/dui_tex */
void gfx_flip(void);			/* show the back buffer */
void gfx_lcd(int on);			/* LCD controller refresh on/off (screen dark) */
void gfx_invalidate(void);
void gfx_copy32(void *dst, const void *src, int n);	/* n multiple of 32, 4-aligned */		/* the screen was drawn by someone else: the next flip copies everything */
void gfx_flip_rect(int x, int y, int w, int h);	/* show part of it */
void gfx_slide(int dir, void (*between)(void));	/* show the back buffer sliding in (dir +1 from the right, -1 from the left) */
int gfx_backlight(int level);		/* 0..100 */
extern uint16_t *gfx_fb;		/* back buffer */
#ifndef GFX_NO_FB_ALIAS
#define fb gfx_fb
#endif
extern int gfx_fd;

void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bottom);
void gfx_frame(int x, int y, int w, int h, uint16_t c);
void gfx_blit(struct img im, int x, int y);		/* opaque */
void gfx_blit_key(struct img im, int x, int y);	/* black (0x0000) pixels are transparent */
struct img gfx_tex(int group, int item);	/* Sony texture; w = 0 if unavailable */
struct img gfx_anim(int item);
int gfx_tex_n(int group);		/* items in a texture group */
void gfx_warm_res(void (*touch)(const void *p, long n));	/* home animation, font, texture index: touch(p, n) each */		/* frame of Sony's home-menu animation (ICX1115_home_anim.dpk) */
struct img gfx_scaled(struct img src, int w, int h, uint16_t *buf);	/* nearest-neighbour resize into buf */

/* text: UTF-8, Sony font (22 px line). scale 2 doubles pixels. Returns x after the text. */
int gfx_text(int x, int y, const char *s, uint16_t c, int scale);
int gfx_text_w(const char *s, int scale);
void gfx_text_clip(int x, int y, const char *s, uint16_t c, int maxw);	/* ends with "..." if too wide */
void gfx_text_center(int cx, int y, const char *s, uint16_t c, int scale);
int gfx_text_wrap(int x, int y, const char *s, uint16_t c, int maxw, int lines, int lineh);
extern const char *(*gfx_tr)(const char *s);	/* optional translation of every text drawn (0 = none) */
/* Sony's bold condensed digits (0-9 : / - .), 18 px high, as on Sony's player (time, "1/86"); returns x after */
int gfx_digits(int x, int y, const char *s, uint16_t c);
int gfx_digits_w(const char *s);
#define LINE_H 22
#endif
