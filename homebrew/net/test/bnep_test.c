#include "../bnep.h"
#include <stdio.h>
#include <string.h>
void lg(const char *f, ...) { (void)f; }
static unsigned char last[1700]; static int lastn;
static int send(const unsigned char *p, int n) { memcpy(last, p, n); lastn = n; return 0; }
static unsigned char got[1700]; static int gotn;
static void input(const unsigned char *p, int n) { memcpy(got, p, n); gotn = n; }
int main(void)
{
	unsigned char lm[6] = {1,2,3,4,5,6}, rm[6] = {9,8,7,6,5,4};
	bnep_init(lm, rm, send, input);
	bnep_setup(); printf("setup: %d bytes, first %02x %02x %02x\n", lastn, last[0], last[1], last[2]);
	unsigned char rsp[] = {1, 2, 0, 0}; bnep_input(rsp, 4); printf("state %d\n", bnep_state());
	unsigned char fr[60]; memcpy(fr, rm, 6); memcpy(fr + 6, lm, 6); fr[12] = 8; fr[13] = 0; memset(fr + 14, 0xAA, 46);
	bnep_send_ethernet(fr, 60); printf("tx compressed: %d bytes type %02x\n", lastn, last[0]);
	unsigned char bc[60]; memset(bc, 0xff, 6); memcpy(bc + 6, lm, 6); bc[12] = 8; bc[13] = 6;
	bnep_send_ethernet(bc, 60); printf("tx broadcast: %d bytes type %02x\n", lastn, last[0]);
	unsigned char rx[] = {0x82, 0x08, 0x00, 0x00, 0x02, 0xAA, 0xBB, 0x11, 0x22, 0x33};	/* compressed + one extension header */
	bnep_input(rx, sizeof rx); printf("rx: %d bytes, dst %02x src %02x type %02x%02x payload %02x\n", gotn, got[0], got[6], got[12], got[13], got[14]);
	return 0;
}
