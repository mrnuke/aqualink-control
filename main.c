/*
 * Aqualink control - An software aqualink master implementation
 *
 * SPDX-License-Identifier: GPL-3.0
 */

#define __USE_GNU
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <termios.h>

#include <libubox/ustream.h>
#include <libubox/ulog.h>

struct aqua_ctx {
	struct ustream_fd stream;
	struct uloop_timeout probe_again;
};

static void *memfind(const uint8_t *buf, size_t len,
			   const uint8_t needle[2])
{
	const uint8_t *next;
	uint8_t *start;

	do {
		start = memchr(buf, needle[0], len - 1);
		if (start && start[1] == needle[1])
			break;

		next = start + 1;
		len -= (ptrdiff_t)(next - buf);
		buf = next;
	} while(start);

	return start;
}

static void dump_sump(const uint8_t *buf, size_t len)
{
	size_t i;
	char hex[256];
	char *next = hex;

	for (i = 0; i < len; i++) {
		next += snprintf(next, sizeof(hex), " %02x", buf[i]);
	}

	ULOG_ERR("%s\n", hex);
}

/*
 * The more I am writing this in C, the more I am starting to want a better
 * language. how C treats bytes, chars, and integers the same is just insane.
 * Then the easy of dereferencing bad pointers in insane.
 */
static void aqualink_unpack(uint8_t *buf, size_t len)
{
	const uint8_t escape_seq[] = { 0x10, 0x00 };
	uint8_t *start, *next;

	/* Unescape [10 00] to just [10] */
	do {
		start = memchr(buf, escape_seq[0], len - 1);
		if (!start)
			break;

		if (start[1] != escape_seq[1]) {
			buf++;
			len--;
			continue;
		}

		memmove(start + 1, start + 2, len - 2);
		next = start + 1;
		len -= (ptrdiff_t)(next - buf);
		buf = next;
	} while(start);
}

static void rs485_notify_read(struct ustream *s, int bytes)
{
	const uint8_t header[] = { 0x10, 0x02 };
	const uint8_t footer[] = { 0x10, 0x03 };
	uint8_t *buf, *start, *end;
	int len, start_len;

	ULOG_INFO("We got some %d\n", bytes);

	buf = (uint8_t *)ustream_get_read_buf(s, &len);
	start = memfind(buf, len, header);
	if (!start)
		return;

	start_len = len - (ptrdiff_t)(start - buf);
	end = memfind(buf, start_len, footer);
	if (!end) {
		/* The bytes before the header are junk. */
		ustream_consume(s, start - buf);
		return;
	}

	dump_sump(start, start_len);
	aqualink_unpack(start, start_len);
	ustream_consume(s, end - buf + sizeof(footer));
}

static void rs485_notify_state(struct ustream *s)
{
	if (!s->eof)
		return;

	ULOG_ERR("tty EOF. shutting down\n");
	exit(-1);
}

static int rs485_stream_open(char *path, struct ustream_fd *s)
{
	int ret, tty;

	struct termios tio = {
		.c_oflag = 0,
		.c_iflag = 0,
		.c_cflag = B9600 | CS8 | CREAD | CLOCAL,
		.c_lflag = 0,
		.c_cc = {
			[VMIN] = 1,
		}
	};

	tty = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (tty < 0) {
		ULOG_ERR("%s: cannot open tty: %s\n", path, strerror(errno));
		return -errno;
	}

	ret = tcsetattr(tty, TCSANOW, &tio);
	if (ret) {
		ULOG_ERR("Can't configure serial port: %s\n", strerror(errno));
		return -errno;
	}

	s->stream.string_data = false;
	s->stream.notify_read = rs485_notify_read;
	s->stream.notify_state = rs485_notify_state;

	ustream_fd_init(s, tty);
	tcflush(tty, TCIFLUSH);

	return 0;
}


static void probe_bus(struct uloop_timeout *t)
{
	struct aqua_ctx *ctx = container_of(t, struct aqua_ctx, probe_again);
	static uint8_t probe[] = {0x10, 0x02, 0x68, 0x25, 0x9f, 0x10, 0x03};

	ULOG_INFO("Here yet again, aren't we \n");
	ustream_write(&ctx->stream.stream, (void *)probe, sizeof(probe), false);



	uloop_timeout_set(t, 2 * 1000);
}

int main(int argc, char *argv[])
{
	struct aqua_ctx ctx = {
		.probe_again.cb = probe_bus,
	};

	ulog_open(ULOG_STDIO | ULOG_SYSLOG, LOG_DAEMON, "aqualinkd");
	ulog_threshold(LOG_INFO);
	ULOG_ERR("%s: Wilcommen auf aquascwitz\n", argv[0]);
	uloop_init();


	if (rs485_stream_open("/dev/ttyaqualink", &ctx.stream) < 0)
		return -1;

	uloop_timeout_set(&ctx.probe_again, 1000);
	uloop_run();
	uloop_done();
}
