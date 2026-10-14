/* podsync.c: podcasts over the phone's Bluetooth tethering (PAN): the subscriptions, the shared phone link and the
 * background downloads ("Buscar episodios nuevos").
 * The subscriptions are /contents/Podcasts/feeds.txt ("Name | RSS URL | N"), written by the player when a show is
 * added or removed from the menus (podui.c) and still editable on the PC. A sync reads each feed, compares it with the
 * files in /contents/Podcasts/<Name>/ and downloads the missing ones among the newest N (same file names as the PC
 * script podsync.py, so both can be used). The phone's address is in /contents/Podcasts/telefono.txt (chosen from
 * the menu: OPTION in Podcasts > Teléfono).
 * One phone link serves everyone: pod_net_up / pod_net_down count its users (a sync, the search screens), and a
 * foreground transfer (pod_fetch: a search, a feed to browse) pauses the background queue meanwhile (the transfer it
 * interrupts resumes from its .part).
 *
 * Only connecting is modal (a status screen for ~10 s). After that the work is a state machine advanced by
 * dl_tick(), which the player calls from its key wait (raw_event) so downloads keep going on every screen.
 * dl_screen() shows the queue with a progress bar per episode; the status bar shows an arrow + percent. */
#include "hb.h"
#include "screens.h"
#include "../net/net.h"
#include "../net/rss.h"
#include "../net/pan.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FEEDS "/contents/Podcasts/feeds.txt"
#define PODROOT "/contents/Podcasts"
#define PHONEFILE "/contents/Podcasts/telefono.txt"
#define TMPXML "/tmp/feed.xml"
#define MAXQ 40
#define MAXFEEDS 64
#define FEED_MAX (3 << 20)	/* /tmp is RAM: a feed is read up to here (the newest episodes come first) */

struct feed { char name[64], url[512]; int keep; };
static struct feed feeds[MAXFEEDS];
static int nfeeds;

enum { Q_WAIT, Q_RUN, Q_DONE, Q_FAIL, Q_CANCEL };
struct qitem { char feed[64], title[160], url[1024], dest[PATHLEN]; long got, total; int st;
	char tr[384], tre[6], ch[384]; int stage; };	/* Podcasting 2.0 sidecars: transcript, chapters; stage 0 audio 1 transcript 2 chapters */
static struct qitem q[MAXQ];
static int nq;

/* phases: idle, reading feed fi, downloading episode qi, finished (results kept for the screen) */
enum { P_IDLE, P_FEED, P_COVER, P_EPIS, P_END };
/* show artwork to fetch (cover.jpg in each podcast folder, used for the list thumbnails and as the episode art) */
struct cover { char url[1024], dest[PATHLEN]; };
static struct cover covers[16];
static int ncovers, ci;
static int phase, fi, qi, newcount, errors, dirty;
static int net_refs, had_bt;	/* users of the phone link (pod_net_up); Bluetooth audio was on before it */
static int sync_only = -1;	/* a sync of one show only (its index), -1 = all of them */
static int held, held_sidecar;	/* a foreground transfer has the link: the queue waits (dl_hold) */
static double rate, rate_t; static long rate_got;
static char endmsg[80];
static long feed_got, feed_scan_off; static int feed_items; static double feed_scan_t, feed_t0;
static int no_bt_restart;	/* dl_cancel_quiet: suspend / power off must not bring Bluetooth audio back */

int dl_active(void) { return phase == P_FEED || phase == P_COVER || phase == P_EPIS; }
int dl_take_dirty(void) { int d = dirty; dirty = 0; return d; }

/* like sleep(), but the music keeps playing and the watchdog stays fed */
static void wait_ui(double secs)
{
	double end = now() + secs;
	while (now() < end) { ui_tick(); usleep(eng_playing() ? 15000 : 50000); }
}

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n' || e[-1] == '\t')) *--e = 0;
	while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
}

static int load_feeds(void)
{
	if (dl_active()) return nfeeds;	/* the running sync works with these indexes: keep them */
	FILE *f = fopen(FEEDS, "r");
	nfeeds = 0;
	if (!f) return 0;
	char line[700];
	while (fgets(line, sizeof line, f) && nfeeds < MAXFEEDS) {
		if (!memcmp(line, "\xEF\xBB\xBF", 3)) memmove(line, line + 3, strlen(line + 3) + 1);	/* Notepad's BOM */
		trim(line);
		if (!line[0] || line[0] == '#') continue;
		char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : 0;
		if (!a || !b) continue;
		*a = *b = 0;
		struct feed *fd = &feeds[nfeeds];
		snprintf(fd->name, sizeof fd->name, "%s", line); trim(fd->name);
		for (char *c = fd->name; *c; ) { if (strchr("\\/:*?\"<>|", *c)) memmove(c, c + 1, strlen(c)); else c++; }
		trim(fd->name);
		snprintf(fd->url, sizeof fd->url, "%s", a + 1); trim(fd->url);
		fd->keep = atoi(b + 1); if (fd->keep < 1) fd->keep = 2; if (fd->keep > 20) fd->keep = 20;
		if (fd->name[0] && fd->url[0]) nfeeds++;
	}
	fclose(f);
	return nfeeds;
}

/* the show's folder: its name as the FAT driver stores it (Latin-1) */
static void feed_dir(int fidx, char *out, size_t max)
{
	char fsn[128];
	utf8_to_fs(feeds[fidx].name, fsn, sizeof fsn);
	snprintf(out, max, PODROOT "/%s", fsn);
}

/* feeds.txt rewritten as a whole: a new file, then a rename over the old one (a power cut leaves one or the other) */
static int save_feeds(void)
{
	FILE *f = fopen(FEEDS ".tmp", "w");
	if (!f) { lg("podcasts: can't write " FEEDS ".tmp"); return -1; }
	fprintf(f, "# Podcasts que sigue el Walkman: Nombre | URL del RSS | cuántos de los últimos episodios bajar\n");
	for (int i = 0; i < nfeeds; i++) fprintf(f, "%s | %s | %d\n", feeds[i].name, feeds[i].url, feeds[i].keep);
	int bad = ferror(f);
	if (fclose(f) || bad || rename(FEEDS ".tmp", FEEDS)) { unlink(FEEDS ".tmp"); lg("podcasts: saving feeds.txt failed"); return -1; }
	sync();
	return 0;
}

/* ---------- subscriptions (for the menus in podui.c) ---------- */

int pod_feeds(void) { mkdir(PODROOT, 0755); return load_feeds(); }
const char *pod_feed_name(int i) { return i >= 0 && i < nfeeds ? feeds[i].name : ""; }
const char *pod_feed_url(int i) { return i >= 0 && i < nfeeds ? feeds[i].url : ""; }
int pod_feed_keep(int i) { return i >= 0 && i < nfeeds ? feeds[i].keep : 0; }
void pod_feed_path(int i, char *out, size_t max) { if (i >= 0 && i < nfeeds) feed_dir(i, out, max); else if (max) out[0] = 0; }

/* the subscription a podcast folder belongs to (path as listed: Latin-1 on disk), -1 = a folder copied by hand */
int pod_feed_for_folder(const char *path)
{
	const char *leaf = strrchr(path, '/');
	leaf = leaf ? leaf + 1 : path;
	load_feeds();
	for (int i = 0; i < nfeeds; i++) {
		char fsn[128]; utf8_to_fs(feeds[i].name, fsn, sizeof fsn);
		if (!strcmp(fsn, leaf)) return i;
	}
	return -1;
}

int pod_feed_find_url(const char *url)
{
	load_feeds();
	for (int i = 0; i < nfeeds; i++) if (!strcmp(feeds[i].url, url)) return i;
	return -1;
}

/* adds a show (name already folder-safe, see pod_folder_name); returns its index, -1 on failure. A name in use by
 * another show gets " (2)", " (3)"... */
int pod_subscribe(const char *name, const char *url, int keep)
{
	int have = pod_feed_find_url(url);
	if (have >= 0) return have;
	if (nfeeds >= MAXFEEDS) return -1;
	struct feed *fd = &feeds[nfeeds];
	snprintf(fd->name, sizeof fd->name, "%s", name);
	for (int k = 2; k < 100; k++) {
		int clash = 0;
		for (int i = 0; i < nfeeds; i++) if (!strcasecmp(feeds[i].name, fd->name)) clash = 1;
		if (!clash) break;
		snprintf(fd->name, sizeof fd->name, "%.56s (%d)", name, k);
	}
	snprintf(fd->url, sizeof fd->url, "%s", url);
	fd->keep = keep < 1 ? 1 : keep > 20 ? 20 : keep;
	nfeeds++;
	if (save_feeds()) { nfeeds--; return -1; }
	char dir[PATHLEN]; feed_dir(nfeeds - 1, dir, sizeof dir);
	mkdir(dir, 0755);
	lg("podcasts: subscribed %s (%s)", fd->name, url);
	return nfeeds - 1;
}

int pod_set_keep(int i, int keep)
{
	if (i < 0 || i >= nfeeds) return -1;
	feeds[i].keep = keep < 1 ? 1 : keep > 20 ? 20 : keep;
	return save_feeds();
}

/* removes a show from feeds.txt; delete_files also deletes its folder (only the files in it: episodes, sidecars,
 * cover, the feed copy). Stops a running sync first: its indexes would be stale. 0 ok */
int pod_unsubscribe(int i, int delete_files)
{
	if (dl_active()) dl_cancel();
	load_feeds();
	if (i < 0 || i >= nfeeds) return -1;
	char dir[PATHLEN]; feed_dir(i, dir, sizeof dir);
	struct feed gone = feeds[i];
	memmove(feeds + i, feeds + i + 1, sizeof feeds[0] * (nfeeds - i - 1));
	nfeeds--;
	if (save_feeds()) { memmove(feeds + i + 1, feeds + i, sizeof feeds[0] * (nfeeds - i)); feeds[i] = gone; nfeeds++; return -1; }
	lg("podcasts: unsubscribed %s", gone.name);
	if (delete_files && gone.name[0] && !strstr(gone.name, "..")) {
		DIR *d = opendir(dir);
		if (d) {
			struct dirent *e;
			char fp[PATHLEN + 256];
			while ((e = readdir(d))) {
				if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
				snprintf(fp, sizeof fp, "%s/%s", dir, e->d_name);
				if (unlink(fp)) lg("podcasts: could not delete %s", fp);
			}
			closedir(d);
		}
		if (rmdir(dir)) lg("podcasts: folder %s kept (not empty?)", dir);
		sync();
	}
	return 0;
}

/* Episodes the user deleted on the Walkman are listed (one file name per line, as stored on disk) in
 * <podcast folder>/borrados.txt, so the automatic sync never brings them back. Old episodes are never deleted
 * automatically: the sync only adds the missing ones among the newest N of each feed. */
#define FORGOTTEN "borrados.txt"

static int forgotten(const char *dir, const char *fn_utf8)
{
	char p[PATHLEN + 16]; snprintf(p, sizeof p, "%s/" FORGOTTEN, dir);
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	char line[400]; int hit = 0;
	while (!hit && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && fs_match(line, fn_utf8)) hit = 1;
	}
	fclose(f);
	return hit;
}

/* deletes an episode (audio + transcript + chapters) and remembers it so it is not downloaded again; 0 ok */
int podcast_forget(const char *path)
{
	char dir[PATHLEN], base[PATHLEN], side[PATHLEN + 24];
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (!slash) return -1;
	*slash = 0;
	snprintf(base, sizeof base, "%s", slash + 1);
	char list[PATHLEN + 16]; snprintf(list, sizeof list, "%s/" FORGOTTEN, dir);
	FILE *f = fopen(list, "a");	/* remembered first: a power cut right after the delete must not bring it back */
	if (f) { fprintf(f, "%s\n", base); fclose(f); }
	if (unlink(path)) { lg("podcast: delete %s failed", path); return -1; }
	char *dot = strrchr(base, '.');
	static const char *ext[] = { ".srt", ".vtt", ".lrc", ".chapters.json" };
	for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++) {
		snprintf(side, sizeof side, "%s/%.*s%s", dir, dot ? (int)(dot - base) : (int)strlen(base), base, ext[i]);
		unlink(side);
	}
	lg("podcast: deleted %s (remembered in " FORGOTTEN ")", path);
	sync();
	return 0;
}

/* is the episode already there? fn is the UTF-8 name; files the PC copied may hold characters this mount
 * shows as '?', and ours are stored in Latin-1 (utf8_to_fs) */
static int have_file(const char *dir, const char *fn)
{
	if (forgotten(dir, fn)) return 1;	/* deleted by the user: counts as "had it" */
	char fsn[PATHLEN], p[PATHLEN];
	utf8_to_fs(fn, fsn, sizeof fsn);
	snprintf(p, sizeof p, "%s/%s", dir, fsn);
	if (access(p, F_OK) == 0) return 1;
	DIR *d = opendir(dir);
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while (!found && (e = readdir(d))) if (fs_match(e->d_name, fn)) found = 1;
	closedir(d);
	return found;
}

/* ---------- the state machine ---------- */

/* complete <item>s in the part of the feed downloaded so far. Only bytes not seen before are read (with an
 * overlap of 6 for a tag cut by the previous read); a "</item>" counts if it ends after the previous scan end. */
static int feed_count_items(void) { return pod_count_items(TMPXML, &feed_scan_off, &feed_items); }

static void sidecar_path(const struct qitem *it, const char *ext, char *out, size_t max)
{
	snprintf(out, max, "%s", it->dest);
	char *dot = strrchr(out, '.');
	if (dot && !strchr(dot, '/')) *dot = 0;
	strncat(out, ext, max - strlen(out) - 1);
}

/* after the audio: fetch the transcript and the chapters (small files) before the next episode */
static int start_sidecar(struct qitem *it)
{
	char path[PATHLEN + 24];
	while (it->stage < 2) {
		it->stage++;
		const char *url = it->stage == 1 ? it->tr : it->ch;
		if (!url[0]) continue;
		sidecar_path(it, it->stage == 1 ? it->tre : ".chapters.json", path, sizeof path);
		strncat(path, ".part", sizeof path - strlen(path) - 1);	/* renamed when complete (dl_tick) */
		unlink(path);
		if (!net_download_start(url, path)) { lg("podsync: %s -> %s", it->stage == 1 ? "transcript" : "chapters", path); return 1; }
	}
	return 0;
}

/* Unchanged feeds are not downloaded again: each podcast folder keeps the server's validators (.feedinfo: ETag and
 * Last-Modified) and the feed as last read (.feed.xml, possibly cut after the newest episodes). The next sync asks
 * "only if changed"; a 304 answer (~300 bytes) reuses the copy, so episodes that failed last time are still tried. */
static void feed_file(char *out, size_t max, int fidx, const char *leaf) { char d[PATHLEN]; feed_dir(fidx, d, sizeof d); snprintf(out, max, "%s/%s", d, leaf); }

static int copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : 0;
	if (!a || !b) { if (a) fclose(a); if (b) fclose(b); return -1; }
	static char buf[8192]; size_t n; int ok = 0;
	while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { ok = -1; break; }
	if (ferror(a)) ok = -1;
	fclose(a);
	if (fclose(b)) ok = -1;
	return ok;
}

static void feed_validators(int fidx, char *etag, size_t emax, char *lastmod, size_t lmax)
{
	char p[PATHLEN + 32]; etag[0] = lastmod[0] = 0;
	feed_file(p, sizeof p, fidx, ".feed.xml");
	if (access(p, R_OK)) return;	/* no copy to fall back on: a normal download */
	feed_file(p, sizeof p, fidx, ".feedinfo");
	FILE *f = fopen(p, "r");
	if (!f) return;
	char l[320];
	while (fgets(l, sizeof l, f)) {
		l[strcspn(l, "\r\n")] = 0;
		if (!strncmp(l, "etag=", 5)) snprintf(etag, emax, "%s", l + 5);
		else if (!strncmp(l, "lastmod=", 8)) snprintf(lastmod, lmax, "%s", l + 8);
	}
	fclose(f);
}

/* after a feed download (complete or stopped early): 304 -> the saved copy becomes the feed to parse; otherwise the
 * new feed and its validators are saved for next time */
static void feed_cache(void)
{
	static int clock_done;	/* the first server answer of a sync also sets the clock */
	if (!clock_done) { char d[64]; if (net_resp_header("Date", d, sizeof d)) { clock_from_http(d); clock_done = 1; } }
	char dir[PATHLEN], cp[PATHLEN + 32], ip[PATHLEN + 32];
	feed_dir(fi, dir, sizeof dir);
	mkdir(dir, 0755);
	feed_file(cp, sizeof cp, fi, ".feed.xml");
	feed_file(ip, sizeof ip, fi, ".feedinfo");
	if (net_not_modified()) {
		if (copy_file(cp, TMPXML)) lg("podsync: %s: no saved copy", feeds[fi].name);
		else lg("podsync: %s: unchanged, using the saved copy", feeds[fi].name);
		return;
	}
	char etag[256], lm[64];
	net_resp_header("ETag", etag, sizeof etag);
	/* Apache (mod_deflate) tags the gzip variant "xyz-gzip" and then does not recognise that tag: send the base one */
	{ char *g = strstr(etag, "-gzip\""); if (g) memmove(g, g + 5, strlen(g + 5) + 1); }
	net_resp_header("Last-Modified", lm, sizeof lm);
	if (!etag[0] && !lm[0]) { unlink(ip); return; }	/* the server can't tell: always a full download */
	char tmp[PATHLEN + 40];
	unlink(ip);	/* no validators while the copy changes: a cut here means a full download next time, never a stale 304 */
	snprintf(tmp, sizeof tmp, "%s.tmp", cp);
	if (copy_file(TMPXML, tmp) || rename(tmp, cp)) { unlink(tmp); return; }
	snprintf(tmp, sizeof tmp, "%s.tmp", ip);
	FILE *f = fopen(tmp, "w");
	if (f) { fprintf(f, "etag=%s\nlastmod=%s\n", etag, lm); if (fclose(f) == 0) rename(tmp, ip); else unlink(tmp); }
}

static void start_feed(void)
{
	while (fi < nfeeds) {
		if (sync_only >= 0 && fi != sync_only) { if (fi < sync_only) { fi = sync_only; continue; } fi = nfeeds; break; }
		unlink(TMPXML);
		char etag[256], lm[64];
		feed_validators(fi, etag, sizeof etag, lm, sizeof lm);
		if (!net_download_start_cond(feeds[fi].url, TMPXML, etag, lm)) { phase = P_FEED; dirty = 1; feed_got = 0; feed_scan_off = 0; feed_items = 0; feed_scan_t = feed_t0 = now(); return; }
		lg("podsync: feed start failed %s", feeds[fi].url); errors++; fi++;
	}
	phase = P_COVER; ci = -1;
}

static void start_cover(void)
{
	for (ci = ci + 1; ci < ncovers; ci++) {
		char part[PATHLEN + 8]; snprintf(part, sizeof part, "%s.part", covers[ci].dest);
		unlink(part);
		if (!net_download_start(covers[ci].url, part)) { dirty = 1; return; }
	}
	phase = P_EPIS; qi = -1;
}

static void start_episode(void)
{
	for (qi = qi + 1; qi < nq; qi++) {
		if (q[qi].st != Q_WAIT) continue;
		char part[PATHLEN + 8]; snprintf(part, sizeof part, "%s.part", q[qi].dest);
		if (net_download_start(q[qi].url, part)) { q[qi].st = Q_FAIL; errors++; continue; }
		q[qi].st = Q_RUN; rate_got = -1; rate_t = now(); rate = 0; dirty = 1;	/* -1: take the first reading as the base (resumed .part) */
		return;
	}
	phase = P_END;
}

static void feed_done(void)
{
	FILE *f = fopen(TMPXML, "rb");
	if (!f) { errors++; return; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *xml = n > 0 && n < 4000000 ? malloc(n) : 0;
	if (!xml || fread(xml, 1, n, f) != (size_t)n) { free(xml); fclose(f); errors++; return; }
	fclose(f);
	static struct rss_item items[40];
	int ni = rss_parse(xml, n, items, 40);
	struct feed *fd = &feeds[fi];
	char dir[PATHLEN]; feed_dir(fi, dir, sizeof dir);
	mkdir(dir, 0755);
	char img[1024];
	if (ncovers < 16 && rss_channel_image(xml, n, img, sizeof img)) {
		char cj[PATHLEN]; snprintf(cj, sizeof cj, "%s/cover.jpg", dir);
		if (access(cj, F_OK)) { snprintf(covers[ncovers].url, sizeof covers[0].url, "%s", img); snprintf(covers[ncovers].dest, sizeof covers[0].dest, "%s", cj); ncovers++; }
	}
	free(xml);
	lg("podsync: %s: %d items", fd->name, ni);
	for (int i = 0; i < ni && i < fd->keep && nq < MAXQ; i++) {
		char fn[256]; rss_filename(&items[i], fn, sizeof fn);
		if (have_file(dir, fn)) continue;
		struct qitem *it = &q[nq++];
		memset(it, 0, sizeof *it);
		snprintf(it->feed, sizeof it->feed, "%s", fd->name);
		snprintf(it->title, sizeof it->title, "%s", items[i].title);
		snprintf(it->url, sizeof it->url, "%s", items[i].url);
		char fsn[256]; utf8_to_fs(fn, fsn, sizeof fsn);
		snprintf(it->dest, sizeof it->dest, "%s/%s", dir, fsn);
		it->total = items[i].bytes;
		it->st = Q_WAIT;
		snprintf(it->tr, sizeof it->tr, "%s", items[i].transcript);
		snprintf(it->tre, sizeof it->tre, "%s", items[i].tr_ext);
		snprintf(it->ch, sizeof it->ch, "%s", items[i].chapters);
	}
}

/* episodes the last search found and that are still not on the Walkman (queued, failed, cancelled): kept in a
 * file so the home badge survives a restart; an episode copied by hand stops counting (its file exists) */
#define PENDING "/contents/HB/pendientes.txt"
static void save_pending(void)
{
	FILE *f = fopen(PENDING ".tmp", "w");
	if (!f) return;
	for (int i = 0; i < nq; i++) if (q[i].st != Q_DONE) fprintf(f, "%s\n", q[i].dest);
	if (fclose(f) == 0) rename(PENDING ".tmp", PENDING);
}

int dl_pending(void)
{
	int n = 0;
	if (phase != P_IDLE) {	/* this session's queue is in memory */
		for (int i = 0; i < nq; i++) if (q[i].st != Q_DONE && access(q[i].dest, F_OK)) n++;
		return n;
	}
	FILE *f = fopen(PENDING, "r");
	if (!f) return 0;
	char line[PATHLEN + 8];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && access(line, F_OK)) n++;
	}
	fclose(f);
	return n;
}

static void finish(void)
{
	phase = P_END; dirty = 1; held = held_sidecar = 0; sync_only = -1;
	unlink(TMPXML);
	save_pending();
	pod_net_down();
	if (newcount) snprintf(endmsg, sizeof endmsg, "%d episodio%s nuevo%s", newcount, newcount == 1 ? "" : "s", newcount == 1 ? "" : "s");
	else if (errors) snprintf(endmsg, sizeof endmsg, "Hubo errores");
	else snprintf(endmsg, sizeof endmsg, "No hay episodios nuevos");
	lg("podsync: end, %d new, %d errors", newcount, errors);
}

void dl_tick(void)
{
	if (!dl_active() || held) { if (net_refs > 0) pan_pump(0); return; }
	pan_pump(0);
	if (!pan_link_ok()) {
		lg("podsync: link lost");
		net_download_abort();
		if (phase == P_EPIS && qi >= 0 && qi < nq && q[qi].st == Q_RUN) q[qi].st = Q_FAIL;
		for (int i = 0; i < nq; i++) if (q[i].st == Q_WAIT) q[i].st = Q_FAIL;
		errors++;
		finish();
		snprintf(endmsg, sizeof endmsg, "Se cortó la conexión");
		return;
	}
	long got = 0, total = 0;
	int r = net_download_poll(&got, &total);
	if (phase == P_EPIS && qi >= 0 && qi < nq && q[qi].stage == 0) {
		struct qitem *it = &q[qi];
		if (got != it->got) dirty = 1;
		it->got = got; if (total > 0) it->total = total;
		if (rate_got < 0) { rate_got = got; rate_t = now(); }
		else if (now() - rate_t >= 1) { rate = (got - rate_got) / (now() - rate_t) / 1024; rate_got = got; rate_t = now(); dirty = 1; }
	}
	if (!r && phase == P_FEED) {
		if (got != feed_got) { feed_got = got; dirty = 1; }
		if (got > FEED_MAX) { lg("podsync: %s: feed over %d KB, using what arrived", feeds[fi].name, FEED_MAX >> 10); net_download_abort(); r = 1; }
		/* only the newest `keep` episodes matter: stop as soon as they are complete (Podnews: 2.3 MB -> ~50 KB) */
		if (now() - feed_scan_t > 0.7) {
			feed_scan_t = now();
			if (feed_count_items() >= feeds[fi].keep) {
				lg("podsync: %s: %d items after %ld KB, stopping the feed early", feeds[fi].name, feed_items, got >> 10);
				net_download_abort();
				r = 1;
			}
		}
	}
	if (!r) return;
	if (phase == P_FEED) {
		if (r == 1) { feed_cache(); feed_done(); } else { errors++; lg("podsync: feed failed %s", feeds[fi].url); }
		fi++;
		start_feed();
		if (phase == P_COVER) start_cover();
		if (phase == P_EPIS) start_episode();
	} else if (phase == P_COVER) {
		char part[PATHLEN + 8]; snprintf(part, sizeof part, "%s.part", covers[ci].dest);
		if (r == 1) { rename(part, covers[ci].dest); lg("podsync: artwork %s", covers[ci].dest); }
		else { unlink(part); lg("podsync: artwork failed %s", covers[ci].url); }
		start_cover();
		if (phase == P_EPIS) start_episode();
	} else if (phase == P_EPIS) {
		struct qitem *it = &q[qi];
		if (it->stage > 0) {	/* a sidecar finished (failures are harmless) */
			char sp[PATHLEN + 32];
			sidecar_path(it, it->stage == 1 ? it->tre : ".chapters.json", sp, sizeof sp);
			char part[PATHLEN + 40]; snprintf(part, sizeof part, "%s.part", sp);
			if (r == 1) rename(part, sp); else { unlink(part); lg("podsync: sidecar %d failed", it->stage); }
			if (!start_sidecar(it)) start_episode();
		} else {
			char part[PATHLEN + 8]; snprintf(part, sizeof part, "%s.part", it->dest);
			if (r == 1) { rename(part, it->dest); sync(); it->st = Q_DONE; newcount++; lg("podsync: got %s", it->dest); save_pending(); }
			else { it->st = Q_FAIL; errors++; lg("podsync: failed %s (resumable)", it->dest); }
			dirty = 1;
			if (!(r == 1 && start_sidecar(it))) start_episode();
		}
	}
	if (phase == P_END) finish();
}

void dl_cancel_quiet(void) { no_bt_restart = 1; dl_cancel(); no_bt_restart = 0; }

void dl_cancel(void)
{
	if (!dl_active()) return;
	net_download_abort();
	for (int i = 0; i < nq; i++) if (q[i].st == Q_WAIT || q[i].st == Q_RUN) q[i].st = Q_CANCEL;
	finish();
	snprintf(endmsg, sizeof endmsg, newcount ? "Cancelado (%d nuevo%s)" : "Cancelado", newcount, newcount == 1 ? "" : "s");
}

/* ---------- the phone link (modal connect, shared by the sync and the search screens) ---------- */

static const char *net_title = "Podcasts";
static double last_status, status_t0;
static int spin;
static void pan_status(const char *what)
{
	hb_alive();
	if (eng_playing()) eng_step();
	if (now() - last_status < 0.4) return;
	last_status = now();
	int step = 1;
	if (what[0] >= '1' && what[0] <= '9' && what[1] == '/') { step = what[0] - '0'; what += 4; }	/* "n/4 text" */
	scr_progress(net_title, step, 4, what, (int)(now() - status_t0), spin++,
		step == 2 ? "El teléfono tiene que tener activado el anclaje por Bluetooth" : "");
	flip();
}

/* the phone's address and PIN from telefono.txt; 0 = there is one */
static int read_phone(char *addr, size_t amax, char *pin, size_t pmax, char *name, size_t nmax)
{
	addr[0] = 0; snprintf(pin, pmax, "0000"); if (name && nmax) name[0] = 0;
	FILE *f = fopen(PHONEFILE, "r");
	if (!f) return -1;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if (!line[0] || line[0] == '#') continue;
		if (!strncmp(line, "pin", 3)) { char *e = strchr(line, '='); if (e) { snprintf(pin, pmax, "%s", e + 1); trim(pin); } }
		else if (!strncmp(line, "name", 4)) { char *e = strchr(line, '='); if (e && name) { snprintf(name, nmax, "%s", e + 1); trim(name); } }
		else if (!addr[0]) snprintf(addr, amax, "%s", line);
	}
	fclose(f);
	/* "AA:BB:CC:DD:EE:FF" (how phones show it) -> "AABB:CC:DDEEFF" (NAP:UAP:LAP, how the stack wants it) */
	unsigned m[6];
	if (sscanf(addr, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6 && strlen(addr) == 17)
		snprintf(addr, amax, "%02X%02X:%02X:%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
	return addr[0] ? 0 : -1;
}

const char *pod_phone_name(void)
{
	static char addr[64], pin[16], name[64];
	if (read_phone(addr, sizeof addr, pin, sizeof pin, name, sizeof name)) return 0;
	return name[0] ? name : addr;
}

/* the menu's "Teléfono para internet": a Bluetooth search (bt.c), the choice saved in telefono.txt (the PIN line
 * is kept). The pairing itself happens on the first connection (pan_up asks the phone with the PIN). 0 = saved */
int pod_choose_phone(void)
{
	if (dl_active() || net_refs) { message("Teléfono", "Ocupado con los podcasts.", "Esperá a que terminen las descargas."); msg_wait(3); return -1; }
	char addr[64], name[64], oaddr[64], pin[16];
	read_phone(oaddr, sizeof oaddr, pin, sizeof pin, 0, 0);
	if (bt_pick_device("Elegí el teléfono", "En el teléfono: Bluetooth visible", addr, sizeof addr, name, sizeof name)) return -1;
	FILE *f = fopen(PHONEFILE ".tmp", "w");
	if (!f) return -1;
	fprintf(f, "# Teléfono que comparte internet por Bluetooth (anclaje): dirección, nombre y PIN para emparejar\n%s\nname=%s\npin=%s\n", addr, name, pin);
	if (fclose(f) || rename(PHONEFILE ".tmp", PHONEFILE)) { unlink(PHONEFILE ".tmp"); return -1; }
	sync();
	lg("podcasts: phone %s (%s)", addr, name);
	message("Teléfono", name[0] ? name : addr, "Activá el anclaje por Bluetooth en el teléfono"); msg_wait(3);
	return 0;
}

/* makes sure the phone link is up (connecting with a progress screen titled title if needed) and counts one more
 * user of it; every 0 return must be matched by pod_net_down. -1 = no link (the user was told why) */
int pod_net_up(const char *title)
{
	if (net_refs > 0 && pan_link_ok()) { net_refs++; return 0; }
	if (net_refs > 0) { lg("podcasts: link gone, connecting again"); pan_down(); }	/* the others keep their count */
	char addr[64], pin[16];
	if (read_phone(addr, sizeof addr, pin, sizeof pin, 0, 0)) {
		message(title, "Primero elegí el teléfono", "que comparte internet por Bluetooth"); msg_wait(3);
		if (pod_choose_phone() || read_phone(addr, sizeof addr, pin, sizeof pin, 0, 0)) return -1;
	}
	if (!net_refs) { had_bt = bt_is_on(); if (had_bt) bt_shutdown(); }
	net_title = title;
	{	/* without the CA bundle HTTPS still works, unverified: say so once */
		static int told;
		if (!told && access("/contents/HB/cacert.pem", R_OK)) { told = 1; message(title, "Sin cacert.pem: HTTPS sin verificar certificados", ""); msg_wait(3); }
	}
	last_status = 0; status_t0 = now();
	int r = pan_up(addr, pin, pan_status);
	if (r) {
		static const char *why[] = { "", "Dirección inválida", "No se pudo abrir el Bluetooth", "El teléfono no responde", "El teléfono rechazó la conexión", "No hubo dirección IP", "El teléfono cortó la conexión" };
		message(title, why[-r < 7 ? -r : 0], r == -6 ? "Activá el anclaje por Bluetooth en el teléfono" : "¿Está activo el anclaje Bluetooth?");
		lg("podcasts: pan_up failed %d", r);
		wait_ui(4);
		if (!net_refs) { if (had_bt && !no_bt_restart) bt_auto_start(); had_bt = 0; }
		return -1;
	}
	net_refs++;
	return 0;
}

int pod_net_busy(void) { return net_refs > 0; }

void pod_net_down(void)
{
	if (net_refs > 0) net_refs--;
	if (net_refs) return;
	pan_down();
	if (had_bt && !no_bt_restart) bt_auto_start();
	had_bt = 0;
}

/* ---------- foreground transfers (the search screens) ---------- */

/* the background queue lets the link go for a foreground transfer: the running transfer stops (an episode resumes
 * from its .part later) and dl_tick waits until dl_hold(0) starts it again */
static void dl_hold(int on)
{
	if (on) {
		if (held++ || !dl_active()) return;
		net_download_abort();
		if (phase == P_EPIS && qi >= 0 && qi < nq) {
			if (q[qi].stage > 0) { q[qi].stage--; held_sidecar = 1; }	/* the sidecar is fetched again */
			else { if (q[qi].st == Q_RUN) q[qi].st = Q_WAIT; qi--; }
		} else if (phase == P_COVER) ci--;
		lg("podsync: queue paused for a foreground transfer");
		return;
	}
	if (!held || --held || !dl_active()) return;
	lg("podsync: queue goes on");
	if (phase == P_FEED) start_feed();
	if (phase == P_COVER) start_cover();
	if (phase == P_EPIS) {
		if (held_sidecar) { held_sidecar = 0; if (!start_sidecar(&q[qi])) start_episode(); }
		else start_episode();
	}
	if (phase == P_END) finish();
}

/* downloads url to dest (from scratch) over the link (pod_net_up first), with the queue paused meanwhile. cb(got,
 * total) is called while it runs: 0 go on, 1 cancel, 2 enough (stop, what arrived counts as done). 0 ok, -1 failed,
 * -2 cancelled */
int pod_fetch(const char *url, const char *dest, int (*cb)(long got, long total))
{
	if (!pan_link_ok()) return -1;
	dl_hold(1);
	unlink(dest);
	char meta[PATHLEN + 8]; snprintf(meta, sizeof meta, "%s.meta", dest); unlink(meta);
	int r = net_download_start(url, dest) ? -1 : 0;
	while (!r) {
		pan_pump(10);
		long got = 0, total = 0;
		int d = net_download_poll(&got, &total);
		if (d) { r = d == 1 ? 1 : -1; break; }
		if (!pan_link_ok()) { net_download_abort(); r = -1; break; }
		int c = cb ? cb(got, total) : 0;
		if (c == 1) { net_download_abort(); r = -2; break; }
		if (c == 2) { net_download_abort(); r = 1; break; }
		ui_tick();
	}
	unlink(meta);
	dl_hold(0);
	if (r == -1) lg("podcasts: fetch failed %s", url);
	return r == 1 ? 0 : r;
}

/* complete <item>s in a feed file being downloaded (the same incremental scan as the sync's early stop) */
int pod_count_items(const char *path, long *scan_off, int *items)
{
	FILE *f = fopen(path, "rb");
	if (!f) return *items;
	long old = *scan_off, from = old > 6 ? old - 6 : 0;
	fseek(f, from, SEEK_SET);
	static char buf[8192];
	long base = from;
	size_t keep = 0, n;
	while ((n = fread(buf + keep, 1, sizeof buf - keep, f)) > 0) {
		size_t len = keep + n;
		for (size_t i = 0; i + 7 <= len; i++)
			if (buf[i] == '<' && buf[i + 1] == '/' && (buf[i + 2] | 32) == 'i' && (buf[i + 3] | 32) == 't' &&
			    (buf[i + 4] | 32) == 'e' && (buf[i + 5] | 32) == 'm' && buf[i + 6] == '>' && base + (long)i + 7 > old)
				(*items)++;
		keep = len < 6 ? len : 6;
		memmove(buf, buf + len - keep, keep);
		base += (long)(len - keep);
	}
	*scan_off = base + (long)keep;
	fclose(f);
	return *items;
}

/* ---------- starting the background work ---------- */

static void new_session(void)
{
	nq = 0; fi = 0; qi = -1; ncovers = 0; ci = -1; newcount = errors = 0; endmsg[0] = 0; held = held_sidecar = 0;
}

/* starts a sync of every show (only = -1) or of one; returns 1 if the background download is running */
static int dl_start(int only)
{
	if (!load_feeds()) { message("Podcasts", "Todavía no seguís ningún podcast.", "Agregá uno con \"Agregar podcast\"."); msg_wait(3); return 0; }
	if (only >= nfeeds) return 0;
	if (pod_net_up("Buscar episodios")) return 0;
	new_session();
	sync_only = only;
	if (only >= 0) fi = only;
	start_feed();
	if (phase == P_COVER) start_cover();
	if (phase == P_EPIS) start_episode();
	if (phase == P_END) finish();
	return dl_active();
}

/* one episode of a subscribed show, chosen from its feed (podui.c): queued after what is running, or a new
 * background session. 0 queued, 1 already there, -1 failed */
int dl_add_episode(int fidx, const struct rss_item *item)
{
	load_feeds();
	if (fidx < 0 || fidx >= nfeeds) return -1;
	char dir[PATHLEN]; feed_dir(fidx, dir, sizeof dir);
	mkdir(dir, 0755);
	char fn[256], fsn[256], dest[PATHLEN];
	rss_filename(item, fn, sizeof fn);
	utf8_to_fs(fn, fsn, sizeof fsn);
	snprintf(dest, sizeof dest, "%s/%s", dir, fsn);
	if (!access(dest, F_OK)) return 1;
	if (dl_active()) for (int i = 0; i < nq; i++) if (!strcmp(q[i].dest, dest) && q[i].st != Q_FAIL && q[i].st != Q_CANCEL) return 1;
	int fresh = !dl_active();
	if (fresh) { if (pod_net_up("Bajar episodio")) return -1; new_session(); }
	if (nq >= MAXQ) { if (fresh) pod_net_down(); return -1; }
	struct qitem *it = &q[nq++];
	memset(it, 0, sizeof *it);
	snprintf(it->feed, sizeof it->feed, "%s", feeds[fidx].name);
	snprintf(it->title, sizeof it->title, "%s", item->title);
	snprintf(it->url, sizeof it->url, "%s", item->url);
	snprintf(it->dest, sizeof it->dest, "%s", dest);
	it->total = item->bytes;
	it->st = Q_WAIT;
	snprintf(it->tr, sizeof it->tr, "%s", item->transcript);
	snprintf(it->tre, sizeof it->tre, "%s", item->tr_ext);
	snprintf(it->ch, sizeof it->ch, "%s", item->chapters);
	lg("podcasts: queued %s", dest);
	if (fresh) {
		phase = P_EPIS; qi = -1;
		if (!held) start_episode();
		if (phase == P_END) finish();
	}
	dirty = 1;
	return 0;
}

/* ---------- UI ---------- */

/* overall progress of the running download for the status bar: 0..100, -1 while reading feeds */
int dl_percent(void)
{
	if (phase == P_EPIS && qi >= 0 && qi < nq && q[qi].total > 0) return (int)((long long)q[qi].got * 100 / q[qi].total);
	return -1;
}

/* the value shown next to "Buscar episodios nuevos" in the podcast list */
const char *dl_row_value(void)
{
	static char s[40];
	if (phase == P_FEED) { if (sync_only >= 0) snprintf(s, sizeof s, "leyendo"); else snprintf(s, sizeof s, "leyendo %d/%d", fi + 1, nfeeds); }
	else if (phase == P_COVER) snprintf(s, sizeof s, "imágenes");
	else if (phase == P_EPIS) {
		int p = dl_percent();
		if (p >= 0) snprintf(s, sizeof s, "%d/%d · %d%%", qi + 1, nq, p); else snprintf(s, sizeof s, "%d/%d", qi + 1, nq);
	} else if (phase == P_END && endmsg[0]) snprintf(s, sizeof s, "%s", newcount ? "listo" : "");
	else s[0] = 0;
	return s;
}

/* the show's cover.jpg (next to its episodes) at 40 px for the downloads list, decoded once per show; a show
 * whose cover isn't there yet (downloaded later in the same run) is tried again next time */
#define FC_MAX 16
static uint16_t fc_px[FC_MAX][40 * 40];
static char fc_name[FC_MAX][64];
static signed char fc_ok[FC_MAX];
static int nfc;
static struct img feed_cover(const struct qitem *it)
{
	struct img im = { 0, 0, 0 };
	int k;
	for (k = 0; k < nfc; k++) if (!strcmp(fc_name[k], it->feed)) break;
	if (k == nfc) {
		char cj[PATHLEN + 16]; snprintf(cj, sizeof cj, "%s", it->dest);
		char *sl = strrchr(cj, '/');
		if (!sl || nfc == FC_MAX) return im;
		snprintf(sl, sizeof cj - (sl - cj), "/cover.jpg");
		if (access(cj, F_OK)) return im;
		snprintf(fc_name[k], sizeof fc_name[k], "%s", it->feed);
		fc_ok[k] = cover_load(cj, fc_px[k], 40) ? 1 : -1;
		nfc++;
	}
	if (fc_ok[k] == 1) { im.w = im.h = 40; im.px = fc_px[k]; }
	return im;
}

static int draw(int top)
{
	char h1[96];
	char sub[64] = "";
	static int spin;
	static const char *sp[4] = { "", ".", "..", "..." };
	if (phase == P_FEED) {
		if (sync_only >= 0) snprintf(h1, sizeof h1, "Leyendo %s", feeds[fi].name);
		else snprintf(h1, sizeof h1, "Leyendo %s (%d/%d)", feeds[fi].name, fi + 1, nfeeds);
		double el = now() - feed_t0;
		snprintf(sub, sizeof sub, "%ld KB · %.0f KB/s · %d episodios%s", feed_got >> 10, el > 0.5 ? feed_got / 1024.0 / el : 0.0, feed_items, sp[spin++ & 3]);
	}
	else if (phase == P_COVER) snprintf(h1, sizeof h1, "Bajando imágenes (%d/%d)", ci + 1, ncovers);
	else if (phase == P_EPIS) {
		snprintf(h1, sizeof h1, "Bajando %d de %d", qi + 1, nq);
		long left = 0;	/* bytes still to fetch in the whole queue -> time left at the current speed */
		for (int i = qi; i < nq; i++) if (q[i].st <= 1 && q[i].total > 0) left += q[i].total - q[i].got;
		if (rate > 1 && left > 0) {
			int m = (int)(left / 1024 / rate / 60 + 1);
			if (m >= 60) snprintf(sub, sizeof sub, "%.0f kB/s · %dh%02d", rate, m / 60, m % 60);
			else snprintf(sub, sizeof sub, "%.0f kB/s · %d min", rate, m);
		} else if (rate > 1) snprintf(sub, sizeof sub, "%.0f kB/s", rate);
	}
	else snprintf(h1, sizeof h1, "%s", endmsg[0] ? endmsg : "Sin descargas");
	static struct dl_row rows[MAXQ];
	for (int i = 0; i < nq; i++) { rows[i].title = q[i].title; rows[i].feed = q[i].feed; rows[i].got = q[i].got; rows[i].total = q[i].total; rows[i].st = q[i].st; rows[i].cover = feed_cover(&q[i]); }
	return scr_downloads(h1, sub, dl_active(), phase == P_FEED, rows, nq, top, rate);
}

/* the downloads screen; downloads keep running after BACK */
void dl_screen(void)
{
	int top = 0, redraw = 1;
	double last = 0, user_t = -100;
	for (;;) {
		if (now() - last > 0.5 && dl_take_dirty()) redraw = 1;
		/* the one downloading is the card on top; the list below starts at what comes next unless the user scrolled */
		if (now() - user_t > 8 && phase == P_EPIS && top != 0) { top = 0; redraw = 1; }
		if (redraw && backlight_on) { top = draw(top); flip(); redraw = 0; last = now(); }
		idle_tick(0);	/* downloads keep the unit awake by themselves (dl_active); finished ones must not */
		if (eng_playing()) eng_step();
		int k = getkey(eng_playing() ? 20 : 200);
		if (k == K_REDRAW) { redraw = 1; continue; }
		if (k < 0) continue;
		redraw = 1;
		if (k == K_BACK) return;
		if (k == K_DOWN && top < nq - DL_ROWS) { top++; user_t = now(); }
		else if (k == K_UP && top > 0) { top--; user_t = now(); }
		else if (k == K_OPTION && dl_active()) dl_cancel();
	}
}

/* "Buscar episodios nuevos" row (only = -1) or a show's "Buscar episodios nuevos" (its index): start (or show)
 * the downloads */
void podsync_update(int only)
{
	if (!dl_active() && !dl_start(only) && phase != P_END) return;
	dl_screen();
}
