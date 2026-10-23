/* i18n.c: the interface in English. Every text the player draws or measures goes through gfx_tr (common/gfx.c), so
 * the Spanish strings in the code stay the source and this table maps what is shown. Patterns: "%d" is a run of
 * digits, "%s" any text (the shortest that lets the rest match; a captured text that is itself in the table is
 * translated too, so "Aleatorio: sí" becomes "Shuffle: on"). Longer / more specific patterns come first.
 * Song titles and names pass through unchanged (no entry matches them). A small cache keeps the last results. */
#include "hb.h"
#include <string.h>

int ui_lang;	/* 0 español, 1 English (pos.txt "lang") */

struct tr { const char *es, *en; };
static const struct tr tab[] = {
	/* home and main lists */
	{ "Música", "Music" }, { "Audiolibros", "Audiobooks" }, { "Podcasts", "Podcasts" }, { "Juegos", "Games" },
	{ "Ajustes", "Settings" }, { "Modo foco", "Focus" }, { "Reproduciendo", "Now playing" }, { "Inicio", "Home" },
	{ "Volver al Walkman (Sony)", "Back to the Walkman (Sony)" }, { "Volviendo al Walkman...", "Back to the Walkman..." },
	{ "Apagar", "Power" }, { "Suspender", "Sleep" }, { "Apagar del todo", "Turn off completely" },
	{ "Aleatorio (todas)", "Shuffle all" }, { "Canciones", "Songs" }, { "Artistas", "Artists" }, { "Álbumes", "Albums" },
	{ "Aleatorio", "Shuffle" }, { "Artista desconocido", "Unknown artist" }, { "Sin álbum", "No album" },
	{ "Ver como lista", "Show as list" }, { "Ver como mosaico", "Show as grid" }, { "Opciones", "Options" },
	{ "Leyendo la biblioteca...", "Reading the library..." }, { "Abriendo...", "Opening..." },
	{ "No hay canciones", "No songs" }, { "No hay archivos MP3", "No audio files" }, { "(vacío)", "(empty)" }, { "Vacío", "Empty" },
	{ "No encontré MP3 en H:\\Music.", "No music found in H:\\Music." },
	{ "Copialos desde la PC y volvé a entrar.", "Copy it from the PC and come back." },
	{ "Copiá carpetas a H:\\Audiolibros", "Copy folders to H:\\Audiolibros" },
	{ "Copiá carpetas a H:\\Podcasts", "Copy folders to H:\\Podcasts" },
	{ "Copiá juegos a H:\\Juegos (GB), Juegos\\GBA, Juegos\\NES, Juegos\\Doom", "Copy games to H:\\Juegos (GB), Juegos\\GBA, Juegos\\NES, Juegos\\Doom" },
	{ "No hay nada sonando", "Nothing is playing" }, { "%d canciones", "%d songs" }, { "canción", "song" },
	{ "Terminados", "Finished" }, { "En cola", "Queued" }, { "en cola", "queued" },
	/* playback */
	{ "PAUSA", "PAUSED" }, { "Temporizador: no", "Sleep timer: off" }, { "Temporizador: %d min", "Sleep timer: %d min" },
	{ "Velocidad: %d.%dx", "Speed: %d.%dx" }, { "Velocidad", "Speed" }, { "Aleatorio: %s", "Shuffle: %s" },
	{ "Capítulos (%d)", "Chapters (%d)" }, { "Capítulos", "Chapters" }, { "Capítulo %d", "Chapter %d" },
	{ "Auriculares", "Headphones" }, { "Bluetooth", "Bluetooth" }, { "No hay salida de audio.", "No audio output." },
	{ "No hay salida de audio", "No audio output" }, { "Bluetooth desconectado", "Bluetooth disconnected" },
	{ "Archivo protegido (DRM)", "Protected file (DRM)" }, { "ATRAC necesita el DSP", "ATRAC needs the DSP" },
	{ "WMA necesita el DSP", "WMA needs the DSP" }, { "Error del DSP (ATRAC)", "DSP error (ATRAC)" }, { "Error del DSP (WMA)", "DSP error (WMA)" },
	{ "Formato ATRAC no soportado", "Unsupported ATRAC format" }, { "Formato WMA no soportado", "Unsupported WMA format" },
	{ "escuchado", "played" }, { "quedan %d min", "%d min left" }, { "en %d:%d", "at %d:%d" },
	{ "Repetir: todo", "Repeat: all" }, { "Repetir: una", "Repeat: one" }, { "Repetir: no", "Repeat: off" },
	{ "sí", "on" }, { "no", "off" },
	/* dates: "25 jun", "25 jun 2026", "25 jun · 1 h 12 min" */
	{ "%d ene%s", "%d Jan%s" }, { "%d feb%s", "%d Feb%s" }, { "%d mar%s", "%d Mar%s" }, { "%d abr%s", "%d Apr%s" },
	{ "%d may%s", "%d May%s" }, { "%d jun%s", "%d Jun%s" }, { "%d jul%s", "%d Jul%s" }, { "%d ago%s", "%d Aug%s" },
	{ "%d sep%s", "%d Sep%s" }, { "%d oct%s", "%d Oct%s" }, { "%d nov%s", "%d Nov%s" }, { "%d dic%s", "%d Dec%s" },
	{ " · en el Walkman", " · on the Walkman" }, { "en el Walkman", "on the Walkman" },
	/* settings */
	{ "Sonido", "Sound" }, { "Pantalla", "Display" }, { "Despertador", "Alarm" }, { "Pitido", "Beep" }, { "Acerca de", "About" },
	{ "Idioma", "Language" }, { "Restablecer", "Reset" }, { "Brillo", "Brightness" }, { "Apagar pantalla", "Screen off after" },
	{ "Sony (negro)", "Sony" }, { "Azul noche", "Navy" }, { "Verde", "Green" }, { "Violeta", "Violet" }, { "Vino", "Wine" }, { "Gris", "Grey" },
	{ "Filas grandes con portada", "Big rows with covers" }, { "máx. batería", "max. battery" }, { "efectos", "effects" },
	{ "Hora", "Time" }, { "Sonar con", "Ring with" }, { "lo último", "last played" }, { "activado", "on" }, { "apagado", "off" }, { "campanas", "bells" }, { "personal", "custom" }, { "procesado", "processed" }, { "ecualizador", "equalizer" }, { "crossfeed", "crossfeed" },
	{ "izq/der: horas / minutos", "left/right: h / min" }, { "arriba/abajo: cambiar", "up/down: change" },
	{ "PLAY: guardar", "PLAY: save" }, { "Empezar", "Start" }, { "cualquier tecla: apagar", "any key: stop" },
	{ "cualquier tecla: seguir", "any key: continue" }, { "Preparando la campana...", "Preparing the bell..." },
	{ "Ecualizador", "Equalizer" }, { "Ecualizador avanzado", "Advanced equalizer" }, { "EQ avanzado", "Advanced EQ" },
	{ "EQ avanzado (procesador)", "Advanced EQ (processor)" }, { "Preset", "Preset" }, { "Personalizado", "Custom" },
	{ "Personaliz.", "Custom" }, { "Plano", "Flat" }, { "Exclusivo", "Unique" }, { "Efecto de son. des.", "Effects off" },
	{ "Bajos", "Bass" }, { "Agudos", "Treble" }, { "Realce de bajos", "Bass boost" }, { "realce de bajos", "bass boost" },
	{ "Estéreo amplio", "Wide stereo" }, { "estéreo amplio", "wide stereo" }, { "Graves a volumen bajo", "Bass at low volume" },
	{ "graves a volumen bajo", "bass at low volume" }, { "Crossfeed", "Crossfeed" },
	{ "Perfil de auriculares", "Headphone profile" }, { "perfil de auriculares", "headphone profile" },
	{ "Copiá el perfil AutoEq de tus auriculares a H:\\HB\\auriculares.txt", "Copy your headphones' AutoEq profile to H:\\HB\\auriculares.txt" },
	{ "(ParametricEQ.txt de autoeq.app)", "(ParametricEQ.txt from autoeq.app)" },
	{ "Voz", "Voice" }, { "Voz / podcast", "Voice / podcast" }, { "Voz y podcasts", "Voice and podcasts" },
	{ "Realce de voz", "Voice boost" }, { "Recortar silencios", "Trim silences" }, { "Volumen parejo", "Even volume" },
	{ "por pista", "per track" }, { "por álbum", "per album" }, { "Normalizador", "Normalizer" },
	{ "Efectos de Sony", "Sony effects" }, { "VPT (sonido envolvente)", "VPT (surround)" }, { "DSEE (mejora MP3)", "DSEE (MP3 enhancer)" },
	{ "Máxima batería", "Max. battery" }, { "DSP de Sony", "Sony DSP" }, { "Ahora", "Now" },
	{ "Ahora: máxima batería", "Now: max. battery" }, { "Ahora: procesado", "Now: processed" },
	{ "El DSP de Sony decodifica y reproduce", "Sony's DSP decodes and plays" },
	{ "solo: el procesador casi no trabaja.", "by itself: the processor barely works." },
	{ "No usa Máxima batería porque:", "Not using Max. battery because:" }, { "Máxima batería está desactivado", "Max. battery is off" },
	{ "efectos activos: %s", "effects on: %s" }, { "efectos de voz activos", "voice effects on" },
	{ "el DSP de Sony está apagado", "Sony's DSP is off" }, { "el audio va por Bluetooth", "audio goes over Bluetooth" },
	{ "velocidad distinta de 1x", "speed other than 1x" }, { "Modo 1", "Mode 1" }, { "Modo 2", "Mode 2" }, { "Modo 3", "Mode 3" },
	{ "Modo 4", "Mode 4" }, { "Modo 5", "Mode 5" }, { "Modo 6", "Mode 6" }, { "Duración", "Length" }, { "Campana cada", "Bell every" },
	{ "Fondo", "Background" }, { "ruido blanco", "white noise" }, { "ruido rosa", "pink noise" }, { "ruido marrón", "brown noise" },
	{ "campana al inicio y al final", "bell at start and end" }, { "campana cada %d min", "bell every %d min" },
	{ "inicio/fin", "start/end" }, { "Tiempo ahorrado (sesión)", "Time saved (this session)" }, { "Sesión terminada.", "Session finished." },
	{ "HOME 3 s: volver a Sony", "HOME 3 s: back to Sony" },
	{ "Info", "Info" },
	/* Bluetooth */
	{ "Bluetooth: apagado", "Bluetooth: off" }, { "Bluetooth: encendido", "Bluetooth: on" }, { "BT: %s", "BT: %s" },
	{ "Conectar: %s", "Connect: %s" }, { "Buscar dispositivo nuevo", "Find a new device" }, { "Buscar dispositivo", "Find a device" },
	{ "Apagar Bluetooth", "Turn Bluetooth off" }, { "Desconectar", "Disconnect" },
	{ "Modo receptor (desde el teléfono)", "Receiver mode (from the phone)" }, { "Mando para el teléfono", "Remote for the phone" },
	{ "Mando Bluetooth", "Bluetooth remote" }, { "Receptor Bluetooth", "Bluetooth receiver" },
	{ "Buscando... (modo emparejamiento)", "Searching... (pairing mode)" }, { "Encendiendo...", "Turning on..." },
	{ "Encendiendo Bluetooth...", "Turning Bluetooth on..." }, { "Apagando Bluetooth...", "Turning Bluetooth off..." },
	{ "Emparejando con", "Pairing with" }, { "Conectando con", "Connecting to" }, { "Conectado a", "Connected to" },
	{ "No se pudo conectar.", "Could not connect." }, { "Probá de nuevo.", "Try again." }, { "No conectó.", "Did not connect." },
	{ "Probá 'Buscar dispositivo nuevo'.", "Try 'Find a new device'." }, { "No encontré dispositivos.", "No devices found." },
	{ "Revisá que esté en modo emparejamiento.", "Make sure it is in pairing mode." },
	{ "No respondió. Poné el dispositivo en modo emparejamiento y apretá PLAY.", "No answer. Put the device in pairing mode and press PLAY." },
	{ "No pude encender el Bluetooth.", "Could not turn Bluetooth on." },
	{ "En el teléfono, buscá el Walkman (NW-A829) y conectalo. PIN: 0000", "On the phone, find the Walkman (NW-A829) and connect. PIN: 0000" },
	{ "Esperando conexión...", "Waiting for a connection..." }, { "Sonando desde el teléfono", "Playing from the phone" },
	{ "(en silencio)", "(silent)" }, { "Vol %d%%   BACK: terminar", "Vol %d%%   BACK: stop" },
	{ "No se conectó ningún teléfono.", "No phone connected." },
	{ "Ocupado bajando podcasts.", "Busy downloading podcasts." }, { "Esperá o cancelá en Podcasts > Descargas", "Wait or cancel in Podcasts > Downloads" },
	{ "Hay podcasts bajándose.", "Podcasts are downloading." },
	/* podcasts */
	{ "Buscar nuevos", "Check for new" }, { "Agregar podcast", "Add podcast" }, { "Descargas", "Downloads" },
	{ "desde el teléfono", "from the phone" }, { "buscar por nombre", "search by name" },
	{ "Buscar episodios nuevos", "Check for new episodes" }, { "Buscar episodios", "New episodes" },
	{ "Buscando episodios nuevos en tus podcasts...", "Looking for new episodes..." },
	{ "Buscar podcast", "Find a podcast" }, { "Nombre del podcast", "Podcast name" }, { "Buscar", "Search" },
	{ "Buscando...", "Searching..." }, { "No pude buscar.", "The search failed." }, { "No encontré podcasts con", "No podcasts found for" },
	{ "Se cortó la conexión con el teléfono.", "The phone connection dropped." }, { "Se cortó la conexión", "Connection lost" },
	{ "Podcast", "Podcast" }, { "Cargando...", "Loading..." }, { "Seguir este podcast", "Follow this podcast" },
	{ "Bajar episodios nuevos", "Get new episodes" }, { "Ver los episodios", "Episodes" }, { "Ver todos los episodios", "All episodes" },
	{ "Dejar de seguirlo", "Unfollow" }, { "Ya lo seguís", "Following" }, { "Lo seguís", "Following" },
	{ "Lo seguís · %d episodios", "Following · %d episodes" }, { "Lo seguís · %d episodio", "Following · %d episode" },
	{ "%d episodios · %s", "%d episodes · %s" }, { "%d episodio · %s", "%d episode · %s" },
	{ "%s · %d episodios", "%s · %d episodes" }, { "%s · %d episodio", "%s · %d episode" },
	{ "%d episodios nuevos", "%d new episodes" }, { "%d episodio nuevo", "%d new episode" },
	{ "%d nuevos de %d", "%d new of %d" }, { "%d nuevo de %d", "%d new of %d" },
	{ "%d episodios", "%d episodes" }, { "%d episodio", "%d episode" }, { "Episodio", "Episode" },
	{ "%d archivos", "%d files" }, { "%d archivo", "%d file" },
	{ "Bajar los 3 últimos episodios", "Get the latest 3 episodes" }, { "Bajar los %d últimos", "Get the latest %d" },
	{ "Ahora no", "Not now" }, { "Listo: ya lo seguís.", "Done: you follow it." }, { "\"Buscar nuevos\" trae sus episodios.", "\"Check for new\" gets its episodes." },
	{ "Seguir el podcast y bajarlo", "Follow and download it" }, { "Cancelar", "Cancel" },
	{ "Leyendo los episodios...", "Reading the episodes..." }, { "No pude leer los episodios.", "Could not read the episodes." },
	{ "Probá de nuevo más tarde.", "Try again later." }, { "Este podcast no tiene episodios", "This podcast has no episodes" },
	{ "que el Walkman pueda bajar.", "the Walkman can download." }, { "Ese episodio ya está en el Walkman.", "That episode is already on the Walkman." },
	{ "Bajando en segundo plano:", "Downloading in the background:" }, { "Ya está en la cola de descargas.", "Already in the download queue." },
	{ "No pude empezar la descarga.", "Could not start the download." }, { "No pude guardar la suscripción.", "Could not save the subscription." },
	{ "¿Está llena la memoria?", "Is the memory full?" }, { "No pude cambiar la suscripción.", "Could not change the subscription." },
	{ "Dejar de seguir, conservar episodios", "Unfollow, keep episodes" }, { "Dejar de seguir y borrar episodios", "Unfollow and delete episodes" },
	{ "¿Cuántos bajar?", "How many to get?" }, { "Directorio de podcasts", "Podcast directory" }, { "Buscar en: %s", "Search in: %s" },
	{ "Teléfono: elegir", "Phone: choose" }, { "Teléfono: %s", "Phone: %s" }, { "Teléfono", "Phone" }, { "elegir", "choose" },
	{ "Elegí el teléfono", "Choose the phone" }, { "En el teléfono: Bluetooth visible", "On the phone: Bluetooth visible" },
	{ "Revisá que el Bluetooth del teléfono esté visible.", "Make sure the phone's Bluetooth is visible." },
	{ "Activá el anclaje por Bluetooth en el teléfono", "Turn on Bluetooth tethering on the phone" },
	{ "Primero elegí el teléfono", "First choose the phone" }, { "que comparte internet por Bluetooth", "that shares its internet over Bluetooth" },
	{ "Ocupado con los podcasts.", "Busy with podcasts." }, { "Esperá a que terminen las descargas.", "Wait for the downloads to finish." },
	{ "Todavía no seguís ningún podcast.", "You don't follow any podcast yet." },
	{ "Agregá uno con \"Agregar podcast\".", "Add one with \"Add podcast\"." },
	{ "Borrar episodio", "Delete episode" }, { "Sí, borrarlo", "Yes, delete it" }, { "No pude borrar el episodio.", "Could not delete the episode." },
	{ "Bajar episodio", "Download episode" }, { "Sin descargas", "No downloads" }, { "No hay episodios nuevos", "No new episodes" },
	{ "No había episodios para bajar.", "There were no episodes to get." }, { "Hubo errores", "There were errors" },
	{ "Cancelado (%d nuevos)", "Cancelled (%d new)" }, { "Cancelado (%d nuevo)", "Cancelled (%d new)" }, { "Cancelado", "Cancelled" },
	{ "Bajando %d de %d", "Downloading %d of %d" }, { "Bajando imágenes (%d/%d)", "Downloading artwork (%d/%d)" },
	{ "Leyendo %s (%d/%d)", "Reading %s (%d/%d)" }, { "Leyendo %s", "Reading %s" }, { "leyendo %d/%d", "reading %d/%d" },
	{ "leyendo", "reading" }, { "imágenes", "artwork" }, { "listo", "done" },
	{ "%d KB · %d KB/s · %d episodios%s", "%d KB · %d KB/s · %d episodes%s" },
	{ "cancelado · %s", "cancelled · %s" }, { "listo · %s", "done · %s" }, { "error, se retoma · %s", "error, will resume · %s" },
	{ "OPTION: cancelar", "OPTION: cancel" }, { "BACK: volver", "BACK: back" }, { "BACK: borrar", "BACK: delete" },
	{ "Paso %d de %d", "Step %d of %d" }, { "%d de %d kB", "%d of %d kB" }, { "%d de %d", "%d of %d" }, { "El teléfono tiene que tener activado el anclaje por Bluetooth", "Bluetooth tethering must be on on the phone" },
	{ "Conectando con el teléfono", "Connecting to the phone" }, { "Emparejando con el teléfono", "Pairing with the phone" },
	{ "Reiniciando el Bluetooth", "Restarting Bluetooth" }, { "Abriendo la conexión a internet", "Opening the internet connection" },
	{ "Pidiendo dirección de internet", "Getting an internet address" }, { "Encendiendo el Bluetooth", "Turning Bluetooth on" },
	{ "Desconectando...", "Disconnecting..." }, { "Dirección inválida", "Invalid address" }, { "No se pudo abrir el Bluetooth", "Could not open Bluetooth" },
	{ "El teléfono no responde", "The phone does not answer" }, { "El teléfono rechazó la conexión", "The phone refused the connection" },
	{ "No hubo dirección IP", "No IP address" }, { "El teléfono cortó la conexión", "The phone hung up" }, { "¿Está activo el anclaje Bluetooth?", "Is Bluetooth tethering on?" },
	{ "Sin cacert.pem: HTTPS sin verificar certificados", "No cacert.pem: HTTPS without certificate checks" },
	/* keyboard */
	{ "espacio", "space" }, { "Borrar", "Delete" },
	/* games */
	{ "Game Boy", "Game Boy" },
	{ "PLAY = A · OPTION = B · BACK = Start (2 s: salir) · BT = Select", "PLAY = A · OPTION = B · BACK = Start (2 s: quit) · BT = Select" },
	{ "PLAY = A · OPTION = B · VOL = L/R · BACK = Start (2 s: salir) · BT 2 s: menú", "PLAY = A · OPTION = B · VOL = L/R · BACK = Start (2 s: quit) · BT 2 s: menu" },
	{ "PLAY dispara · OPTION usa · BT corre · VOL: lateral · BACK: menú, 2 s: salir", "PLAY fires · OPTION uses · BT runs · VOL: strafe · BACK: menu, 2 s: quit" },
	{ "BACK 2 s: salir. BT 2 s: menú del juego.", "BACK 2 s: quit. BT 2 s: game menu." },
	{ "Con tu música de fondo. BACK 2 s: salir. BT 2 s: menú.", "With your music on. BACK 2 s: quit. BT 2 s: menu." },
	/* first run, reset, about, power */
	{ "Batería agotada", "Battery empty" }, { "esperá: Walkman", "wait: Walkman" }, { "HOME: Audiolibros", "HOME: player" },
	{ "Bienvenido", "Welcome" }, { "Restablecer todo", "Reset everything" },
	{ "Borra posiciones, ajustes, Bluetooth y modo de inicio. No toca tu música.", "Clears positions, settings, Bluetooth and the boot mode. Your music stays." },
	{ "Sí, restablecer", "Yes, reset" }, { "Reiniciando...", "Restarting..." },
	{ "HOME 3 s en cualquier pantalla: volver al Walkman de Sony.", "HOME 3 s on any screen: back to Sony's Walkman." },
	{ "En el Walkman de Sony, HOME 3 s: volver acá.", "In Sony's Walkman, HOME 3 s: back here." },
	{ "OPTION: más opciones en casi todas las pantallas.", "OPTION: more choices on almost every screen." },
	{ "PLAY: seguir", "PLAY: continue" }, { "Versión", "Version" }, { "Espacio libre", "Free space" }, { "Licencia: GPL-2.0", "License: GPL-2.0" },
	{ "El despertador no suena con el equipo apagado del todo.", "The alarm can't ring when the unit is fully off." },
	{ "Usá Suspender para que suene.", "Use Sleep so it rings." },
	{ "Biblioteca: se muestran %d canciones (máximo).", "Library: showing %d songs (the maximum)." },
	{ "Versión %s", "Version %s" }, { "Libre: %s", "Free: %s" }, { "Ajustes: %s", "Settings: %s" }, { "HOME 3 s: ir a Sony", "HOME 3 s: to Sony" }, { "silencio", "silence" },
	{ "Idioma / Language", "Idioma / Language" },
	{ "Walkman de Sony: ahorro", "Sony Walkman: power saving" },
	{ "El Walkman de Sony va a usar la mitad de reloj: menos baterí" "a, menú" "s más lentos.", "Sony's Walkman will run at half the clock: less battery, slower menus." },
	{ "Experimental. Se aplica al volver a Sony.", "Experimental. Applies when you go back to Sony." },
	{ "Sin memoria para la lista", "Not enough memory for the list" }, { "Carpeta muy grande: se muestran %d", "Folder too big: showing %d" },
};

/* p: pattern, s: text. Captures go to cs/cl (at most 6) */
static int match(const char *p, const char *s, const char **cs, int *cl, int *nc)
{
	while (*p) {
		if (p[0] == '%' && p[1] == '%') { if (*s != '%') return 0; p += 2; s++; continue; }	/* "%%" is one '%' */
		if (p[0] == '%' && p[1] == 'd') {
			const char *t = s;
			while (*t >= '0' && *t <= '9') t++;
			if (t == s || *nc >= 6) return 0;
			cs[*nc] = s; cl[*nc] = (int)(t - s); (*nc)++;
			s = t; p += 2;
			continue;
		}
		if (p[0] == '%' && p[1] == 's') {
			if (*nc >= 6) return 0;
			int k = *nc;
			for (const char *t = s; ; t++) {
				*nc = k + 1; cs[k] = s; cl[k] = (int)(t - s);
				if (match(p + 2, t, cs, cl, nc)) return 1;
				if (!*t) { *nc = k; return 0; }
			}
		}
		if (*p != *s) return 0;
		p++; s++;
	}
	return !*s;
}

static const char *exact(const char *s, int len)
{
	for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++)
		if (!strchr(tab[i].es, '%') && (int)strlen(tab[i].es) == len && !strncmp(tab[i].es, s, len)) return tab[i].en;
	return 0;
}

/* the translation into out, 0 = no entry matches */
static int translate(const char *s, char *out, size_t max)
{
	const char *cs[6]; int cl[6];
	for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++) {
		int nc = 0;
		if (!match(tab[i].es, s, cs, cl, &nc)) continue;
		size_t k = 0; int ci = 0;
		for (const char *p = tab[i].en; *p && k + 1 < max; p++) {
			if (p[0] == '%' && p[1] == '%') { out[k++] = '%'; p++; continue; }
			if (p[0] == '%' && (p[1] == 'd' || p[1] == 's') && ci < nc) {
				const char *src = cs[ci]; int len = cl[ci];
				const char *t = p[1] == 's' ? exact(src, len) : 0;
				if (t) { src = t; len = (int)strlen(t); }
				for (int j = 0; j < len && k + 1 < max; j++) out[k++] = src[j];
				ci++; p++;
				continue;
			}
			out[k++] = *p;
		}
		out[k] = 0;
		return 1;
	}
	return 0;
}

#define CACHE 64
static struct { unsigned h; char src[112], dst[160]; signed char hit; } cache[CACHE];

const char *ui_tr(const char *s)
{
	if (!ui_lang || !s || !*s) return s;
	unsigned h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 16777619u;
	size_t len = strlen(s);
	if (len < sizeof cache[0].src) {
		int i = h & (CACHE - 1);
		if (cache[i].hit && cache[i].h == h && !strcmp(cache[i].src, s)) return cache[i].hit > 0 ? cache[i].dst : s;
		cache[i].h = h; memcpy(cache[i].src, s, len + 1);
		cache[i].hit = translate(s, cache[i].dst, sizeof cache[i].dst) ? 1 : -1;
		return cache[i].hit > 0 ? cache[i].dst : s;
	}
	static char big[4][320]; static int bi;	/* long texts: not cached */
	char *o = big[bi++ & 3];
	return translate(s, o, sizeof big[0]) ? o : s;
}

void ui_lang_set(int lang)
{
	ui_lang = lang ? 1 : 0;
	memset(cache, 0, sizeof cache);
	gfx_tr = ui_tr;
}
