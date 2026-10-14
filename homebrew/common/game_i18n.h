/* game_i18n.h: the emulators' pause menus in English when the player starts them with HB_LANG=en (the player's own
 * translation is hbplayer/i18n.c; these small programs only need their menu). Include once, then call
 * game_i18n_init() at start. */
#ifndef GAME_I18N_H
#define GAME_I18N_H
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const game_tr_tab[][2] = {
	{ "Juego en pausa", "Game paused" }, { "Continuar", "Resume" }, { "Guardar estado", "Save state" }, { "Cargar estado", "Load state" },
	{ "Salir del juego", "Quit game" }, { "Pantalla: rotada", "Screen: rotated" }, { "Pantalla: normal", "Screen: normal" },
	{ "Pantalla: horizontal 1.33x", "Screen: landscape 1.33x" }, { "Pantalla: horizontal 1:1", "Screen: landscape 1:1" },
	{ "Pantalla: vertical 1:1", "Screen: portrait 1:1" }, { "Estado guardado", "State saved" }, { "No se pudo guardar", "Could not save" },
	{ "No hay estado guardado", "No saved state" }, { "Volumen: (sin sonido)", "Volume: (muted)" },
	{ "No se puede abrir la ROM", "Can't open the ROM" }, { "No se pudo cargar el juego", "Could not load the game" },
	{ "\xc2\xbfMemoria insuficiente o ROM da\xc3\xb1" "ada?", "Not enough memory or a damaged ROM?" },
};

static const char *game_tr(const char *s)
{
	static char b[96];
	for (unsigned i = 0; i < sizeof game_tr_tab / sizeof *game_tr_tab; i++) if (!strcmp(s, game_tr_tab[i][0])) return game_tr_tab[i][1];
	if (!strncmp(s, "Volumen: ", 9)) { snprintf(b, sizeof b, "Volume: %s", s + 9); return b; }	/* "Volumen: 12  < >" */
	return s;
}

static void game_i18n_init(void)
{
	const char *l = getenv("HB_LANG");
	if (l && !strcmp(l, "en")) gfx_tr = game_tr;
}
#endif
