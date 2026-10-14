/* pan.c: internet over the phone's Bluetooth tethering (PAN user), for the player. Brings up Sony's BCHS stack
 * (bt_test 3 + bc_attach), puts the UART in low-latency mode (without it the 512-byte tty flip buffer drops every
 * second 331-byte BCSP frame and throughput collapses to ~1 KB/s), connects L2CAP PSM 15 to the phone with our
 * BNEP framing and starts lwIP + DHCP. The pairing key comes from /var/hb/bt/sc_db.db.
 * The caller must have stopped any other Bluetooth use first (the a2d helper) and calls pan_pump() often. */
#include "pan.h"
#include "btcm.h"
#include "bnep.h"
#include "net.h"
#include <fcntl.h>
#include <linux/serial.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern void lg(const char *fmt, ...);
extern double now(void);

static pid_t pid_bca = -1;
static int up;
static void (*tick)(const char *) = 0;

static int l2_send(const unsigned char *p, int n) { return btcm_send(p, n); }
static void l2_recv(const unsigned char *p, int n) { bnep_input(p, n); }
static int eth_send(const unsigned char *f, int n) { return bnep_send_ethernet(f, n); }
static void eth_recv(const unsigned char *f, int n) { net_input(f, n); }

static void set_low_latency(void)
{
	int fd = open("/dev/tts/0", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { lg("pan: open tts: failed"); return; }
	struct serial_struct ss;
	if (!ioctl(fd, TIOCGSERIAL, &ss)) {
		ss.flags |= ASYNC_LOW_LATENCY;
		if (ioctl(fd, TIOCSSERIAL, &ss)) lg("pan: TIOCSSERIAL failed");
	}
	close(fd);
}

static void wait_s(double s, const char *what)
{
	double t0 = now();
	while (now() - t0 < s) { if (tick) tick(what); usleep(100000); }
}

/* Sony's BCHS module can be left wedged when a Bluetooth-audio connection attempt is cut off (the stack
 * answers local requests but never confirms an L2CAP connect). Reloading the module clears it. */
void bt_reload_stack(void)
{
	lg("pan: reloading hci_bchs");
	system("killall a2d_hb a2d_demo_app bc_attach > /dev/null 2>&1; /usr/local/test/bt/bt_test 4 > /dev/null 2>&1; "
	       "rmmod hci_bchs > /dev/null 2>&1; insmod /lib/modules/hci_bchs.ko > /dev/null 2>&1; "
	       "for d in peer_com av2alsa sc_db; do m=$(awk -v n=$d '$2==n {print $1}' /proc/devices); "
	       "[ -n \"$m\" ] && { rm -f /dev/$d; mknod /dev/$d c $m 0; chmod 666 /dev/$d; }; done");
}

static void stack_up(void)
{
	system("killall a2d_hb a2d_demo_app bc_attach > /dev/null 2>&1");	/* nothing else may hold the stack */
	system("/usr/local/test/bt/bt_test 3 > /dev/null 2>&1");
	pid_bca = fork();
	if (pid_bca == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
		execl("/usr/local/test/bt/bc_attach", "bc_attach", "-b", "2592000", "-d", "/dev/tts/0", (char *)0);
		_exit(127);
	}
	/* the stack needs ~3 s: parse the CA bundle meanwhile (~0.5-1 s that used to come after DHCP) */
	double t0 = now();
	if (access("/contents/HB/cacert.pem", R_OK) == 0) net_set_ca("/contents/HB/cacert.pem");
	double left = 3 - (now() - t0);
	if (left > 0) wait_s(left, "1/4 Encendiendo el Bluetooth");
	set_low_latency();
}

static void stack_down(void)
{
	if (pid_bca > 0) {	/* never block: the watchdog resets the device after 15 s */
		kill(pid_bca, SIGTERM);
		for (int i = 0; i < 20 && waitpid(pid_bca, 0, WNOHANG) == 0; i++) { if (tick) tick("Apagando Bluetooth..."); usleep(100000); }
		if (waitpid(pid_bca, 0, WNOHANG) == 0) { kill(pid_bca, SIGKILL); waitpid(pid_bca, 0, 0); }
		pid_bca = -1;
	}
	usleep(300000);
	system("/usr/local/test/bt/bt_test 4 > /dev/null 2>&1");
}

/* keeps the link serviced for up to ms milliseconds */
void pan_pump(int ms)
{
	if (!up) return;
	btcm_poll(ms);
	for (int k = 0; k < 32 && btcm_poll(0) != -1; k++) ;
	net_poll();
}

int pan_link_ok(void) { return up && btcm_connected() && net_ready(); }

int pan_up(const char *phone_addr, const char *pin, void (*status)(const char *))
{
	struct bd_addr phone;
	tick = status;
	if (bd_parse(phone_addr, &phone)) { lg("pan: bad address %s", phone_addr); return -1; }
	double t_start = now();
	stack_up();
	if (btcm_open("/var/hb/bt/sc_db.db")) { stack_down(); return -2; }
	struct bd_addr me;
	unsigned char lmac[6] = { 0x00, 0x13, 0xA9, 0x00, 0x00, 0x01 }, rmac[6];
	if (!btcm_local_addr(&me)) bd_to_mac(&me, lmac);
	bd_to_mac(&phone, rmac);
	btcm_set_data_cb(l2_recv);
	btcm_register_psm();
	double t0 = now();
	while (now() - t0 < 2) if (btcm_poll(100) == 0x8017) break;	/* CM_L2CA_REGISTER_CFM: arrives at once */
	int tries = 0, reloaded = 0;
	for (;;) {
		if (tick) tick("2/4 Conectando con el teléfono");
		btcm_connect(&phone, 1691);
		t0 = now();
		while (!btcm_connected() && !btcm_connect_failed() && now() - t0 < 25) { btcm_poll(100); if (tick) tick("2/4 Conectando con el teléfono"); }
		if (btcm_connected()) break;
		if (!btcm_connect_failed() && !reloaded) {	/* no answer at all from the stack: wedged, reload it once */
			reloaded = 1;
			if (tick) tick("2/4 Reiniciando el Bluetooth");
			btcm_close();
			if (pid_bca > 0) { kill(pid_bca, SIGKILL); waitpid(pid_bca, 0, 0); pid_bca = -1; }
			bt_reload_stack();
			stack_up();
			if (btcm_open("/var/hb/bt/sc_db.db")) { stack_down(); return -2; }
			btcm_set_data_cb(l2_recv);
			btcm_register_psm();
			t0 = now();
			while (now() - t0 < 2) if (btcm_poll(100) == 0x8017) break;
			continue;
		}
		if (++tries >= 2) { lg("pan: L2CAP connect failed"); btcm_close(); stack_down(); return -3; }
		if (tick) tick("2/4 Emparejando con el teléfono");
		btcm_bond(&phone, pin ? pin : "0000");
		t0 = now();
		while (now() - t0 < 30) { int t = btcm_poll(200); if (tick) tick("2/4 Emparejando con el teléfono"); if (t == -2) break; }
	}
	bnep_init(lmac, rmac, l2_send, eth_recv);
	bnep_setup();
	t0 = now();
	while (bnep_state() == 1 && now() - t0 < 10 && btcm_connected()) { btcm_poll(100); if (tick) tick("3/4 Abriendo la conexión a internet"); }
	if (bnep_state() != 2) { lg("pan: BNEP setup failed (%d)", bnep_state()); pan_down(); return -4; }
	up = 1;
	net_start(lmac, eth_send);
	t0 = now();
	int kicks = 0;
	while (!net_ready() && now() - t0 < 30) {
		pan_pump(20); if (tick) tick("4/4 Pidiendo dirección de internet");
		/* Android accepts the BNEP setup and hangs up at once when its Bluetooth tethering is off: say so now
		 * instead of asking for an address for 30 s over a dead link */
		if (!btcm_connected()) { lg("pan: the phone hung up after the setup (tethering off?)"); pan_down(); return -6; }
		if (kicks < 2 && now() - t0 > 0.6 + kicks) { kicks++; net_dhcp_kick(); }	/* at 0.6 s and 1.6 s */
	}
	if (!net_ready()) { lg("pan: no DHCP lease"); pan_down(); return -5; }
	lg("pan: dhcp took %.1f s", now() - t0);
	lg("pan: up in %.1f s", now() - t_start);
	return 0;
}

void pan_down(void)
{
	if (up) net_stop();
	up = 0;
	btcm_disconnect();
	double t0 = now();
	while (now() - t0 < 1) { btcm_poll(100); if (tick) tick("Desconectando..."); }
	btcm_close();
	stack_down();
	lg("pan: down");
}
