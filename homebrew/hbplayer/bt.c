/* bt.c: Bluetooth A2DP output for the homebrew player.
 * The CSR stack lives in the kernel (hci_bchs.ko); we drive Sony's factory tool a2d_demo_app through a FIFO
 * (keystrokes) and parse its log. Sequence that works (see firmware_work/hb/boot_bt11.sh):
 *   bt_test 3; bc_attach -b 2592000 -d /dev/tts/0 &; a2d "1" (source role); "0" search; "C" stop;
 *   "<index>" select; "1" pair + passkey 0000; "2" connect IMMEDIATELY after "Bonding completed!!".
 * a2d menus: idle 0 search / 1 pair / 2 connect; connected 0 disconnect; ' ' reprints; ESC quits.
 * Link keys persist in STATEDIR/bt/sc_db.db (a2d's cwd). The last device is remembered in STATEDIR/bt.txt. */
#include "hb.h"
#include "../net/pan.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG "/tmp/a2d.log"
#define FIFO "/tmp/a2in"

static int on, connected, dropped, fifo = -1;
static pid_t pid_bca = -1, pid_a2d = -1;
static long logpos;	/* how far bt_poll has parsed */
static char dev_addr[32], dev_name[64];
static int pending_key = -1;

int bt_is_on(void) { return on; }
int bt_connected(void) { return connected; }
const char *bt_device_name(void) { return dev_name[0] ? dev_name : dev_addr; }
int bt_take_dropped(void) { int d = dropped; dropped = 0; return d; }
void bt_link_lost(void) { if (connected) { connected = 0; dropped = 1; lg("bt: link lost"); } }

static void load_dev(void)
{
	char fn[PATHLEN + 16]; snprintf(fn, sizeof fn, "%s/bt.txt", statedir);
	FILE *f = fopen(fn, "r");
	dev_addr[0] = dev_name[0] = 0;
	if (!f) return;
	char line[128];
	if (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *sp = strchr(line, ' ');
		if (sp) { *sp = 0; snprintf(dev_name, sizeof dev_name, "%s", sp + 1); }
		snprintf(dev_addr, sizeof dev_addr, "%s", line);
	}
	fclose(f);
}

static void save_dev(const char *addr, const char *name)
{
	char fn[PATHLEN + 16], tmp[PATHLEN + 24]; snprintf(fn, sizeof fn, "%s/bt.txt", statedir);
	FILE *f = safe_open(fn, tmp, sizeof tmp);
	if (f) { fprintf(f, "%s %s\n", addr, name); safe_close(f, tmp, fn); }
	snprintf(dev_addr, sizeof dev_addr, "%s", addr);
	snprintf(dev_name, sizeof dev_name, "%s", name);
}

static void send(const char *s)
{
	if (fifo >= 0) { if (write(fifo, s, strlen(s)) < 0) lg("bt: fifo write failed"); }
	lg("bt: send '%s'", s[0] == 27 ? "ESC" : s);
}

static int read_log(long from, char *buf, int max)
{
	int fd = open(LOG, O_RDONLY);
	if (fd < 0) { buf[0] = 0; return 0; }
	lseek(fd, from, SEEK_SET);
	int n = read(fd, buf, max - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;
	return n;
}

static long log_size(void) { struct stat st; return stat(LOG, &st) ? 0 : st.st_size; }

static int helper_alive(void)
{
	if (pid_a2d <= 0) return 0;
	if (waitpid(pid_a2d, 0, WNOHANG) == pid_a2d) { pid_a2d = -1; return 0; }
	return 1;
}

/* waits for any of the patterns (NULL-terminated) after offset `from`; index, or -1 on timeout / BACK / helper died */
static int wait_for(long from, int secs, const char **pats)
{
	static char buf[32768];
	double end = now() + secs;
	while (now() < end) {
		read_log(from, buf, sizeof buf);
		for (int i = 0; pats[i]; i++) if (strstr(buf, pats[i])) return i;
		if (!helper_alive()) { lg("bt: helper died"); return -1; }
		ui_tick();
		if (getkey(250) == K_BACK) return -1;
	}
	return -1;
}

static pid_t spawn(const char *dir, int in, const char *out, char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		if (dir && chdir(dir)) _exit(126);
		if (in >= 0) dup2(in, 0); else { int n = open("/dev/null", O_RDONLY); dup2(n, 0); }
		int o = open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
		dup2(o, 1); dup2(o, 2);
		execv(argv[0], argv);
		_exit(127);
	}
	return p;
}

static void reap(pid_t *p)
{
	if (*p <= 0) return;
	kill(*p, SIGTERM);
	for (int i = 0; i < 20; i++) { if (waitpid(*p, 0, WNOHANG) == *p) { *p = -1; return; } hb_alive(); usleep(100000); }
	kill(*p, SIGKILL);
	waitpid(*p, 0, 0);
	*p = -1;
}

/* ---- background start-up / auto-connect state machine (driven by bt_poll) ----
 * /tmp/a2d_hb is a patched copy of a2d_demo_app (homebrew/bt/patch_a2d.py) that starts with the "-a" device
 * already selected, so "2" connects straight away with the link key kept in sc_db.db: no search, no PIN. */
enum { ST_OFF, ST_ATTACH, ST_APP, ST_ROLE, ST_READY, ST_CONNECTING };
static int state, auto_tries, user_off, in_menu;
#define AUTO_TRIES 3	/* reconnect attempts after start / resume, 20 s apart (was 10 x 30 s) */

/* start the radio by itself (boot, resume, after podcasts) only if a device was connected the last time:
 * <statedir>/bt_auto holds 1 after a connection, 0 once the user disconnects / switches off or the tries run out */
static int auto_wanted(void)
{
	char fn[PATHLEN + 16]; snprintf(fn, sizeof fn, "%s/bt_auto", statedir);
	FILE *f = fopen(fn, "r");
	if (!f) return 1;	/* never decided (first run with this version): as before */
	int v = fgetc(f) == '1';
	fclose(f);
	return v;
}
static void set_auto(int v)
{
	static int last = -1;
	if (v == last) return;
	char fn[PATHLEN + 16], tmp[PATHLEN + 24]; snprintf(fn, sizeof fn, "%s/bt_auto", statedir);
	FILE *f = safe_open(fn, tmp, sizeof tmp);
	if (f) { fputc(v ? '1' : '0', f); safe_close(f, tmp, fn); last = v; lg("bt: auto start %s", v ? "on" : "off"); }
}
static double st_t, last_try;

static void start_async(void)
{
	load_dev();
	unlink(LOG); unlink(FIFO);
	system("/usr/local/test/bt/bt_test 3 > /dev/null 2>&1");
	char *bca[] = { "/usr/local/test/bt/bc_attach", "-b", "2592000", "-d", "/dev/tts/0", 0 };
	unlink("/tmp/bc_attach.log");	/* spawn appends: a fresh log per start, or it grows in RAM with every start */
	pid_bca = spawn(0, -1, "/tmp/bc_attach.log", bca);
	on = 1; connected = 0; logpos = 0; auto_tries = 0; last_try = 0;
	state = ST_ATTACH; st_t = now();
	lg("bt: starting");
}

static void spawn_app(void)
{
	mkfifo(FIFO, 0600);
	fifo = open(FIFO, O_RDWR);
	set_cloexec(fifo);
	char btdir[PATHLEN + 8]; snprintf(btdir, sizeof btdir, "%s/bt", statedir);
	mkdir(btdir, 0755);
	int rd = open(FIFO, O_RDONLY | O_NONBLOCK);
	if (rd >= 0) { int fl = fcntl(rd, F_GETFL); fcntl(rd, F_SETFL, fl & ~O_NONBLOCK); }
	int patched = access(HB_BIN "/a2d_hb", X_OK) == 0;
	char *a2d[] = { patched ? HB_BIN "/a2d_hb" : "/usr/local/test/bt/a2d_demo_app", dev_addr[0] ? "-a" : 0, dev_addr, 0 };
	pid_a2d = spawn(btdir, rd, LOG, a2d);
	if (rd >= 0) close(rd);
	state = ST_APP; st_t = now();
	lg("bt: helper %s %s", patched ? "patched" : "stock", dev_addr);
}

static void sm_tick(void)
{
	static char buf[4096];
	switch (state) {
	case ST_ATTACH: if (now() - st_t >= 3) spawn_app(); break;
	case ST_APP: if (now() - st_t >= 2) { send("1"); state = ST_ROLE; st_t = now(); } break;
	case ST_ROLE: {
		long sz = log_size();
		read_log(sz > 3000 ? sz - 3000 : 0, buf, sizeof buf);
		static int reloads;	/* after the PAN link (podcasts) the stack often needs one reload: then start again, once */
		if (strstr(buf, "AVRCP has been activated")) { state = ST_READY; logpos = log_size(); reloads = 0; lg("bt: ready"); }
		else if (now() - st_t > 15) {
			lg("bt: stack did not start, reloading it");
			bt_shutdown(); bt_reload_stack();
			if (reloads++ < 1) { lg("bt: starting again after the reload"); start_async(); }
			else reloads = 0;
		}
		break; }
	case ST_READY:	/* auto-connect the remembered device: right away, then every 20 s, AUTO_TRIES tries; then give up */
		if (!connected && !in_menu && dev_addr[0] && access(HB_BIN "/a2d_hb", X_OK) == 0 && auto_tries < AUTO_TRIES && now() - last_try >= (auto_tries ? 20 : 0)) {
			send("2"); state = ST_CONNECTING; st_t = last_try = now(); auto_tries++;
		} else if (!connected && !in_menu && auto_tries >= AUTO_TRIES && now() - last_try > 20) {
			lg("bt: no device, radio off to save battery");	/* the Bluetooth menu / button bring it back */
			if (auto_tries < 100) set_auto(0);	/* not around: don't try by itself next time either */
			bt_shutdown();
		}
		break;
	case ST_CONNECTING: if (now() - st_t > 35) state = ST_READY; break;
	}
}

void bt_auto_start(void)
{
	if (dl_active()) return;	/* the radio is busy with the podcast downloads */
	load_dev();
	if (!on && !user_off && dev_addr[0] && auto_wanted()) start_async();
}

/* blocking: make sure the stack is up (for the Bluetooth screens) */
static int bt_on(void)
{
	if (!on) { user_off = 0; start_async(); }
	message("Bluetooth", "Encendiendo...", "");
	double end = now() + 25;
	while (on && state < ST_READY && now() < end) { bt_poll(); ui_tick(); if (getkey(200) == K_BACK) break; }
	return on && state >= ST_READY ? 0 : -1;
}

void bt_shutdown(void)
{
	static int busy;
	if (!on || busy) return;
	busy = 1;
	state = ST_OFF;	/* no more state-machine work (bt_poll must not re-enter us) */
	bt_poll();
	if (connected && helper_alive()) { send("0"); sleep(2); }
	if (helper_alive()) { send(" "); usleep(300000); send("\x1b"); sleep(1); }
	reap(&pid_a2d);
	reap(&pid_bca);
	if (fifo >= 0) close(fifo);
	fifo = -1;
	system("/usr/local/test/bt/bt_test 4 > /dev/null 2>&1");
	unlink(FIFO);
	unlink(LOG);
	on = connected = dropped = 0;
	aout_stop();
	lg("bt: off");
	busy = 0;
}

void bt_poll(void)
{
	if (!on) return;
	if (state != ST_OFF) sm_tick();
	if (!on || state < ST_ROLE) return;
	if (!helper_alive()) { lg("bt: helper gone"); bt_shutdown(); return; }
	static char buf[8192];
	if (log_size() <= logpos) return;
	int got = read_log(logpos, buf, sizeof buf);
	char *end = strrchr(buf, '\n');
	if (!end) { if (got >= (int)sizeof buf - 1) logpos += got; return; }	/* a full buffer without a newline: skip it, or bt_poll stalls here */
	*end = 0;
	logpos += (end - buf) + 1;
	for (char *l = strtok(buf, "\n"); l; l = strtok(0, "\n")) {
		if (strstr(l, "AV connect confirm - success") || strstr(l, "AV connect indication")) { connected = 1; auto_tries = 0; if (state == ST_CONNECTING) state = ST_READY; lg("bt: connected"); set_auto(1); }
		else if (strstr(l, "AV connect confirm - error")) { if (state == ST_CONNECTING) state = ST_READY; lg("bt: connect failed"); }
		else if (strstr(l, "AV disconnect") || strstr(l, "Disonnecting")) {
			if (connected) dropped = 1;
			connected = 0; aout_stop(); lg("bt: disconnected");
		}
		else if (strstr(l, "AVRCP command received")) {
			lg("bt: avrcp %s", l);
			if (strstr(l, "lay") || strstr(l, "ause") || strstr(l, "top")) pending_key = K_PLAY;
			else if (strstr(l, "orward")) pending_key = K_RIGHT;
			else if (strstr(l, "ackward")) pending_key = K_LEFT;
		}
	}
}

int bt_avrcp_key(void) { int k = pending_key; pending_key = -1; return k; }

/* ---- search / pair / connect ---- */

struct found { int idx; char name[64]; char addr[32]; };
static struct found fl[10];
static int nfound;

static void copy_field(char *dst, int max, const char *src)
{
	int k = 0;
	while (src[k] && src[k] != '\n' && src[k] != '\r' && src[k] != '\t' && k < max - 1) { dst[k] = src[k]; k++; }
	while (k > 0 && dst[k - 1] == ' ') k--;
	dst[k] = 0;
}

static void parse_found(long from)
{
	static char buf[32768];
	read_log(from, buf, sizeof buf);
	nfound = 0;
	char *s = buf;
	while ((s = strstr(s, ")\tName:"))) {
		char *ls = s; while (ls > buf && ls[-1] != '\n' && ls[-1] != '\t') ls--;
		int idx = atoi(ls);
		char *nm = s + 7; while (*nm == ' ') nm++;
		char *next = strstr(nm, ")\tName:");
		char *ad = strstr(nm, "Address:");
		struct found f; f.idx = idx;
		copy_field(f.name, sizeof f.name, nm);
		f.addr[0] = 0;
		if (ad && (!next || ad < next)) { ad += 8; while (*ad == ' ') ad++; copy_field(f.addr, sizeof f.addr, ad); char *sp = strchr(f.addr, ' '); if (sp) *sp = 0; }
		s = nm;
		if (idx < 0 || idx > 9 || !f.addr[0]) continue;	/* a2d reads single keystrokes: only 0-9 selectable */
		int dup = 0;
		for (int i = 0; i < nfound; i++) if (!strcmp(fl[i].addr, f.addr)) dup = 1;
		if (dup || nfound >= 10) continue;
		if (!f.name[0]) snprintf(f.name, sizeof f.name, "(%s)", f.addr);
		fl[nfound++] = f;
	}
}

static void stop_search(void) { send("C"); sleep(3); send(" "); usleep(500000); }

/* a device is selected in a2d (search stopped, index sent): optionally pair, then connect at once */
static int pair_connect(struct found *f, int do_pair)
{
	long from = log_size();
	if (do_pair) {
		message("Bluetooth", "Emparejando con", f->name);
		send("1");
		const char *p1[] = { "passkey", "Bonding completed", "Bonding failed", 0 };
		int r = wait_for(from, 25, p1);
		if (r == 0) { send("0000\n"); const char *p2[] = { "Bonding completed", "Bonding failed", 0 }; r = wait_for(from, 30, p2) == 0 ? 1 : 2; }
		if (r != 1) { lg("bt: bonding failed (%d)", r); return 0; }
		from = log_size();
	}
	send("2");	/* immediately: the stack closes the ACL link right after bonding */
	message("Bluetooth", "Conectando con", f->name);
	const char *p3[] = { "AV connect confirm - success", "AV connect confirm - error", 0 };
	if (wait_for(from, 30, p3) == 0) { connected = 1; save_dev(f->addr, f->name); logpos = log_size(); return 1; }
	return 0;
}

static int select_found(struct found *f)
{
	char d[4]; snprintf(d, sizeof d, "%d", f->idx);
	stop_search();
	send(d); sleep(1);
	return 1;
}

static const char *found_label(int i) { return fl[i].name; }

static void search_new(void)
{
	long from = log_size();
	send("0");
	struct list_view v = { "Buscar dispositivo", 0, 0, 0, found_label, 0, 0, LI_NONE, "Buscando... (modo emparejamiento)", 0 };
	double end = now() + 60;
	int lastn = -1;
	while (now() < end) {
		parse_found(from);
		v.n = nfound;
		if (v.sel >= v.n) v.sel = v.n ? v.n - 1 : 0;
		if (v.sel < v.top) v.top = v.sel;
		if (v.sel >= v.top + LIST_ROWS) v.top = v.sel - LIST_ROWS + 1;
		scr_list(&v); gfx_flip(); lastn = nfound;
		ui_tick();
		int k = getkey(400);
		if (k == K_UP && v.sel > 0) v.sel--;
		else if (k == K_DOWN && v.sel < v.n - 1) v.sel++;
		else if (k == K_BACK) { stop_search(); return; }
		else if (k == K_PLAY && nfound) {
			struct found f = fl[v.sel];
			select_found(&f);
			if (pair_connect(&f, 1)) { message("Bluetooth", "Conectado a", f.name); msg_wait(2); }
			else { message("Bluetooth", "No se pudo conectar.", "Probá de nuevo."); msg_wait(3); }
			return;
		}
	}
	stop_search();
	message("Bluetooth", "No encontré dispositivos.", "Revisá que esté en modo emparejamiento."); msg_wait(3);
}

/* a nearby device for another use (the phone that shares its internet, for podcasts): the stack's search, nothing
 * paired or connected here; Bluetooth is left as it was. 0 = picked (addr "NAP:UAP:LAP" as pan_up wants it) */
int bt_pick_device(const char *title, const char *hint, char *addr, size_t amax, char *name, size_t nmax)
{
	if (dl_active()) return -1;
	int was_on = on, r = -1;
	in_menu = 1;
	if (bt_on()) {
		message(title, "No pude encender el Bluetooth.", ""); msg_wait(3);
		in_menu = 0;
		if (!was_on) bt_shutdown();
		return -1;
	}
	long from = log_size();
	send("0");
	struct list_view v = { .title = title, .label = found_label, .empty = hint, .bar_y = -1 };
	double end = now() + 60;
	while (now() < end) {
		parse_found(from);
		v.n = nfound;
		if (v.sel >= v.n) v.sel = v.n ? v.n - 1 : 0;
		if (v.sel < v.top) v.top = v.sel;
		if (v.sel >= v.top + LIST_ROWS) v.top = v.sel - LIST_ROWS + 1;
		scr_list(&v); gfx_flip();
		ui_tick();
		int k = getkey(400);
		if (k == K_UP && v.sel > 0) v.sel--;
		else if (k == K_DOWN && v.sel < v.n - 1) v.sel++;
		else if (k == K_BACK) break;
		else if (k == K_PLAY && nfound) {
			snprintf(addr, amax, "%s", fl[v.sel].addr);
			snprintf(name, nmax, "%s", fl[v.sel].name[0] == '(' ? "" : fl[v.sel].name);
			r = 0;
			break;
		}
	}
	stop_search();
	in_menu = 0;
	if (!was_on) bt_shutdown();
	if (r && now() >= end) { message(title, "No encontré dispositivos.", "Revisá que el Bluetooth del teléfono esté visible."); msg_wait(3); }
	return r;
}

/* the remembered device: connect directly (patched helper, key in sc_db.db); if that fails, pair again */
static void reconnect(void)
{
	message("Bluetooth", "Conectando con", bt_device_name());
	double end = now() + 40;
	if (state != ST_CONNECTING) { long from = log_size(); send("2"); state = ST_CONNECTING; st_t = now(); (void)from; }
	while (!connected && now() < end && state == ST_CONNECTING) { bt_poll(); ui_tick(); if (getkey(250) == K_BACK) return; }
	if (connected) { message("Bluetooth", "Conectado a", bt_device_name()); msg_wait(2); return; }
	/* not accepted: probably the device forgot us. Pair again (the device must be in pairing mode). */
	message("Bluetooth", "No respondió. Poné el dispositivo en modo emparejamiento y apretá PLAY.", bt_device_name());
	int k;
	double give_up = now() + 120;	/* nobody answers: back to the menu (the screen dims and the battery checks meanwhile) */
	do { idle_tick(0); k = getkey(500); } while (k != K_PLAY && k != K_BACK && now() < give_up);
	if (k != K_PLAY) return;
	struct found f; memset(&f, 0, sizeof f);
	snprintf(f.addr, sizeof f.addr, "%s", dev_addr); snprintf(f.name, sizeof f.name, "%s", bt_device_name());
	if (pair_connect(&f, 1)) { message("Bluetooth", "Conectado a", f.name); msg_wait(2); }
	else { message("Bluetooth", "No conectó.", "Probá 'Buscar dispositivo nuevo'."); msg_wait(3); }
}

/* ---- receiver mode: the phone streams to the Walkman, which plays it on the wired output ---- */
static void bt_stack_up(const char *role)
{
	bt_shutdown();
	user_off = 0;
	unlink(LOG); unlink(FIFO);
	system("/usr/local/test/bt/bt_test 3 > /dev/null 2>&1");
	char *bca[] = { "/usr/local/test/bt/bc_attach", "-b", "2592000", "-d", "/dev/tts/0", 0 };
	unlink("/tmp/bc_attach.log");	/* fresh log per start (see start_async) */
	pid_bca = spawn(0, -1, "/tmp/bc_attach.log", bca);
	sleep(3);
	mkfifo(FIFO, 0600);
	fifo = open(FIFO, O_RDWR); set_cloexec(fifo);
	char btdir[PATHLEN + 8]; snprintf(btdir, sizeof btdir, "%s/bt", statedir);
	mkdir(btdir, 0755);
	int rd = open(FIFO, O_RDONLY | O_NONBLOCK);
	if (rd >= 0) { int fl = fcntl(rd, F_GETFL); fcntl(rd, F_SETFL, fl & ~O_NONBLOCK); }
	char *a2d[] = { "/usr/local/test/bt/a2d_demo_app", 0 };
	pid_a2d = spawn(btdir, rd, LOG, a2d);
	if (rd >= 0) close(rd);
	on = 1; connected = 0; logpos = 0; state = ST_ROLE; st_t = now();
	sleep(2);
	send(role);
}

void bt_receiver(void)
{
	message("Receptor Bluetooth", "Encendiendo...", "");
	bt_stack_up("0");	/* sink role */
	in_menu = 1;
	long from = 0;
	double end = now() + 180;
	int fdin = -1, ok = 0;
	static char buf[8192];
	static short pcm[2048 * 2];
	while (now() < end) {
		read_log(from, buf, sizeof buf);
		if (strstr(buf, "passkey")) { send("0000\n"); from = log_size(); }
		if (strstr(buf, "AV connect indication")) { ok = 1; break; }
		if (!helper_alive()) break;
		message("Receptor Bluetooth", "En el teléfono, buscá el Walkman (NW-A829) y conectalo. PIN: 0000", "Esperando conexión...");
		char b[32]; snprintf(b, sizeof b, "%d s", (int)(end - now()));
		gfx_text_center(W / 2, 262, b, C_GREY, 1); gfx_flip();
		ui_tick();
		if (getkey(500) == K_BACK) break;
	}
	if (ok) {
		connected = 1; logpos = log_size();
		aout_force_wired(1);
		fdin = open("/dev/sound/adsp", O_RDONLY | O_NONBLOCK);
		if (fdin >= 0) {
			int fmt = AFMT_S16_LE, ch = 2, r = 44100;
			ioctl(fdin, SNDCTL_DSP_SETFMT, &fmt); ioctl(fdin, SNDCTL_DSP_CHANNELS, &ch); ioctl(fdin, SNDCTL_DSP_SPEED, &r);
			lg("bt rx: capture rate %d", r);
			if (aout_start(44100)) { close(fdin); fdin = -1; }
		} else lg("bt rx: cannot open adsp for capture");
	}
	int lastdraw = -1, silent = 0;
	while (fdin >= 0) {
		fd_set rs; FD_ZERO(&rs); FD_SET(fdin, &rs);
		struct timeval tv = { 0, 200000 };
		if (select(fdin + 1, &rs, 0, 0, &tv) > 0) {
			int n = read(fdin, pcm, sizeof pcm);
			if (n > 0) {
				int frames = n / 4, g = aout_gain();
				eq_process(pcm, frames, 44100);
				if (g != 4096) for (int i = 0; i < 2 * frames; i++) pcm[i] = hb_clip((pcm[i] * g) >> 12);
				aout_write(pcm, frames);
				silent = 0;
			}
		} else silent++;
		hb_alive();
		bt_poll();
		if (!connected) { lg("bt rx: phone disconnected"); break; }
		int sec = (int)now();
		if (sec != lastdraw) {
			lastdraw = sec;
			if (backlight_on) {	/* drawn without message(): that would keep the screen lit for the whole session */
				scr_message("Receptor Bluetooth", "Sonando desde el teléfono", silent > 10 ? "(en silencio)" : "");
				char b[32]; snprintf(b, sizeof b, "Vol %d%%   BACK: terminar", volume * 100 / VOLMAX);
				gfx_text_center(W / 2, 262, b, C_GREY, 1); gfx_flip();
			}
		}
		idle_tick(1);	/* backlight timeout, low battery (playing: no auto-suspend) */
		int k = getkey(0);
		if (k == K_BACK) break;
		if (k == K_VOLUP && volume < VOLMAX) { volume++; aout_set_volume(); }
		if (k == K_VOLDN && volume > 0) { volume--; aout_set_volume(); }
	}
	if (fdin >= 0) close(fdin);
	aout_stop();
	aout_force_wired(0);
	in_menu = 0;
	bt_shutdown();
	if (!ok) { message("Receptor Bluetooth", "No se conectó ningún teléfono.", ""); msg_wait(2); }
}

static char mitems[5][48];
static const char *m_label(int i) { return mitems[i]; }

void bt_menu(void)
{
	if (dl_active()) { message("Bluetooth", "Ocupado bajando podcasts.", "Esperá o cancelá en Podcasts > Descargas"); msg_wait(3); return; }
	load_dev();
	in_menu = 1;
	for (;;) {
		bt_poll();
		int n = 0, act[6];
		if (connected) { snprintf(mitems[n], 48, "Desconectar"); act[n++] = 3; }
		else {
			if (dev_addr[0]) { snprintf(mitems[n], 48, "Conectar: %.30s", bt_device_name()); act[n++] = 1; }
			snprintf(mitems[n], 48, "Buscar dispositivo nuevo"); act[n++] = 2;
		}
		if (on) { snprintf(mitems[n], 48, "Apagar Bluetooth"); act[n++] = 4; }
		snprintf(mitems[n], 48, "Modo receptor (desde el teléfono)"); act[n++] = 5;
		snprintf(mitems[n], 48, "Mando para el teléfono"); act[n++] = 6;
		char title[64];
		if (connected) snprintf(title, sizeof title, "BT: %s", bt_device_name());
		else snprintf(title, sizeof title, on ? "Bluetooth: encendido" : "Bluetooth: apagado");
		struct list_view v = { title, n, 0, 0, m_label, 0, 0, LI_NONE, 0, 0 };
		int s = list_run(&v);
		if (s < 0) { in_menu = 0; return; }
		int a = act[s];
		if (a == 1 || a == 2) {
			if (bt_on()) { message("Bluetooth", "No pude encender el Bluetooth.", ""); msg_wait(3); continue; }
			if (a == 1) reconnect(); else search_new();
			if (connected) { in_menu = 0; return; }
		} else if (a == 3) {
			bt_poll();
			if (connected) { send("0"); sleep(2); }
			connected = 0; aout_stop();
			auto_tries = 100;	/* the user asked for it: don't reconnect on our own */
			set_auto(0);
		} else if (a == 4) {
			bt_shutdown();
			user_off = 1;
			set_auto(0);
		} else if (a == 5) {
			eng_pause(1);
			bt_receiver();
		} else if (a == 6) {
			bt_shutdown();
			remote_mode();
			in_menu = 0;
			return;
		}
	}
}
