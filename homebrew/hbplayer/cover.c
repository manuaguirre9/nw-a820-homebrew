/* cover.c: album / book art. Looks for an embedded ID3v2 APIC (or v2.2 PIC) or FLAC PICTURE JPEG, else cover.jpg / folder.jpg
 * next to the file (also when the embedded image can't be decoded). Decoded with TJpgDec (integer, baseline JPEG
 * only) using its 1/2..1/8 down-scaling so big podcast covers fit in RAM, then box-scaled to the requested square;
 * progressive JPEGs (TJpgDec can't) go through stb_image. */
#include "hb.h"
#include "../third_party/tjpgd/src/tjpgd.h"
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS	/* the device's old glibc: no TLS */
#define STBI_NO_SIMD
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#define open(p, f) _open(p, (f) | O_BINARY)	/* PC screenshot build: binary reads */
#endif

struct src { const unsigned char *p; size_t n, pos; uint16_t *out; int ow, oh, blocks; };
void (*cover_tick)(void);
int (*cover_cancel)(void);	/* set by a caller that wants to give up when a key comes (cover_aborted() after) */
static int aborted;
int cover_aborted(void) { return aborted; }
static int cancelled(void) { if (!aborted && cover_cancel && cover_cancel()) aborted = 1; return aborted; }

static size_t in_fn(JDEC *jd, uint8_t *buf, size_t n)
{
	struct src *s = jd->device;
	if (s->pos + n > s->n) n = s->n - s->pos;
	if (buf) memcpy(buf, s->p + s->pos, n);
	s->pos += n;
	return n;
}

static int out_fn(JDEC *jd, void *bitmap, JRECT *r)
{
	struct src *s = jd->device;
	const uint16_t *b = bitmap;
	if (!(++s->blocks & 15)) { hb_alive(); if (cover_tick) cover_tick(); if (cancelled()) return 0; }
	int w = r->right - r->left + 1;
	for (int y = r->top; y <= r->bottom; y++)
		for (int x = r->left; x <= r->right; x++)
			if (x < s->ow && y < s->oh) s->out[y * s->ow + x] = b[(y - r->top) * w + (x - r->left)];
	return 1;
}

/* JPEGs TJpgDec refuses (progressive): stb_image, full size RGB, then the same centre crop + box filter */
static int decode_stb(const unsigned char *data, size_t n, uint16_t *dst, int size)
{
	int w, h, c;
	if (!stbi_info_from_memory(data, (int)n, &w, &h, &c) || w <= 0 || h <= 0 || (long)w * h > 1200000) return 0;	/* ~1100x1100 max: RAM */
	unsigned char *px = stbi_load_from_memory(data, (int)n, &w, &h, &c, 3);
	if (!px) return 0;
	int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
	for (int y = 0; y < size; y++) {
		if (!(y & 7)) { if (cover_tick) cover_tick(); if (cancelled()) { stbi_image_free(px); return 0; } }
		for (int x = 0; x < size; x++) {
			int sx0 = x0 + x * side / size, sx1 = x0 + (x + 1) * side / size, sy0 = y0 + y * side / size, sy1 = y0 + (y + 1) * side / size;
			if (sx1 <= sx0) sx1 = sx0 + 1;
			if (sy1 <= sy0) sy1 = sy0 + 1;
			int r = 0, g = 0, b = 0, k = 0;
			for (int yy = sy0; yy < sy1; yy += (sy1 - sy0 + 2) / 3)
				for (int xx = sx0; xx < sx1; xx += (sx1 - sx0 + 2) / 3) {
					const unsigned char *q = px + ((size_t)yy * w + xx) * 3;
					r += q[0]; g += q[1]; b += q[2]; k++;
				}
			dst[y * size + x] = RGB(r / k, g / k, b / k);
		}
	}
	stbi_image_free(px);
	return 1;
}

/* decode JPEG data into a size x size RGB565 square (centre crop); 1 on success */
static int decode(const unsigned char *data, size_t n, uint16_t *dst, int size)
{
	static uint32_t pool[16384 / 4];	/* TJpgDec carves 32-bit aligned blocks out of it: must be word aligned (unaligned = SIGBUS on ARM9) */
	JDEC jd;
	struct src s = { data, n, 0, 0, 0, 0, 0 };
	JRESULT pr = jd_prepare(&jd, in_fn, pool, sizeof pool, &s);
	if (pr == JDR_FMT3) return decode_stb(data, n, dst, size);	/* progressive (or another JPEG flavour TJpgDec lacks) */
	if (pr != JDR_OK) return 0;
	int scale = 0;
	while (scale < 3 && (jd.width >> (scale + 1)) >= (unsigned)size && (jd.height >> (scale + 1)) >= (unsigned)size) scale++;
	s.ow = jd.width >> scale; s.oh = jd.height >> scale;
	if (s.ow <= 0 || s.oh <= 0 || (long)s.ow * s.oh > 700000) return 0;
	s.out = calloc((size_t)s.ow * s.oh, 2);
	if (!s.out) return 0;
	int ok = jd_decomp(&jd, out_fn, scale) == JDR_OK;
	if (ok) {	/* centre crop to a square, box filter down to size */
		int side = s.ow < s.oh ? s.ow : s.oh, x0 = (s.ow - side) / 2, y0 = (s.oh - side) / 2;
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++) {
				int sx0 = x0 + x * side / size, sx1 = x0 + (x + 1) * side / size, sy0 = y0 + y * side / size, sy1 = y0 + (y + 1) * side / size;
				if (sx1 <= sx0) sx1 = sx0 + 1;
				if (sy1 <= sy0) sy1 = sy0 + 1;
				int r = 0, g = 0, b = 0, c = 0;
				for (int yy = sy0; yy < sy1; yy += (sy1 - sy0 + 2) / 3)
					for (int xx = sx0; xx < sx1; xx += (sx1 - sx0 + 2) / 3) {
						uint16_t v = s.out[yy * s.ow + xx];
						r += v >> 11; g += (v >> 5) & 63; b += v & 31; c++;
					}
				dst[y * size + x] = (uint16_t)((r / c) << 11 | (g / c) << 5 | (b / c));
			}
	}
	free(s.out);
	return ok;
}

static unsigned syncsafe(const unsigned char *p) { return (p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

/* returns malloc'd JPEG bytes of the first APIC/PIC frame, or 0 */
static unsigned char *find_apic(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	unsigned char h[10];
	unsigned char *tag = 0, *img = 0;
	if (read(fd, h, 10) != 10 || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) goto out;
	unsigned tsz = syncsafe(h + 6);
	if (tsz < 10 || tsz > 2 * 1024 * 1024) goto out;	/* bigger tags: no cover (RAM) */
	tag = malloc(tsz);
	if (!tag) goto out;
	for (unsigned got = 0; got < tsz; ) {	/* read in chunks, keeping the audio fed */
		ssize_t r = read(fd, tag + got, tsz - got > 65536 ? 65536 : tsz - got);
		if (r <= 0) goto out;
		got += r;
		if (cover_tick) cover_tick();
		if (cancelled()) goto out;
	}
	int v = h[3];
	unsigned pos = 0, hdr = v == 2 ? 6 : 10;
	if (v >= 3 && (h[5] & 0x40)) {	/* extended header; a corrupt size must not wrap pos (tsz >= 10: 4 bytes readable) */
		unsigned ext = v == 4 ? syncsafe(tag) : (unsigned)tag[0] << 24 | (unsigned)tag[1] << 16 | (unsigned)tag[2] << 8 | tag[3];
		if (ext > tsz - 4) goto out;
		pos = v == 4 ? ext : ext + 4;
	}
	while (pos < tsz && tsz - pos >= hdr) {	/* no pos + hdr: can't wrap */
		const unsigned char *f = tag + pos;
		if (!f[0]) break;
		unsigned fsz = v == 2 ? (unsigned)f[3] << 16 | (unsigned)f[4] << 8 | f[5] : v == 4 ? syncsafe(f + 4) : (unsigned)f[4] << 24 | (unsigned)f[5] << 16 | (unsigned)f[6] << 8 | f[7];
		if (fsz > tsz - pos - hdr) break;
		const unsigned char *d = f + hdr, *e = d + fsz;
		if ((v == 2 && !memcmp(f, "PIC", 3)) || (v > 2 && !memcmp(f, "APIC", 4))) {
			if (fsz < 2) break;	/* encoding byte + at least something: d[0] must be inside the frame */
			int enc = d[0];
			const unsigned char *p = d + 1;
			/* p never moves past e (a truncated frame: no image) */
			if (v == 2) p = e - p > 3 ? p + 3 : e; else { while (p < e && *p) p++; if (p < e) p++; }	/* image format / mime */
			if (p < e) p++;	/* picture type */
			if (enc == 1 || enc == 2) { while (e - p > 1 && (p[0] || p[1])) p += 2; p = e - p > 2 ? p + 2 : e; }
			else { while (p < e && *p) p++; if (p < e) p++; }
			if (p < e && e - p > 4 && p[0] == 0xFF && p[1] == 0xD8) {
				*len = e - p;
				img = malloc(*len);
				if (img) memcpy(img, p, *len);
			}
			break;
		}
		pos += hdr + fsz;
	}
out:
	free(tag);
	close(fd);
	return img;
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

/* FLAC: the JPEG of a PICTURE metadata block (the front cover, type 3, when there are several); malloc'd, or 0 */
static unsigned char *find_flac_pic(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	unsigned char b[10], *blk = 0, *img = 0;
	long off = 0;	/* an ID3v2 tag some taggers put in front of "fLaC" */
	if (read(fd, b, 10) == 10 && !memcmp(b, "ID3", 3)) off = 10 + (long)syncsafe(b + 6) + (b[5] & 0x10 ? 10 : 0);
	if (lseek(fd, off, SEEK_SET) < 0 || read(fd, b, 4) != 4 || memcmp(b, "fLaC", 4)) goto out;
	off += 4;
	for (int i = 0; i < 128; i++) {
		if (lseek(fd, off, SEEK_SET) < 0 || read(fd, b, 4) != 4) break;
		unsigned n = (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
		if ((b[0] & 127) == 6 && n > 32 && n <= 4 * 1024 * 1024 && (blk = malloc(n))) {
			for (unsigned got = 0; got < n; ) {	/* read in chunks, keeping the audio fed */
				ssize_t r = read(fd, blk + got, n - got > 65536 ? 65536 : n - got);
				if (r <= 0) goto out;
				got += r;
				if (cover_tick) cover_tick();
				if (cancelled()) goto out;
			}
			/* type, mime, description, width, height, depth, colours, data; every length checked against the block */
			const unsigned char *p = blk + 4, *e = blk + n;
			unsigned type = be32(blk), l = be32(p);
			p += 4;
			if (l <= (size_t)(e - p) - 4) { p += l; l = be32(p); p += 4;	/* mime */
				if (l <= (size_t)(e - p) && (size_t)(e - p) - l >= 20) { p += l + 16; l = be32(p); p += 4;	/* description, sizes */
					if (l >= 4 && l <= (size_t)(e - p) && p[0] == 0xFF && p[1] == 0xD8 && (!img || type == 3)) {
						free(img);
						if ((img = malloc(l))) { memcpy(img, p, l); *len = l; }
					}
				}
			}
			free(blk); blk = 0;
			if (img && type == 3) break;
		}
		if (b[0] & 128) break;	/* the last metadata block */
		off += 4 + n;
	}
out:
	free(blk);
	close(fd);
	return img;
}

static unsigned char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *b = n > 0 && n < 4 * 1024 * 1024 ? malloc(n) : 0;
	if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = 0; }
	fclose(f);
	*len = b ? (size_t)n : 0;
	return b;
}

#define CACHE_DIR "/contents/HB/cache"

static unsigned long path_hash(const char *s)
{
	unsigned long h = 2166136261u;
	for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

/* cache files go into 256 subfolders by the key's last byte: a FAT directory is searched entry by entry, and a
 * 4000-song library makes ~12000 entries (images + markers) in one folder */
static void cache_name(char *fn, size_t max, const char *path, int size, const char *ext)
{
	unsigned long h = path_hash(path);
	snprintf(fn, max, CACHE_DIR "/%02lx/%08lx_%d.%s", h & 255, h, size, ext);
}
static void cache_mkdir(const char *fn)
{
	char d[PATHLEN]; snprintf(d, sizeof d, "%s", fn);
	char *sl = strrchr(d, '/');
	if (sl) { *sl = 0; mkdir("/contents/HB", 0755); mkdir(CACHE_DIR, 0755); mkdir(d, 0755); }
}

static int cache_read(const char *path, int size, uint16_t *dst)
{
	char fn[PATHLEN]; cache_name(fn, sizeof fn, path, size, "rgb");
	FILE *f = fopen(fn, "rb");
	if (!f) return 0;
	int ok = fread(dst, 2, (size_t)size * size, f) == (size_t)size * size;
	fclose(f);
	return ok;
}

/* markers next to the images: "<key>_<size>.no" = the file has no cover (don't read its tag again), "<key>_<size>.dir"
 * = its cover is the folder image (cached once under that image's own key, not once per track) */
static void marker_name(char *fn, size_t max, const char *path, int size, const char *ext) { cache_name(fn, max, path, size, ext); }
static int marker_has(const char *path, int size, const char *ext)
{
	char fn[PATHLEN]; marker_name(fn, sizeof fn, path, size, ext);
	return access(fn, F_OK) == 0;
}
static void marker_set(const char *path, int size, const char *ext)
{
	char fn[PATHLEN]; marker_name(fn, sizeof fn, path, size, ext);
	cache_mkdir(fn);
	FILE *f = fopen(fn, "wb");
	if (f) fclose(f);
}

/* "<key>_<size>.ref" = the track's embedded image is cached under a content key (the JPEG's hash): every track of
 * an album carries the same picture, decoded once per album instead of once per track */
static int ref_cached(const char *path, int size, uint16_t *dst)
{
	char fn[PATHLEN], key[64];
	marker_name(fn, sizeof fn, path, size, "ref");
	FILE *f = fopen(fn, "r");
	if (!f) return 0;
	int ok = fgets(key, sizeof key, f) != 0;
	fclose(f);
	return ok && !strncmp(key, "apic:", 5) && cache_read(key, size, dst);
}
static void ref_set(const char *path, int size, const char *key)
{
	char fn[PATHLEN]; marker_name(fn, sizeof fn, path, size, "ref");
	cache_mkdir(fn);
	FILE *f = fopen(fn, "w");
	if (f) { fputs(key, f); fclose(f); }
}

static const char *folder_names[] = { "cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg", "front.jpg", 0 };

/* the folder image of a track from the cache (the track is marked .dir) */
static int folder_cached(const char *path, int size, uint16_t *dst)
{
	char dir[PATHLEN], p[PATHLEN + 16];
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0;
	for (int i = 0; folder_names[i]; i++) { snprintf(p, sizeof p, "%s/%s", dir, folder_names[i]); if (cache_read(p, size, dst)) return 1; }
	return 0;
}

static int cover_decode_src(const char *path, uint16_t *dst, int size, char *src, size_t srcmax);

/* written to a temporary name and renamed: the background cover process (warm.c) and the player may both be
 * writing or reading the same image */
static void cache_write(const char *path, int size, const uint16_t *src)
{
	char fn[PATHLEN], tmp[PATHLEN + 16]; cache_name(fn, sizeof fn, path, size, "rgb");
	cache_mkdir(fn);
	snprintf(tmp, sizeof tmp, "%s.%d", fn, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) return;
	int ok = fwrite(src, 2, (size_t)size * size, f) == (size_t)size * size;
	if (fclose(f) || !ok || rename(tmp, fn)) unlink(tmp);
}
static int cache_has(const char *path, int size)
{
	char fn[PATHLEN]; cache_name(fn, sizeof fn, path, size, "rgb");
	return access(fn, F_OK) == 0;
}

/* what a decode found, remembered for one size: the image (under the picture's key, the folder image's path or the
 * track's own path) and the track's pointer to it, or the "no cover" marker */
static void remember(const char *path, int size, const uint16_t *img, int ok, const char *src)
{
	if (!ok) marker_set(path, size, "no3");
	else if (!strncmp(src, "apic:", 5)) { if (!cache_has(src, size)) cache_write(src, size, img); ref_set(path, size, src); }	/* embedded: one image per picture */
	else if (src[0]) { if (!cache_has(src, size)) cache_write(src, size, img); marker_set(path, size, "dir"); }	/* one image per folder */
	else cache_write(path, size, img);
}

/* box filter from one square size down to a smaller one */
static void scale_down(const uint16_t *s, int sn, uint16_t *d, int dn)
{
	for (int y = 0; y < dn; y++) {
		int y0 = y * sn / dn, y1 = (y + 1) * sn / dn;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < dn; x++) {
			int x0 = x * sn / dn, x1 = (x + 1) * sn / dn;
			if (x1 <= x0) x1 = x0 + 1;
			int r = 0, g = 0, b = 0, c = 0;
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++) { uint16_t v = s[yy * sn + xx]; r += v >> 11; g += (v >> 5) & 63; b += v & 31; c++; }
			d[y * dn + x] = (uint16_t)((r / c) << 11 | (g / c) << 5 | (b / c));
		}
	}
}

/* cover of `path` at size x size (112 = player, 28 = list thumbnail): cache, then the tag / folder image. Results
 * are remembered: the image (per track for embedded art, per folder image otherwise) or a "no cover" marker. */
int cover_load(const char *path, uint16_t *dst, int size)
{
	aborted = 0;
	if (cache_read(path, size, dst)) {
		if (size <= 28) {	/* older versions cached a failed thumbnail as an all-black image */
			int any = 0;
			for (int i = 0; i < size * size && !any; i++) any = dst[i] != 0;
			if (!any) return 0;
		}
		return 1;
	}
	if (marker_has(path, size, "no3")) return 0;	/* "no3": older markers included progressive JPEGs ("no") and FLAC pictures ("no2"), now readable */
	if (marker_has(path, size, "dir") && folder_cached(path, size, dst)) return 1;
	if (ref_cached(path, size, dst)) return 1;
	/* decoded once at the player's size, and every smaller size the lists use is made from that image: an album
	 * used to be decoded again for the list, the mosaic and the player (1.2 s each for a big FLAC cover). Music:
	 * album list, mosaic, big rows; podcasts and books: the show screen, folder rows, episode rows, downloads */
	static const int music_sizes[] = { PLAYER_ART, GRID_TH, BIG_THUMB, 28, 0 };
	static const int other_sizes[] = { PLAYER_ART, POD_ART, BIG_THUMB, EP_THUMB, 40, 28, 0 };
	const int *sizes = !strncmp(path, "/contents/Music/", 16) ? music_sizes : other_sizes;
	static uint16_t small[PLAYER_ART * PLAYER_ART];
	char src[PATHLEN + 16];
	int big = size < PLAYER_ART ? PLAYER_ART : size;
	uint16_t *full = big == size ? dst : malloc((size_t)big * big * 2);
	if (!full) { full = dst; big = size; }
	int ok = cover_decode_src(path, full, big, src, sizeof src);
	if (aborted) { if (full != dst) free(full); return 0; }	/* a key came: nothing is remembered, the caller tries again later */
	remember(path, big, full, ok, src);
	int done_req = big == size;
	for (int k = 0; sizes[k]; k++) {
		int sz = sizes[k];
		if (sz >= big) continue;
		uint16_t *out = sz == size ? dst : small;
		if (ok) scale_down(full, big, out, sz);
		remember(path, sz, out, ok, src);
		if (sz == size) done_req = 1;
	}
	if (!done_req) { if (ok) scale_down(full, big, dst, size); remember(path, size, dst, ok, src); }
	if (full != dst) free(full);
	return ok;
}

/* list thumbnail: decoded straight at 28 px (the JPEG decoder's 1/8 scale: no full-size decode) */

/* decodes the cover; src = the folder image it came from ("" = embedded in the tag) */
static int cover_decode_src(const char *path, uint16_t *dst, int size, char *src, size_t srcmax)
{
	size_t n = 0;
	src[0] = 0;
	unsigned char *jpg = find_apic(path, &n);
	if (!jpg && !aborted) jpg = find_flac_pic(path, &n);
	if (aborted) { free(jpg); return 0; }
	if (jpg) {	/* cached under the picture's own key: the album's other tracks decoded it already? */
		unsigned long h = 2166136261u;
		for (size_t i = 0; i < n; i++) h = (h ^ jpg[i]) * 16777619u;
		char key[64]; snprintf(key, sizeof key, "apic:%08lx:%lu", h, (unsigned long)n);
		int hit = cache_read(key, size, dst);
		if (hit || decode(jpg, n, dst, size)) {
			free(jpg);
			if (aborted) return 0;
			snprintf(src, srcmax, "%s", key);
			return 1;
		}
		free(jpg); jpg = 0;	/* embedded image unreadable: try the folder image */
		if (aborted) return 0;
	}
	{
		char dir[PATHLEN], p[PATHLEN + 16];
		snprintf(dir, sizeof dir, "%s", path);
		char *sl = strrchr(dir, '/');
		if (sl) *sl = 0;
		for (int i = 0; folder_names[i] && !jpg; i++) {
			snprintf(p, sizeof p, "%s/%s", dir, folder_names[i]);
			if (access(p, R_OK)) continue;
			if (cache_read(p, size, dst)) { snprintf(src, srcmax, "%s", p); return 1; }	/* another track decoded it */
			jpg = read_file(p, &n);
			if (jpg) snprintf(src, srcmax, "%s", p);
		}
	}
	if (!jpg) return 0;
	int ok = decode(jpg, n, dst, size);
	free(jpg);
	return ok;
}

/* a JPEG file by itself (podcast thumbnails downloaded to /tmp): decoded, never cached */
int cover_decode_file(const char *jpg_path, uint16_t *dst, int size)
{
	size_t n = 0;
	unsigned char *jpg = read_file(jpg_path, &n);
	if (!jpg) return 0;
	int ok = decode(jpg, n, dst, size);
	free(jpg);
	return ok;
}

/* anything remembered for this file at this size (an image, a pointer to one, or "no cover")? Only looks for the
 * files, without reading them: the background pass asks this for every song at every start */
int cover_known(const char *path, int size)
{
	return cache_has(path, size) || marker_has(path, size, "ref") || marker_has(path, size, "dir") || marker_has(path, size, "no3");
}

/* only what the cache already has: no tag reading or decoding (instant) */
int cover_cached(const char *path, uint16_t *dst, int size)
{
	if (cache_read(path, size, dst)) return 1;
	if (marker_has(path, size, "dir") && folder_cached(path, size, dst)) return 1;
	return ref_cached(path, size, dst);
}
