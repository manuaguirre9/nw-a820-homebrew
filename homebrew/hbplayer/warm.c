/* warm.c: the first visit of a screen used to wait for the flash. Sony's textures and home animation are mapped from
 * the firmware (each page is read on its first touch), the player's own code runs from the FAT partition, and the
 * music folders and the cover cache are cold after a boot. A child process at the lowest priority reads all of that
 * once, right after the home screen shows: the page cache and the directory cache are shared, so the player then
 * finds it in RAM (a minor fault instead of a flash read). The child also moves the cover cache's flat files from
 * before 2026-10-14 into the per-byte subfolders cover.c uses now (they were orphaned: every cover decoded again),
 * and reads the tags of new music files into the library cache. Once the player has loaded that cache, a second
 * child makes every album's cover at every size (warm_covers_start): the first visit of Música and of the album
 * lists then finds everything ready.
 * It never logs (the player's stdio buffers would be flushed twice): a summary goes to /tmp/hb_warm.txt. */
#include "hb.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CACHE "/contents/HB/cache"
#define THUMB_MAX (GRID_TH * GRID_TH * 2)	/* cache files up to the mosaic size are list thumbnails: read them */

static pid_t warm_pid = -1, covers_pid = -1;
static double warm_t0, covers_t0;
static volatile unsigned sink;
static long touched, nfiles, ndirs, moved;

static void touch(const void *p, long n)
{
	const volatile unsigned char *c = p;
	for (long i = 0; i < n; i += 4096) sink += c[i];
	if (n > 0) sink += c[n - 1];
	touched += n;
}

static long read_all(const char *path, long max)
{
	static char buf[16384];
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	long got = 0; ssize_t r;
	while (got < max && (r = read(fd, buf, sizeof buf)) > 0) got += r;
	close(fd);
	return got;
}

/* stat every folder and file under Music: the library scan then finds the directory entries in RAM */
static void walk(const char *dir, int depth)
{
	if (depth > 4) return;
	DIR *d = opendir(dir);
	if (!d) return;
	ndirs++;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[PATHLEN]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st)) continue;
		nfiles++;
		if (S_ISDIR(st.st_mode)) walk(p, depth + 1);
	}
	closedir(d);
}

static int is_hex(const char *s, int n) { for (int i = 0; i < n; i++) if (!strchr("0123456789abcdef", s[i])) return 0; return 1; }

/* the cover cache: old flat files ("0779d653_112.rgb" in cache/) move to cache/53/, then every small file of every
 * subfolder (list thumbnails, markers) is read */
static void cache_warm(void)
{
	static char flat[512][24];
	int nflat = 0;
	DIR *d = opendir(CACHE);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)))	/* names first: renaming while reading the folder could skip entries */
		if (strlen(e->d_name) >= 12 && strlen(e->d_name) < sizeof flat[0] && e->d_name[8] == '_' && is_hex(e->d_name, 8) && nflat < 512) snprintf(flat[nflat++], sizeof flat[0], "%s", e->d_name);
	closedir(d);
	for (int i = 0; i < nflat; i++) {
		char from[64], dir[48], to[80];
		snprintf(from, sizeof from, CACHE "/%s", flat[i]);
		snprintf(dir, sizeof dir, CACHE "/%.2s", flat[i] + 6);
		snprintf(to, sizeof to, "%s/%s", dir, flat[i]);
		mkdir(dir, 0755);
		if (access(to, F_OK) == 0 || rename(from, to)) unlink(from);	/* a newer copy is already there */
		else moved++;
	}
	if (nflat) sync();
	read_all(CACHE "/library.bin", 4L << 20);
	d = opendir(CACHE);
	if (!d) return;
	while ((e = readdir(d))) {
		if (strlen(e->d_name) != 2 || !is_hex(e->d_name, 2)) continue;
		char sub[48]; snprintf(sub, sizeof sub, CACHE "/%s", e->d_name);
		DIR *s = opendir(sub);
		struct dirent *f;
		while (s && (f = readdir(s))) {
			if (f->d_name[0] == '.') continue;
			char p[96]; snprintf(p, sizeof p, "%s/%s", sub, f->d_name);
			struct stat st;
			if (!stat(p, &st) && st.st_size > 0 && st.st_size <= THUMB_MAX) read_all(p, THUMB_MAX);
		}
		if (s) closedir(s);
	}
	closedir(d);
}

/* a child that may run for minutes must not keep the player's devices open: the player's close of the watchdog,
 * the sound device or the Bluetooth link would not release them while a copy of the descriptor lives here */
static void child_setup(void)
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
	for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) signal(sigs[i], SIG_DFL);	/* not the player's crash handler */
	setpriority(PRIO_PROCESS, 0, 19);
	for (int fd = 3; fd < 256; fd++) close(fd);
	cover_tick = 0; cover_cancel = 0;	/* the player's audio and keys are not ours */
}

static void child(void)
{
	child_setup();
	double t0 = now();
	gfx_warm_res(touch);	/* the home animation first: it is what the first key press shows */
	scr_warm(touch);
	double t1 = now();
	long exe = read_all("/proc/self/exe", 8L << 20);	/* our code, from the FAT partition */
	exe += read_all(HB_BIN "/dspdec", 4L << 20);	/* the DSP helper: started at the first track */
	double t2 = now();
	walk("/contents/Music", 0);
	double t3 = now();
	cache_warm();
	double t4 = now();
	int songs = music_build_cache();
	double t5 = now();
	FILE *f = fopen("/tmp/hb_warm.txt", "w");
	if (f) {
		fprintf(f, "res %ld KB %.1f s, code %ld KB %.1f s, music %ld entries %.1f s, covers %.1f s (%ld moved), library %d songs %.1f s",
			touched >> 10, t1 - t0, exe >> 10, t2 - t1, nfiles, t3 - t2, t4 - t3, moved, songs, t5 - t4);
		fclose(f);
	}
	_exit(0);
}

/* after music_preload: the library is in this process, the child gets a copy of it */
void warm_covers_start(void)
{
	if (covers_pid > 0) return;
	covers_t0 = now();
	covers_pid = fork();
	if (covers_pid == 0) {
		child_setup();
		int decoded = 0, inherited = 0, albums = music_album_covers(&decoded, &inherited);
		FILE *f = fopen("/tmp/hb_covers.txt", "w");
		if (f) { fprintf(f, "%d albums, %d songs needed work (library: %d songs %s)", albums, decoded, inherited, inherited ? "inherited" : "read from the cache"); fclose(f); }
		_exit(0);
	}
	if (covers_pid < 0) lg("warm: covers fork failed");
}

void warm_start(void)
{
	if (warm_pid > 0) return;
	warm_t0 = now();
	warm_pid = fork();
	if (warm_pid == 0) child();
	if (warm_pid < 0) lg("warm: fork failed");
}

/* from idle_tick: 1 when the first child has just finished, 2 when it also checked the library cache to the end */
int warm_poll(void)
{
	if (covers_pid > 0 && waitpid(covers_pid, 0, WNOHANG) == covers_pid) {
		covers_pid = -1;
		char b[120] = "";
		FILE *f = fopen("/tmp/hb_covers.txt", "r");
		if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); unlink("/tmp/hb_covers.txt"); }
		lg("warm: album covers done in %.1f s: %s", now() - covers_t0, b);
	}
	if (warm_pid <= 0 || waitpid(warm_pid, 0, WNOHANG) != warm_pid) return 0;
	warm_pid = -1;
	char b[300] = "";
	FILE *f = fopen("/tmp/hb_warm.txt", "r");
	if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); unlink("/tmp/hb_warm.txt"); }
	lg("warm: done in %.1f s: %s", now() - warm_t0, b);
	return strstr(b, ", library ") ? 2 : 1;
}

/* before the player lets /contents go (USB, Sony UI): the child keeps files open there */
void warm_stop(void)
{
	if (covers_pid > 0) { kill(covers_pid, SIGKILL); waitpid(covers_pid, 0, 0); covers_pid = -1; }
	if (warm_pid <= 0) return;
	kill(warm_pid, SIGKILL);
	waitpid(warm_pid, 0, 0);
	warm_pid = -1;
}
