/* lwipopts.h: lwIP configuration for the NW-A829 homebrew (single-threaded raw API over a BNEP link). */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS 1
#define LWIP_TIMERS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 0	/* the phone's own private subnet: no ARP probe of the offered address (~1 s) */
#define LWIP_DNS 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_STATS 1
#define MEMP_STATS 1
#define LWIP_STATS_DISPLAY 0
#define LWIP_CHECKSUM_ON_COPY 1
#define LWIP_SINGLE_NETIF 1

#define MEM_ALIGNMENT 4
#define MEM_SIZE (96 * 1024)
#define MEMP_NUM_PBUF 32
#define MEMP_NUM_TCP_PCB 6
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 160
#define PBUF_POOL_SIZE 64
#define PBUF_POOL_BUFSIZE 1600	/* one pbuf per Ethernet frame (was 3 x 512): fewer TLS reads / receive calls per packet */
#ifndef TCP_ACK_EVERY_SEGMENT
#define TCP_ACK_EVERY_SEGMENT 0	/* 0: lwIP's delayed ACK (one ACK per 2 segments) = half the uplink packets; 1 was set while
				 * debugging the 1 KB/s link (build with -DTCP_ACK_EVERY_SEGMENT=1 to compare) */
#endif
#define TCP_TMR_INTERVAL 100	/* a pending delayed ACK goes out within 100 ms (default 250) */
#define TCP_STATS 1
#ifndef TCP_MSS
#define TCP_MSS 1400	/* measured 2026-09-29: 280 -> ~69 KB/s, 900/1400 -> ~128 KB/s at 248 MHz (per-packet kernel cost dominates) */
#endif
#define TCP_WND (TCP_MSS <= 300 ? 64 * TCP_MSS : 36000)	/* > one TLS record (16 KB): mbedTLS only frees window after a whole record */
#define TCP_SND_BUF (6 * TCP_MSS)
#define TCP_SND_QUEUELEN 32
#define TCP_QUEUE_OOSEQ 1
#define LWIP_TCP_KEEPALIVE 1
#define DNS_TABLE_SIZE 4
#define DNS_MAX_NAME_LENGTH 128

#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1
#define ALTCP_MBEDTLS_DEBUG LWIP_DBG_OFF
#define ALTCP_MBEDTLS_RNG_FN mbedtls_entropy_func

#define LWIP_DEBUG 0
#define LWIP_PLATFORM_ASSERT(x) do { extern void lg(const char *, ...); lg("lwip assert: %s", x); } while (0)
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_RAND() ((u32_t)rand())
#endif
