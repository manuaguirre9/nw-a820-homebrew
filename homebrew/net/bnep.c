/* bnep.c: BNEP (Bluetooth Network Encapsulation Protocol, PAN profile) framing in userland, on top of a raw
 * L2CAP channel to the phone's NAP on PSM 0x000F. Ethernet frames from lwIP are compressed/expanded here;
 * the L2CAP transport itself (CSR CM_L2CA_* primitives over /dev/peer_com) lives in btcm.c.
 * Reference: Bluetooth PAN profile / BNEP spec 1.0. */
#include "bnep.h"
#include <string.h>
#include <stdio.h>

extern void lg(const char *fmt, ...);

#define BNEP_GENERAL 0x00
#define BNEP_CONTROL 0x01
#define BNEP_COMPRESSED 0x02
#define BNEP_COMPRESSED_SRC_ONLY 0x03
#define BNEP_COMPRESSED_DST_ONLY 0x04
#define BNEP_EXT 0x80

#define CTRL_COMMAND_NOT_UNDERSTOOD 0x00
#define CTRL_SETUP_REQ 0x01
#define CTRL_SETUP_RSP 0x02
#define CTRL_FILTER_NET_SET 0x03
#define CTRL_FILTER_NET_RSP 0x04
#define CTRL_FILTER_MC_SET 0x05
#define CTRL_FILTER_MC_RSP 0x06

#define UUID_PANU 0x1115
#define UUID_NAP 0x1116

static unsigned char local_mac[6], remote_mac[6];
static int state;	/* 0 idle, 1 setup sent, 2 connected, -1 failed */
static int (*l2_send)(const unsigned char *, int);
static void (*eth_in)(const unsigned char *, int);
static unsigned char txbuf[1700];

void bnep_init(const unsigned char lmac[6], const unsigned char rmac[6], int (*send)(const unsigned char *, int), void (*input)(const unsigned char *, int))
{
	memcpy(local_mac, lmac, 6); memcpy(remote_mac, rmac, 6);
	l2_send = send; eth_in = input; state = 0;
}

int bnep_state(void) { return state; }

/* SETUP_CONNECTION_REQUEST: we are a PANU, the phone is the NAP */
int bnep_setup(void)
{
	unsigned char m[] = { BNEP_CONTROL, CTRL_SETUP_REQ, 2, UUID_NAP >> 8, UUID_NAP & 255, UUID_PANU >> 8, UUID_PANU & 255 };
	state = 1;
	return l2_send(m, sizeof m);
}

static void control(const unsigned char *p, int n)
{
	if (n < 1) return;
	switch (p[0]) {
	case CTRL_SETUP_RSP:
		if (n >= 3) { int code = p[1] << 8 | p[2]; lg("bnep: setup response %d", code); state = code == 0 ? 2 : -1; }
		break;
	case CTRL_SETUP_REQ: {	/* a NAP normally doesn't ask us, but answer "not allowed" politely */
		unsigned char r[] = { BNEP_CONTROL, CTRL_SETUP_RSP, 0, 4 };
		l2_send(r, sizeof r);
		break;
	}
	case CTRL_FILTER_NET_SET: { unsigned char r[] = { BNEP_CONTROL, CTRL_FILTER_NET_RSP, 0, 0 }; l2_send(r, sizeof r); break; }
	case CTRL_FILTER_MC_SET: { unsigned char r[] = { BNEP_CONTROL, CTRL_FILTER_MC_RSP, 0, 0 }; l2_send(r, sizeof r); break; }
	case CTRL_FILTER_NET_RSP: case CTRL_FILTER_MC_RSP: break;
	default: {
		unsigned char r[] = { BNEP_CONTROL, CTRL_COMMAND_NOT_UNDERSTOOD, p[0] };
		l2_send(r, sizeof r);
	}
	}
}

/* an L2CAP packet arrived: parse the BNEP header, rebuild the Ethernet frame */
void bnep_input(const unsigned char *p, int n)
{
	if (n < 1) return;
	int type = p[0] & 0x7f, ext = p[0] & BNEP_EXT;
	const unsigned char *dst = local_mac, *src = remote_mac;
	int pos = 1;
	static unsigned char frame[1600];
	switch (type) {
	case BNEP_CONTROL:
		lg("bnep rx: control %02x len %d", n > 1 ? p[1] : -1, n);
		control(p + 1, n - 1);
		return;
	case BNEP_GENERAL: if (n < 15) return; dst = p + 1; src = p + 7; pos = 13; break;
	case BNEP_COMPRESSED: if (n < 3) return; pos = 1; break;
	case BNEP_COMPRESSED_SRC_ONLY: if (n < 9) return; src = p + 1; pos = 7; break;
	case BNEP_COMPRESSED_DST_ONLY: if (n < 9) return; dst = p + 1; pos = 7; break;
	default: return;
	}
	if (pos + 2 > n) return;
	int ethertype = p[pos] << 8 | p[pos + 1];
	{ static int tr; if (tr++ < 40) lg("bnep rx: type %d ext %d ethertype %04x len %d dst %02x:%02x:%02x:%02x:%02x:%02x", type, !!ext, ethertype, n, dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]); }
	pos += 2;
	while (ext && pos + 2 <= n) {	/* skip extension headers: type, length, data */
		int len = p[pos + 1];
		ext = p[pos] & BNEP_EXT;
		pos += 2 + len;
	}
	if (pos > n) return;
	int payload = n - pos;
	if (14 + payload > (int)sizeof frame) return;
	memcpy(frame, dst, 6); memcpy(frame + 6, src, 6);
	frame[12] = (unsigned char)(ethertype >> 8); frame[13] = (unsigned char)ethertype;
	memcpy(frame + 14, p + pos, payload);
	if (eth_in) eth_in(frame, 14 + payload);
}

/* an Ethernet frame from lwIP: compress the header when it is the usual local->remote unicast */
int bnep_send_ethernet(const unsigned char *frame, int len)
{
	if (state != 2 || len < 14) return -1;
	const unsigned char *dst = frame, *src = frame + 6;
	int pos;
	if (!memcmp(dst, remote_mac, 6) && !memcmp(src, local_mac, 6)) { txbuf[0] = BNEP_COMPRESSED; pos = 1; }
	else if (!memcmp(src, local_mac, 6)) { txbuf[0] = BNEP_COMPRESSED_DST_ONLY; memcpy(txbuf + 1, dst, 6); pos = 7; }
	else { txbuf[0] = BNEP_GENERAL; memcpy(txbuf + 1, dst, 6); memcpy(txbuf + 7, src, 6); pos = 13; }
	txbuf[pos] = frame[12]; txbuf[pos + 1] = frame[13]; pos += 2;
	int payload = len - 14;
	if (pos + payload > (int)sizeof txbuf) return -1;
	memcpy(txbuf + pos, frame + 14, payload);
	{ static int tr; if (tr++ < 40) lg("bnep tx: type %d ethertype %02x%02x len %d", txbuf[0], frame[12], frame[13], pos + payload); }
	int r = l2_send(txbuf, pos + payload);
	if (r) lg("bnep tx: send failed");
	return r;
}
