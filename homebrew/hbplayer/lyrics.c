/* lyrics.c: synchronized lyrics from an .lrc file next to the track ("[mm:ss.xx]text", several tags per line
 * allowed), or a podcast transcript (.srt / .vtt, Podcasting 2.0 <podcast:transcript>, saved by podsync).
 * Kept sorted by time; the UI asks for the line at the current position. */
#include "hb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXL 4000
struct line { int cs; char *text; };
static struct line lines[MAXL];
static int nlines;
static char *blob;

static int cmp(const void *a, const void *b) { return ((const struct line *)a)->cs - ((const struct line *)b)->cs; }

int lrc_count(void) { return nlines; }

void lrc_unload(void) { free(blob); blob = 0; nlines = 0; }

/* SRT / WebVTT: "00:01:02,500 --> 00:01:05,000" (or "01:02.500") followed by text lines until a blank line.
 * The text lines of a cue are joined with spaces; <v Speaker> style tags are removed. Works in place in blob. */
static int cue_time(const char *s, int *cs)
{
	int h = 0, m = 0, sec = 0, ms = 0;
	if (sscanf(s, "%d:%d:%d%*[,.]%d", &h, &m, &sec, &ms) == 4) { *cs = (h * 3600 + m * 60 + sec) * 100 + ms / 10; return 1; }
	if (sscanf(s, "%d:%d%*[,.]%d", &m, &sec, &ms) == 3) { *cs = (m * 60 + sec) * 100 + ms / 10; return 1; }
	return 0;
}

static int load_cues(void)
{
	/* drop CRs so lines end in plain LF */
	char *w = blob;
	for (char *r = blob; *r; r++) if (*r != '\r') *w++ = *r;
	*w = 0;
	char *p = blob;
	while (*p && nlines < MAXL) {
		char *eol = strchr(p, '\n');
		char *arrow = strstr(p, "-->");
		int cs;
		if (arrow && (!eol || arrow < eol) && cue_time(p, &cs)) {
			if (!eol) break;
			/* text lines follow until an empty line; join them with spaces, strip <tags> */
			char *t = eol + 1, *start = t, *o = t;
			if (*t == '\n') { p = t + 1; continue; }	/* empty cue: skip it, don't swallow the next one */
			while (*t) {
				if (*t == '\n') { if (t[1] == '\n' || !t[1]) { t++; break; } if (o > start && o[-1] != ' ') *o++ = ' '; t++; continue; }
				if (*t == '<') { char *c = strchr(t, '>'); if (c && c - t < 40) { t = c + 1; continue; } }
				*o++ = *t++;
			}
			char *next = t;
			*o = 0;
			while (*start == ' ') start++;
			if (o > start) { lines[nlines].cs = cs; lines[nlines].text = start; nlines++; }
			p = next;
			continue;
		}
		if (!eol) break;
		p = eol + 1;
	}
	qsort(lines, nlines, sizeof lines[0], cmp);
	lg("transcript: %d cues", nlines);
	return nlines;
}

int lrc_load(const char *path)
{
	lrc_unload();
	char fn[PATHLEN]; snprintf(fn, sizeof fn, "%s", path);
	char *dot = strrchr(fn, '.');
	if (!dot || strchr(dot, '/')) return 0;
	strcpy(dot, ".lrc");
	FILE *f = fopen(fn, "rb");
	if (!f) { strcpy(dot, ".srt"); f = fopen(fn, "rb"); }
	if (!f) { strcpy(dot, ".vtt"); f = fopen(fn, "rb"); }
	if (!f) return 0;
	int cues = strcmp(dot, ".lrc") != 0;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 1500000) { fclose(f); return 0; }
	blob = malloc(n + 1);
	if (!blob || fread(blob, 1, n, f) != (size_t)n) { fclose(f); lrc_unload(); return 0; }
	fclose(f);
	blob[n] = 0;
	if (cues) return load_cues();
	for (char *p = blob; *p && nlines < MAXL; ) {
		char *eol = strpbrk(p, "\r\n");
		if (eol) *eol = 0;
		/* collect all leading [mm:ss.xx] tags */
		int stamps[8], ns = 0;
		char *q = p;
		while (*q == '[' && ns < 8) {
			int m, s, c = 0;
			char *close = strchr(q, ']');
			if (!close) break;
			if (sscanf(q, "[%d:%d.%d]", &m, &s, &c) >= 2) stamps[ns++] = m * 6000 + s * 100 + (c < 10 && strlen(q) > 7 && q[6] == '.' && q[8] == ']' ? c * 10 : c);
			q = close + 1;
		}
		while (*q == ' ') q++;
		for (int i = 0; i < ns && nlines < MAXL; i++) { lines[nlines].cs = stamps[i]; lines[nlines].text = q; nlines++; }
		if (!eol) break;
		p = eol + 1;
		while (*p == '\r' || *p == '\n') p++;
	}
	qsort(lines, nlines, sizeof lines[0], cmp);
	return nlines;
}

/* index of the line active at `sec` (-1 before the first) */
int lrc_index(double sec)
{
	int cs = (int)(sec * 100), idx = -1;
	for (int i = 0; i < nlines && lines[i].cs <= cs; i++) idx = i;
	return idx;
}

const char *lrc_text(int idx) { return idx >= 0 && idx < nlines ? lines[idx].text : ""; }
