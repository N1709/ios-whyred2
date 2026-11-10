/*
 * Minimal raw I2C probe for the DSX, to run in Android recovery.
 *
 *   i2cprobe BUS ADDR PAGE REG LEN MODE
 *
 * MODE 0: page select (ff PAGE), then write REG + repeated start read LEN
 * MODE 1: page select (ff PAGE), then write REG, stop, then a separate read
 * MODE 2: no page select, just write REG + repeated start read LEN
 * MODE 3: no page select, write REG, stop, then separate read
 * MODE 4: page select, then write REG + dummy 0x00, then repeated start read
 * MODE 5: no page select, write REG + dummy 0x00, then repeated start read
 * MODE 6: page select, then a plain read of LEN bytes
 * MODE 7: a plain read of LEN bytes, no write at all
 * MODE 8: page select, then write LEN as one byte to REG
 * MODE 9: no page select, write LEN as one byte to REG
 *
 * Modes 8 and 9 exist because the vendor driver's first act in probe is an
 * F01 command write (register 0x0026 = 0x01, a device reset). If the part is
 * streaming its register space, that command is the documented way back into
 * app mode, and it can only be tried from the host.
 *
 * Why it exists: this is the only way to compare the chip's answers on the
 * known-good recovery QUP with what the XNU bit-bang sees. Wrong addresses
 * fail with ENOTCONN, so a successful transfer with zero bytes really means
 * "the slave ACKed and drove nothing".
 *
 * Build (host):
 *   aarch64-linux-gnu-gcc -O2 -static -o i2cprobe tools/i2cprobe.c
 *   adb push i2cprobe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/i2cprobe
 *   adb shell /data/local/tmp/i2cprobe 1 0x20 0 0xe9 6 0
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

static int
xfer(int fd, struct i2c_msg *m, int n, const char *what)
{
	struct i2c_rdwr_ioctl_data d = { .msgs = m, .nmsgs = n };

	if (ioctl(fd, I2C_RDWR, &d) < 0) {
		printf("%s: %s\n", what, strerror(errno));
		return -1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int bus, addr, page, reg, len, mode;
	char path[32], pagebuf[2], regbuf;
	unsigned char data[64] = { 0 };
	int fd, i;

	if (argc != 7) {
		fprintf(stderr, "usage: %s BUS ADDR PAGE REG LEN MODE\n", argv[0]);
		return 2;
	}
	bus = strtol(argv[1], 0, 0);
	addr = strtol(argv[2], 0, 0);
	page = strtol(argv[3], 0, 0);
	reg = strtol(argv[4], 0, 0);
	len = strtol(argv[5], 0, 0);
	mode = strtol(argv[6], 0, 0);
	if (len < 1 || len > (int)sizeof(data))
		return 2;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
		printf("I2C_SLAVE 0x%02x: %s\n", addr, strerror(errno));
		return 1;
	}

	pagebuf[0] = (char)0xff;
	pagebuf[1] = (char)page;
	regbuf = (char)reg;

	if (mode <= 1 || mode == 4 || mode == 6 || mode == 8) {
		struct i2c_msg p = { .addr = addr, .flags = 0, .len = 2,
		    .buf = (unsigned char *)pagebuf };

		if (xfer(fd, &p, 1, "page select"))
			return 1;
	}

	if (mode == 8 || mode == 9) {
		char wb[2] = { (char)reg, (char)len };
		struct i2c_msg w = { .addr = addr, .flags = 0, .len = 2,
		    .buf = (unsigned char *)wb };

		if (xfer(fd, &w, 1, "write reg=value"))
			return 1;
		printf("wrote %02x=%02x\n", reg & 0xff, len & 0xff);
		return 0;
	}

	if (mode == 4 || mode == 5) {
		char wb[2] = { (char)reg, 0x00 };
		struct i2c_msg m[2] = {
			{ .addr = addr, .flags = 0, .len = 2,
			    .buf = (unsigned char *)wb },
			{ .addr = addr, .flags = I2C_M_RD, .len = len,
			    .buf = data },
		};

		if (xfer(fd, m, 2, "write reg+dummy + read"))
			return 1;
	} else if (mode == 6 || mode == 7) {
		struct i2c_msg r = { .addr = addr, .flags = I2C_M_RD,
		    .len = len, .buf = data };

		if (xfer(fd, &r, 1, "plain read"))
			return 1;
	} else if (mode == 0 || mode == 2) {
		struct i2c_msg m[2] = {
			{ .addr = addr, .flags = 0, .len = 1,
			    .buf = (unsigned char *)&regbuf },
			{ .addr = addr, .flags = I2C_M_RD, .len = len,
			    .buf = data },
		};

		if (xfer(fd, m, 2, "write reg + read"))
			return 1;
	} else {
		struct i2c_msg w = { .addr = addr, .flags = 0, .len = 1,
		    .buf = (unsigned char *)&regbuf };
		struct i2c_msg r = { .addr = addr, .flags = I2C_M_RD,
		    .len = len, .buf = data };

		if (xfer(fd, &w, 1, "write reg"))
			return 1;
		if (xfer(fd, &r, 1, "read"))
			return 1;
	}

	for (i = 0; i < len; i++)
		printf("%02x ", data[i]);
	printf("|");
	for (i = 0; i < len; i++)
		putchar(data[i] >= 0x20 && data[i] < 0x7f ? data[i] : '.');
	printf("|\n");
	return 0;
}
