/* podsearch.c: finding podcasts by name. The Apple Podcasts directory answers a plain HTTPS GET with JSON and needs
 * no account or key:
 *   https://itunes.apple.com/search?media=podcast&entity=podcast&limit=15&country=US&term=corridor+cast
 * Each result carries the show's name, author, episode count, RSS feed and artwork; the artwork URL ends in
 * ".../100x100bb.jpg" and the server makes any size asked for, so thumbnails come at exactly the size the list
 * draws (a few KB each). This file only builds the request and reads the answer; podsync.c does the network. */
#include "hb.h"
#include "podsearch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char pod_country[4] = "US";	/* the directory's store: decides the ranking and what is listed */

/* percent-encodes a UTF-8 search text for a query string (spaces as '+') */
static void url_term(const char *in, char *out, size_t max)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t k = 0;
	for (const unsigned char *s = (const unsigned char *)in; *s && k + 4 < max; s++) {
		unsigned c = *s;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[k++] = (char)c;
		else if (c == ' ') out[k++] = '+';
		else { out[k++] = '%'; out[k++] = hex[c >> 4]; out[k++] = hex[c & 15]; }
	}
	out[k] = 0;
}

void pod_search_url(const char *term, char *out, size_t max)
{
	char t[400];
	url_term(term, t, sizeof t);
	snprintf(out, max, "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=%d&country=%s&term=%s", POD_RESULTS, pod_country, t);
}

/* the same artwork at px x px: ".../100x100bb.jpg" -> ".../<px>x<px>bb.jpg" (other URLs are returned as they are) */
void pod_art_url(const char *art, int px, char *out, size_t max)
{
	snprintf(out, max, "%s", art);
	char *sl = strrchr(out, '/');
	if (!sl || !strstr(sl, "bb.")) return;
	char *dot = strstr(sl, "bb.");
	char tail[16]; snprintf(tail, sizeof tail, "%s", dot);	/* "bb.jpg" */
	snprintf(sl + 1, max - (size_t)(sl + 1 - out), "%dx%d%s", px, px, tail);
}

/* ---------- a small JSON reader: just enough for the directory's answer ---------- */
static const char *js, *je;

static void ws(void) { while (js < je && (*js == ' ' || *js == '\n' || *js == '\r' || *js == '\t')) js++; }

static unsigned hex4(const char *p)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v = v << 4 | (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
	}
	return v;
}

static size_t put_utf8(char *o, size_t k, size_t max, unsigned c)
{
	if (c < 0x80) { if (k + 1 < max) o[k++] = (char)c; }
	else if (c < 0x800) { if (k + 2 < max) { o[k++] = (char)(0xC0 | c >> 6); o[k++] = (char)(0x80 | (c & 63)); } }
	else if (c < 0x10000) { if (k + 3 < max) { o[k++] = (char)(0xE0 | c >> 12); o[k++] = (char)(0x80 | (c >> 6 & 63)); o[k++] = (char)(0x80 | (c & 63)); } }
	else if (k + 4 < max) { o[k++] = (char)(0xF0 | c >> 18); o[k++] = (char)(0x80 | (c >> 12 & 63)); o[k++] = (char)(0x80 | (c >> 6 & 63)); o[k++] = (char)(0x80 | (c & 63)); }
	return k;
}

/* a string at js (which points at '"'), decoded into out (may be 0 to skip it); 0 ok */
static int str(char *out, size_t max)
{
	if (js >= je || *js != '"') return -1;
	js++;
	size_t k = 0;
	while (js < je && *js != '"') {
		unsigned c = (unsigned char)*js++;
		if (c == '\\' && js < je) {
			char e = *js++;
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u':
				if (je - js < 4) return -1;
				c = hex4(js); js += 4;
				if (c >= 0xD800 && c < 0xDC00 && je - js >= 6 && js[0] == '\\' && js[1] == 'u') {	/* surrogate pair */
					unsigned lo = hex4(js + 2);
					if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); js += 6; }
				}
				break;
			default: c = (unsigned char)e;	/* \" \\ \/ */
			}
			if (out) k = put_utf8(out, k, max, c);
			continue;
		}
		if (out && k + 1 < max) out[k++] = (char)c;
	}
	if (js >= je) return -1;
	js++;
	if (out && max) out[k < max ? k : max - 1] = 0;
	return 0;
}

static int skip(int depth)
{
	ws();
	if (js >= je || depth > 32) return -1;
	if (*js == '"') return str(0, 0);
	if (*js == '{' || *js == '[') {
		char close = *js == '{' ? '}' : ']';
		js++; ws();
		if (js < je && *js == close) { js++; return 0; }
		for (;;) {
			if (close == '}') { ws(); if (str(0, 0)) return -1; ws(); if (js >= je || *js++ != ':') return -1; }
			if (skip(depth + 1)) return -1;
			ws();
			if (js >= je) return -1;
			if (*js == ',') { js++; continue; }
			if (*js == close) { js++; return 0; }
			return -1;
		}
	}
	while (js < je && *js != ',' && *js != '}' && *js != ']') js++;	/* number, true, false, null */
	return 0;
}

static void clean(char *s)
{
	for (char *c = s; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
	char *e = s + strlen(s);
	while (e > s && e[-1] == ' ') *--e = 0;
	while (*s == ' ') memmove(s, s + 1, strlen(s));
}

/* one result object at js */
static int result(struct pod_result *r)
{
	memset(r, 0, sizeof *r);
	ws();
	if (js >= je || *js++ != '{') return -1;
	ws();
	if (js < je && *js == '}') { js++; return 0; }
	for (;;) {
		char key[32];
		ws();
		if (str(key, sizeof key)) return -1;
		ws();
		if (js >= je || *js++ != ':') return -1;
		ws();
		char *dst = 0; size_t dmax = 0;
		if (!strcmp(key, "collectionName")) { dst = r->name; dmax = sizeof r->name; }
		else if (!strcmp(key, "artistName")) { dst = r->author; dmax = sizeof r->author; }
		else if (!strcmp(key, "feedUrl")) { dst = r->feed; dmax = sizeof r->feed; }
		else if (!strcmp(key, "artworkUrl100")) { dst = r->art; dmax = sizeof r->art; }
		else if (!strcmp(key, "primaryGenreName")) { dst = r->genre; dmax = sizeof r->genre; }
		if (dst && js < je && *js == '"') { if (str(dst, dmax)) return -1; }
		else if (!strcmp(key, "trackCount") && js < je && *js >= '0' && *js <= '9') { r->count = atoi(js); if (skip(1)) return -1; }
		else if (skip(1)) return -1;
		ws();
		if (js >= je) return -1;
		if (*js == ',') { js++; continue; }
		if (*js == '}') { js++; break; }
		return -1;
	}
	clean(r->name); clean(r->author); clean(r->genre);
	return 0;
}

/* the results of a directory answer; only shows with a feed are kept. Returns how many, -1 = not a valid answer */
int pod_search_parse(const char *json, size_t len, struct pod_result *res, int max)
{
	js = json; je = json + len;
	ws();
	if (js >= je || *js++ != '{') return -1;
	int n = 0;
	for (;;) {
		char key[32];
		ws();
		if (js < je && *js == '}') break;
		if (str(key, sizeof key)) return -1;
		ws();
		if (js >= je || *js++ != ':') return -1;
		ws();
		if (!strcmp(key, "results") && js < je && *js == '[') {
			js++;
			ws();
			if (js < je && *js == ']') js++;
			else for (;;) {
				struct pod_result r;
				if (result(&r)) return n ? n : -1;
				if (n < max && r.feed[0] && r.name[0]) res[n++] = r;
				ws();
				if (js < je && *js == ',') { js++; continue; }
				if (js < je && *js == ']') { js++; break; }
				return n ? n : -1;
			}
		} else if (skip(1)) return n ? n : -1;
		ws();
		if (js < je && *js == ',') { js++; continue; }
		break;
	}
	return n;
}

/* a folder name for the show (UTF-8; the caller converts it with utf8_to_fs): no characters FAT refuses, no
 * leading/trailing dots or spaces, at most 60 bytes cut on a character boundary */
void pod_folder_name(const char *name, char *out, size_t max)
{
	size_t k = 0;
	for (const char *s = name; *s && k + 1 < max && k < 60; s++) {
		unsigned char c = (unsigned char)*s;
		if (c >= 0xF0) { while ((s[1] & 0xC0) == 0x80) s++; c = ' '; }	/* emoji: FAT on the Walkman can't keep them */
		if (c < 0x20 || strchr("\\/:*?\"<>|", c)) c = ' ';
		if (c == ' ' && (!k || out[k - 1] == ' ')) continue;	/* one space at a time */
		out[k++] = (char)c;
	}
	while (k && (out[k - 1] & 0xC0) == 0x80) k--;	/* don't end inside a character */
	if (k && (unsigned char)out[k - 1] >= 0xC0) k--;
	out[k] = 0;
	clean(out);
	while (out[0] == '.') memmove(out, out + 1, strlen(out));
	k = strlen(out);
	while (k && (out[k - 1] == '.' || out[k - 1] == ' ')) out[--k] = 0;
	if (!out[0]) snprintf(out, max, "Podcast");
}
