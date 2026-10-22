/* music.c: music library built from the MP3 files under /contents/Music (what you drag and drop from the PC).
 * Tags come from ID3v2 (v2.2-2.4 text frames, any encoding -> UTF-8) or ID3v1; missing tags fall back to the
 * folder / file names. Views: all songs, artists -> albums -> songs, albums -> songs, shuffle all. */
#include "hb.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/contents/Music"
#define MAXT 4000	/* songs in the library (8 GB of MP3 is ~2000) */

struct song { char path[PATHLEN]; char title[96], artist[64], album[64], genre[32], year[8]; int trk; long size, mtime; };
static struct song *lib;
static int nlib;

/* library cache: the tags of every file with its size and date, so entering Música only stats the files and reads
 * the tags of new / changed ones (it used to open and parse every file on the first visit after each boot) */
#define LIBCACHE "/contents/HB/cache/library.bin"
#define LIBMAGIC 0x3442494cUL	/* "LIB4": FLAC tags read (LIB3 had FLAC files by their file names) */
static struct song *old;	/* the cache being matched during a scan (sorted by path) */
static int nold, cache_hits, cache_dirty;
static int quiet, gave_up;	/* music_preload: no messages, stop at the first file the cache lacks */
static int bg;	/* music_build_cache, in the warm process: no messages, no UI or audio, no log (the player's) */

static int cmp_path(const void *a, const void *b) { return strcmp(((const struct song *)a)->path, ((const struct song *)b)->path); }

static void cache_load(void)
{
	nold = 0; old = 0;
	FILE *f = fopen(LIBCACHE, "rb");
	if (!f) return;
	unsigned long magic = 0; int n = 0;
	if (fread(&magic, sizeof magic, 1, f) == 1 && magic == LIBMAGIC && fread(&n, sizeof n, 1, f) == 1 && n > 0 && n <= MAXT
	    && (old = malloc(sizeof *old * n)) && fread(old, sizeof *old, n, f) == (size_t)n) nold = n;
	else { free(old); old = 0; }
	fclose(f);
	if (nold) qsort(old, nold, sizeof *old, cmp_path);
}

static void cache_save(void)
{
	mkdir("/contents/HB", 0755); mkdir("/contents/HB/cache", 0755);
	char tmp[] = LIBCACHE ".tmp";
	FILE *f = fopen(tmp, "wb");
	if (!f) return;
	unsigned long magic = LIBMAGIC;
	int ok = fwrite(&magic, sizeof magic, 1, f) == 1 && fwrite(&nlib, sizeof nlib, 1, f) == 1 && fwrite(lib, sizeof *lib, nlib, f) == (size_t)nlib;
	fclose(f);
	if (ok) rename(tmp, LIBCACHE); else unlink(tmp);
}

/* ---------- tag parsing ---------- */

static void put_utf8(char **o, char *end, unsigned c)
{
	char *p = *o;
	if (c < 0x80) { if (p + 1 < end) *p++ = (char)c; }
	else if (c < 0x800) { if (p + 2 < end) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 63)); } }
	else if (p + 3 < end) { *p++ = (char)(0xE0 | c >> 12); *p++ = (char)(0x80 | ((c >> 6) & 63)); *p++ = (char)(0x80 | (c & 63)); }
	*o = p;
}

/* ID3 text payload (encoding byte + data) -> UTF-8 */
static void id3_text(const unsigned char *d, int n, char *out, int max)
{
	char *o = out, *end = out + max;
	if (n < 1) { *out = 0; return; }
	int enc = d[0]; d++; n--;
	if (enc == 1 || enc == 2) {
		int be = enc == 2;
		if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { be = 0; d += 2; n -= 2; }
		else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { be = 1; d += 2; n -= 2; }
		for (int i = 0; i + 1 < n; i += 2) {
			unsigned c = be ? (d[i] << 8 | d[i + 1]) : (d[i + 1] << 8 | d[i]);
			if (!c) break;
			if (c >= 0xD800 && c < 0xE000) c = '?';
			put_utf8(&o, end, c);
		}
	} else if (enc == 3) {
		for (int i = 0; i < n && d[i] && o + 1 < end; i++) *o++ = (char)d[i];
	} else {
		for (int i = 0; i < n && d[i]; i++) put_utf8(&o, end, d[i]);
	}
	*o = 0;
	while (o > out && (o[-1] == ' ')) *--o = 0;
}

/* ID3v1 genre numbers (standard 0-79 + Winamp extensions) */
static const char *const v1_genres[] = {
	"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
	"Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
	"Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
	"Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul", "Punk",
	"Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
	"Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40",
	"Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes",
	"Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
	"Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass",
	"Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band",
	"Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata",
	"Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore",
	"Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A cappella", "Euro-House",
	"Dance Hall",
};

/* TCON forms: "Rock", "17", "(17)", "(17)Rock", "(RX)", "(CR)" -> a readable name */
static void genre_fix(char *g, int max)
{
	const char *p = g;
	char num[8] = "";
	if (*p == '(') {
		const char *e = strchr(p, ')');
		if (!e) return;
		if (e[1]) { memmove(g, e + 1, strlen(e + 1) + 1); return; }	/* "(17)Rock": the text wins */
		snprintf(num, sizeof num, "%.*s", (int)(e - p - 1) < 7 ? (int)(e - p - 1) : 7, p + 1);
	} else snprintf(num, sizeof num, "%s", p);
	if (!strcmp(num, "RX")) { snprintf(g, max, "Remix"); return; }
	if (!strcmp(num, "CR")) { snprintf(g, max, "Cover"); return; }
	if (!num[0] || strspn(num, "0123456789") != strlen(num)) return;	/* plain text genre */
	unsigned n = (unsigned)atoi(num);
	if (n < sizeof v1_genres / sizeof *v1_genres) snprintf(g, max, "%s", v1_genres[n]); else g[0] = 0;
}

static unsigned syncsafe(const unsigned char *p) { return (p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

/* UTF-8 text into a field, cut at a character boundary when it doesn't fit; only fills an empty field (the first
 * TITLE= etc. wins) */
static void set_utf8(char *dst, int max, const char *v, int n)
{
	if (dst[0]) return;
	if (n > max - 1) { n = max - 1; while (n > 0 && ((unsigned char)v[n] & 0xC0) == 0x80) n--; }
	memcpy(dst, v, n); dst[n] = 0;
	while (n > 0 && dst[n - 1] == ' ') dst[--n] = 0;
}

static unsigned le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* FLAC: the VORBIS_COMMENT metadata block ("TITLE=...", UTF-8; keys in any case). Big entries (lyrics, an
 * embedded picture) are skipped without reading them */
static int read_flac_tags(int fd, long off, struct song *s, char *aartist, int aamax)	/* 1: a FLAC file */
{
	unsigned char b[4];
	if (lseek(fd, off, SEEK_SET) < 0 || read(fd, b, 4) != 4 || memcmp(b, "fLaC", 4)) return 0;
	off += 4;
	for (int blocks = 0; blocks < 128; blocks++) {
		if (lseek(fd, off, SEEK_SET) < 0 || read(fd, b, 4) != 4) return 1;
		unsigned len = (unsigned)b[1] << 16 | b[2] << 8 | b[3];
		if ((b[0] & 127) == 4) break;
		if (b[0] & 128) return 1;	/* the last block, no comments */
		off += 4 + len;
	}
	unsigned len = (unsigned)b[1] << 16 | b[2] << 8 | b[3], left = len;
	if (left < 8 || read(fd, b, 4) != 4) return 1;
	unsigned vlen = le32(b);	/* vendor string */
	left -= 4;
	if (vlen > left - 4 || lseek(fd, vlen, SEEK_CUR) < 0 || read(fd, b, 4) != 4) return 1;
	left -= vlen + 4;
	unsigned count = le32(b);
	char c[512];
	for (unsigned i = 0; i < count && i < 1000 && left >= 4; i++) {
		if (read(fd, b, 4) != 4) return 1;
		unsigned n = le32(b);
		left -= 4;
		if (n > left) return 1;
		left -= n;
		if (n >= sizeof c) { if (lseek(fd, n, SEEK_CUR) < 0) return 1; continue; }
		if (read(fd, c, n) != (ssize_t)n) return 1;
		c[n] = 0;
		char *eq = memchr(c, '=', n);
		if (!eq) continue;
		*eq = 0;
		const char *v = eq + 1; int vn = (int)(c + n - v);
		if (!strcasecmp(c, "TITLE")) set_utf8(s->title, sizeof s->title, v, vn);
		else if (!strcasecmp(c, "ARTIST")) set_utf8(s->artist, sizeof s->artist, v, vn);
		else if (!strcasecmp(c, "ALBUM")) set_utf8(s->album, sizeof s->album, v, vn);
		else if (!strcasecmp(c, "ALBUMARTIST") || !strcasecmp(c, "ALBUM ARTIST")) set_utf8(aartist, aamax, v, vn);
		else if (!strcasecmp(c, "GENRE")) set_utf8(s->genre, sizeof s->genre, v, vn);
		else if ((!strcasecmp(c, "DATE") || !strcasecmp(c, "YEAR")) && strspn(v, "0123456789") >= 4 && !s->year[0]) snprintf(s->year, sizeof s->year, "%.4s", v);
		else if (!strcasecmp(c, "TRACKNUMBER") && !s->trk) s->trk = atoi(v);
	}
	return 1;
}

static void read_tags(const char *path, struct song *s)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return;
	unsigned char h[10];
	char aartist[64] = "";	/* album artist (TPE2 / ALBUMARTIST): the artist when a file names no other */
	long flac_off = 0;	/* where "fLaC" should be: after an ID3v2 tag some taggers put in front */
	if (read(fd, h, 10) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
		flac_off = 10 + (long)syncsafe(h + 6) + (h[5] & 0x10 ? 10 : 0);
		int v = h[3];
		unsigned tsz = syncsafe(h + 6), pos = 10, hdr = v == 2 ? 6 : 10;
		unsigned char f[10], d[512];
		if (v >= 3 && (h[5] & 0x40) && read(fd, f, 4) == 4) {	/* extended header: v2.4 counts its own 4 bytes, v2.3 doesn't */
			unsigned ext = v == 4 ? syncsafe(f) : (unsigned)f[0] << 24 | f[1] << 16 | f[2] << 8 | f[3];
			pos = ext < tsz ? 10 + ext + (v == 4 ? 0 : 4) : tsz + 10;
		}
		while (pos + hdr <= tsz + 10) {
			if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, f, hdr) != (ssize_t)hdr || !f[0]) break;
			unsigned fsz = v == 2 ? (unsigned)(f[3] << 16 | f[4] << 8 | f[5]) : v == 4 ? syncsafe(f + 4) : (unsigned)(f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7]);
			if (fsz > tsz) break;
			char *dst = 0; int dmax = 0, istrk = 0;
			if (!memcmp(f, v == 2 ? "TT2" : "TIT2", v == 2 ? 3 : 4)) { dst = s->title; dmax = sizeof s->title; }
			else if (!memcmp(f, v == 2 ? "TP1" : "TPE1", v == 2 ? 3 : 4)) { dst = s->artist; dmax = sizeof s->artist; }
			else if (!memcmp(f, v == 2 ? "TAL" : "TALB", v == 2 ? 3 : 4)) { dst = s->album; dmax = sizeof s->album; }
			else if (!memcmp(f, v == 2 ? "TP2" : "TPE2", v == 2 ? 3 : 4)) { dst = aartist; dmax = sizeof aartist; }
			else if (!memcmp(f, v == 2 ? "TCO" : "TCON", v == 2 ? 3 : 4)) { dst = s->genre; dmax = sizeof s->genre; }
			else if (!memcmp(f, v == 2 ? "TRK" : "TRCK", v == 2 ? 3 : 4)) istrk = 1;
			else if (v == 2 ? !memcmp(f, "TYE", 3) : !memcmp(f, "TYER", 4) || !memcmp(f, "TDRC", 4)) istrk = 2;	/* year (v2.4: a date) */
			if ((dst || istrk) && fsz > 1) {
				int n = fsz < sizeof d ? (int)fsz : (int)sizeof d;
				if (read(fd, d, n) == n) {
					char tmp[96];
					if (istrk == 2) { id3_text(d, n, tmp, sizeof tmp); if (strspn(tmp, "0123456789") >= 4 && !s->year[0]) snprintf(s->year, sizeof s->year, "%.4s", tmp); }
					else if (istrk) { id3_text(d, n, tmp, sizeof tmp); s->trk = atoi(tmp); }
					else id3_text(d, n, dst, dmax);
				}
			}
			pos += hdr + fsz;
		}
	}
	int flac = read_flac_tags(fd, flac_off, s, aartist, sizeof aartist);
	if (!s->artist[0] && aartist[0]) snprintf(s->artist, sizeof s->artist, "%s", aartist);
	if (s->genre[0]) genre_fix(s->genre, sizeof s->genre);
	/* ID3v1, at the very end: not looked for in FLAC files (seeking to the end of a 100 MB file walks its whole
	 * FAT cluster chain, and FLAC never carries one in practice) */
	if (!flac && (!s->title[0] || !s->artist[0] || !s->genre[0] || !s->year[0])) {
		unsigned char t[128];
		if (lseek(fd, -128, SEEK_END) > 0 && read(fd, t, 128) == 128 && !memcmp(t, "TAG", 3)) {
			unsigned char b[31];
			if (!s->title[0]) { memcpy(b, t + 3, 30); b[30] = 0; char e[2] = { 0, 0 }; unsigned char tb[32]; tb[0] = 0; memcpy(tb + 1, b, 31); id3_text(tb, 31, s->title, sizeof s->title); (void)e; }
			if (!s->artist[0]) { unsigned char tb[32]; tb[0] = 0; memcpy(tb + 1, t + 33, 30); tb[31] = 0; id3_text(tb, 31, s->artist, sizeof s->artist); }
			if (!s->album[0]) { unsigned char tb[32]; tb[0] = 0; memcpy(tb + 1, t + 63, 30); tb[31] = 0; id3_text(tb, 31, s->album, sizeof s->album); }
			if (!s->trk && t[125] == 0) s->trk = t[126];
			if (!s->year[0] && t[93] >= '1' && t[93] <= '2' && t[94] >= '0' && t[94] <= '9') snprintf(s->year, sizeof s->year, "%.4s", (const char *)t + 93);
			if (!s->genre[0] && t[127] < sizeof v1_genres / sizeof *v1_genres) snprintf(s->genre, sizeof s->genre, "%s", v1_genres[t[127]]);
		}
	}
	close(fd);
}

static void scan(const char *dir, int depth, const char *folder)
{
	if (depth > 4) return;
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && nlib < MAXT) {
		if (e->d_name[0] == '.') continue;
		char p[PATHLEN];
		if (!fs_path(dir, e->d_name, p, sizeof p)) { if (!bg) lg("music: can't open \"%s/%s\" (its name has characters this Walkman can't show, and no short name)", dir, e->d_name); continue; }
		struct stat st;
		if (stat(p, &st)) continue;
		if (S_ISDIR(st.st_mode)) { scan(p, depth + 1, e->d_name); if (gave_up) break; continue; }
		if (!dec_is_audio(e->d_name)) continue;
		struct song *s = &lib[nlib];
		memset(s, 0, sizeof *s);
		snprintf(s->path, sizeof s->path, "%s", p);
		struct song *c = nold ? bsearch(s, old, nold, sizeof *old, cmp_path) : 0;
		if (c && c->size == (long)st.st_size && c->mtime == (long)st.st_mtime) { *s = *c; cache_hits++; nlib++; continue; }
		if (quiet) { gave_up = 1; break; }	/* reading tags takes time: the visit does it, with its progress message */
		cache_dirty = 1;
		s->size = (long)st.st_size; s->mtime = (long)st.st_mtime;
		read_tags(p, s);
		if (!bg) {
			ui_tick();
			if (!(nlib % 20)) { char b[48]; snprintf(b, sizeof b, "%d canciones", nlib); message("Música", "Leyendo la biblioteca...", b); }
		}
		if (!s->title[0]) { fs_to_utf8(e->d_name, s->title, sizeof s->title); char *dot = strrchr(s->title, '.'); if (dot && dot > s->title) *dot = 0; }
		if (!s->album[0]) { if (folder) fs_to_utf8(folder, s->album, sizeof s->album); else snprintf(s->album, sizeof s->album, "Sin álbum"); }
		if (!s->artist[0]) snprintf(s->artist, sizeof s->artist, "Artista desconocido");
		nlib++;
	}
	closedir(d);
}

static int cmp_song(const void *a, const void *b)
{
	const struct song *x = a, *y = b;
	int r = strcasecmp(x->artist, y->artist);
	if (!r) r = strcasecmp(x->album, y->album);
	if (!r) r = x->trk - y->trk;
	if (!r) r = strcasecmp(x->title, y->title);
	return r;
}

static int load_library(void)
{
	if (lib) return nlib;
	lib = malloc(sizeof *lib * MAXT);
	if (!lib) return 0;
	if (!quiet && !bg) message("Música", "Leyendo la biblioteca...", "");
	nlib = 0; cache_hits = 0; cache_dirty = 0; gave_up = 0;
	double t0 = now();
	cache_load();
	scan(ROOT, 0, 0);
	if (gave_up) { free(old); old = 0; nold = 0; free(lib); lib = 0; nlib = 0; lg("music: preload stopped at a new file (%.1f s)", now() - t0); return 0; }
	if (nlib != cache_hits || nold != nlib) cache_dirty = 1;	/* new, changed or deleted files */
	free(old); old = 0; nold = 0;
	qsort(lib, nlib, sizeof *lib, cmp_song);
	if (cache_dirty) cache_save();
	if (bg) return nlib;
	lg("music: %d songs (%d from the cache) in %.2f s%s", nlib, cache_hits, now() - t0, quiet ? " (preload)" : "");
	if (nlib >= MAXT) { static char m[64]; snprintf(m, sizeof m, "Biblioteca: se muestran %d canciones (máximo).", MAXT); eng_note(m); }
	return nlib;
}

/* trusted: the warm process has just checked every file against the cache and saved it, and nothing in Music can
 * change while the player runs (USB restarts it): the cache is taken as it is, without a stat per file (with
 * thousands of songs that check alone took seconds) */
void music_preload(int trusted)
{
	if (lib || access(LIBCACHE, R_OK)) return;	/* loaded already, or no cache yet (the first visit builds it) */
	if (trusted) {
		double t0 = now();
		cache_load();
		if (nold) {
			lib = old; nlib = nold; old = 0; nold = 0;	/* exactly nlib entries: the library never grows once loaded */
			qsort(lib, nlib, sizeof *lib, cmp_song);
			lg("music: %d songs from the cache the start-up check left (%.2f s)", nlib, now() - t0);
			return;
		}
	}
	quiet = 1;
	load_library();
	quiet = 0;
}

/* the warm process (warm.c, a child at the lowest priority) reads the tags of new files into the library cache
 * while the home screen shows: the first visit of Música then only checks the files. Returns the number of songs */
int music_build_cache(void)
{
	bg = 1;
	int n = load_library();
	bg = 0;
	return n;
}

/* the warm process again, after music_preload: every album's cover at every size the lists and the player use
 * (cover_load makes them all from one decode; the first song of each artist + album stands for the album), then
 * every other song's pointer to its picture, so the player shows it at once (reading a big FLAC picture again to
 * learn it is the album's took 0.1-4 s on the first play of each song). Returns the albums; *decoded = songs that
 * needed work, *inherited = songs of the player's library this process found (0: it read the cache itself) */
int music_album_covers(int *decoded, int *inherited)
{
	static uint16_t px[PLAYER_ART * PLAYER_ART];
	static const int sizes[] = { GRID_TH, BIG_THUMB, 28, PLAYER_ART };
	int n = 0;
	*decoded = 0;
	*inherited = lib ? nlib : 0;
	/* on the Walkman this child once found no library although the player had just loaded it (2026-10-21, cause
	 * unknown): then the cache the start-up check wrote is read here */
	if (!lib || !nlib) {
		cache_load();
		if (!nold) return 0;
		lib = old; nlib = nold; old = 0; nold = 0;
		qsort(lib, nlib, sizeof *lib, cmp_song);
	}
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < nlib; i++) {
			int first = !i || strcasecmp(lib[i].artist, lib[i - 1].artist) || strcasecmp(lib[i].album, lib[i - 1].album);
			if (pass == 0 && !first) continue;	/* the albums first: the lists show them */
			if (pass == 1 && first) continue;
			if (pass == 0) n++;
			int work = 0;
			for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++)	/* the first one missing makes them all */
				if (!cover_known(lib[i].path, sizes[k])) { cover_load(lib[i].path, px, sizes[k]); work = 1; }
			*decoded += work;
		}
	return n;
}

/* ---------- views ---------- */

static int sel_idx[MAXT], nsel;	/* the songs shown in the current song list (indexes into lib) */
static char names[MAXT][64];
static int nnames;
static const char *song_label(int i) { return lib[sel_idx[i]].title; }
static const char *name_label(int i) { return names[i]; }

/* thumbnails for the album list: one per name (the first song of the album), decoded lazily */
static uint16_t *thumbs;		/* nnames x px x px */
static signed char *thumb_state;	/* 0 unknown, 1 ready, -1 none */
static int thumb_songidx[MAXT], name_count[MAXT], tpx = 28;	/* covers 28 px, 60 with big rows, 72 in the mosaic */
static struct img album_thumb(int i)
{
	struct img im = { 0, 0, 0 };
	if (!thumbs || thumb_state[i] != 1) return im;
	im.w = im.h = tpx; im.px = thumbs + (size_t)i * tpx * tpx;
	return im;
}
static double thumbs_t0;	/* the visit's start, for one timing line when every cover is in */
static int thumbs_logged, thumbs_n;
static int thumbs_pending(void)
{
	if (!thumbs) return 0;
	for (int j = 0; j < nnames; j++) {
		int i = (list_vis_top + j) % nnames;
		if (!thumb_state[i]) {
			cover_cancel = key_waiting;	/* a cover being decoded never delays a key */
			int ok = cover_load(lib[thumb_songidx[i]].path, thumbs + (size_t)i * tpx * tpx, tpx);
			cover_cancel = 0;
			if (cover_aborted()) return 0;	/* the key first; this one again on the next pass */
			thumb_state[i] = ok ? 1 : -1; thumbs_n++; return 1;
		}
	}
	if (!thumbs_logged) { thumbs_logged = 1; if (thumbs_n) lg("albums: %d covers (%d px) in %d ms", thumbs_n, tpx, (int)((now() - thumbs_t0) * 1000)); }
	return 0;
}
static void thumbs_setup(void)
{
	free(thumbs); free(thumb_state);
	tpx = ui_album_grid ? GRID_TH : ui_big_rows ? BIG_THUMB : 28;
	thumbs = calloc((size_t)nnames * tpx * tpx, 2); thumb_state = calloc(nnames, 1);
	thumbs_t0 = now(); thumbs_logged = 0; thumbs_n = 0;
}
/* big rows: the number of songs under the album name */
static const char *count_value(int i)
{
	static char b[24];
	if (name_count[i] <= 0) return 0;
	snprintf(b, sizeof b, "%d %s", name_count[i], name_count[i] == 1 ? "canción" : "canciones");
	return b;
}
static void thumbs_free(void) { free(thumbs); free(thumb_state); thumbs = 0; thumb_state = 0; }

static void play_songs2(int start, const char *ctx, int shuffle);
static void play_songs(int start, const char *ctx) { play_songs2(start, ctx, -1); }

static void play_songs2(int start, const char *ctx, int shuffle)
{
	struct track *qtmp = malloc(sizeof *qtmp * (nsel ? nsel : 1));	/* only while handing it over: the engine packs its own copy */
	if (!qtmp) { eng_note("Sin memoria para la lista"); return; }
	for (int i = 0; i < nsel; i++) {
		struct song *s = &lib[sel_idx[i]];
		snprintf(qtmp[i].path, sizeof qtmp[i].path, "%s", s->path);
		snprintf(qtmp[i].title, sizeof qtmp[i].title, "%s", s->title);
		snprintf(qtmp[i].sub, sizeof qtmp[i].sub, "%s - %s", s->artist, s->album);
		snprintf(qtmp[i].genre, sizeof qtmp[i].genre, "%s", s->genre);
		snprintf(qtmp[i].year, sizeof qtmp[i].year, "%s", s->year);
	}
	eng_play2(qtmp, nsel, start, ENG_MUSIC, ctx, shuffle);
	free(qtmp);
	nowplaying_run();
}

static void song_list(const char *title)
{
	struct list_view v = { title, nsel, 0, 0, song_label, 0, 0, LI_TRACK, "No hay canciones", 0 };
	for (;;) {
		int s = list_run(&v);
		if (s < 0) return;
		play_songs(s, title);
	}
}

/* fills names[] with the unique artists (artist == 0) or the albums of one artist, and thumb_songidx[] with the
 * first song of each name. The library is sorted by artist, then album: equal names are adjacent, one pass. */
static int uniq(const char *artist)
{
	nnames = 0;
	for (int i = 0; i < nlib && nnames < MAXT; i++) {
		if (artist && strcasecmp(lib[i].artist, artist)) continue;
		const char *n = artist ? lib[i].album : lib[i].artist;
		if (nnames && !strcasecmp(names[nnames - 1], n)) { name_count[nnames - 1]++; continue; }
		thumb_songidx[nnames] = i; name_count[nnames] = 1;
		snprintf(names[nnames++], sizeof names[0], "%s", n);
	}
	return nnames;
}

static void album_songs(const char *artist, const char *album)
{
	nsel = 0;
	for (int i = 0; i < nlib; i++)
		if ((!artist || !strcasecmp(lib[i].artist, artist)) && !strcasecmp(lib[i].album, album)) sel_idx[nsel++] = i;
	song_list(album);
}

/* OPTION in an album list: switch list <-> mosaic (remembered), or the sound settings */
static char ao_items[3][32];
static const char *ao_label(int i) { return ao_items[i]; }
static int album_option(int i)
{
	(void)i;
	snprintf(ao_items[0], sizeof ao_items[0], "%s", ui_album_grid ? "Ver como lista" : "Ver como mosaico");
	snprintf(ao_items[1], sizeof ao_items[1], "Sonido");
	snprintf(ao_items[2], sizeof ao_items[2], "Cancelar");
	struct list_view m = { "Álbumes", 3, 0, 0, ao_label, 0, 0, LI_NONE, 0, 0, 0, -1, 0 };
	int r = list_run(&m);
	if (r == 1) { if (sound_quick_menu(eng_active())) nowplaying_run(); return 0; }
	if (r != 0) return 0;
	ui_album_grid = !ui_album_grid;
	settings_save();
	return 1;	/* list_run returns -2: the album list is set up again in the new view */
}

static void album_view(struct list_view *v)
{
	thumbs_setup();
	v->grid = ui_album_grid;
	v->big = !ui_album_grid && ui_big_rows;
	v->value = v->grid || v->big ? count_value : 0;
	v->option = album_option;
	v->top = v->grid ? v->sel - v->sel % 3 : v->sel;
}

/* all albums: an album name can appear under several artists. The songs sorted by album name group them (one
 * pass; comparing each song with every album found so far was songs x albums); the albums keep the library's
 * order (by the first song of each) */
static int by_album[MAXT];
static int cmp_by_album(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b, r = strcasecmp(lib[x].album, lib[y].album);
	return r ? r : x - y;
}
static int cmp_by_first(const void *a, const void *b) { return thumb_songidx[*(const int *)a] - thumb_songidx[*(const int *)b]; }
static void all_albums(void)
{
	nnames = 0;
	for (int i = 0; i < nlib; i++) by_album[i] = i;
	qsort(by_album, nlib, sizeof *by_album, cmp_by_album);
	for (int i = 0; i < nlib && nnames < MAXT; i++) {
		int s = by_album[i];
		if (i && !strcasecmp(lib[s].album, lib[by_album[i - 1]].album)) { name_count[nnames - 1]++; continue; }
		thumb_songidx[nnames] = s; name_count[nnames] = 1; nnames++;	/* s = the album's first song (ties by index) */
	}
	/* back into the library's order; by_album is free again and holds the permutation */
	for (int k = 0; k < nnames; k++) by_album[k] = k;
	qsort(by_album, nnames, sizeof *by_album, cmp_by_first);
	static int ts[MAXT], nc[MAXT];
	for (int k = 0; k < nnames; k++) { ts[k] = thumb_songidx[by_album[k]]; nc[k] = name_count[by_album[k]]; }
	for (int k = 0; k < nnames; k++) {
		thumb_songidx[k] = ts[k]; name_count[k] = nc[k];
		snprintf(names[k], sizeof names[0], "%s", lib[ts[k]].album);
	}
}

/* the album list is built once per visit (names, first songs, thumbnails): coming back from an album is instant
 * and keeps the selection and the covers already decoded */
static void albums_of(const char *artist)
{
	char keep[64];
	snprintf(keep, sizeof keep, "%s", artist ? artist : "");
	if (artist) uniq(keep);
	else all_albums();
	struct list_view v = { artist ? keep : "Álbumes", nnames, 0, 0, name_label, 0, 0, LI_NONE, "Vacío", 0, album_thumb, -1 };
	album_view(&v);
	for (;;) {
		list_idle_hook = thumbs_pending;
		int s = list_run(&v);
		list_idle_hook = 0;
		if (s == -2) { album_view(&v); continue; }	/* switched list <-> mosaic */
		if (s < 0) { thumbs_free(); return; }
		char album[64]; snprintf(album, sizeof album, "%s", names[s]);
		album_songs(artist ? keep : 0, album);
	}
}

static void artists(void)
{
	uniq(0);
	struct list_view v = { "Artistas", nnames, 0, 0, name_label, 0, 0, LI_FOLDER, "Vacío", 0 };
	for (;;) {
		int s = list_run(&v);
		if (s < 0) return;
		char a[64]; snprintf(a, sizeof a, "%s", names[s]);
		albums_of(a);
		uniq(0);	/* albums_of used names[]: the artists again (one pass); v keeps the selection */
	}
}

static const char *root_items[] = { "Aleatorio (todas)", "Canciones", "Artistas", "Álbumes" };
static const char *root_label(int i) { return root_items[i]; }

void music_menu(void)
{
	if (!load_library()) { message("Música", "No encontré MP3 en H:\\Music.", "Copialos desde la PC y volvé a entrar."); msg_wait(3); return; }
	struct list_view v = { "Música", 4, 0, 0, root_label, 0, 0, LI_FOLDER, 0, 0 };
	for (;;) {
		int s = list_run(&v);
		if (s < 0) return;
		nsel = 0;
		if (s == 0 || s == 1) {
			for (int i = 0; i < nlib; i++) sel_idx[nsel++] = i;
			if (s == 0) play_songs2(rand() % (nsel ? nsel : 1), "Aleatorio", 1);
			else song_list("Canciones");
		} else if (s == 2) artists();
		else albums_of(0);
	}
}
