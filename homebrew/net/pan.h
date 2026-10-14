/* pan.h: internet over the phone's Bluetooth tethering (see pan.c) */
#ifndef PAN_H
#define PAN_H
/* status(text) is called while waiting (update the screen, keep the watchdog alive). 0 = ok, <0 = failed step */
int pan_up(const char *phone_addr, const char *pin, void (*status)(const char *));
void pan_pump(int ms);		/* service the link; call often (it blocks up to ms) */
int pan_link_ok(void);
void pan_down(void);
void bt_reload_stack(void);	/* rmmod/insmod Sony's hci_bchs when it is wedged (see pan.c) */
#endif
