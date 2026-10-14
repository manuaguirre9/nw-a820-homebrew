/* net.c: TCP/IP for the NW-A829 homebrew over a Bluetooth PAN link. lwIP (NO_SYS, raw API) sits on an Ethernet
 * netif whose frames are carried by BNEP (bnep.c); DHCP + DNS; HTTPS through mbedTLS (lwIP altcp_tls).
 * The player calls net_poll() from its loops (like the audio engine). Downloads go to a file with progress. */
#include "net.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "netif/ethernet.h"
#include "zlib.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern void lg(const char *fmt, ...);
extern void set_cloexec(int fd);

static struct netif nif;
static int link_up, have_ip;
static struct altcp_tls_config *tls_cfg;

/* TLS speed-ups for the ARM926 (no crypto hardware):
 * - key exchange curves: X25519 and P-256 first (mbedTLS's default order starts with P-521 / brainpool, which a
 *   server that follows the client's order picks and which costs seconds here)
 * - session resumption: the session of the last few servers is kept; reconnecting to one (next feed file, cover,
 *   transcript, chapters, the next episode on the same CDN) skips the public-key work */
static void tls_tune(struct altcp_tls_config *c)
{
	static const mbedtls_ecp_group_id curves[] = { MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_SECP384R1,
		MBEDTLS_ECP_DP_SECP521R1, MBEDTLS_ECP_DP_NONE };
	if (c) mbedtls_ssl_conf_curves((mbedtls_ssl_config *)c, curves);	/* the config is the struct's first member */
}

#define NSESS 4
static struct { char host[128]; int used; mbedtls_ssl_session s; unsigned age; } sess[NSESS];
static unsigned sess_clock;

static void sess_restore(mbedtls_ssl_context *ssl, const char *host)
{
	for (int i = 0; i < NSESS; i++)
		if (sess[i].used && !strcmp(sess[i].host, host)) {
			if (mbedtls_ssl_set_session(ssl, &sess[i].s) == 0) lg("net: %s: resuming the TLS session", host);
			return;
		}
}

static void sess_save(mbedtls_ssl_context *ssl, const char *host)
{
	int k = -1;
	for (int i = 0; i < NSESS && k < 0; i++) if (sess[i].used && !strcmp(sess[i].host, host)) k = i;
	if (k < 0) for (int i = 0; i < NSESS && k < 0; i++) if (!sess[i].used) k = i;
	if (k < 0) { k = 0; for (int i = 1; i < NSESS; i++) if (sess[i].age < sess[k].age) k = i; }	/* oldest */
	if (sess[k].used) mbedtls_ssl_session_free(&sess[k].s);
	mbedtls_ssl_session_init(&sess[k].s);
	if (mbedtls_ssl_get_session(ssl, &sess[k].s) == 0) {
		snprintf(sess[k].host, sizeof sess[k].host, "%s", host);
		sess[k].used = 1; sess[k].age = ++sess_clock;
	} else { mbedtls_ssl_session_free(&sess[k].s); sess[k].used = 0; }
}
static int (*send_frame)(const unsigned char *frame, int len);

u32_t sys_now(void)
{
	struct timeval tv; gettimeofday(&tv, 0);
	return (u32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* ---- netif glue ---- */

static int win_last = -1, win_min = 65535, win_zero, win_acks;
extern double now(void);
static unsigned long isn_srv; static int trace_n; static double trace_t0;
int net_trace_max;	/* packet trace lines to log (pantest sets 400; the player keeps 0) */
static void trace_pkt(const char *dir, unsigned long seq, unsigned long ack, int plen, int win, int flags)
{
	if (!isn_srv || trace_n >= net_trace_max) return;
	if (!trace_t0) trace_t0 = now();
	trace_n++;
	lg("tr %7.0f %s seq %lu ack %lu len %d win %d fl %02x", (now() - trace_t0) * 1000, dir, seq, ack, plen, win, flags);
}
static err_t nif_output(struct netif *n, struct pbuf *p)
{
	static unsigned char frame[1600];
	(void)n;
	if (p->tot_len > sizeof frame) return ERR_BUF;
	pbuf_copy_partial(p, frame, p->tot_len, 0);
	/* trace the receive window we advertise to the server */
	if (p->tot_len >= 54 && frame[12] == 8 && frame[13] == 0 && frame[23] == 6) {
		const unsigned char *ip = frame + 14, *t = ip + (ip[0] & 15) * 4;
		int dport = t[2] << 8 | t[3];
		if (dport == 443 || dport == 80) {
			int w = t[14] << 8 | t[15];
			{ int iplen = ip[2] << 8 | ip[3], hl = (t[12] >> 4) * 4;
			  unsigned long ack = (unsigned long)t[8] << 24 | t[9] << 16 | t[10] << 8 | t[11];
			  trace_pkt("OUT", 0, ack - isn_srv, iplen - (int)(t - ip) - hl, w, t[13]); }
			win_last = w; if (w < win_min) win_min = w; if (w == 0) win_zero++; win_acks++;
		}
	}
	return send_frame && send_frame(frame, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

static err_t nif_init(struct netif *n)
{
	n->name[0] = 'b'; n->name[1] = 't';
	n->output = etharp_output;
	n->linkoutput = nif_output;
	n->mtu = 1500;
	n->hwaddr_len = 6;
	n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
	n->hostname = "walkman";
	return ERR_OK;
}

static void status_cb(struct netif *n)
{
	have_ip = netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));
	if (have_ip) {
		char a[16], g[16];
		snprintf(a, sizeof a, "%s", ip4addr_ntoa(netif_ip4_addr(n)));
		snprintf(g, sizeof g, "%s", ip4addr_ntoa(netif_ip4_gw(n)));
		lg("net: ip %s gw %s dns %s", a, g, ipaddr_ntoa(dns_getserver(0)));
	}
}

/* a received Ethernet frame from BNEP */
static unsigned long tcp_next; static int tcp_gaps, tcp_dups, tcp_segs, tcp_have;
void net_tcp_stats(void)
{
	lg("net tcp: %d segs from server, %d gaps, %d dups | our window last %d min %d, %d zero of %d acks | pbuf pool used %d/%d",
		tcp_segs, tcp_gaps, tcp_dups, win_last, win_min, win_zero, win_acks, (int)lwip_stats.memp[MEMP_PBUF_POOL]->used, PBUF_POOL_SIZE);
	lg("net lwip tcp: recv %d xmit %d drop %d memerr %d err %d proterr %d opterr %d rterr %d chkerr %d lenerr %d",
		lwip_stats.tcp.recv, lwip_stats.tcp.xmit, lwip_stats.tcp.drop, lwip_stats.tcp.memerr, lwip_stats.tcp.err,
		lwip_stats.tcp.proterr, lwip_stats.tcp.opterr, lwip_stats.tcp.rterr, lwip_stats.tcp.chkerr, lwip_stats.tcp.lenerr);
}
void net_input(const unsigned char *frame, int len)
{
	/* trace the server's TCP stream (port 443/80): sequence gaps = lost packets, repeats = retransmissions */
	if (len >= 14 + 40 && frame[12] == 8 && frame[13] == 0 && frame[23] == 6) {
		const unsigned char *ip = frame + 14, *t = ip + (ip[0] & 15) * 4;
		int sport = t[0] << 8 | t[1];
		if (sport == 443 || sport == 80) {
			int iplen = ip[2] << 8 | ip[3], hl = (t[12] >> 4) * 4;
			int plen = iplen - (int)(t - ip) - hl;
			unsigned long seq = (unsigned long)t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7];
			if (t[13] & 2) isn_srv = seq;
			trace_pkt("IN ", seq - isn_srv, 0, plen, t[14] << 8 | t[15], t[13]);
			if (t[13] & 2) { tcp_next = seq + 1; tcp_have = 1; }
			else if (plen > 0 && tcp_have) {
				tcp_segs++;
				long d = (long)(seq - tcp_next);
				if (d > 0) tcp_gaps++;
				else if (d < 0) tcp_dups++;
				if ((long)(seq + plen - tcp_next) > 0) tcp_next = seq + plen;
			}
		}
	}
	/* trace DNS answers: IPv4 (no options assumed) + UDP from port 53 -> header flags */
	if (len >= 14 + 20 + 8 + 12 && frame[12] == 8 && frame[13] == 0 && frame[23] == 17) {
		const unsigned char *u = frame + 14 + (frame[14] & 15) * 4;
		if (u + 20 <= frame + len && u[0] == 0 && u[1] == 53)
			lg("net: dns answer rcode %d answers %d", u[11] & 15, u[14] << 8 | u[15]);
	}
	struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
	if (!p) { static int drops; if (drops++ % 20 == 0) lg("net: no pbuf, frame dropped (%d)", drops); return; }
	pbuf_take(p, frame, (u16_t)len);
	if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
}

void net_init(void)
{
	static int inited;
	if (!inited) { lwip_init(); inited = 1; }
}

/* the phone ignores the first DHCP DISCOVER (lwIP's own retry comes 2 s later): send it again */
void net_dhcp_kick(void)
{
	struct dhcp *d = netif_dhcp_data(&nif);
	/* only while still waiting for an OFFER: restarting after one arrived would throw it away */
	if (link_up && !have_ip && d && d->state == DHCP_STATE_SELECTING) { dhcp_start(&nif); lg("net: dhcp again"); }
}

int net_start(const unsigned char mac[6], int (*send)(const unsigned char *, int))
{
	net_init();
	send_frame = send;
	memcpy(nif.hwaddr, mac, 6);
	netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, 0, nif_init, ethernet_input);
	netif_set_default(&nif);
	netif_set_status_callback(&nif, status_cb);
	netif_set_up(&nif);
	netif_set_link_up(&nif);
	link_up = 1; have_ip = 0;
	dhcp_start(&nif);
	lg("net: dhcp started");
	return 0;
}

void net_stop(void)
{
	if (!link_up) return;
	dhcp_release_and_stop(&nif);
	netif_set_link_down(&nif);
	netif_set_down(&nif);
	netif_remove(&nif);
	link_up = have_ip = 0;
}

int net_ready(void) { return link_up && have_ip; }
void net_poll(void) { if (link_up) sys_check_timeouts(); }

/* ---- ICMP echo to the gateway (the phone): round-trip time of the Bluetooth link alone ---- */

static struct raw_pcb *ping_pcb;
static u16_t ping_seq; static double ping_sent; static int ping_got;
static double ping_rtt;

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
	(void)arg; (void)pcb; (void)addr;
	struct icmp_echo_hdr eh;
	u16_t ihl = (u16_t)((((u8_t *)p->payload)[0] & 15) * 4);
	if (p->tot_len >= ihl + sizeof eh && pbuf_copy_partial(p, &eh, sizeof eh, ihl) == sizeof eh &&
	    eh.type == ICMP_ER && eh.id == PP_HTONS(0x574d) && eh.seqno == lwip_htons(ping_seq)) {
		ping_rtt = (now() - ping_sent) * 1000; ping_got = 1;
		pbuf_free(p);
		return 1;
	}
	return 0;
}

/* sends one echo request of `size` payload bytes and waits up to 3 s; poll() pumps the link. returns ms or -1 */
double net_ping_gw(int size, void (*poll)(void))
{
	if (!have_ip) return -1;
	if (!ping_pcb) {
		ping_pcb = raw_new(IP_PROTO_ICMP);
		if (!ping_pcb) return -1;
		raw_recv(ping_pcb, ping_recv, 0);
		raw_bind(ping_pcb, IP_ADDR_ANY);
	}
	struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)(sizeof(struct icmp_echo_hdr) + size), PBUF_RAM);
	if (!p) return -1;
	struct icmp_echo_hdr *eh = (struct icmp_echo_hdr *)p->payload;
	ICMPH_TYPE_SET(eh, ICMP_ECHO); ICMPH_CODE_SET(eh, 0);
	eh->id = PP_HTONS(0x574d); eh->seqno = lwip_htons(++ping_seq); eh->chksum = 0;
	for (int i = 0; i < size; i++) ((u8_t *)p->payload)[sizeof *eh + i] = (u8_t)i;
	eh->chksum = inet_chksum(eh, p->len);
	ip_addr_t gw; ip_addr_copy_from_ip4(gw, *netif_ip4_gw(&nif));
	ping_got = 0; ping_sent = now();
	raw_sendto(ping_pcb, p, &gw);
	pbuf_free(p);
	while (!ping_got && now() - ping_sent < 3) { poll(); sys_check_timeouts(); }
	return ping_got ? ping_rtt : -1;
}

/* ---- HTTP(S) GET to a file, with resume via Range ---- */
/* HTTP/1.0 on purpose: no chunked transfer encoding to decode. A download that gets under 2 KB in 60 s fails
 * (a half-open TCP link through the phone's NAT, or a trickle, would otherwise hang forever). */

struct dl {
	char host[128], path[1024]; int port, tls;
	struct altcp_pcb *pcb;
	FILE *out; long got, total, start;
	int state;	/* 0 resolving, 1 connecting, 2 headers, 3 body, 4 done, 5 redirect pending, 6 restart from 0, -1 error */
	int redirects, dns_tries, restarts;
	char hdr[4096]; int hdrlen;
	char url[1024]; char redirect[1024];
	double t0, t_progress; long got_seen;
	/* resume safety: <dest>.meta keeps the file's validator (ETag / Last-Modified) and full length from the first
	 * response. A resume sends If-Range, so a server whose file changed (dynamic ad insertion) sends the whole new
	 * file instead of the rest of a different one; a length mismatch starts over from 0. */
	char dest[512], meta_val[256]; long meta_len;
	int peek;	/* the caller reads the file while it downloads (feeds in /tmp): flush every packet */
	/* feeds: conditional GET (If-None-Match / If-Modified-Since -> 304 "not modified") and gzip, inflated on the fly */
	char etag[256], lastmod[64];
	int notmod, gz, gz_done;	/* gz_done: the gzip stream reached its end (a cut one is not a complete feed) */
	z_stream zs;
	double t_conn;	/* when the TCP connect started (handshake timing) */
};

static int ca_loaded;	/* the CA bundle was parsed: certificates are then checked */
static void meta_path(char *p, size_t max);
static void meta_load(void);
static void meta_save(const char *val, long len);
static struct dl D;

static int parse_url(const char *url, struct dl *d)
{
	const char *p = url;
	if (!strncmp(p, "https://", 8)) { d->tls = 1; d->port = 443; p += 8; }
	else if (!strncmp(p, "http://", 7)) { d->tls = 0; d->port = 80; p += 7; }
	else return -1;
	const char *slash = strchr(p, '/');
	size_t hl = slash ? (size_t)(slash - p) : strlen(p);
	if (hl >= sizeof d->host) return -1;
	memcpy(d->host, p, hl); d->host[hl] = 0;
	char *colon = strchr(d->host, ':');
	if (colon) { *colon = 0; d->port = atoi(colon + 1); }
	if (slash && strlen(slash) >= sizeof d->path) return -1;
	snprintf(d->path, sizeof d->path, "%s", slash ? slash : "/");
	return 0;
}

static void dl_fail(const char *why) { lg("net: download failed: %s", why); D.state = -1; }

static void meta_path(char *p, size_t max) { snprintf(p, max, "%s.meta", D.dest); }
static void meta_load(void)
{
	char p[600]; meta_path(p, sizeof p);
	D.meta_val[0] = 0; D.meta_len = -1;
	FILE *f = fopen(p, "r");
	if (!f) return;
	char l[300];
	while (fgets(l, sizeof l, f)) {
		l[strcspn(l, "\r\n")] = 0;
		if (!strncmp(l, "len=", 4)) D.meta_len = atol(l + 4);
		else if (!strncmp(l, "val=", 4)) snprintf(D.meta_val, sizeof D.meta_val, "%s", l + 4);
	}
	fclose(f);
}
static void meta_save(const char *val, long len)
{
	char p[600]; meta_path(p, sizeof p);
	FILE *f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "len=%ld\nval=%s\n", len, val);
	fclose(f);
}

/* header value (case-insensitive name), copied into out; 0 if absent */
static int header(const char *name, char *out, size_t max)
{
	size_t l = strlen(name);
	for (const char *h = strchr(D.hdr, '\n'); h; h = strchr(h + 1, '\n')) {
		if (!strncasecmp(h + 1, name, l) && h[1 + l] == ':') {
			const char *v = h + 2 + l;
			while (*v == ' ' || *v == '\t') v++;
			const char *e = strpbrk(v, "\r\n");
			size_t n = e ? (size_t)(e - v) : strlen(v);
			if (n >= max) return 0;
			memcpy(out, v, n); out[n] = 0;
			return 1;
		}
	}
	return 0;
}

/* body bytes to the file; gzip bodies are inflated as they arrive (so the feed can still be read while it
 * downloads and stopped early). D.got counts bytes on the wire. A failed write (card full, I/O error) fails the
 * download: the file keeps what was written, a prefix of the body, so it can be resumed. */
static void body(const char *d, int n)
{
	D.got += n;
	if (!D.gz) { if (fwrite(d, 1, n, D.out) != (size_t)n) dl_fail("file write"); return; }
	static unsigned char zo[8192];
	D.zs.next_in = (Bytef *)d; D.zs.avail_in = (uInt)n;
	while (D.zs.avail_in > 0) {
		D.zs.next_out = zo; D.zs.avail_out = sizeof zo;
		int r = inflate(&D.zs, Z_NO_FLUSH);
		size_t k = sizeof zo - D.zs.avail_out;
		if (fwrite(zo, 1, k, D.out) != k) { dl_fail("file write"); break; }
		if (r == Z_STREAM_END) { D.gz_done = 1; break; }
		if (r != Z_OK && r != Z_BUF_ERROR) { lg("net: gzip error %d", r); dl_fail("gzip"); break; }
		if (r == Z_BUF_ERROR && D.zs.avail_out == sizeof zo) break;
	}
}

static void gz_end(void) { if (D.gz) { inflateEnd(&D.zs); D.gz = 0; } }

static err_t dl_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
	(void)arg; (void)err;
	if (!p) {	/* the server closed the connection */
		if (D.state == 5 || D.state == 6 || D.state == 4 || D.state == -1) return ERR_OK;	/* redirect / restart pending, already decided */
		/* a known length must match exactly; without one the close is the only end mark (HTTP/1.0), but a gzip
		 * stream must at least have reached its end */
		if (D.state == 3 && (D.total < 0 ? !D.gz || D.gz_done : D.got == D.total)) D.state = 4;
		else { lg("net: closed early (state %d) %ld/%ld", D.state, D.got, D.total); D.state = -1; }
		return ERR_OK;
	}
	altcp_recved(pcb, p->tot_len);
	if (D.state != 2 && D.state != 3) { pbuf_free(p); return ERR_OK; }
	for (struct pbuf *q = p; q; q = q->next) {
		const char *d = q->payload; int n = q->len;
		if (D.state == 2) {	/* headers */
			int prev = D.hdrlen;
			int take = n < (int)sizeof D.hdr - 1 - D.hdrlen ? n : (int)sizeof D.hdr - 1 - D.hdrlen;
			memcpy(D.hdr + D.hdrlen, d, take); D.hdrlen += take; D.hdr[D.hdrlen] = 0;
			char *end = strstr(D.hdr, "\r\n\r\n");
			if (!end) { if (D.hdrlen >= (int)sizeof D.hdr - 1) { dl_fail("headers too long"); break; } continue; }
			int code = atoi(D.hdr + 9);
			lg("net: http %d", code);
			if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
				char loc[1024];
				if (D.redirects < 8 && header("Location", loc, sizeof loc)) {
					if (loc[0] == '/' && loc[1] == '/') snprintf(D.redirect, sizeof D.redirect, "%s:%s", D.tls ? "https" : "http", loc);	/* protocol-relative */
					else if (loc[0] == '/') snprintf(D.redirect, sizeof D.redirect, "%s://%s%s", D.tls ? "https" : "http", D.host, loc);	/* relative */
					else snprintf(D.redirect, sizeof D.redirect, "%s", loc);
					D.state = 5;
				} else dl_fail("redirect");
				break;
			}
			if (code == 416 && D.start > 0) {	/* nothing left to send: complete only if it matches the known length */
				if (D.meta_len == D.start) { lg("net: already complete (416)"); D.total = D.got = D.start; D.state = 4; }
				else { lg("net: 416 but %ld of %ld bytes: starting over", D.start, D.meta_len); D.state = 6; }
				break;
			}
			if (code == 304 && (D.etag[0] || D.lastmod[0])) { lg("net: not modified"); D.notmod = 1; D.total = D.got = 0; D.state = 4; break; }
			if (code != 200 && code != 206) { dl_fail("http status"); break; }
			char ce[32];
			if (header("Content-Encoding", ce, sizeof ce) && strstr(ce, "gzip")) {
				memset(&D.zs, 0, sizeof D.zs);
				if (inflateInit2(&D.zs, 16 + MAX_WBITS) != Z_OK) { dl_fail("gzip init"); break; }
				D.gz = 1; D.gz_done = 0;
			}
			if (code == 200 && D.start) {	/* no Range support, or the file changed (If-Range): start over */
				lg("net: server sent the whole file (%s), starting over", D.meta_val[0] ? "changed" : "no ranges");
				fflush(D.out); if (ftruncate(fileno(D.out), 0)) lg("net: truncate failed");
				fseek(D.out, 0, SEEK_SET); D.got = D.start = 0;
			}
			char cl[32], cr[96];
			D.total = header("Content-Length", cl, sizeof cl) ? D.got + atol(cl) : -1;
			if (code == 206 && header("Content-Range", cr, sizeof cr)) {	/* "bytes a-b/TOTAL" */
				const char *sl = strchr(cr, '/');
				long full = sl && sl[1] != '*' ? atol(sl + 1) : -1;
				if (full > 0 && D.meta_len > 0 && full != D.meta_len) {
					lg("net: file changed on the server (%ld -> %ld bytes), starting over", D.meta_len, full);
					D.state = 6; break;
				}
				if (full > 0) D.total = full;
			}
			if (D.start == 0 || D.meta_len < 0) {	/* remember what this file is, for a later resume */
				char val[256] = "";
				if (!(header("ETag", val, sizeof val) && strncmp(val, "W/", 2))) { val[0] = 0; header("Last-Modified", val, sizeof val); }
				snprintf(D.meta_val, sizeof D.meta_val, "%s", val); D.meta_len = D.total;
				meta_save(val, D.total);
			}
			/* the body starts right after the blank line; skip the header bytes that came in this pbuf */
			int used = (int)(end + 4 - D.hdr) - prev;
			if (used < 0) used = 0;
			d += used; n -= used;
			D.state = 3;
			if (n > 0) body(d, n);
			continue;
		}
		if (D.state == 3) body(d, n);
	}
	pbuf_free(p);
	if (D.out && D.peek) fflush(D.out);	/* podsync peeks at feeds while they download */
	if (D.state == 3 && D.total > 0 && D.got >= D.total) {
		if (D.got == D.total) D.state = 4;
		else { lg("net: %ld bytes, more than the %ld announced", D.got, D.total); dl_fail("length mismatch"); }
	}
	return ERR_OK;
}

/* appends to the request at *n; once something doesn't fit *n becomes -1 and stays (never a cut-off request, and
 * never n past the end: sizeof req - n would wrap and the next snprintf write past the stack buffer) */
static void req_add(char *req, size_t max, int *n, const char *fmt, ...)
{
	if (*n < 0) return;
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(req + *n, max - *n, fmt, ap);
	va_end(ap);
	*n = k < 0 || (size_t)k >= max - *n ? -1 : *n + k;
}

static err_t dl_connected(void *arg, struct altcp_pcb *pcb, err_t err)
{
	(void)arg;
	if (err != ERR_OK) { dl_fail("connect"); return err; }
	if (D.tls) {
		mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
		lg("net: %s: tls %s in %.2f s", D.host, mbedtls_ssl_get_ciphersuite(ssl), sys_now() / 1000.0 - D.t_conn);
	}
	if (D.tls && ca_loaded) {	/* for TLS this runs after the handshake: check the server's certificate */
		uint32_t fl = mbedtls_ssl_get_verify_result((mbedtls_ssl_context *)altcp_tls_context(pcb));
		/* expired / not yet valid are tolerated: the Walkman's clock can't be trusted */
		uint32_t bad = fl & ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
		if (bad) { lg("net: %s: certificate rejected (flags %#lx)", D.host, (unsigned long)fl); dl_fail("certificate"); return ERR_OK; }
		if (fl) lg("net: %s: certificate date outside its validity (clock?), accepted", D.host);
	}
	if (D.tls) sess_save((mbedtls_ssl_context *)altcp_tls_context(pcb), D.host);	/* only a checked session is kept */
	/* room for the longest fields (path 1023 + host 127 + If-Range 255 + If-None-Match 255 + If-Modified-Since 63
	 * + the fixed lines ~250); req_add still refuses to go past the end */
	char req[2048];
	int n = 0;
	req_add(req, sizeof req, &n, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Walkman-NW-A829/1.0\r\nConnection: close\r\nAccept: */*\r\n", D.path, D.host);
	if (D.start > 0) req_add(req, sizeof req, &n, "Range: bytes=%ld-\r\n", D.start);
	if (D.start > 0 && D.meta_val[0]) req_add(req, sizeof req, &n, "If-Range: %s\r\n", D.meta_val);
	if (D.peek && D.start == 0) req_add(req, sizeof req, &n, "Accept-Encoding: gzip\r\n");	/* feeds: 5-10x smaller */
	if (D.etag[0]) req_add(req, sizeof req, &n, "If-None-Match: %s\r\n", D.etag);
	if (D.lastmod[0]) req_add(req, sizeof req, &n, "If-Modified-Since: %s\r\n", D.lastmod);
	req_add(req, sizeof req, &n, "\r\n");
	if (n < 0) { dl_fail("request too long"); return ERR_OK; }
	D.state = 2; D.hdrlen = 0;
	altcp_write(pcb, req, n, TCP_WRITE_FLAG_COPY);
	altcp_output(pcb);
	return ERR_OK;
}

static void dl_err(void *arg, err_t err)
{
	(void)arg;
	lg("net: tcp error %d", err);
	D.pcb = 0;	/* already freed by lwIP */
	if (D.state == 5 || D.state == 6 || D.state == 4 || D.state == -1) return;
	if (D.state == 3 && D.total > 0 && D.got == D.total) D.state = 4; else D.state = -1;
}

static void dl_open(const ip_addr_t *ip)
{
	if (D.tls) {
		if (!tls_cfg) { tls_cfg = altcp_tls_create_config_client(0, 0); tls_tune(tls_cfg); }	/* CA bundle attached by net_set_ca() */
		D.pcb = altcp_tls_new(tls_cfg, IPADDR_TYPE_V4);
		if (D.pcb) {
			mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(D.pcb);
			mbedtls_ssl_set_hostname(ssl, D.host);
			sess_restore(ssl, D.host);
		}
		D.t_conn = sys_now() / 1000.0;
	} else D.pcb = altcp_tcp_new();
	if (!D.pcb) { dl_fail("pcb"); return; }
	altcp_arg(D.pcb, 0);
	altcp_recv(D.pcb, dl_recv);
	altcp_err(D.pcb, dl_err);
	D.state = 1;
	if (altcp_connect(D.pcb, ip, D.port, dl_connected) != ERR_OK) dl_fail("connect call");
}

static unsigned dns_gen;	/* callbacks from an older download are ignored */
static void dns_cb(const char *name, const ip_addr_t *ip, void *arg)
{
	(void)name;
	if ((unsigned)(uintptr_t)arg != dns_gen || D.state != 0) return;
	if (!ip && D.dns_tries < 2) {	/* the phone's resolver failed: fall back to public DNS */
		ip_addr_t s;
		if (D.dns_tries) IP_ADDR4(&s, 1, 1, 1, 1); else IP_ADDR4(&s, 8, 8, 8, 8);
		D.dns_tries++;
		dns_setserver(0, &s);
		lg("net: dns failed, retrying via %s", ipaddr_ntoa(&s));
		ip_addr_t r;
		err_t e = dns_gethostbyname(D.host, &r, dns_cb, (void *)(uintptr_t)dns_gen);
		if (e == ERR_OK) dl_open(&r); else if (e != ERR_INPROGRESS) dl_fail("dns start");
		return;
	}
	if (!ip) { dl_fail("dns"); return; }
	dl_open(ip);
}

static void dl_begin(void)
{
	ip_addr_t ip;
	D.state = 0; D.hdrlen = 0; D.pcb = 0; D.hdr[0] = 0;
	D.got = D.start;
	D.t_progress = sys_now() / 1000.0; D.got_seen = D.got;
	dns_gen++;
	err_t r = dns_gethostbyname(D.host, &ip, dns_cb, (void *)(uintptr_t)dns_gen);
	if (r == ERR_OK) dl_open(&ip);
	else if (r != ERR_INPROGRESS) dl_fail("dns start");
}

/* start a download; returns 0 if started. Resumes `dest` if it already exists (Range). etag / lastmod (may be 0):
 * the validators of a copy the caller already has; if the server says it did not change, the download ends at once
 * with net_not_modified() = 1 and `dest` empty. */
int net_download_start_cond(const char *url, const char *dest, const char *etag, const char *lastmod)
{
	if (D.pcb) { altcp_abort(D.pcb); D.pcb = 0; }
	if (D.out) { fclose(D.out); D.out = 0; }
	gz_end();
	memset(&D, 0, sizeof D);
	if (etag) snprintf(D.etag, sizeof D.etag, "%s", etag);
	if (lastmod) snprintf(D.lastmod, sizeof D.lastmod, "%s", lastmod);
	if (strlen(url) >= sizeof D.url) { dl_fail("url too long"); return -1; }
	snprintf(D.url, sizeof D.url, "%s", url);
	if (parse_url(url, &D)) { dl_fail("url"); return -1; }
	snprintf(D.dest, sizeof D.dest, "%s", dest);
	D.peek = !strncmp(dest, "/tmp/", 5);
	/* episodes: 32 KB writes to the FAT volume instead of one write per ~500-byte TLS chunk (~270/s -> ~4/s) */
	static char vb[32768];
	D.out = fopen(dest, "r+b");
	if (D.out) { if (!D.peek) setvbuf(D.out, vb, _IOFBF, sizeof vb); fseek(D.out, 0, SEEK_END); D.start = ftell(D.out); }
	else { D.out = fopen(dest, "wb"); D.start = 0; if (D.out && !D.peek) setvbuf(D.out, vb, _IOFBF, sizeof vb); }
	meta_load();
	if (D.start == 0) { char p[600]; meta_path(p, sizeof p); unlink(p); D.meta_val[0] = 0; D.meta_len = -1; }
	if (!D.out) { dl_fail("file"); return -1; }
	set_cloexec(fileno(D.out));
	D.total = -1; D.t0 = sys_now() / 1000.0;
	dl_begin();
	return 0;
}

int net_download_start(const char *url, const char *dest) { return net_download_start_cond(url, dest, 0, 0); }

int net_not_modified(void) { return D.notmod; }

/* a header of the last response (after a download ended or was stopped); 0 if absent */
int net_resp_header(const char *name, char *out, size_t max) { out[0] = 0; return header(name, out, max); }

/* -1 error, 0 in progress, 1 done; fills progress */
int net_download_poll(long *got, long *total)
{
	net_poll();
	if (D.state == 5) {	/* follow a redirect */
		if (D.pcb) { altcp_close(D.pcb); D.pcb = 0; }
		D.redirects++;
		gz_end();
		if (parse_url(D.redirect, &D)) { dl_fail("redirect url"); }
		else { lg("net: -> %s", D.redirect); D.dns_tries = 0; dl_begin(); }
	}
	if (D.state == 6) {	/* the partial file belongs to another version: from 0, on the same final URL */
		if (D.pcb) { altcp_abort(D.pcb); D.pcb = 0; }
		gz_end();
		if (++D.restarts > 2) dl_fail("keeps changing");
		else {
			fflush(D.out); if (ftruncate(fileno(D.out), 0)) lg("net: truncate failed");
			fseek(D.out, 0, SEEK_SET);
			char p[600]; meta_path(p, sizeof p); unlink(p);
			D.start = 0; D.meta_val[0] = 0; D.meta_len = -1;
			dl_begin();
		}
	}
	double t = sys_now() / 1000.0;
	/* minimum rate, in 60 s windows: under 2 KB in one fails (resumable). A trickle would otherwise keep the CPU at
	 * full clock and the player from suspending for ever. A clock jump (set by hand, sys_now wrap) restarts the window. */
	if (D.state >= 0 && D.state <= 3) {
		if (t < D.t_progress || t - D.t_progress > 600) { D.got_seen = D.got; D.t_progress = t; }
		else if (t - D.t_progress >= 60) {
			if (D.got - D.got_seen < 2048) { lg("net: %ld bytes in 60 s", D.got - D.got_seen); dl_fail("stalled"); }
			else { D.got_seen = D.got; D.t_progress = t; }
		}
	}
	if (got) *got = D.got;
	if (total) *total = D.total;
	if (D.state == 4 || D.state == -1) {
		if (D.pcb) { altcp_abort(D.pcb); D.pcb = 0; }
		gz_end();
		int r = D.state == 4 ? 1 : -1;
		if (D.out) {	/* on flash before the caller renames it; a failed flush / close (card full) fails it, .part and .meta kept */
			if (r == 1 && (fflush(D.out) || (fsync(fileno(D.out)) && errno != EINVAL))) { lg("net: flush failed (errno %d)", errno); r = -1; }
			if (fclose(D.out) && r == 1) { lg("net: close failed (errno %d)", errno); r = -1; }
			D.out = 0;
			if (r < 0) D.state = -1;
		}
		if (r == 1) { char p[600]; meta_path(p, sizeof p); unlink(p); lg("net: done %ld bytes in %.0f s", D.got, t - D.t0); }
		return r;
	}
	return 0;
}

void net_download_abort(void)
{
	if (D.pcb) { altcp_abort(D.pcb); D.pcb = 0; }
	gz_end();
	if (D.out) { fclose(D.out); D.out = 0; }
	D.state = -1;
}

/* CA certificates (PEM bundle) for HTTPS */
int net_set_ca(const char *pem_path)
{
	static int loaded;
	if (loaded && tls_cfg) return 0;	/* parsed once per process (a few hundred KB) */
	net_init();	/* the TLS layer allocates from lwIP's heap */
	FILE *f = fopen(pem_path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *pem = malloc(n + 1);
	if (!pem || fread(pem, 1, n, f) != (size_t)n) { fclose(f); free(pem); return -1; }
	fclose(f); pem[n] = 0;
	tls_cfg = altcp_tls_create_config_client(pem, (size_t)n + 1);
	tls_tune(tls_cfg);
	free(pem);
	lg("net: ca bundle %s", tls_cfg ? "loaded" : "FAILED");
	loaded = ca_loaded = tls_cfg != 0;
	return tls_cfg ? 0 : -1;
}
