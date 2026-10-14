/* nolibc_oabi: raw OABI syscalls (swi 0x900000+nr), to confirm the kernel's syscall convention. */
static const char msg[] = "hello from nolibc (OABI swi)\n";
void _start(void)
{
	register long r0 __asm__("r0") = 1, r1 __asm__("r1") = (long)msg, r2 __asm__("r2") = sizeof msg - 1;
	__asm__ volatile("svc 0x900004" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
	r0 = 0;
	__asm__ volatile("svc 0x900001" : "+r"(r0) : : "memory");
	for (;;) ;
}
