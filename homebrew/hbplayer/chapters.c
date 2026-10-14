/* chapters.c: Podcasting 2.0 chapters (<podcast:chapters> JSON, saved by podsync next to the episode as
 * "<episode>.chapters.json"): {"chapters":[{"startTime":0,"title":"Intro"},...]}. A small scanner, not a full
 * JSON parser: it walks the objects inside "chapters" and picks startTime / title (skipping "toc": false). */
#include "hb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 200
struct chap { double start; char title[80]; };
static struct chap ch[MAXC];
static int nch;

int chap_count(void) { return nch; }
double chap_start(int i) { return i >= 0 && i < nch ? ch[i].start : 0; }
const char *chap_title(int i) { return i >= 0 && i < nch ? ch[i].title : ""; }

int chap_index(double sec)
{
	int idx = -1;
	for (int i = 0; i < nch && ch[i].start <= sec + 0.01; i++) idx = i;
	return idx;
}

static void json_string(const char *p, char *out, size_t max)
{
	size_t k = 0;
	while (*p && *p != '"' && k + 1 < max) {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'n' || *p == 't') { out[k++] = ' '; p++; continue; }
			if (*p == 'u') {	/* \uXXXX -> UTF-8, surrogate pairs combined */
				if (!p[1] || !p[2] || !p[3] || !p[4]) break;
				unsigned cp = (unsigned)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, 0, 16);
				p += 5;
				if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u' && p[2] && p[3] && p[4] && p[5]) {
					unsigned lo = (unsigned)strtoul((char[5]){ p[2], p[3], p[4], p[5], 0 }, 0, 16);
					if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
				}
				if (cp >= 0x10000) { if (k + 4 < max) { out[k++] = (char)(0xF0 | cp >> 18); out[k++] = (char)(0x80 | ((cp >> 12) & 63)); out[k++] = (char)(0x80 | ((cp >> 6) & 63)); out[k++] = (char)(0x80 | (cp & 63)); } continue; }
				if (cp < 0x80) out[k++] = (char)cp;
				else if (cp < 0x800 && k + 2 < max) { out[k++] = (char)(0xC0 | cp >> 6); out[k++] = (char)(0x80 | (cp & 63)); }
				else if (k + 3 < max) { out[k++] = (char)(0xE0 | cp >> 12); out[k++] = (char)(0x80 | ((cp >> 6) & 63)); out[k++] = (char)(0x80 | (cp & 63)); }
				continue;
			}
		}
		out[k++] = *p++;
	}
	out[k] = 0;
}

static const char *key(const char *obj, const char *end, const char *name)
{
	size_t l = strlen(name);
	for (const char *p = obj; p + l + 2 < end; p++)
		if (*p == '"' && !strncmp(p + 1, name, l) && p[l + 1] == '"') {
			p += l + 2;
			while (p < end && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
			return p;
		}
	return 0;
}

static int cmp(const void *a, const void *b) { double d = ((const struct chap *)a)->start - ((const struct chap *)b)->start; return d < 0 ? -1 : d > 0; }


/* loads the chapters for an audio file; returns how many */
int chap_load(const char *audio_path)
{
	nch = 0;
	char fn[PATHLEN + 16];
	snprintf(fn, sizeof fn, "%s", audio_path);
	char *dot = strrchr(fn, '.');
	if (!dot || strchr(dot, '/')) return 0;
	strcpy(dot, ".chapters.json");
	FILE *f = fopen(fn, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 300000) { fclose(f); return 0; }
	char *js = malloc(n + 1);
	if (!js || fread(js, 1, n, f) != (size_t)n) { free(js); fclose(f); return 0; }
	fclose(f);
	js[n] = 0;
	const char *p = strstr(js, "\"chapters\"");
	p = p ? strchr(p, '[') : 0;
	while (p && nch < MAXC) {
		const char *o = strchr(p, '{');
		if (!o) break;
		/* end of this object: next '}' at depth 0 (chapter objects may hold a nested "location" object) */
		int depth = 0; const char *e = o;
		int instr = 0;	/* braces inside strings (titles) don't count */
		for (; *e; e++) {
			if (instr) { if (*e == '\\' && e[1]) e++; else if (*e == '"') instr = 0; continue; }
			if (*e == '"') instr = 1; else if (*e == '{') depth++; else if (*e == '}' && --depth == 0) break;
		}
		if (!*e) break;
		const char *st = key(o, e, "startTime"), *ti = key(o, e, "title"), *toc = key(o, e, "toc");
		if (st && !(toc && !strncmp(toc, "false", 5))) {
			ch[nch].start = atof(st);
			if (ti && *ti == '"') json_string(ti + 1, ch[nch].title, sizeof ch[nch].title);
			else snprintf(ch[nch].title, sizeof ch[nch].title, "Capítulo %d", nch + 1);
			nch++;
		}
		p = e + 1;
	}
	free(js);
	qsort(ch, nch, sizeof ch[0], cmp);
	lg("chapters: %d from %s", nch, fn);
	return nch;
}
