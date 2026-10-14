/* dspprobe: READ-ONLY look at the MP201 SPX DSP state from userland via /dev/mem.
 * Safe to run with or without gemini running (it never writes anything).
 * Usage: dspprobe [-s]    (-s: dump more of the shared-memory PE info block)
 * Addresses come from the Sony kernel (jp_file1.bin) + NEC GPL inter_dsp.c (see ../FINDINGS.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static int memfd = -1;

static volatile uint32_t *map(uint32_t phys, uint32_t len)
{
	uint32_t base = phys & ~0xfffu, off = phys - base;
	void *p = mmap(0, len + off, PROT_READ, MAP_SHARED, memfd, base);
	if (p == MAP_FAILED) {
		printf("mmap %08x failed\n", phys);
		return 0;
	}
	return (volatile uint32_t *)((char *)p + off);
}

static void dump(const char *tag, uint32_t phys, int words)
{
	volatile uint32_t *p = map(phys, words * 4);
	int i;
	if (!p)
		return;
	printf("%s @%08x:", tag, phys);
	for (i = 0; i < words; i++)
		printf("%s%08x", (i % 8) ? " " : "\n  ", p[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	char line[256];
	FILE *f;
	int full = argc > 1 && !strcmp(argv[1], "-s");

	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (memfd < 0) {
		perror("/dev/mem");
		return 1;
	}
	/* 1. DSP->ARM IPI counter (IRQ 1 "interdsp") */
	if ((f = fopen("/proc/interrupts", "r"))) {
		while (fgets(line, sizeof line, f))
			if (strstr(line, "interdsp"))
				fputs(line, stdout);
		fclose(f);
	}
	/* 2. SMU clock status for the DSP (read/write regs only; RESETREQDSP is write-only and is NOT read) */
	{
		volatile uint32_t *smu = map(0xC8010000, 0x100);
		if (smu) {
			printf("SMU GCLKCTRL0=%08x (bit0 DSP_CLK_GCK)  GCLKCTRL2=%08x (bit25 DSP_CLKENA_GCK)\n",
			       smu[0x80 / 4], smu[0x88 / 4]);
			printf("SMU AHBCLKCTRL0=%08x (bit1 DSPLP auto clock)  AUTO_FRQ_CHANGE=%08x (bit1 DSP_REQ_MASK)\n",
			       smu[0xa0 / 4], smu[0xb0 / 4]);
		}
	}
	/* 3. SWT: DSP NMI status (bus error / debug ack) */
	dump("SWT DSP_NMI_STATUS", 0x40120000, 1);
	/* 4. DCV = DSP address-bank remapper: BANKn_OFFSET @+0x00.., BANKn_SET @+0x40.. */
	dump("DCV bank regs", 0x40060000, 32);
	/* 5. InterDSP shared memory (Sony kernel: 0x33FE0000, 128 KB) */
	dump("SHMEM PE0 info (arm_chinfo[3], dsp_chinfo[3], task_chtbl...)", 0x33FE0000, full ? 64 : 24);
	dump("SHMEM driver manager (magic b5cc7a92, mutex, block_factor, nblocks)", 0x33FE0300, 4);
	dump("same via 0x31FE0300 (equal => 32 MB SDRAM aliased at +32 MB)", 0x31FE0300, 4);
	/* 6. DSP code window: DSP addr 0x200/0x400 -> phys 0x33B00200/0x33B00400 (DCV bank0 = 0x33b0) */
	dump("DSP code @DSP 0x200 (reset/vector table)", 0x33B00200, 8);
	dump("DSP code @DSP 0x400", 0x33B00400, 8);
	dump("DSP data window (audio manager data 0x33C006E0)", 0x33C006E0, 8);
	printf("If Sony's audio DSP firmware is resident, the 0x33B00200 words equal file offset 0x38 of\n"
	       "/usr/lib/omf/audio/omf_dsp_manager.mp201: 00000000 1eb41e00 cb281e04 b2a01e04 ...\n");
	return 0;
}
