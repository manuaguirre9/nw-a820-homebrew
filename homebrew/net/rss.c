/* rss.c: minimal podcast RSS parser: <item> -> title, enclosure url, pubDate (newest first as in the feed).
 * Tolerant of CDATA and entities; no full XML. Used to decide which episodes are missing on the device. */
#include "rss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *find_ci(const char *h, const char *n, const char *end)
{
	size_t l = strlen(n);
	for (const char *p = h; p + l <= end; p++) if (!strncasecmp(p, n, l)) return p;
	return 0;
}

static void copy_text(char *dst, size_t max, const char *s, const char *e)
{
	/* strip CDATA and tags, decode a few entities, trim */
	size_t k = 0;
	while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
	int cdata = 0;
	if (e - s >= 9 && !strncmp(s, "<![CDATA[", 9)) {
		s += 9; cdata = 1;
		const char *ce = find_ci(s, "]]>", e);
		if (ce) e = ce;
	}
	while (s < e && k + 1 < max) {
		if (*s == '<' && !cdata) { const char *c = memchr(s, '>', e - s); if (!c) break; s = c + 1; continue; }
		if (cdata) { if (*s == '\r' || *s == '\n' || *s == '\t') { if (k && dst[k - 1] != ' ') dst[k++] = ' '; s++; continue; } dst[k++] = *s++; continue; }
		if (*s == '&') {
			if (!strncmp(s, "&amp;", 5)) { dst[k++] = '&'; s += 5; continue; }
			if (!strncmp(s, "&lt;", 4)) { dst[k++] = '<'; s += 4; continue; }
			if (!strncmp(s, "&gt;", 4)) { dst[k++] = '>'; s += 4; continue; }
			if (!strncmp(s, "&quot;", 6)) { dst[k++] = '"'; s += 6; continue; }
			if (!strncmp(s, "&apos;", 6)) { dst[k++] = '\''; s += 6; continue; }
			if (s[1] == '#') {	/* numeric entity: keep ASCII, encode the rest as UTF-8 */
				char *e2; unsigned long cp = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, &e2, 16) : strtoul(s + 2, &e2, 10);
				if (*e2 == ';') {
					if (cp < 0x80) { if (k + 1 < max) dst[k++] = (char)cp; }
					else if (cp < 0x800 && k + 2 < max) { dst[k++] = (char)(0xC0 | cp >> 6); dst[k++] = (char)(0x80 | (cp & 63)); }
					else if (cp < 0x10000 && k + 3 < max) { dst[k++] = (char)(0xE0 | cp >> 12); dst[k++] = (char)(0x80 | ((cp >> 6) & 63)); dst[k++] = (char)(0x80 | (cp & 63)); }
					else if (cp < 0x110000 && k + 4 < max) { dst[k++] = (char)(0xF0 | cp >> 18); dst[k++] = (char)(0x80 | ((cp >> 12) & 63)); dst[k++] = (char)(0x80 | ((cp >> 6) & 63)); dst[k++] = (char)(0x80 | (cp & 63)); }
					s = e2 + 1; continue;
				}
			}
		}
		if (*s == '\r' || *s == '\n' || *s == '\t') { if (k && dst[k - 1] != ' ') dst[k++] = ' '; s++; continue; }
		dst[k++] = *s++;
	}
	while (k && dst[k - 1] == ' ') k--;
	dst[k] = 0;
	while (dst[0] == ' ') memmove(dst, dst + 1, strlen(dst));
}

static int tag_text(const char *from, const char *end, const char *tag, char *dst, size_t max)
{
	char open[64], close[64];
	snprintf(open, sizeof open, "<%s", tag); snprintf(close, sizeof close, "</%s>", tag);
	const char *o = find_ci(from, open, end);
	if (!o) { dst[0] = 0; return 0; }
	const char *gt = memchr(o, '>', end - o);
	if (!gt) { dst[0] = 0; return 0; }
	const char *c = find_ci(gt + 1, close, end);
	if (!c) { dst[0] = 0; return 0; }
	copy_text(dst, max, gt + 1, c);
	return 1;
}

/* "Thu, 20 Aug 2026 16:42:09 +0000" -> "2026-08-20" (falls back to "0000-00-00") */
static void date_iso(const char *pub, char *out)
{
	static const char *mon[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
	int d = 0, y = 0, m = 0;
	char ms[8] = "";
	if (sscanf(pub, "%*[^,], %d %3s %d", &d, ms, &y) == 3 || sscanf(pub, "%d %3s %d", &d, ms, &y) == 3)
		for (int i = 0; i < 12; i++) if (!strcasecmp(ms, mon[i])) m = i + 1;
	if (m && y >= 1000 && y <= 9999 && d >= 1 && d <= 31) snprintf(out, 12, "%04d-%02d-%02d", y, m, d); else strcpy(out, "0000-00-00");
}

/* value of attribute `name` inside the tag starting at t (up to '>'), 0 if absent */
static int attr(const char *t, const char *end, const char *name, char *out, size_t max)
{
	const char *gt = memchr(t, '>', end - t);
	if (!gt) return 0;
	char pat[32]; snprintf(pat, sizeof pat, "%s=\"", name);
	const char *a = find_ci(t, pat, gt);
	if (!a) return 0;
	a += strlen(pat);
	const char *q = memchr(a, '"', gt - a);
	if (!q || (size_t)(q - a) >= max) return 0;
	memcpy(out, a, q - a); out[q - a] = 0;
	/* &amp; in URLs */
	for (char *x = out; (x = strstr(x, "&amp;")); ) memmove(x + 1, x + 5, strlen(x + 5) + 1);
	return 1;
}

/* Podcasting 2.0: <podcast:transcript url type> (prefer SRT, then VTT) and <podcast:chapters url> */
static void p20(const char *p, const char *ie, struct rss_item *it)
{
	int best = 0;	/* 2 srt, 1 vtt */
	for (const char *t = p; (t = find_ci(t, "<podcast:transcript", ie)); t++) {
		char type[48] = "", url[384];
		attr(t, ie, "type", type, sizeof type);
		if (!attr(t, ie, "url", url, sizeof url)) continue;
		int rank = strstr(type, "subrip") || strstr(type, "srt") ? 2 : strstr(type, "vtt") ? 1 : 0;
		if (rank > best) { best = rank; snprintf(it->transcript, sizeof it->transcript, "%s", url); strcpy(it->tr_ext, rank == 2 ? ".srt" : ".vtt"); }
	}
	const char *c = find_ci(p, "<podcast:chapters", ie);
	if (c) attr(c, ie, "url", it->chapters, sizeof it->chapters);
}

/* the show's artwork: <itunes:image href="..."> or <image><url>...</url> in the channel (before the first <item>) */
int rss_channel_image(const char *xml, size_t len, char *url, size_t max)
{
	const char *end = xml + len, *first = find_ci(xml, "<item", end);
	if (first) end = first;
	url[0] = 0;
	const char *t = find_ci(xml, "<itunes:image", end);
	if (t && attr(t, end, "href", url, max)) return 1;
	const char *im = find_ci(xml, "<image>", end);
	if (im) { const char *u = find_ci(im, "<url>", end), *ue = u ? find_ci(u, "</url>", end) : 0;
		if (u && ue && (size_t)(ue - u - 5) < max) { copy_text(url, max, u + 5, ue); return url[0] != 0; } }
	return 0;
}

int rss_parse(const char *xml, size_t len, struct rss_item *items, int max)
{
	const char *end = xml + len, *p = xml;
	int n = 0;
	while (n < max && (p = find_ci(p, "<item", end))) {
		const char *ie = find_ci(p, "</item>", end);
		if (!ie) break;
		struct rss_item *it = &items[n];
		memset(it, 0, sizeof *it);
		tag_text(p, ie, "title", it->title, sizeof it->title);
		char pub[64];
		tag_text(p, ie, "pubDate", pub, sizeof pub);
		date_iso(pub, it->date);
		const char *enc = find_ci(p, "<enclosure", ie);
		if (enc) {
			char len[24];
			if (!attr(enc, ie, "url", it->url, sizeof it->url)) fprintf(stderr, "rss: enclosure url missing or too long\n");
			if (attr(enc, ie, "length", len, sizeof len)) it->bytes = atol(len);
		}
		p20(p, ie, it);
		if (it->url[0]) n++;
		p = ie + 7;
	}
	return n;
}

/* file name used on the device for an item: "YYYY-MM-DD - title.ext" (same scheme as the PC script) */
void rss_filename(const struct rss_item *it, char *out, size_t max)
{
	const char *ext = ".mp3";
	const char *q = strchr(it->url, '?');
	size_t ul = q ? (size_t)(q - it->url) : strlen(it->url);
	if (ul > 4 && !strncasecmp(it->url + ul - 4, ".m4a", 4)) ext = ".m4a";
	else if (ul > 4 && !strncasecmp(it->url + ul - 4, ".aac", 4)) ext = ".aac";
	/* same as the PC script: drop \/:*?"<>|, strip, collapse whitespace runs to one space, keep 90 characters
	 * (code points, not bytes). Python's slice can leave a trailing space; so do we. */
	char t[400];
	size_t k = 0;
	int chars = 0, pend_space = 0;
	const char *s = it->title;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || ((unsigned char)*s == 0xC2 && (unsigned char)s[1] == 0xA0)) s += (unsigned char)*s == 0xC2 ? 2 : 1;
	for (; *s && chars < 90 && k + 5 < sizeof t; s++) {
		unsigned char c = (unsigned char)*s;
		if (strchr("\\/:*?\"<>|", c)) continue;
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { pend_space = 1; continue; }
		/* Unicode spaces that Python's \s also collapses: U+00A0, U+2000..U+200A, U+202F, U+205F, U+3000 */
		if (c == 0xC2 && (unsigned char)s[1] == 0xA0) { pend_space = 1; s++; continue; }
		if (c == 0xE2 && (unsigned char)s[1] == 0x80 && (((unsigned char)s[2] >= 0x80 && (unsigned char)s[2] <= 0x8A) || (unsigned char)s[2] == 0xAF)) { pend_space = 1; s += 2; continue; }
		if (c == 0xE2 && (unsigned char)s[1] == 0x81 && (unsigned char)s[2] == 0x9F) { pend_space = 1; s += 2; continue; }
		if (c == 0xE3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80) { pend_space = 1; s += 2; continue; }
		if (pend_space && k > 0) { t[k++] = ' '; chars++; if (chars >= 90) break; }
		pend_space = 0;
		t[k++] = (char)c;
		if ((c & 0xC0) != 0x80) {	/* count a code point at its first byte, copy its continuation bytes */
			while ((s[1] & 0xC0) == 0x80 && k + 1 < sizeof t) t[k++] = *++s;
			chars++;
		}
	}
	t[k] = 0;
	snprintf(out, max, "%s - %s%s", it->date, t[0] ? t : "episodio", ext);
}
