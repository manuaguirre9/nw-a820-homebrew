/* nolibc: fallback test with raw ARM EABI syscalls, no C library (in case musl's TLS setup fails on 2.6.10). */
static long sys3(long n, long a, long b, long c)
{
	register long r7 __asm__("r7") = n, r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
	__asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
}
static const char msg[] = "hello from nolibc\n";
void _start(void)
{
	sys3(4, 1, (long)msg, sizeof msg - 1);	/* write */
	sys3(1, 0, 0, 0);			/* exit */
	for (;;) ;
}
