#include <stddef.h>
/* net.h: TCP/IP over Bluetooth PAN for the homebrew player (see net.c, bnep.c). */
#ifndef NET_H
#define NET_H
void net_init(void);	/* lwIP init (idempotent) */
void net_dhcp_kick(void);	/* DHCP DISCOVER again */
int net_start(const unsigned char mac[6], int (*send_frame)(const unsigned char *frame, int len));
void net_stop(void);
void net_input(const unsigned char *frame, int len);	/* Ethernet frame received from BNEP */
void net_poll(void);					/* timers; call often */
int net_ready(void);					/* link up and an IP address obtained */
int net_set_ca(const char *pem_path);			/* CA bundle for HTTPS */
int net_download_start_cond(const char *url, const char *dest, const char *etag, const char *lastmod);	/* conditional GET */
int net_not_modified(void);	/* the last conditional download: 304, the caller's copy is current */
int net_resp_header(const char *name, char *out, size_t max);	/* header of the last response */
int net_download_start(const char *url, const char *dest);
int net_download_poll(long *got, long *total);		/* -1 error, 0 running, 1 done */
void net_download_abort(void);
void net_tcp_stats(void);
double net_ping_gw(int size, void (*poll)(void));	/* ms, -1 on timeout */
#endif
