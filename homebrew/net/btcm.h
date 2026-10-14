#ifndef BTCM_H
#define BTCM_H
struct bd_addr { unsigned nap; unsigned uap; unsigned long lap; };
int bd_parse(const char *s, struct bd_addr *a);		/* "NAP:UAP:LAP" hex */
void bd_to_mac(const struct bd_addr *a, unsigned char mac[6]);
int btcm_open(const char *scdb_file);			/* open /dev/peer_com, serve the key db, activate SC */
void btcm_close(void);
int btcm_poll(int timeout_ms);				/* handle one incoming primitive; -1 if none */
void btcm_set_data_cb(void (*cb)(const unsigned char *, int));
int btcm_local_addr(struct bd_addr *a);		/* CM_READ_LOCAL_BD_ADDR */
int btcm_register_psm(void);
void btcm_stats(void);
void btcm_active(void);				/* PSM 0x0F (BNEP) */
int btcm_connect(const struct bd_addr *a, int mtu);
int btcm_connected(void);
int btcm_connect_failed(void);
int btcm_mtu(void);
int btcm_send(const unsigned char *data, int len);
void btcm_disconnect(void);
int btcm_bond(const struct bd_addr *a, const char *pin);
void btcm_set_sec(unsigned secl);		/* CM_L2CA_CONNECT_REQ security level (default 0x28) */
int btcm_bccmd(const unsigned short *w, int nw, unsigned short *out, int max);	/* BCCMD, reply words */
#endif
