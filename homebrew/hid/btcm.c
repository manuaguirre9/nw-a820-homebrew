/* btcm.c (hid variant): our client of the CSR BCHS Bluetooth stack in the kernel (/dev/peer_com).
 * Copied from homebrew/net/btcm.c and extended for a HID device: several L2CAP channels (control 0x11 +
 * interrupt 0x13), incoming connections (CM_L2CA_CONNECT_ACCEPT_REQ), an SDP record (CM_SDS_REGISTER_REQ),
 * local name, class of device, scan enable, debond.
 * Wire format: big-endian integers, u16 primitive type first, no padding. Every layout below was read from the
 * Serialize_X_T / Deserialize_X_T functions of hci_bchs.ko (re/primtab.txt has the type numbers). */
#include "btcm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

extern void lg(const char *fmt, ...);
extern double now(void);

#define CLASS_SC 0x102
#define CLASS_CM 0x104

/* CM downstream (CM_init_converter in hci_bchs.ko) */
#define CM_SET_LOCAL_NAME_REQ 0x00		/* u16 type, u16 phandle, char name[41] */
#define CM_READ_LOCAL_BD_ADDR_REQ 0x18		/* u16 type, u16 phandle */
#define CM_WRITE_SCAN_ENABLE_REQ 0x1c		/* u16, u16 phandle, u8 disableInquiryScan, u8 disablePageScan */
#define CM_CONNECTABLE_REQ 0x1e			/* u16, u16 phandle, u8 connectable */
#define CM_WRITE_COD_REQ 0x23			/* u16, u16 phandle, u32 cod (only service bits 13..23 are used) */
#define CM_WRITE_MAJOR_MINOR_COD_REQ 0x39	/* u16, u16 phandle, u32 major (&0x1f00), u32 minor (&0xfc) */
#define CM_L2CA_REGISTER_REQ 0x42		/* u16, u16 phandle, u16 psm, u8 n, n bytes */
#define CM_L2CA_CONNECT_REQ 0x43		/* u16, u16 phandle, bd(u16 nap,u8 uap,u32 lap), u16 lpsm, u16 rpsm, u16 mtu, u16 flush, u16 secl, u8 qos */
#define CM_L2CA_CONNECT_ACCEPT_REQ 0x44		/* u16, u16 phandle, u16 psm, u32 cod, u16 secl, u16 mtu, u16 flush, u16 profile uuid, u8 qos */
#define CM_L2CA_CANCEL_CONNECT_ACCEPT_REQ 0x45	/* u16, u16 phandle, u16 psm */
#define CM_L2CA_DISCONNECT_REQ 0x46		/* u16, u16 cid */
#define CM_L2CA_MODE_CHANGE_REQ 0x47		/* u16, u16 cid, u8 mode */
#define CM_SDS_REGISTER_REQ 0x57		/* u16, u16 phandle, u16 len, len bytes (attribute id/value pairs) */
#define CM_SDS_UNREGISTER_REQ 0x58		/* u16, u16 phandle, u32 record handle */
#define CM_BCCMD_REQ 0x5f
#define CM_L2CA_DATA_REQ 0x68			/* u16, u16 cid, u16 len, data */
/* CM upstream */
#define CM_SDS_REGISTER_CFM 0x8015		/* u16, u32 handle, u8 result */
#define CM_SDS_UNREGISTER_CFM 0x8016		/* u16, u32 handle, u8 result */
#define CM_L2CA_REGISTER_CFM 0x8017		/* u16, u16 psm */
#define CM_L2CA_CONNECT_CFM 0x8018		/* u16, u16 cid, u8 result, u16 psm, u16 mtu, u32 lap, u8 uap, u16 nap */
#define CM_L2CA_CONNECT_ACCEPT_CFM 0x8019	/* same layout as CONNECT_CFM */
#define CM_L2CA_CANCEL_CONNECT_ACCEPT_CFM 0x801a
#define CM_L2CA_DATA_CFM 0x801b			/* u16, u16 cid, u8 result */
#define CM_L2CA_DATA_IND 0x801c			/* u16, u16 cid, u16 len, data */
#define CM_L2CA_DISCONNECT_IND 0x801d		/* u16, u16 cid, u8 reason */
#define CM_L2CA_MODE_CHANGE_IND 0x8021		/* u16, u16 cid, u8 mode, u8 result, u16 interval */
#define CM_SET_LOCAL_NAME_CFM 0x802f		/* u16, u8 result */
#define CM_READ_LOCAL_BD_ADDR_CFM 0x8030	/* u16, u32 lap, u8 uap, u16 nap */
#define CM_WRITE_COD_CFM 0x8036			/* u16, u8 result */
#define CM_BCCMD_CFM 0x8042
#define CM_WRITE_MAJOR_MINOR_COD_CFM 0x8061	/* u16, u8 result */
/* SC */
#define SC_BOND_REQ 1
#define SC_DEBOND_REQ 2				/* u16, u16 phandle, u32 lap, u8 uap, u16 nap */
#define SC_PASSKEY_RES 3
#define SC_ACTIVATE_REQ 8

static int pc = -1;		/* /dev/peer_com */
static unsigned short phandle;	/* our application handle (queue id) as assigned by peer_com */
static struct bd_addr local;
static int have_local, tx_count, rx_count;
static int data_pending;	/* DATA_REQs without DATA_CFM (the CM keeps no queue: one at a time) */
static unsigned char rx[2048];
static unsigned char bccmd_rx[256]; static int bccmd_rxlen, bccmd_wait;
static struct btcm_cb cb;
unsigned char btcm_last[1024]; int btcm_last_len;

/* ---------- byte helpers ---------- */
static unsigned char *put16(unsigned char *p, unsigned v) { *p++ = (unsigned char)(v >> 8); *p++ = (unsigned char)v; return p; }
static unsigned char *put32(unsigned char *p, unsigned long v) { p = put16(p, (unsigned)(v >> 16)); return put16(p, (unsigned)v); }
static unsigned get16(const unsigned char *p) { return p[0] << 8 | p[1]; }
static unsigned long get32(const unsigned char *p) { return (unsigned long)get16(p) << 16 | get16(p + 2); }
static unsigned char *put_bd(unsigned char *p, const struct bd_addr *a)	/* CM_L2CA_CONNECT_REQ: u16 nap, u8 uap, u32 lap */
{ p = put16(p, a->nap); *p++ = a->uap; return put32(p, a->lap); }

int bd_parse(const char *s, struct bd_addr *a)	/* "0011:22:334455" (nap:uap:lap) */
{
	unsigned nap, uap; unsigned long lap;
	if (sscanf(s, "%x:%x:%lx", &nap, &uap, &lap) != 3) return -1;
	a->nap = nap & 0xffff; a->uap = uap & 0xff; a->lap = lap & 0xffffff;
	return 0;
}

void bd_fmt(const struct bd_addr *a, char out[20]) { snprintf(out, 20, "%04X:%02X:%06lX", a->nap, a->uap, a->lap); }

/* ---------- peer_com envelope + SC helpers ---------- */
#include "peercom_env.h"

static void sc_bond_event(const struct bd_addr *a, int result) { if (cb.bond) cb.bond(a, result); }

/* ---------- link key database service (/dev/sc_db) ---------- */
static int scdb = -1;
static char scdb_path[512];

static void scdb_serve_open(const char *path)
{
	snprintf(scdb_path, sizeof scdb_path, "%s", path);
	scdb = open("/dev/sc_db", O_RDWR);
	if (scdb < 0) { lg("btcm: sc_db open failed"); return; }
	scdb_load_and_sync(scdb, path);
}

static void scdb_poll(void) { if (scdb >= 0) scdb_service(scdb, scdb_path); }

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
	if (scdb >= 0) close(scdb);
	scdb = -1;
	if (pc >= 0) close(pc);
	pc = -1;
}

int btcm_fd(void) { return pc; }
void btcm_set_cb(const struct btcm_cb *c) { cb = *c; }

static void hexlog(const char *what, unsigned type, const unsigned char *p, int n)
{
	char hx[3 * 40 + 1]; int k, m = n < 40 ? n : 40;
	for (k = 0; k < m; k++) sprintf(hx + 3 * k, " %02x", p[k]);
	hx[3 * m] = 0;
	lg("btcm: %s prim 0x%04x (%d bytes):%s", what, type, n, hx);
}

static void conn_cfm(int n, int incoming)
{
	int cid = get16(rx + 2), result = rx[4];
	unsigned psm = n >= 7 ? get16(rx + 5) : 0, mtu = n >= 9 ? get16(rx + 7) : 0;
	struct bd_addr a = { 0, 0, 0 };
	if (n >= 16) { a.lap = get32(rx + 9); a.uap = rx[13]; a.nap = get16(rx + 14); }
	char b[20]; bd_fmt(&a, b);
	lg("btcm: %s cfm psm 0x%02x cid 0x%04x result 0x%02x mtu %u peer %s", incoming ? "accept" : "connect", psm, cid, result, mtu, b);
	if (result == 0) { if (cb.l2_up) cb.l2_up((int)psm, cid, (int)mtu, &a, incoming); }
	else if (cb.l2_fail) cb.l2_fail((int)psm, result, incoming);
}

/* handles one incoming primitive; returns its type or -1 if none within timeout_ms */
int btcm_poll(int timeout_ms)
{
	scdb_poll();
	unsigned short cls = 0;
	int n = pc_recv(pc, &cls, rx, sizeof rx, timeout_ms);
	if (n < 2) return -1;
	unsigned type = get16(rx);
	btcm_last_len = n < (int)sizeof btcm_last ? n : (int)sizeof btcm_last;
	memcpy(btcm_last, rx, btcm_last_len);
	if (cls == CLASS_CM) {
		switch (type) {
		case CM_L2CA_REGISTER_CFM: lg("btcm: psm 0x%02x registered", n >= 4 ? get16(rx + 2) : 0); break;
		case CM_L2CA_CONNECT_CFM: conn_cfm(n, 0); break;
		case CM_L2CA_CONNECT_ACCEPT_CFM: conn_cfm(n, 1); break;
		case CM_L2CA_CANCEL_CONNECT_ACCEPT_CFM: hexlog("cancel-accept cfm", type, rx, n); break;
		case CM_READ_LOCAL_BD_ADDR_CFM:
			if (n >= 9) { local.lap = get32(rx + 2); local.uap = rx[6]; local.nap = get16(rx + 7); have_local = 1;
				char b[20]; bd_fmt(&local, b); lg("btcm: local address %s", b); }
			break;
		case CM_L2CA_MODE_CHANGE_IND:
			lg("btcm: cid 0x%04x link mode %d result %d interval %d", n > 3 ? (int)get16(rx + 2) : -1, n > 4 ? rx[4] : -1, n > 5 ? rx[5] : -1, n > 7 ? (int)get16(rx + 6) : -1);
			break;
		case CM_L2CA_DATA_CFM:
			if (data_pending > 0) data_pending--;
			if (n > 4 && rx[4]) lg("btcm: data cfm cid 0x%04x error 0x%02x", get16(rx + 2), rx[4]);
			break;
		case CM_L2CA_DATA_IND: {
			int cid = get16(rx + 2), len = get16(rx + 4); rx_count++;
			if (len > n - 6) { lg("btcm: rx truncated: %d of %d bytes", n - 6, len); break; }
			if (cb.l2_data) cb.l2_data(cid, rx + 6, len);
			break;
		}
		case CM_L2CA_DISCONNECT_IND:
			lg("btcm: cid 0x%04x disconnected, reason 0x%02x", get16(rx + 2), n > 4 ? rx[4] : 0xff);
			if (cb.l2_down) cb.l2_down(get16(rx + 2), n > 4 ? rx[4] : 0xff);
			break;
		case CM_SET_LOCAL_NAME_CFM: lg("btcm: set name cfm result %d", n > 2 ? rx[2] : -1); break;
		case CM_WRITE_COD_CFM: lg("btcm: write cod cfm result %d", n > 2 ? rx[2] : -1); break;
		case CM_WRITE_MAJOR_MINOR_COD_CFM: lg("btcm: write major/minor cod cfm result %d", n > 2 ? rx[2] : -1); break;
		case CM_SDS_REGISTER_CFM: lg("btcm: sdp register cfm handle 0x%08lx result %d", n >= 6 ? get32(rx + 2) : 0, n > 6 ? rx[6] : -1); break;
		case CM_SDS_UNREGISTER_CFM: lg("btcm: sdp unregister cfm handle 0x%08lx result %d", n >= 6 ? get32(rx + 2) : 0, n > 6 ? rx[6] : -1); break;
		default:
			hexlog("cm", type, rx, n);
			if (type == CM_BCCMD_CFM && bccmd_wait) { memcpy(bccmd_rx, rx, n < (int)sizeof bccmd_rx ? n : (int)sizeof bccmd_rx); bccmd_rxlen = n; bccmd_wait = 0; }
		}
	} else if (cls == CLASS_SC) {
		sc_handle(rx, n);	/* peercom_env.h: PIN requests -> PIN, bond results */
		if (sc_bond_done) { sc_bond_done = 0; return -2; }
	} else hexlog("other-class", type, rx, n);
	return (int)type;
}

int btcm_wait(unsigned type, int timeout_ms)
{
	double t0 = now();
	while ((now() - t0) * 1000 < timeout_ms) if (btcm_poll(50) == (int)type) return 0;
	lg("btcm: timeout waiting for prim 0x%04x", type);
	return -1;
}

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
	int k;
	for (k = 2; k + 10 <= bccmd_rxlen; k++)
		if ((bccmd_rx[k + 4] | bccmd_rx[k + 5] << 8) == w[2] && (bccmd_rx[k + 6] | bccmd_rx[k + 7] << 8) == w[3]) break;
	if (k + 10 > bccmd_rxlen) return -1;
	int cnt = 0;
	for (; k + 1 < bccmd_rxlen && cnt < max; k += 2) out[cnt++] = (unsigned short)(bccmd_rx[k] | bccmd_rx[k + 1] << 8);
	return cnt;
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

int btcm_set_name(const char *name)
{
	unsigned char m[4 + 41], *p = m;
	p = put16(p, CM_SET_LOCAL_NAME_REQ); p = put16(p, phandle);
	memset(p, 0, 41); strncpy((char *)p, name, 40); p += 41;
	pc_send(pc, CLASS_CM, m, (int)(p - m));
	return btcm_wait(CM_SET_LOCAL_NAME_CFM, 3000);
}

int btcm_set_cod(unsigned service, unsigned major, unsigned minor)
{
	unsigned char m[12], *p = m;
	int r = 0;
	if (service) {
		p = put16(p, CM_WRITE_COD_REQ); p = put16(p, phandle); p = put32(p, service);
		pc_send(pc, CLASS_CM, m, (int)(p - m));
		r |= btcm_wait(CM_WRITE_COD_CFM, 3000);
	}
	p = m;
	p = put16(p, CM_WRITE_MAJOR_MINOR_COD_REQ); p = put16(p, phandle); p = put32(p, major); p = put32(p, minor);
	pc_send(pc, CLASS_CM, m, (int)(p - m));
	r |= btcm_wait(CM_WRITE_MAJOR_MINOR_COD_CFM, 3000);
	return r;
}

int btcm_scan_enable(int inquiry, int page)
{
	unsigned char m[6], *p = m;
	p = put16(p, CM_WRITE_SCAN_ENABLE_REQ); p = put16(p, phandle);
	*p++ = inquiry ? 0 : 1; *p++ = page ? 0 : 1;	/* the fields are "disable" flags */
	return pc_send(pc, CLASS_CM, m, 6);
}

int btcm_connectable(int on)
{
	unsigned char m[5], *p = m;
	p = put16(p, CM_CONNECTABLE_REQ); p = put16(p, phandle); *p++ = on ? 1 : 0;
	return pc_send(pc, CLASS_CM, m, 5);
}

int btcm_sds_register(const unsigned char *rec, int len, unsigned long *handle)
{
	unsigned char m[1024], *p = m;
	if (len + 6 > (int)sizeof m) return -1;
	p = put16(p, CM_SDS_REGISTER_REQ); p = put16(p, phandle); p = put16(p, len);
	memcpy(p, rec, len); p += len;
	pc_send(pc, CLASS_CM, m, (int)(p - m));
	if (btcm_wait(CM_SDS_REGISTER_CFM, 5000)) return -1;
	if (btcm_last_len < 7) return -1;
	if (handle) *handle = get32(btcm_last + 2);
	return btcm_last[6];	/* 0 = success */
}

int btcm_sds_unregister(unsigned long handle)
{
	unsigned char m[8], *p = m;
	p = put16(p, CM_SDS_UNREGISTER_REQ); p = put16(p, phandle); p = put32(p, handle);
	pc_send(pc, CLASS_CM, m, 8);
	return btcm_wait(CM_SDS_UNREGISTER_CFM, 3000);
}

int btcm_l2_register(unsigned psm)
{
	unsigned char m[8], *p = m;
	p = put16(p, CM_L2CA_REGISTER_REQ); p = put16(p, phandle); p = put16(p, psm); *p++ = 0;	/* no UUID list */
	pc_send(pc, CLASS_CM, m, (int)(p - m));
	return btcm_wait(CM_L2CA_REGISTER_CFM, 3000);
}

int btcm_l2_accept(unsigned psm, unsigned secl, unsigned mtu, unsigned uuid)
{
	unsigned char m[24], *p = m;
	p = put16(p, CM_L2CA_CONNECT_ACCEPT_REQ); p = put16(p, phandle); p = put16(p, psm);
	p = put32(p, 0);		/* class of device bits to add while listening: none */
	p = put16(p, secl); p = put16(p, mtu);
	p = put16(p, 0xffff);		/* flush timeout: infinite */
	p = put16(p, uuid);		/* profile uuid (for the SC service registration) */
	*p++ = 0;			/* no QoS */
	lg("btcm: accepting on psm 0x%02x (secl 0x%04x)", psm, secl);
	return pc_send(pc, CLASS_CM, m, (int)(p - m));
}

int btcm_l2_cancel_accept(unsigned psm)
{
	unsigned char m[6], *p = m;
	p = put16(p, CM_L2CA_CANCEL_CONNECT_ACCEPT_REQ); p = put16(p, phandle); p = put16(p, psm);
	return pc_send(pc, CLASS_CM, m, 6);
}

int btcm_l2_connect(const struct bd_addr *a, unsigned psm, unsigned mtu, unsigned secl)
{
	unsigned char m[32], *p = m;
	p = put16(p, CM_L2CA_CONNECT_REQ); p = put16(p, phandle); p = put_bd(p, a);
	p = put16(p, psm); p = put16(p, psm); p = put16(p, mtu);
	p = put16(p, 0xffff);
	p = put16(p, secl);
	*p++ = 0;
	char b[20]; bd_fmt(a, b);
	lg("btcm: connecting to %s psm 0x%02x", b, psm);
	return pc_send(pc, CLASS_CM, m, (int)(p - m));
}

int btcm_l2_send(int cid, const unsigned char *data, int len)
{
	if (cid < 0) return -1;
	for (int i = 0; data_pending > 0 && i < 30; i++) btcm_poll(10);	/* at most ~300 ms behind */
	if (data_pending > 0) { lg("btcm: data cfm missing, sending anyway"); data_pending = 0; }
	unsigned char m[1024], *p = m;
	if (len + 6 > (int)sizeof m) return -1;
	p = put16(p, CM_L2CA_DATA_REQ); p = put16(p, cid); p = put16(p, len);
	memcpy(p, data, len);
	data_pending++; tx_count++;
	return pc_send(pc, CLASS_CM, m, len + 6);
}

int btcm_l2_disconnect(int cid)
{
	if (cid < 0) return -1;
	unsigned char m[4]; put16(m, CM_L2CA_DISCONNECT_REQ); put16(m + 2, cid);
	return pc_send(pc, CLASS_CM, m, 4);
}

void btcm_l2_active(int cid)
{
	if (cid < 0) return;
	unsigned char m[5], *p = m;
	p = put16(p, CM_L2CA_MODE_CHANGE_REQ); p = put16(p, cid); *p++ = 0;
	pc_send(pc, CLASS_CM, m, 5);
}

void btcm_set_pin(const char *pin) { sc_set_pin(pin); }

int btcm_bond(const struct bd_addr *a, const char *pin)
{
	sc_set_pin(pin);
	unsigned char m[24], *p = m;
	p = put16(p, SC_BOND_REQ); p = put16(p, phandle); p = put_bd_sc(p, a);
	memset(p, 0, 7); p += 7;
	return pc_send(pc, CLASS_SC, m, (int)(p - m));
}

int btcm_debond(const struct bd_addr *a)
{
	unsigned char m[12], *p = m;
	p = put16(p, SC_DEBOND_REQ); p = put16(p, phandle); p = put_bd_sc(p, a);
	return pc_send(pc, CLASS_SC, m, (int)(p - m));
}
