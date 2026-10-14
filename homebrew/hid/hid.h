/* hid.h: Bluetooth HID device (keyboard + consumer control) on top of btcm (Sony's BCHS stack). */
#ifndef HID_H
#define HID_H
#include "btcm.h"

/* consumer-control bits (report ID 2, one byte) */
#define CC_PLAYPAUSE 0x01	/* 0xCD */
#define CC_NEXT      0x02	/* 0xB5 */
#define CC_PREV      0x04	/* 0xB6 */
#define CC_VOLUP     0x08	/* 0xE9 */
#define CC_VOLDN     0x10	/* 0xEA */
#define CC_MUTE      0x20	/* 0xE2 */
#define CC_ACBACK    0x40	/* AC Back 0x224 */
#define CC_ACHOME    0x80	/* AC Home 0x223 */

/* keyboard usages (page 7) */
#define KB_ENTER 0x28
#define KB_ESC   0x29
#define KB_SPACE 0x2C
#define KB_PGUP  0x4B
#define KB_PGDN  0x4E
#define KB_RIGHT 0x4F
#define KB_LEFT  0x50
#define KB_DOWN  0x51
#define KB_UP    0x52

struct hid_cfg {
	const char *name;	/* local name */
	unsigned cod;		/* 24-bit class of device, e.g. 0x002580 */
	const char *pin;	/* legacy PIN we answer with */
	unsigned secl;		/* incoming security level (BlueStack SECL_*), default 0x0006 = in auth + encryption */
	const char *host_file;	/* where the paired host's address is kept */
};

enum hid_state { HID_IDLE, HID_LISTENING, HID_CONNECTING, HID_CTRL, HID_CONNECTED };

int hid_init(const struct hid_cfg *cfg);	/* after btcm_open: name, CoD, SDP record, PSMs, accept, scan */
void hid_shutdown(void);
void hid_tick(void);				/* call often: re-arms accepts, reconnect state machine */
enum hid_state hid_state(void);
const char *hid_state_str(void);
const char *hid_host_str(void);			/* "" if none */
int hid_have_host(void);
int hid_reconnect(void);			/* connect out to the stored host (control, then interrupt) */
void hid_disconnect(void);
int hid_key(unsigned usage, int down);		/* keyboard key press/release */
int hid_consumer(unsigned bit, int down);	/* consumer control press/release */
int hid_tap_consumer(unsigned bit);		/* press + release */
#endif
