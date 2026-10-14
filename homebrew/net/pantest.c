/* pantest: feasibility spike. Connects to a phone's Bluetooth tethering (PAN NAP) as a PAN User using our own
 * L2CAP client + BNEP framing, asks for an address by DHCP and downloads a small file over HTTP(S).
 * usage: pantest NAP:UAP:LAP [url] [dest] [pin]     (the Bluetooth stack must already be attached: bt_test 3 +
 * bc_attach; a2d_demo_app must NOT be running). Logs to stderr. */
#include "btcm.h"
#include "bnep.h"
#include "net.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void lg(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	fprintf(stderr, "[%ld] ", (long)time(0));
	vfprintf(stderr, fmt, ap); fputc('\n', stderr); fflush(stderr);
	va_end(ap);
}

void set_cloexec(int fd) { (void)fd; }
double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static int l2_send(const unsigned char *p, int n) { return btcm_send(p, n); }
static void l2_recv(const unsigned char *p, int n) { bnep_input(p, n); }
static int eth_send(const unsigned char *f, int n) { return bnep_send_ethernet(f, n); }
static void eth_recv(const unsigned char *f, int n) { net_input(f, n); }
static void ping_poll(void) { btcm_poll(2); }

extern int net_trace_max;
int main(int argc, char **argv)
{
		net_trace_max = 400;
if (argc < 2) { fprintf(stderr, "usage: pantest NAP:UAP:LAP [url] [dest] [pin]\n"); return 2; }
	struct bd_addr phone;
	if (bd_parse(argv[1], &phone)) { fprintf(stderr, "bad address\n"); return 2; }
	const char *url = argc > 2 ? argv[2] : "http://example.com/";
	const char *dest = argc > 3 ? argv[3] : "/tmp/pantest.out";
	const char *pin = argc > 4 ? argv[4] : "0000";
	if (btcm_open("/var/hb/bt/sc_db.db")) return 1;
	/* the local BD address: read from NVP would be ideal; the MAC only needs to be unique on the link */
	unsigned char lmac[6] = { 0x00, 0x13, 0xA9, 0x00, 0x00, 0x01 }, rmac[6];
	if (getenv("BTMAC")) { struct bd_addr me; if (!bd_parse(getenv("BTMAC"), &me)) bd_to_mac(&me, lmac); }
	bd_to_mac(&phone, rmac);
	{ struct bd_addr me; if (!getenv("BTMAC") && !btcm_local_addr(&me)) bd_to_mac(&me, lmac); }
	lg("pantest: our mac %02x:%02x:%02x:%02x:%02x:%02x", lmac[0], lmac[1], lmac[2], lmac[3], lmac[4], lmac[5]);
	/* BlueCore settings that govern the BCSP link (read-only, then the ack timeout in the RAM store) */
	{
		static unsigned short seq = 1;
		unsigned short r[32];
		unsigned short bid[9] = { 0, 9, seq++, 0x2819, 0, 0, 0, 0, 0 };
		int n = btcm_bccmd(bid, 9, r, 32);
		lg("bccmd buildid: n %d status %d value %d", n, n > 4 ? r[4] : -1, n > 5 ? r[5] : -1);
		static const unsigned short keys[] = { 0x01be, 0x01bf, 0x01c0, 0x01c1, 0x01c3, 0x01c4, 0x01c5, 0x01c6, 0x0405, 0x0406, 0x0407, 0x01f9, 0x01aa };
		for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
			unsigned short q[9] = { 0, 9, seq++, 0x7003, 0, keys[i], 1, 0, 0 };
			n = btcm_bccmd(q, 9, r, 32);
			lg("bccmd pskey 0x%04x: n %d status %d value %d (0x%04x)", keys[i], n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1, n > 8 ? r[8] : 0);
		}
		{	/* which PS stores exist and hold what: 1 PSI, 2 PSF, 4 PSROM, 8 RAM */
			static const unsigned short sk[] = { 0x01c4, 0x01be, 0x01c6 }, st[] = { 1, 2, 4, 8 };
			for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 4; j++) {
				unsigned short q[9] = { 0, 9, seq++, 0x7003, 0, sk[i], 1, st[j], 0 };
				n = btcm_bccmd(q, 9, r, 32);
				lg("bccmd store %d key 0x%04x: n %d status %d value %d", st[j], sk[i], n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1);
			}
			unsigned short fv[9] = { 0, 9, seq++, 0x2819, 0, 0, 0, 0, 0 };	/* also chip variant: ANA_FTRIM 0x281a? keep buildid */
			(void)fv;
		}
		if (getenv("PSI_CLEAR")) {	/* back to factory: remove only our UART_ACK_TIMEOUT key from PSI (PS_CLR_STORES 0x500c) */
			unsigned short q[9] = { 0, 9, seq++, 0x7003, 0, 0x01c4, 1, 0x0001, 0 };
			n = btcm_bccmd(q, 9, r, 32);
			lg("bccmd PSI 0x01c4 before clear: n %d status %d value %d", n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1);
			if (n > 8 && r[4] == 0) {
				unsigned short c[9] = { 2, 9, seq++, 0x500c, 0, 0x01c4, 0x0001, 0, 0 };
				n = btcm_bccmd(c, 9, r, 32);
				lg("bccmd PS_CLR_STORES 0x01c4 PSI: n %d status %d", n, n > 4 ? r[4] : -1);
			}
			unsigned short q2[9] = { 0, 9, seq++, 0x7003, 0, 0x01c4, 1, 0x0001, 0 };
			n = btcm_bccmd(q2, 9, r, 32);
			lg("bccmd PSI 0x01c4 after clear: n %d status %d (8 = absent)", n, n > 4 ? r[4] : -1);
			unsigned short q3[9] = { 0, 9, seq++, 0x7003, 0, 0x01be, 1, 0x0001, 0 };
			n = btcm_bccmd(q3, 9, r, 32);
			lg("bccmd PSI 0x01be (Sony baud, must stay): n %d status %d value 0x%04x", n, n > 4 ? r[4] : -1, n > 8 ? r[8] : 0);
			if (getenv("PSONLY")) { btcm_close(); return 0; }
		}
		const char *psi = getenv("PSI_ACKMS");	/* persistent: UART_ACK_TIMEOUT in the chip's PSI store (user approved) */
		if (psi) {
			unsigned short v = (unsigned short)atoi(psi);
			unsigned short q[9] = { 0, 9, seq++, 0x7003, 0, 0x01c4, 1, 0x0001, 0 };
			n = btcm_bccmd(q, 9, r, 32);
			lg("bccmd PSI 0x01c4 before: n %d status %d value %d", n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1);
			if (v >= 5 && v <= 250 && !(n > 8 && r[4] == 0 && r[8] == v)) {
				unsigned short s[9] = { 2, 9, seq++, 0x7003, 0, 0x01c4, 1, 0x0001, v };
				n = btcm_bccmd(s, 9, r, 32);
				lg("bccmd PSI 0x01c4 = %d: n %d status %d", v, n, n > 4 ? r[4] : -1);
				unsigned short q2[9] = { 0, 9, seq++, 0x7003, 0, 0x01c4, 1, 0x0001, 0 };
				n = btcm_bccmd(q2, 9, r, 32);
				lg("bccmd PSI 0x01c4 after: n %d status %d value %d", n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1);
			}
			if (getenv("PSONLY")) { btcm_close(); return 0; }
		}
		const char *ack = getenv("ACKMS");	/* write UART_ACK_TIMEOUT (ms) and the old SEQ_TIMEOUT key to RAM, read back */
		if (ack) {
			unsigned short v = (unsigned short)atoi(ack);
			static const unsigned short wk[] = { 0x01c4, 0x0405 };
			for (unsigned i = 0; i < 2; i++) {
				unsigned short s[9] = { 2, 9, seq++, 0x7003, 0, wk[i], 1, 0x0008, v };
				n = btcm_bccmd(s, 9, r, 32);
				lg("bccmd set pskey 0x%04x = %d (RAM): n %d status %d", wk[i], v, n, n > 4 ? r[4] : -1);
				unsigned short q[9] = { 0, 9, seq++, 0x7003, 0, wk[i], 1, 0x0008, 0 };
				n = btcm_bccmd(q, 9, r, 32);
				lg("bccmd readback 0x%04x (RAM): n %d status %d value %d", wk[i], n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1);
			}
			if (getenv("WARMRESET")) {	/* the RAM store survives a warm reset; the stack must re-sync BCSP */
				unsigned short w[9] = { 2, 9, seq++, 0x4002, 0, 0, 0, 0, 0 };
				n = btcm_bccmd(w, 9, r, 32);
				lg("bccmd warm reset: n %d", n);
				double t1 = now();
				while (now() - t1 < 6) btcm_poll(100);
				unsigned short q[9] = { 0, 9, seq++, 0x7003, 0, 0x01c4, 1, 0, 0 };
				n = btcm_bccmd(q, 9, r, 32);
				lg("bccmd after reset 0x01c4: n %d status %d value %d", n, n > 4 ? r[4] : -1, n > 8 ? r[8] : -1);
				struct bd_addr me2;
				lg("after reset: local addr %s", btcm_local_addr(&me2) ? "FAILED" : "ok");
			}
		}
	}
	btcm_set_data_cb(l2_recv);
	btcm_register_psm();
	double t0 = now();
	while (now() - t0 < 3) btcm_poll(100);
	int tries = 0;
	for (;;) {
		btcm_connect(&phone, 1691);
		t0 = now();
		while (!btcm_connected() && !btcm_connect_failed() && now() - t0 < 30) btcm_poll(100);
		if (btcm_connected()) break;
		if (++tries >= 4) { lg("pantest: L2CAP connect failed"); btcm_close(); return 3; }
		lg("pantest: retry after bonding");
		btcm_bond(&phone, pin);
		t0 = now();
		while (now() - t0 < 60) { int t = btcm_poll(200); if (t == -2) break; }	/* -2: bond finished (sc_handle) */
	}
	bnep_init(lmac, rmac, l2_send, eth_recv);
	bnep_setup();
	t0 = now();
	while (bnep_state() == 1 && now() - t0 < 10) btcm_poll(100);
	if (bnep_state() != 2) { lg("pantest: BNEP setup failed (%d)", bnep_state()); btcm_close(); return 4; }
	lg("pantest: BNEP up, starting DHCP");
	net_start(lmac, eth_send);
	t0 = now();
	while (!net_ready() && now() - t0 < 30) { btcm_poll(20); net_poll(); }
	if (!net_ready()) { lg("pantest: no DHCP lease"); btcm_close(); return 5; }
	/* link latency: echo to the phone with payloads that fit one ACL fragment (32, 250) and span several (1000) */
	{
		static const int sizes[] = { 32, 32, 32, 32, 32, 250, 250, 250, 250, 250, 1000, 1000, 1000, 1000, 1000 };
		for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
			double ms = net_ping_gw(sizes[i], ping_poll);
			lg("ping gw size %d: %s%.0f ms", sizes[i], ms < 0 ? "timeout " : "", ms < 0 ? 3000.0 : ms);
		}
	}
	if (access("/contents/HB/cacert.pem", R_OK) == 0) net_set_ca("/contents/HB/cacert.pem");
	lg("pantest: GET %s", url);
	if (net_download_start(url, dest)) { btcm_close(); return 6; }
	t0 = now();
	long got = 0, total = 0; int r;
	double tp = now();
	if (getenv("PAN_MHZ")) { FILE *pm = fopen("/proc/pm", "w"); if (pm) { fprintf(pm, "M%s", getenv("PAN_MHZ")); fclose(pm); lg("pantest: cpu M%s", getenv("PAN_MHZ")); } }
	int secs = getenv("PAN_SECS") ? atoi(getenv("PAN_SECS")) : 150;
	while ((r = net_download_poll(&got, &total)) == 0 && now() - t0 < secs) {
		btcm_poll(5);
		for (int k = 0; k < 32 && btcm_poll(0) != -1; k++) ;	/* drain whatever else is queued */
		if (now() - tp >= 5) {
			static long last_got; static unsigned long long lb, lt;
			unsigned long long v[7] = { 0 }, busy = 0, tot = 0;
			FILE *sf = fopen("/proc/stat", "r");
			if (sf) { if (fscanf(sf, "cpu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) >= 4) for (int i = 0; i < 7; i++) { tot += v[i]; if (i != 3 && i != 4) busy += v[i]; } fclose(sf); }
			double dt = now() - tp;
			lg("pantest: %ld / %ld bytes, avg %.1f KB/s, now %.1f KB/s, cpu busy %.0f%% (usr %llu sys %llu irq %llu sirq %llu)", got, total, got / 1024.0 / (now() - t0),
			   (got - last_got) / 1024.0 / dt, tot > lt ? 100.0 * (busy - lb) / (tot - lt) : 0, v[0], v[2], v[5], v[6]);
			last_got = got; lb = busy; lt = tot; tp = now();
			btcm_stats(); net_tcp_stats();
		}
	}
	lg("pantest: download %s: %ld/%ld bytes", r == 1 ? "OK" : "FAILED", got, total);
	net_stop();
	btcm_disconnect();
	btcm_close();
	return r == 1 ? 0 : 7;
}
