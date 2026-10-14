#ifndef BNEP_H
#define BNEP_H
void bnep_init(const unsigned char local_mac[6], const unsigned char remote_mac[6],
	int (*l2cap_send)(const unsigned char *, int), void (*ethernet_input)(const unsigned char *, int));
int bnep_setup(void);				/* send SETUP_CONNECTION_REQUEST (PANU -> NAP) */
int bnep_state(void);				/* 0 idle, 1 setup sent, 2 connected, -1 refused */
void bnep_input(const unsigned char *l2cap_packet, int len);
int bnep_send_ethernet(const unsigned char *frame, int len);
#endif
