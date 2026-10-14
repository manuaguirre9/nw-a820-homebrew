/* kbd.c: typing text with the five-way keys (the podcast search). Sony's firmware has no text keyboard (only an
 * initial-letter picker and the Bluetooth passkey digits), so this one is ours, drawn with Sony's bars and font
 * (scr_keyboard): arrows move, PLAY types the key, BACK deletes (and leaves when the text is empty), OPTION or
 * the OK key accept. Up/down keep the column, so a column of letters is a straight line across the rows. */
#include "hb.h"
#include "screens.h"
#include <string.h>

/* the key of row covering column col */
static int key_at(int mode, int upper, int row, int col)
{
	struct kbd_key k[KBD_COLS];
	int n = kbd_row(mode, upper, row, k);
	for (int i = 0; i < n; i++) if (col >= k[i].col && col < k[i].col + k[i].span) return i;
	return n - 1;
}

static void del_last(char *buf)
{
	size_t l = strlen(buf);
	while (l && (buf[l - 1] & 0xC0) == 0x80) l--;	/* the continuation bytes of a UTF-8 character */
	if (l) l--;
	buf[l] = 0;
}

int kbd_input(const char *title, const char *hint, char *buf, size_t max)
{
	int mode = 0, upper = 0, row = 0, key = 0, col = 0, redraw = 1;
	for (;;) {
		if (redraw && backlight_on) { scr_keyboard(title, buf, hint, "Buscar", mode, upper, row, key); flip(); redraw = 0; }
		idle_tick(0);
		int k = getkey(eng_playing() ? 100 : 1000);
		if (k == K_REDRAW) { redraw = 1; continue; }
		if (k < 0) continue;
		redraw = 1;
		struct kbd_key keys[KBD_COLS];
		int n = kbd_row(mode, upper, row, keys);
		if (k == K_LEFT) { key = key > 0 ? key - 1 : n - 1; col = keys[key].col; }
		else if (k == K_RIGHT) { key = key < n - 1 ? key + 1 : 0; col = keys[key].col; }
		else if (k == K_UP || k == K_DOWN) {
			row = (row + (k == K_UP ? KBD_ROWS - 1 : 1)) % KBD_ROWS;
			key = key_at(mode, upper, row, col);
		}
		else if (k == K_BACK) { if (!buf[0]) return 0; del_last(buf); }
		else if (k == K_OPTION) { if (buf[0]) return 1; }
		else if (k == K_PLAY) {
			const struct kbd_key *p = &keys[key];
			switch (p->kind) {
			case KB_CHAR:
				if (strlen(buf) + strlen(p->ch) < max) strcat(buf, p->ch);
				if (upper == 1 && mode == 0) upper = 0;	/* one capital, like a phone's shift */
				break;
			case KB_SHIFT: upper = !upper; break;
			case KB_MODE: mode = !mode; break;
			case KB_SPACE: if (buf[0] && buf[strlen(buf) - 1] != ' ' && strlen(buf) + 1 < max) strcat(buf, " "); break;
			case KB_DEL: del_last(buf); break;
			case KB_OK: if (buf[0]) return 1; break;
			}
		}
		else ui_volume_key(k);
	}
}
