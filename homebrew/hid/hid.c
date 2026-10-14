/* hid.c: Bluetooth HID device (boot-compatible keyboard, report ID 1, + consumer control, report ID 2) for the
 * NW-A829, on Sony's BCHS stack through btcm. The host (phone/PC) pairs with us (legacy PIN) and opens L2CAP
 * PSM 0x11 (control) then 0x13 (interrupt); or we reconnect to the stored host the same way.
 * HIDP: control messages are answered (HANDSHAKE, GET/SET_REPORT, GET/SET_PROTOCOL, GET/SET_IDLE,
 * VIRTUAL_CABLE_UNPLUG); input reports go out on the interrupt channel as 0xA1 <id> <data>. */
#include "hid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void lg(const char *fmt, ...);
extern double now(void);

#define PSM_CTRL 0x11
#define PSM_INTR 0x13
#define L2_MTU 672
#define UUID_HID 0x1124
#define SECL_OUT 0x0028		/* what homebrew/net uses for outgoing connections (proven) */

/* HIDP headers */
#define HIDP_HANDSHAKE 0x00
#define HIDP_CONTROL 0x10
#define HIDP_GET_REPORT 0x40
#define HIDP_SET_REPORT 0x50
#define HIDP_GET_PROTOCOL 0x60
#define HIDP_SET_PROTOCOL 0x70
#define HIDP_GET_IDLE 0x80
#define HIDP_SET_IDLE 0x90
#define HIDP_DATA 0xA0
#define HS_OK 0x0
#define HS_INVALID_ID 0x2
#define HS_UNSUPPORTED 0x3
#define HS_INVALID_PARAM 0x4

/* ---------- report descriptor ---------- */
static const unsigned char report_desc[] = {
	/* report ID 1: boot-compatible keyboard, 8 bytes: modifiers, reserved, 6 keys; output: 5 LEDs */
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
	0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xC0,
	/* report ID 2: consumer control, 8 one-bit buttons */
	0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
	0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x0A, 0x24, 0x02, 0x0A, 0x23, 0x02,
	0x81, 0x02,
	0xC0,
};

/* ---------- SDP record: attribute id/value pairs in ascending order, no 0x0000 handle (SDS adds it) -- the
 * format of the records Sony's own AVRCP code registers (hci_bchs.ko .rodata+0x574) ---------- */
static unsigned char rec[600];
static int rl;
static void b1(unsigned v) { rec[rl++] = (unsigned char)v; }
static void b2(unsigned v) { b1(v >> 8); b1(v); }
static void attr(unsigned id) { b1(0x09); b2(id); }
static void u8v(unsigned v) { b1(0x08); b1(v); }
static void u16v(unsigned v) { b1(0x09); b2(v); }
static void boolv(int v) { b1(0x28); b1(v ? 1 : 0); }
static void uuid16(unsigned v) { b1(0x19); b2(v); }
static void strv(const char *s) { int n = (int)strlen(s); b1(0x25); b1(n); memcpy(rec + rl, s, n); rl += n; }
static int seq_open(void) { b1(0x35); b1(0); return rl - 1; }	/* returns the index of the length byte */
static void seq_close(int at) { rec[at] = (unsigned char)(rl - at - 1); }

static void build_record(const struct hid_cfg *c)
{
	int s, t, u;
	rl = 0;
	attr(0x0001); s = seq_open(); uuid16(UUID_HID); seq_close(s);			/* ServiceClassIDList */
	attr(0x0004); s = seq_open();							/* ProtocolDescriptorList */
		t = seq_open(); uuid16(0x0100); u16v(PSM_CTRL); seq_close(t);		/*  L2CAP, PSM 0x11 */
		t = seq_open(); uuid16(0x0011); seq_close(t);				/*  HIDP */
	seq_close(s);
	attr(0x0005); s = seq_open(); uuid16(0x1002); seq_close(s);			/* BrowseGroupList: public */
	attr(0x0006); s = seq_open(); u16v(0x656E); u16v(0x006A); u16v(0x0100); seq_close(s);	/* LanguageBase: en, UTF-8, 0x100 */
	attr(0x0009); s = seq_open(); t = seq_open(); uuid16(UUID_HID); u16v(0x0101); seq_close(t); seq_close(s);	/* Profile: HID 1.1 */
	attr(0x000D); s = seq_open(); t = seq_open();					/* AdditionalProtocolDescriptorLists */
		u = seq_open(); uuid16(0x0100); u16v(PSM_INTR); seq_close(u);
		u = seq_open(); uuid16(0x0011); seq_close(u);
	seq_close(t); seq_close(s);
	attr(0x0100); strv(c->name);							/* ServiceName */
	attr(0x0101); strv("Keyboard and media keys");				/* ServiceDescription */
	attr(0x0102); strv("Walkman homebrew");						/* ProviderName */
	attr(0x0200); u16v(0x0100);							/* HIDDeviceReleaseNumber */
	attr(0x0201); u16v(0x0111);							/* HIDParserVersion */
	attr(0x0202); u8v(0x40);							/* HIDDeviceSubclass: keyboard (no pointing collection) */
	attr(0x0203); u8v(0x00);							/* HIDCountryCode */
	attr(0x0204); boolv(1);								/* HIDVirtualCable */
	attr(0x0205); boolv(1);								/* HIDReconnectInitiate */
	attr(0x0206); s = seq_open(); t = seq_open(); u8v(0x22);			/* HIDDescriptorList: report descriptor */
		b1(0x25); b1(sizeof report_desc); memcpy(rec + rl, report_desc, sizeof report_desc); rl += sizeof report_desc;
	seq_close(t); seq_close(s);
	attr(0x0207); s = seq_open(); t = seq_open(); u16v(0x0409); u16v(0x0100); seq_close(t); seq_close(s);	/* HIDLANGIDBaseList */
	attr(0x0209); boolv(1);								/* HIDBatteryPower */
	attr(0x020A); boolv(1);								/* HIDRemoteWake */
	attr(0x020B); u16v(0x0100);							/* HIDProfileVersion */
	attr(0x020C); u16v(0x0C80);							/* HIDSupervisionTimeout (2 s) */
	attr(0x020D); boolv(1);								/* HIDNormallyConnectable */
	attr(0x020E); boolv(0);								/* HIDBootDevice */
}

/* ---------- state ---------- */
static struct hid_cfg cfg;
static int ctrl_cid = -1, intr_cid = -1;
static int acc_ctrl, acc_intr;		/* an accept is pending on that PSM */
static double acc_retry_at;
static enum hid_state st = HID_IDLE;
static double st_since;
static int outgoing;			/* the current connection was made by us (reconnect) */
static struct bd_addr host, cur;	/* stored host, peer of the current connection */
static int have_host;
static unsigned long sdp_handle;
static int protocol = 1;		/* 1 = report, 0 = boot */
static unsigned char idle_rate, leds;
static unsigned char kb[8];		/* modifiers, reserved, 6 keys */
static unsigned char cc;		/* consumer bits */
static char host_str[20];

static void set_state(enum hid_state s)
{
	static const char *nm[] = { "idle", "listening", "connecting", "control-only", "connected" };
	if (s == st) return;
	lg("hid: state %s -> %s", nm[st], nm[s]);
	if (s == HID_CONNECTED) btcm_scan_enable(0, 1);		/* not discoverable while in use */
	if (s == HID_LISTENING && st != HID_IDLE) btcm_scan_enable(1, 1);
	st = s; st_since = now();
}

enum hid_state hid_state(void) { return st; }
const char *hid_state_str(void)
{
	switch (st) {
	case HID_IDLE: return "Apagado";
	case HID_LISTENING: return "Esperando conexion";
	case HID_CONNECTING: return "Reconectando...";
	case HID_CTRL: return "Conectando (control)";
	case HID_CONNECTED: return "Conectado";
	}
	return "?";
}
const char *hid_host_str(void) { return have_host ? host_str : ""; }
int hid_have_host(void) { return have_host; }

static void load_host(void)
{
	char line[64];
	FILE *f = fopen(cfg.host_file, "r");
	have_host = 0;
	if (!f) return;
	if (fgets(line, sizeof line, f) && !bd_parse(line, &host)) { have_host = 1; bd_fmt(&host, host_str); }
	fclose(f);
	if (have_host) lg("hid: stored host %s", host_str);
}

static void save_host(const struct bd_addr *a)
{
	if (!a->lap && !a->nap) return;
	if (have_host && a->lap == host.lap && a->uap == host.uap && a->nap == host.nap) return;
	host = *a; have_host = 1; bd_fmt(&host, host_str);
	FILE *f = fopen(cfg.host_file, "w");
	if (f) { fprintf(f, "%s\n", host_str); fclose(f); sync(); }
	lg("hid: host %s stored in %s", host_str, cfg.host_file);
}

static void forget_host(void)
{
	if (have_host) { lg("hid: forgetting host %s (virtual cable unplug)", host_str); btcm_debond(&host); }
	have_host = 0; host_str[0] = 0;
	unlink(cfg.host_file); sync();
}

/* ---------- HIDP ---------- */
static int send_ctrl(const unsigned char *p, int n) { return ctrl_cid < 0 ? -1 : btcm_l2_send(ctrl_cid, p, n); }
static void handshake(int r) { unsigned char b = (unsigned char)(HIDP_HANDSHAKE | r); send_ctrl(&b, 1); }

static int send_input(int id, int on_ctrl)
{
	unsigned char b[12]; int n = 0;
	b[n++] = HIDP_DATA | 0x01;	/* DATA, input */
	if (protocol == 0) {		/* boot protocol: keyboard only, no report ID */
		if (id != 1) return -1;
		memcpy(b + n, kb, 8); n += 8;
	} else if (id == 1) { b[n++] = 1; memcpy(b + n, kb, 8); n += 8; }
	else if (id == 2) { b[n++] = 2; b[n++] = cc; }
	else return -1;
	if (on_ctrl) return send_ctrl(b, n);
	if (intr_cid < 0) return -1;
	return btcm_l2_send(intr_cid, b, n);
}

static void control_msg(const unsigned char *p, int n)
{
	if (n < 1) return;
	unsigned t = p[0] & 0xF0, prm = p[0] & 0x0F;
	char hx[3 * 16 + 1]; int k, m = n < 16 ? n : 16;
	for (k = 0; k < m; k++) sprintf(hx + 3 * k, " %02x", p[k]);
	hx[3 * m] = 0;
	lg("hid: control rx:%s", hx);
	switch (t) {
	case HIDP_HANDSHAKE: break;
	case HIDP_CONTROL:
		if (prm == 5) {	/* VIRTUAL_CABLE_UNPLUG */
			forget_host();
			hid_disconnect();
		} else lg("hid: control op %u (%s)", prm, prm == 3 ? "suspend" : prm == 4 ? "exit suspend" : prm == 1 || prm == 2 ? "reset" : "nop");
		break;
	case HIDP_GET_REPORT: {
		int rtype = prm & 3, id = protocol ? (n > 1 ? p[1] : 0) : 1;
		if (rtype == 1 && (id == 1 || id == 2)) { if (send_input(id, 1)) handshake(HS_INVALID_ID); }
		else if (rtype == 2 && id == 1) {
			unsigned char b[3]; int k = 0;
			b[k++] = HIDP_DATA | 0x02; if (protocol) b[k++] = 1; b[k++] = leds;
			send_ctrl(b, k);
		}
		else handshake(HS_INVALID_ID);
		break;
	}
	case HIDP_SET_REPORT:
		if ((prm & 3) == 2 && n >= 2) { leds = p[n - 1]; lg("hid: LEDs 0x%02x", leds); }
		handshake(HS_OK);
		break;
	case HIDP_GET_PROTOCOL: { unsigned char b[2] = { HIDP_DATA, (unsigned char)protocol }; send_ctrl(b, 2); break; }
	case HIDP_SET_PROTOCOL: protocol = prm & 1; lg("hid: protocol %s", protocol ? "report" : "boot"); handshake(HS_OK); break;
	case HIDP_GET_IDLE: { unsigned char b[2] = { HIDP_DATA, idle_rate }; send_ctrl(b, 2); break; }
	case HIDP_SET_IDLE: if (n > 1) idle_rate = p[1]; handshake(HS_OK); break;
	case HIDP_DATA: handshake(HS_OK); break;
	default: handshake(HS_UNSUPPORTED);
	}
}

static void intr_msg(const unsigned char *p, int n)
{
	if (n >= 3 && p[0] == (HIDP_DATA | 0x02) && p[1] == 1) { leds = p[2]; lg("hid: LEDs 0x%02x", leds); return; }
	if (n == 2 && p[0] == (HIDP_DATA | 0x02) && protocol == 0) { leds = p[1]; lg("hid: LEDs 0x%02x (boot)", leds); return; }
	lg("hid: interrupt rx %d bytes, first 0x%02x", n, n ? p[0] : 0);
}

/* ---------- btcm callbacks ---------- */
static void on_up(int psm, int cid, int mtu, const struct bd_addr *a, int incoming)
{
	if (psm != PSM_CTRL && psm != PSM_INTR) psm = ctrl_cid < 0 ? PSM_CTRL : PSM_INTR;	/* should not happen */
	if (incoming) { if (psm == PSM_CTRL) acc_ctrl = 0; else acc_intr = 0; }
	char b[20]; bd_fmt(a, b);
	if (psm == PSM_CTRL) {
		if (ctrl_cid >= 0) { lg("hid: second control channel from %s refused", b); btcm_l2_disconnect(cid); return; }
		ctrl_cid = cid; outgoing = !incoming; cur = *a; protocol = 1;
		lg("hid: control channel up (cid 0x%04x, mtu %d) with %s, %s", cid, mtu, b, incoming ? "host connected to us" : "we reconnected");
		set_state(HID_CTRL);
		if (!incoming) btcm_l2_connect(&cur, PSM_INTR, L2_MTU, SECL_OUT);
	} else {
		if (intr_cid >= 0 || ctrl_cid < 0) { lg("hid: unexpected interrupt channel from %s refused", b); btcm_l2_disconnect(cid); return; }
		intr_cid = cid;
		lg("hid: interrupt channel up (cid 0x%04x, mtu %d): HID connected", cid, mtu);
		set_state(HID_CONNECTED);
		if (cur.lap || cur.nap) save_host(&cur); else if (a->lap || a->nap) save_host(a);
	}
}

static void on_fail(int psm, int result, int incoming)
{
	if (incoming) {
		if (psm == PSM_CTRL) acc_ctrl = 0; else if (psm == PSM_INTR) acc_intr = 0; else acc_ctrl = acc_intr = 0;
		acc_retry_at = now() + 1;
		return;
	}
	lg("hid: outgoing connect to psm 0x%02x failed (0x%02x)", psm, result);
	if (psm == PSM_INTR && ctrl_cid >= 0) btcm_l2_disconnect(ctrl_cid);
	if (ctrl_cid < 0) set_state(HID_LISTENING);
}

static void on_down(int cid, int reason)
{
	(void)reason;
	if (cid == intr_cid) { intr_cid = -1; lg("hid: interrupt channel closed"); if (ctrl_cid >= 0) { set_state(HID_CTRL); btcm_l2_disconnect(ctrl_cid); } }
	else if (cid == ctrl_cid) { ctrl_cid = -1; lg("hid: control channel closed"); if (intr_cid >= 0) btcm_l2_disconnect(intr_cid); }
	if (ctrl_cid < 0 && intr_cid < 0) { memset(kb, 0, sizeof kb); cc = 0; set_state(HID_LISTENING); }
}

static void on_data(int cid, const unsigned char *p, int n)
{
	if (cid == ctrl_cid) control_msg(p, n);
	else if (cid == intr_cid) intr_msg(p, n);
	else lg("hid: data on unknown cid 0x%04x (%d bytes)", cid, n);
}

static void on_bond(const struct bd_addr *a, int result)
{
	char b[20]; bd_fmt(a, b);
	lg("hid: pairing with %s %s (%d)", b, result == 0 ? "OK" : "FAILED", result);
	if (result == 0) save_host(a);
}

/* ---------- API ---------- */
int hid_init(const struct hid_cfg *c)
{
	cfg = *c;
	if (!cfg.name) cfg.name = "Walkman NW-A829 Remote";
	if (!cfg.pin) cfg.pin = "0000";
	if (!cfg.secl) cfg.secl = 0x0006;
	if (!cfg.host_file) cfg.host_file = "/var/hb/bt/hid_host";
	static const struct btcm_cb cbs = { on_up, on_fail, on_down, on_data, on_bond };
	btcm_set_cb(&cbs);
	btcm_set_pin(cfg.pin);
	load_host();
	int r = 0;
	if (btcm_set_name(cfg.name)) { lg("hid: set name: no confirm"); r |= 1; }
	lg("hid: class of device 0x%06x", cfg.cod);
	if (btcm_set_cod(cfg.cod & 0xffe000, cfg.cod & 0x1f00, cfg.cod & 0xfc)) { lg("hid: set class of device: no confirm"); r |= 2; }
	build_record(&cfg);
	{
		char hx[3 * 32 + 1]; int k;
		for (k = 0; k < 32 && k < rl; k++) sprintf(hx + 3 * k, " %02x", rec[k]);
		lg("hid: SDP record %d bytes, report descriptor %d bytes:%s ...", rl, (int)sizeof report_desc, hx);
	}
	int sr = btcm_sds_register(rec, rl, &sdp_handle);
	if (sr) { lg("hid: SDP register FAILED (%d)", sr); r |= 4; }
	else lg("hid: SDP record registered, handle 0x%08lx", sdp_handle);
	if (btcm_l2_register(PSM_CTRL)) r |= 8;
	if (btcm_l2_register(PSM_INTR)) r |= 8;
	btcm_l2_accept(PSM_CTRL, cfg.secl, L2_MTU, UUID_HID); acc_ctrl = 1;
	btcm_l2_accept(PSM_INTR, cfg.secl, L2_MTU, UUID_HID); acc_intr = 1;
	btcm_scan_enable(1, 1);
	btcm_connectable(1);
	st = HID_LISTENING; st_since = now();
	lg("hid: ready (discoverable as \"%s\", PIN %s), init flags 0x%x", cfg.name, cfg.pin, r);
	return r;
}

void hid_tick(void)
{
	if (st == HID_IDLE) return;
	double t = now();
	if (t >= acc_retry_at) {
		if (!acc_ctrl && ctrl_cid < 0) { btcm_l2_accept(PSM_CTRL, cfg.secl, L2_MTU, UUID_HID); acc_ctrl = 1; }
		if (!acc_intr && intr_cid < 0) { btcm_l2_accept(PSM_INTR, cfg.secl, L2_MTU, UUID_HID); acc_intr = 1; }
	}
	if (st == HID_CONNECTING && t - st_since > 40) { lg("hid: reconnect timed out"); set_state(HID_LISTENING); }
	if (st == HID_CTRL && t - st_since > 30) { lg("hid: no interrupt channel after 30 s, dropping"); if (ctrl_cid >= 0) btcm_l2_disconnect(ctrl_cid); set_state(HID_LISTENING); ctrl_cid = -1; }
}

int hid_reconnect(void)
{
	if (!have_host) { lg("hid: no stored host to reconnect to"); return -1; }
	if (st != HID_LISTENING) return -1;
	lg("hid: reconnecting to %s", host_str);
	cur = host;
	set_state(HID_CONNECTING);
	return btcm_l2_connect(&host, PSM_CTRL, L2_MTU, SECL_OUT);
}

void hid_disconnect(void)
{
	if (intr_cid >= 0) btcm_l2_disconnect(intr_cid);
	double t0 = now();
	while (intr_cid >= 0 && now() - t0 < 2) btcm_poll(50);
	if (ctrl_cid >= 0) btcm_l2_disconnect(ctrl_cid);
	t0 = now();
	while (ctrl_cid >= 0 && now() - t0 < 2) btcm_poll(50);
	intr_cid = ctrl_cid = -1;
	if (st != HID_IDLE) set_state(HID_LISTENING);
}

void hid_shutdown(void)
{
	hid_disconnect();
	if (acc_ctrl) btcm_l2_cancel_accept(PSM_CTRL);
	if (acc_intr) btcm_l2_cancel_accept(PSM_INTR);
	acc_ctrl = acc_intr = 0;
	double t0 = now();
	while (now() - t0 < 0.5) btcm_poll(50);
	if (sdp_handle) btcm_sds_unregister(sdp_handle);
	sdp_handle = 0;
	btcm_scan_enable(0, 0);
	set_state(HID_IDLE);
}

int hid_key(unsigned usage, int down)
{
	int i;
	if (usage >= 0xE0 && usage <= 0xE7) {
		if (down) kb[0] |= (unsigned char)(1 << (usage - 0xE0)); else kb[0] &= (unsigned char)~(1 << (usage - 0xE0));
	} else if (down) {
		for (i = 2; i < 8; i++) if (kb[i] == usage) break;
		if (i == 8) { for (i = 2; i < 8 && kb[i]; i++) continue; if (i == 8) return -1; kb[i] = (unsigned char)usage; }
	} else {
		for (i = 2; i < 8; i++) if (kb[i] == usage) { memmove(kb + i, kb + i + 1, 7 - i); kb[7] = 0; break; }
	}
	return send_input(1, 0);
}

int hid_consumer(unsigned bit, int down)
{
	if (down) cc |= (unsigned char)bit; else cc &= (unsigned char)~bit;
	return send_input(2, 0);
}

int hid_tap_consumer(unsigned bit) { int r = hid_consumer(bit, 1); r |= hid_consumer(bit, 0); return r; }
