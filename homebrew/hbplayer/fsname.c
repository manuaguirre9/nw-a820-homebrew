/* fsname.c: file names on the Walkman's FAT partition. The firmware mounts /contents without "utf8", so the
 * kernel's vfat driver gives and takes names in ISO-8859-1: a name Windows stored as "Révolution" reads here as
 * the byte 0xE9, and characters outside Latin-1 (curly quotes, dashes, emoji) read as '?'. Our strings are UTF-8,
 * so every name crossing that boundary is converted:
 *   fs_to_utf8   directory entry -> text for the screen
 *   utf8_to_fs   a name we create (podcast episodes) -> Latin-1, with plain equivalents for the rest; the PC
 *                script podsync.py applies the same mapping, so both produce the same names
 *   fs_match     "does this directory entry stand for that UTF-8 name?" ('?' in the entry matches any character) */
#include "hb.h"
#include <string.h>

void fs_to_utf8(const char *in, char *out, size_t max)
{
	size_t k = 0;
	for (const unsigned char *s = (const unsigned char *)in; *s && k + 1 < max; s++) {
		if (*s < 0x80) out[k++] = (char)*s;
		else { if (k + 2 >= max) break; out[k++] = (char)(0xC0 | *s >> 6); out[k++] = (char)(0x80 | (*s & 63)); }
	}
	out[k] = 0;
}

/* next code point of a UTF-8 string (invalid bytes are taken as Latin-1) */
static unsigned next_cp(const unsigned char **ps)
{
	const unsigned char *s = *ps;
	unsigned c = *s++;
	if (c >= 0xC0 && c < 0xE0 && (s[0] & 0xC0) == 0x80) { c = (c & 31) << 6 | (s[0] & 63); s++; }
	else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
	else if (c >= 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) { c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3; }
	*ps = s;
	return c;
}

/* Latin-1 stand-in for a code point above 0xFF ("" = drop it) */
static const char *fallback(unsigned c)
{
	switch (c) {
	case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
	case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "'";	/* '"' is not allowed on FAT */
	case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
	case 0x2026: return "...";
	case 0x2022: case 0x00B7: return "-";
	case 0x20AC: return "EUR";
	case 0x2122: return "TM";
	default: return c >= 0x1F000 ? "" : "_";	/* emoji: drop; anything else: _ */
	}
}

void utf8_to_fs(const char *in, char *out, size_t max)
{
	size_t k = 0;
	const unsigned char *s = (const unsigned char *)in;
	while (*s && k + 1 < max) {
		unsigned c = next_cp(&s);
		if (c < 0x100) out[k++] = (char)c;
		else for (const char *f = fallback(c); *f && k + 1 < max; f++) out[k++] = *f;
	}
	out[k] = 0;
}

int fs_match(const char *fsname, const char *utf8name)
{
	const unsigned char *a = (const unsigned char *)fsname, *b = (const unsigned char *)utf8name;
	while (*a && *b) {
		unsigned cb = next_cp(&b);
		if (*a == '?' && cb >= 0x100) { a++; continue; }	/* the driver shows unmappable characters as '?' */
		if (*a != cb) return 0;
		a++;
	}
	return !*a && !*b;
}
