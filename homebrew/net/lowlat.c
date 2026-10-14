/* lowlat: sets ASYNC_LOW_LATENCY on a serial port so received bytes go straight to the line discipline
 * instead of waiting in the 512-byte tty flip buffer (which overflows at 2.59 Mbaud with back-to-back BCSP
 * frames). usage: lowlat /dev/tts/0 [0|1] */
#include <fcntl.h>
#include <linux/serial.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/tts/0";
	int on = argc > 2 ? atoi(argv[2]) : 1;
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { perror(dev); return 1; }
	struct serial_struct ss;
	if (ioctl(fd, TIOCGSERIAL, &ss)) { perror("TIOCGSERIAL"); return 2; }
	printf("%s: type %d line %d irq %d flags 0x%x baud_base %d xmit_fifo %d\n", dev, ss.type, ss.line, ss.irq, ss.flags, ss.baud_base, ss.xmit_fifo_size);
	if (on) ss.flags |= ASYNC_LOW_LATENCY; else ss.flags &= ~ASYNC_LOW_LATENCY;
	if (ioctl(fd, TIOCSSERIAL, &ss)) { perror("TIOCSSERIAL"); return 3; }
	ioctl(fd, TIOCGSERIAL, &ss);
	printf("%s: flags now 0x%x (low latency %s)\n", dev, ss.flags, ss.flags & ASYNC_LOW_LATENCY ? "on" : "off");
	close(fd);
	return 0;
}
