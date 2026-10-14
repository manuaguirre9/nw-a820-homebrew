/* btcm.h (hid variant): client of Sony's CSR BCHS stack via /dev/peer_com, extended from homebrew/net/btcm.c
 * with several L2CAP channels at once, incoming connections (accept), SDP records, name, class of device and
 * scan enable. Layouts derived from the Serialize_/Deserialize_ functions in hci_bchs.ko (see NOTES.txt). */
#ifndef BTCM_H
#define BTCM_H
struct bd_addr { unsigned nap; unsigned uap; unsigned long lap; };
int bd_parse(const char *s, struct bd_addr *a);		/* "NAP:UAP:LAP" hex */
void bd_fmt(const struct bd_addr *a, char out[20]);	/* same format */

struct btcm_cb {
	void (*l2_up)(int psm, int cid, int mtu, const struct bd_addr *a, int incoming);
	void (*l2_fail)(int psm, int result, int incoming);
	void (*l2_down)(int cid, int reason);
	void (*l2_data)(int cid, const unsigned char *p, int n);
	void (*bond)(const struct bd_addr *a, int result);	/* SC_BOND_IND (incoming pairing) or BOND_CFM */
};

int btcm_open(const char *scdb_file);			/* open /dev/peer_com, serve the key db, activate SC */
void btcm_close(void);
int btcm_fd(void);
void btcm_set_cb(const struct btcm_cb *cb);
int btcm_poll(int timeout_ms);				/* handle one primitive; returns its type, -1 if none */
int btcm_wait(unsigned type, int timeout_ms);		/* poll until a primitive of this type arrived; 0 ok */
extern unsigned char btcm_last[1024]; extern int btcm_last_len;	/* the last primitive received */

int btcm_local_addr(struct bd_addr *a);
int btcm_set_name(const char *name);			/* CM_SET_LOCAL_NAME_REQ, waits for the CFM */
int btcm_set_cod(unsigned service, unsigned major, unsigned minor);	/* CM_WRITE_COD_REQ + CM_WRITE_MAJOR_MINOR_COD_REQ */
int btcm_scan_enable(int inquiry, int page);		/* CM_WRITE_SCAN_ENABLE_REQ (discoverable, connectable) */
int btcm_connectable(int on);				/* CM_CONNECTABLE_REQ */
int btcm_sds_register(const unsigned char *rec, int len, unsigned long *handle);
int btcm_sds_unregister(unsigned long handle);

int btcm_l2_register(unsigned psm);			/* waits for the CFM */
int btcm_l2_accept(unsigned psm, unsigned secl, unsigned mtu, unsigned uuid);
int btcm_l2_cancel_accept(unsigned psm);
int btcm_l2_connect(const struct bd_addr *a, unsigned psm, unsigned mtu, unsigned secl);
int btcm_l2_send(int cid, const unsigned char *data, int len);
int btcm_l2_disconnect(int cid);
void btcm_l2_active(int cid);				/* leave sniff */

void btcm_set_pin(const char *pin);
int btcm_bond(const struct bd_addr *a, const char *pin);
int btcm_debond(const struct bd_addr *a);
int btcm_bccmd(const unsigned short *w, int nw, unsigned short *out, int max);
#endif
