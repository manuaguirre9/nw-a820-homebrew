/* btcm.c: our own client of the CSR BCHS Bluetooth stack in the kernel (/dev/peer_com), enough for a PAN User:
 *   - CM_L2CA_*  (event class 0x104): register PSM 0x0F, connect to the phone's NAP, send/receive data
 *   - SC_*       (class 0x102): bonding with PIN, so a phone can be paired from here
 *   - /dev/sc_db: serve the link keys (the kernel asks userland for them)
 * Wire format: big-endian integers, u16 primitive type first, no padding; bd_addr fields as listed per
 * primitive. Found by reverse engineering hci_bchs.ko and a2d_demo_app (see the notes in memory). */
#include "btcm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

extern void lg(const char *fmt, ...);
extern double now(void);

#define CLASS_SC 0x102
#define CLASS_CM 0x104
#define PSM_BNEP 0x000F

/* CM_L2CA primitives */
#define CM_L2CA_REGISTER_REQ 0x42
#define CM_L2CA_CONNECT_REQ 0x43
#define CM_L2CA_DISCONNECT_REQ 0x46
#define CM_L2CA_DATA_REQ 0x68
#define CM_CANCEL_L2CA_CONNECT_REQ 0x79
#define CM_READ_LOCAL_BD_ADDR_REQ 0x18
#define CM_READ_LOCAL_BD_ADDR_CFM 0x8030
#define CM_L2CA_MODE_CHANGE_IND 0x8021
#define CM_L2CA_MODE_CHANGE_REQ 0x47
#define CM_L2CA_REGISTER_CFM 0x8017
#define CM_L2CA_CONNECT_CFM 0x8018
#define CM_L2CA_DATA_CFM 0x801b
#define CM_L2CA_DATA_IND 0x801c
#define CM_L2CA_DISCONNECT_IND 0x801d
/* SC primitives */
#define SC_BOND_REQ 1
#define SC_PASSKEY_RES 3
#define SC_ACTIVATE_REQ 8

static int pc = -1;		/* /dev/peer_com */
static unsigned short phandle;	/* our application handle (queue id) as assigned by peer_com */
static int cid = -1, remote_mtu = 672, connected, data_pending;
static struct bd_addr peer, local;
static int have_local, tx_count, rx_count, cfm_count, wait_timeouts;
static double wait_total, wait_max;
static int keep_active = 1;
static unsigned char rx[2048];
static unsigned char bccmd_rx[256]; static int bccmd_rxlen, bccmd_wait;
extern double now(void);
#define CM_BCCMD_REQ 0x5f

/* ---------- byte helpers ---------- */
static unsigned char *put16(unsigned char *p, unsigned v) { *p++ = (unsigned char)(v >> 8); *p++ = (unsigned char)v; return p; }
static unsigned char *put32(unsigned char *p, unsigned long v) { p = put16(p, (unsigned)(v >> 16)); return put16(p, (unsigned)v); }
static unsigned get16(const unsigned char *p) { return p[0] << 8 | p[1]; }
static unsigned long get32(const unsigned char *p) { return (unsigned long)get16(p) << 16 | get16(p + 2); }
static unsigned char *put_bd(unsigned char *p, const struct bd_addr *a)	/* CM_L2CA_CONNECT_REQ reads the BD_ADDR as u16 nap, u8 uap, u32 lap */
{ p = put16(p, a->nap); *p++ = a->uap; return put32(p, a->lap); }

int bd_parse(const char *s, struct bd_addr *a)	/* "0011:22:334455" (nap:uap:lap) */
{
	unsigned nap, uap; unsigned long lap;
	if (sscanf(s, "%x:%x:%lx", &nap, &uap, &lap) != 3) return -1;
	a->nap = nap; a->uap = uap; a->lap = lap;
	return 0;
}

void bd_to_mac(const struct bd_addr *a, unsigned char mac[6])	/* NAP:UAP:LAP big-endian = the standard MAC */
{
	mac[0] = a->nap >> 8; mac[1] = a->nap; mac[2] = a->uap;
	mac[3] = a->lap >> 16; mac[4] = a->lap >> 8; mac[5] = a->lap;
}

/* ---------- peer_com envelope (filled in from the protocol notes) ---------- */
#include "peercom_env.h"	/* pc_open(), pc_send(class, prim, len), pc_recv(&class, buf, max, timeout_ms), pc_handle() */

/* ---------- link key database service (/dev/sc_db) ---------- */
static int scdb = -1;
static char scdb_path[512];

static void scdb_serve_open(const char *path)
{
	snprintf(scdb_path, sizeof scdb_path, "%s", path);
	scdb = open("/dev/sc_db", O_RDWR);
	if (scdb < 0) { lg("btcm: sc_db open failed"); return; }
	scdb_load_and_sync(scdb, path);	/* peercom_env.h: pushes the stored records (cmd 1) and syncs (cmd 2) */
}

static void scdb_poll(void) { if (scdb >= 0) scdb_service(scdb, scdb_path); }	/* stores new keys the kernel reports */

/* ---------- API ---------- */

int btcm_open(const char *scdb_file)
{
	if (pc >= 0) return 0;
	pc = pc_open();
	if (pc < 0) { lg("btcm: peer_com open failed"); return -1; }
	phandle = pc_handle(pc);
	lg("btcm: peer_com open, handle %u", phandle);
	scdb_serve_open(scdb_file);
	unsigned char m[4]; put16(m, SC_ACTIVATE_REQ); put16(m + 2, phandle);
	pc_send(pc, CLASS_SC, m, 4);
	return 0;
}

void btcm_close(void)
{
	if (cid >= 0) btcm_disconnect();
	if (scdb >= 0) close(scdb); scdb = -1;
	if (pc >= 0) close(pc); pc = -1;
	connected = 0; cid = -1;
}

static void (*data_cb)(const unsigned char *, int);
void btcm_set_data_cb(void (*cb)(const unsigned char *, int)) { data_cb = cb; }

/* Data that arrives while btcm_send() waits for the previous packet's confirm must NOT be delivered there:
 * btcm_send runs inside lwIP (tcp_output -> netif -> BNEP), and delivering would re-enter lwIP (NO_SYS, not
 * re-entrant) and corrupt its segment queues (crash in tcp_output seen in the player). Such packets are queued
 * and delivered by the next btcm_poll() made from outside a send. */
#define RXQ 24
static unsigned char rxq[RXQ][1700];
static int rxq_len[RXQ], rxq_head, rxq_n, in_send;

static void rxq_flush(void)
{
	while (rxq_n && !in_send) {
		int i = rxq_head;
		rxq_head = (rxq_head + 1) % RXQ; rxq_n--;
		if (data_cb) data_cb(rxq[i], rxq_len[i]);
	}
}

/* handles one incoming primitive; returns its type or -1 if none within timeout_ms */
int btcm_poll(int timeout_ms)
{
	if (!in_send && rxq_n) { rxq_flush(); timeout_ms = 0; }
	scdb_poll();
	unsigned short cls = 0;
	int n = pc_recv(pc, &cls, rx, sizeof rx, timeout_ms);
	if (n < 2) return -1;
	unsigned type = get16(rx);
	if (cls == CLASS_CM) {
		switch (type) {
		case CM_L2CA_REGISTER_CFM: lg("btcm: psm %u registered", get16(rx + 2)); break;
		case CM_L2CA_CONNECT_CFM: {
			int result = rx[4];
			if (result == 0) { cid = get16(rx + 2); remote_mtu = get16(rx + 7); connected = 1; btcm_active(); lg("btcm: l2cap connected cid %d mtu %d", cid, remote_mtu); }
			else { lg("btcm: l2cap connect failed, result 0x%02x", result); connected = -1; }
			break;
		}
		case CM_READ_LOCAL_BD_ADDR_CFM:
			if (n >= 9) { local.lap = get32(rx + 2); local.uap = rx[6]; local.nap = get16(rx + 7); have_local = 1;
				lg("btcm: local address %04X:%02X:%06lX", local.nap, local.uap, local.lap); }
			break;
		case CM_L2CA_MODE_CHANGE_IND:
			lg("btcm: link mode %d result %d interval %d (tx %d rx %d)", n > 4 ? rx[4] : -1, n > 5 ? rx[5] : -1, n > 7 ? (int)get16(rx + 6) : -1, tx_count, rx_count);
			if (n > 4 && rx[4] != 0 && keep_active) btcm_active();
			break;
		case CM_L2CA_DATA_CFM: data_pending = 0; cfm_count++; if (rx[4]) lg("btcm: data cfm error 0x%02x", rx[4]); break;
		case CM_L2CA_DATA_IND: { int len = get16(rx + 4); rx_count++;
			static int big; if (len > big) { big = len; lg("btcm: largest rx so far %d (msg %d)", len, n); }
			if (len > n - 6) { lg("btcm: rx truncated: %d of %d bytes", n - 6, len); break; }
			if (in_send) {	/* inside lwIP: defer */
				if (rxq_n < RXQ && len <= (int)sizeof rxq[0]) { int i = (rxq_head + rxq_n) % RXQ; memcpy(rxq[i], rx + 6, len); rxq_len[i] = len; rxq_n++; }
				else lg("btcm: rx queue full, packet dropped");
				break;
			}
			if (data_cb) data_cb(rx + 6, len); break; }
		case CM_L2CA_DISCONNECT_IND: lg("btcm: l2cap disconnected, reason 0x%02x", rx[4]); connected = 0; cid = -1; break;
		default: {
			char hx[3 * 40 + 1]; int k, m = n < 40 ? n : 40;
			for (k = 0; k < m; k++) sprintf(hx + 3 * k, " %02x", rx[k]);
			lg("btcm: cm prim 0x%04x (%d bytes):%s", type, n, hx);
			if (n >= 4 + 10 && bccmd_wait) { memcpy(bccmd_rx, rx, n < (int)sizeof bccmd_rx ? n : (int)sizeof bccmd_rx); bccmd_rxlen = n; bccmd_wait = 0; }
		}
		}
	} else if (cls == CLASS_SC) {
		sc_handle(rx, n);	/* peercom_env.h: passkey prompts -> PIN, bond results */
		if (sc_bond_done) { sc_bond_done = 0; return -2; }
	} else lg("btcm: class 0x%x prim 0x%04x", cls, type);
	return (int)type;
}

/* BCCMD to the BlueCore via CM_BCCMD_REQ (0x5f): big-endian type + phandle, then the BCCMD message as
 * little-endian words (type, len, seqno, varid, status, payload...). Waits for the confirm and returns the
 * reply words in out (up to max), or -1. */
int btcm_bccmd(const unsigned short *w, int nw, unsigned short *out, int max)
{
	unsigned char m[4 + 64], *p = m;
	if (nw > 32) return -1;
	p = put16(p, CM_BCCMD_REQ); p = put16(p, phandle);
	for (int i = 0; i < nw; i++) { *p++ = (unsigned char)w[i]; *p++ = (unsigned char)(w[i] >> 8); }
	bccmd_wait = 1; bccmd_rxlen = 0;
	pc_send(pc, CLASS_CM, m, (int)(p - m));
	double t0 = now();
	while (bccmd_wait && now() - t0 < 2) btcm_poll(50);
	if (bccmd_wait) { bccmd_wait = 0; return -1; }
	/* confirm: type, phandle?, then the reply words; find the echoed seqno/varid to align */
	int k;
	for (k = 2; k + 10 <= bccmd_rxlen; k++)
		if ((bccmd_rx[k + 4] | bccmd_rx[k + 5] << 8) == w[2] && (bccmd_rx[k + 6] | bccmd_rx[k + 7] << 8) == w[3]) break;
	if (k + 10 > bccmd_rxlen) return -1;
	int cnt = 0;
	for (; k + 1 < bccmd_rxlen && cnt < max; k += 2) out[cnt++] = (unsigned short)(bccmd_rx[k] | bccmd_rx[k + 1] << 8);
	return cnt;
}

void btcm_active(void)	/* leave sniff: full speed while we transfer */
{
	if (cid < 0) return;
	unsigned char m[5], *p = m;
	p = put16(p, CM_L2CA_MODE_CHANGE_REQ); p = put16(p, cid); *p++ = 0;
	pc_send(pc, CLASS_CM, m, 5);
}

void btcm_stats(void)
{
	lg("btcm stats: tx %d cfm %d rx %d, send wait total %.1f s max %.2f s, %d timeouts", tx_count, cfm_count, rx_count, wait_total, wait_max, wait_timeouts);
}

int btcm_local_addr(struct bd_addr *a)
{
	if (!have_local) {
		unsigned char m[4]; put16(m, CM_READ_LOCAL_BD_ADDR_REQ); put16(m + 2, phandle);
		pc_send(pc, CLASS_CM, m, 4);
		for (int i = 0; i < 30 && !have_local; i++) btcm_poll(100);
	}
	if (have_local) *a = local;
	return have_local ? 0 : -1;
}

int btcm_register_psm(void)
{
	unsigned char m[16], *p = m;
	p = put16(p, CM_L2CA_REGISTER_REQ); p = put16(p, phandle); p = put16(p, PSM_BNEP); *p++ = 0;	/* no SDP UUIDs */
	return pc_send(pc, CLASS_CM, m, (int)(p - m));
}

int btcm_connect(const struct bd_addr *a, int mtu)
{
	peer = *a; connected = 0; cid = -1;
	unsigned char m[32], *p = m;
	p = put16(p, CM_L2CA_CONNECT_REQ); p = put16(p, phandle); p = put_bd(p, a);
	p = put16(p, PSM_BNEP); p = put16(p, PSM_BNEP); p = put16(p, mtu);
	p = put16(p, 0xffff);	/* flush timeout: infinite */
	p = put16(p, BT_SEC_LEVEL);
	*p++ = 0;		/* no QoS */
	lg("btcm: connecting to %04X:%02X:%06lX psm 15", a->nap, a->uap, a->lap);
	return pc_send(pc, CLASS_CM, m, (int)(p - m));
}

int btcm_connected(void) { return connected == 1; }
int btcm_connect_failed(void) { return connected == -1; }
int btcm_mtu(void) { return remote_mtu; }

extern void hb_alive(void) __attribute__((weak));	/* the player's watchdog feed (absent in the test tools) */

int btcm_send(const unsigned char *data, int len)
{
	if (cid < 0 || len > remote_mtu) return -1;
	/* one packet in flight at a time (the CM keeps no queue) */
	double w0 = now();
	in_send++;
	for (int i = 0; data_pending && i < 200; i++) { btcm_poll(10); if (hb_alive) hb_alive(); }
	in_send--;
	double w = now() - w0; wait_total += w; if (w > wait_max) wait_max = w;
	if (data_pending) wait_timeouts++;
	unsigned char m[2048], *p = m;
	if (len + 6 > (int)sizeof m) return -1;
	p = put16(p, CM_L2CA_DATA_REQ); p = put16(p, cid); p = put16(p, len);
	memcpy(p, data, len);
	data_pending = 1; tx_count++;
	return pc_send(pc, CLASS_CM, m, len + 6);
}

void btcm_disconnect(void)
{
	if (cid < 0) return;
	unsigned char m[4]; put16(m, CM_L2CA_DISCONNECT_REQ); put16(m + 2, cid);
	pc_send(pc, CLASS_CM, m, 4);
	for (int i = 0; i < 50 && cid >= 0; i++) btcm_poll(20);
	cid = -1; connected = 0;
}

/* bonding: BOND_REQ then answer the passkey prompt with pin; result via sc_handle */
int btcm_bond(const struct bd_addr *a, const char *pin)
{
	sc_set_pin(pin);
	unsigned char m[24], *p = m;
	p = put16(p, SC_BOND_REQ); p = put16(p, phandle); p = put_bd_sc(p, a);	/* SC uses the AV layout: lap u32, uap u8, nap u16 */
	memset(p, 0, 7); p += 7;	/* u8, u16, u8, u8, u8 bond options: zero = defaults */
	return pc_send(pc, CLASS_SC, m, (int)(p - m));
}
