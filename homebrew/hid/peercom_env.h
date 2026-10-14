/* peercom_env.h (copied from homebrew/net, SC part extended for incoming pairing): the /dev/peer_com envelope and the SC (security) helpers, included by btcm.c only.
 *   writev(fd, 4 iovecs): u16 peer_type (0 = message), u16 destination queue, u16 event class, payload
 *   readv(fd, 4 iovecs):  u16 0, u16 our handle, u16 class of the sender, payload (<= 1000 bytes); blocks,
 *                         poll() works. ioctl 0x80024300 returns our application handle (queue id).
 * Queues: CM 0xd, SC 0xe. Classes: SC 0x102, CM 0x104. Big-endian scalars, BD_ADDR = u32 lap, u8 uap, u16 nap. */
#include <poll.h>

#define Q_CM 0xd
#define Q_SC 0xe
#define BT_SEC_LEVEL sec_level
static unsigned sec_level = 0x0028;	/* SECL_OUT_AUTHENTICATION | SECL_OUT_ENCRYPTION (BlueStack); btcm_set_sec() can change it */
void btcm_set_sec(unsigned s) { sec_level = s; }

static int pc_open(void) { int fd = open("/dev/peer_com", O_RDWR); if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC); return fd; }

static unsigned short pc_handle(int fd)
{
	unsigned short h = 0;
	if (ioctl(fd, 0x80024300, &h) < 0) lg("peer_com: handle ioctl failed");
	return h;
}

static int pc_send(int fd, unsigned short cls, const unsigned char *prim, int len)
{
	unsigned short peer_type = 0, queue = cls == CLASS_SC ? Q_SC : Q_CM;
	struct iovec iov[4] = { { &peer_type, 2 }, { &queue, 2 }, { &cls, 2 }, { (void *)prim, (size_t)len } };
	int r = writev(fd, iov, 4);
	if (r < 0) lg("peer_com: writev failed (%d)", errno);
	return r < 0 ? -1 : 0;
}

/* one primitive; returns the payload length, 0 on timeout, -1 on error */
static int pc_recv(int fd, unsigned short *cls, unsigned char *buf, int max, int timeout_ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	if (poll(&p, 1, timeout_ms) <= 0) return 0;
	unsigned short zero = 0, handle = 0;
	static unsigned char big[4096];	/* the kernel does not check the payload size: read into a large buffer */
	struct iovec iov[4] = { { &zero, 2 }, { &handle, 2 }, { cls, 2 }, { big, sizeof big } };
	int r = readv(fd, iov, 4);
	if (r < 6) return -1;
	int n = r - 6;
	if (n > max) n = max;
	memcpy(buf, big, n);
	return n;
}

/* ---- link key database: the kernel keeps the keys in memory but asks userland to persist them ---- */

static void scdb_load_and_sync(int fd, const char *path)
{
	unsigned char rec[100];
	FILE *f = fopen(path, "rb");
	int n = 0;
	while (f && fread(rec, 1, 100, f) == 100) {
		unsigned char cmd = 1;
		struct iovec iov[3] = { { &cmd, 1 }, { rec, 8 }, { rec + 8, 92 } };
		if (writev(fd, iov, 3) < 0) lg("sc_db: add failed"); else n++;
	}
	if (f) fclose(f);
	unsigned char sync = 2;
	struct iovec iov[3] = { { &sync, 1 }, { rec, 0 }, { rec, 0 } };
	if (writev(fd, iov, 3) < 0) lg("sc_db: sync failed");
	lg("sc_db: %d keys loaded, synced", n);
}

/* the kernel reports a new record (cmd 1, 101 bytes) or a removal (cmd 3, 9 bytes): keep the file in sync */
static void scdb_service(int fd, const char *path)
{
	struct pollfd p = { fd, POLLIN, 0 };
	while (poll(&p, 1, 0) > 0) {
		unsigned char cmd, addr[8], rec[92];
		struct iovec iov[3] = { { &cmd, 1 }, { addr, 8 }, { rec, 92 } };
		int r = readv(fd, iov, 3);
		if (r < 9) return;
		/* rewrite the file: replace/remove the entry with this address */
		unsigned char all[64][100]; int n = 0;
		FILE *f = fopen(path, "rb");
		while (f && n < 64 && fread(all[n], 1, 100, f) == 100) if (memcmp(all[n], addr, 8)) n++;
		if (f) fclose(f);
		if (cmd == 1 && r >= 101 && n < 64) { memcpy(all[n], addr, 8); memcpy(all[n] + 8, rec, 92); n++; }
		f = fopen(path, "wb");
		if (f) { fwrite(all, 100, n, f); fclose(f); sync(); }
		lg("sc_db: %s record, %d stored", cmd == 1 ? "new" : "removed", n);
	}
}

/* ---- SC: passkey prompts and bond results ---- */
static char sc_pin[17] = "0000";
static int sc_bond_done, sc_bond_result;
static void sc_set_pin(const char *pin) { snprintf(sc_pin, sizeof sc_pin, "%s", pin); }

static unsigned char *put_bd_sc(unsigned char *p, const struct bd_addr *a)	/* u32 lap, u8 uap, u16 nap */
{ p = put32(p, a->lap); *p++ = a->uap; return put16(p, a->nap); }
static void get_bd_sc(const unsigned char *p, struct bd_addr *a) { a->lap = get32(p); a->uap = p[4]; a->nap = get16(p + 5); }
static void sc_bond_event(const struct bd_addr *a, int result);	/* btcm.c */

static void sc_handle(const unsigned char *m, int n)
{
	unsigned type = get16(m);
	switch (type) {
	case 0x8000: {	/* PASSKEY_IND: u16, u32 cod, bd(7), name[41] -> PASSKEY_RES (type 3, 29 bytes) */
		if (n < 13) return;
		unsigned char r[29], *p = r;
		p = put16(p, SC_PASSKEY_RES); *p++ = 1;
		memcpy(p, m + 6, 7); p += 7;
		int l = (int)strlen(sc_pin); if (l > 16) l = 16;
		*p++ = (unsigned char)l;
		memset(p, 0, 16); memcpy(p, sc_pin, l); p += 16;
		*p++ = 1; *p++ = 1;	/* addDevice, authorised */
		pc_send(pc, CLASS_SC, r, 29);
		{ struct bd_addr a; get_bd_sc(m + 6, &a); char b[20]; bd_fmt(&a, b);
		  lg("sc: PIN requested by %s (cod 0x%06lx), answered \"%s\"", b, get32(m + 2), sc_pin); }
		break;
	}
	case 0x8005: {	/* AUTHORISE_IND: u16, bd(7), name[41], u16 -> AUTHORISE_RES (type 6: u16, u8 accept, bd) */
		if (n < 9) return;
		unsigned char r[10], *p = r;
		p = put16(p, 6); *p++ = 1; memcpy(p, m + 2, 7);
		pc_send(pc, CLASS_SC, r, 10);
		lg("sc: authorisation requested, accepted");
		break;
	}
	case 0x8001:	/* BOND_CFM: u16, u8 result, bd, ... */
		sc_bond_result = n > 2 ? m[2] : -1; sc_bond_done = 1;
		lg("sc: bond result %d", sc_bond_result);
		{ struct bd_addr a; if (n >= 10) get_bd_sc(m + 3, &a); else memset(&a, 0, sizeof a); sc_bond_event(&a, sc_bond_result); }
		break;
	case 0x8006: {	/* BOND_IND (a host paired with us): u16, bd(lap u32, uap u8, nap u16), u32 cod, u8 result, u8 added */
		struct bd_addr a; char b[20];
		if (n < 14) { lg("sc: bond indication (short)"); break; }
		get_bd_sc(m + 2, &a); bd_fmt(&a, b);
		lg("sc: bond indication from %s cod 0x%06lx result %d added %d", b, get32(m + 9), m[13], n > 14 ? m[14] : -1);
		sc_bond_event(&a, m[13]);
		break;
	}
	default: lg("sc: prim 0x%04x", type);
	}
}
