/*
 * Aqualink control - An software aqualink master implementation
 *
 * SPDX-License-Identifier: GPL-3.0
 */

#define ULOG_DEBUG(fmt, ...) ulog(LOG_DEBUG, fmt, ## __VA_ARGS__)

#include "aqualink-internal.h"

#include <errno.h>
#include <getopt.h>
#include <linux/serial.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include <libubox/ustream.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

enum aqua_commands {
	AQUA_PROBE_REQUEST = 0x00,
	AQUA_PROBE_RESPONSE = 0x01,
};

struct rs485_frame {
	struct list_head list;
	uint8_t buf[32];
	size_t len;
};

struct aqua_ctx {
	struct ustream_fd stream;
	struct uloop_timeout probe_again;
	struct uloop_timeout device_work;
	struct uloop_timeout interframe_gap;
	struct uloop_timeout rs485_timeout;
	struct list_head pending_frames;
	struct device slaves[10];
};

static int rs485_send_next_frame(struct aqua_ctx *ctx);

static int compare_dev_addr(const void *a, const void *b)
{
	const struct device *first = a;
	const struct device *second = b;

	if (!second->addr)
		return -1;

	return first->addr - second->addr;
}

static struct device *lookup_slave(struct aqua_ctx *ctx, uint8_t dev_addr)
{
	const struct device key = {
		.addr = dev_addr,
	};

	return bsearch(&key, ctx->slaves, ARRAY_SIZE(ctx->slaves),
			 sizeof(ctx->slaves[0]), compare_dev_addr);
}

static int add_slave(struct aqua_ctx *ctx, uint8_t addr,
		     const struct device_ops *ops)
{
	struct device *dev = lookup_slave(ctx, addr);
	int i, last;

	if (dev)
		return -EEXIST;

	for (i = 0; i < ARRAY_SIZE(ctx->slaves); i++) {
		if (ctx->slaves[i].addr == 0)
			break;

		if (ctx->slaves[i].addr > addr)
			break;
	}

	for (last = 0; last < ARRAY_SIZE(ctx->slaves); last++) {
		if (ctx->slaves[last].addr == 0)
			break;
	}

	if (i >= ARRAY_SIZE(ctx->slaves) || last >= ARRAY_SIZE(ctx->slaves))
		return -ENOMEM;

	if (last > i)
		memmove(ctx->slaves + i + 1, ctx->slaves + i,
			(last - i) * sizeof(ctx->slaves[0]));

	dev = ctx->slaves + i;
	dev->addr = addr;
	dev->ops = ops;

	return 0;
}

static void *memfind(const uint8_t *buf, size_t len, const uint8_t needle[2])
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

static void rs485_no_response(struct uloop_timeout *t)
{
	struct aqua_ctx *ctx = container_of(t, struct aqua_ctx, rs485_timeout);
	struct rs485_frame *request;
	uint8_t slave_addr;

	request = list_first_entry(&ctx->pending_frames, struct rs485_frame,
				   list);
	slave_addr = request->buf[2];

	ULOG_ERR("RS-485 timeout on request to device addr 0x%x\n", slave_addr);

	/* Move on, as we no longer expect a response to this request. */
	list_del(&request->list);
	free(request);
	rs485_send_next_frame(ctx);
}

static void rs485_send_after_interframe_gap(struct uloop_timeout *t)
{
	struct aqua_ctx *ctx = container_of(t, struct aqua_ctx, interframe_gap);

	rs485_send_next_frame(ctx);
}

static int rs485_send_frame(struct aqua_ctx *ctx, struct rs485_frame *frame)
{
	if (ctx->interframe_gap.pending) {
		ctx->interframe_gap.cb = rs485_send_after_interframe_gap;
		return -EAGAIN;
	}

	/* The timeout must include the time to transmit the request frame. */
	ctx->rs485_timeout.cb = rs485_no_response;
	uloop_timeout_set(&ctx->rs485_timeout, 200);

	return ustream_write(&ctx->stream.stream, (void *)frame->buf,
			     frame->len, false);
}

static int rs485_send_next_frame(struct aqua_ctx *ctx)
{
	struct rs485_frame *frame;

	if (list_empty(&ctx->pending_frames))
		return -EAGAIN;

	frame = list_first_entry(&ctx->pending_frames, struct rs485_frame, list);
	return rs485_send_frame(ctx, frame);
}

static int rs485_queue_frame(struct aqua_ctx *ctx, const uint8_t *buf,
			     size_t len)
{
	bool queue_is_empty = list_empty(&ctx->pending_frames);
	struct rs485_frame *frame;

	if (len > sizeof(frame->buf)) {
		ULOG_ERR("Requested frame size %zu too large\n", len);
		return -E2BIG;
	}

	frame = malloc(sizeof(*frame));
	if (!frame)
		return -ENOMEM;

	memset(frame, 0, sizeof(*frame));
	memcpy(frame->buf, buf, len);
	frame->len = len;

	list_add_tail(&frame->list, &ctx->pending_frames);
	if (queue_is_empty)
		return rs485_send_frame(ctx, frame);

	return 0;
}

static void dev_clear_okay(struct uloop_timeout *t)
{
	struct device *dev = container_of(t, struct device, data_expired);

	ULOG_WARN("Communication lost with device addr=0x%x\n", dev->addr);
	dev->connected = 0;
}

static int aqualink_handle_msg(struct aqua_ctx *ctx,
			       const struct rs485_frame *request,
			       const uint8_t *reply, size_t len)
{
	struct device *slave;
	uint8_t cmd, dev_addr;
	int ret = 0;

	if (len < 2)
		return -ENODATA;

	dev_addr = request->buf[2];

	slave = lookup_slave(ctx, dev_addr);
	if (!slave)
		return -ENODEV;

	cmd = reply[1];
	switch (cmd) {
	case AQUA_PROBE_RESPONSE:
		slave->connected = 1;
		slave->data_expired.cb = dev_clear_okay;
		break;
	default:
		ret = slave->ops->handle_reply(slave, reply, len);
		break;
	}

	uloop_timeout_set(&slave->data_expired, 2 * 1000);

	return ret;
}

static int aqualink_handle_frame(struct aqua_ctx *ctx, uint8_t *frame,
				 size_t len)
{
	struct rs485_frame *request;
	uint8_t buf[32];
	int msg_len;

	msg_len = aqualink_frame_to_msg(buf, frame, len);
	if (msg_len < 0) {
		ULOG_ERR("Error decoding frame: %d\n", msg_len);
		return msg_len;
	}

	request = list_first_entry(&ctx->pending_frames, struct rs485_frame,
				   list);

	return aqualink_handle_msg(ctx, request, buf, msg_len);
}


static void rs485_notify_read(struct ustream *s, int bytes)
{
	struct ustream_fd *ufd = container_of(s, struct ustream_fd, stream);
	struct aqua_ctx *ctx = container_of(ufd, struct aqua_ctx, stream);
	const uint8_t header[] = { 0x10, 0x02 };
	const uint8_t footer[] = { 0x10, 0x03 };
	struct rs485_frame *request;
	uint8_t *buf, *start, *end;
	int ret, len, frame_len;

	buf = (uint8_t *)ustream_get_read_buf(s, &len);
	start = memfind(buf, len, header);
	if (!start)
		return;

	len -= (start - buf);
	end = memfind(buf, len, footer);
	if (!end) {
		/* The bytes before the header are junk. */
		ustream_consume(s, start - buf);
		return;
	}

	frame_len = end - start + sizeof(footer);
	ret = aqualink_handle_frame(ctx, start, frame_len);
	if (ret) {
		ULOG_WARN("Unhandled frame (ret=%d)", ret);
	}

	if (list_empty(&ctx->pending_frames)) {
		ULOG_ERR("Discarding unsolicited reply!\n");
		return;
	}

	request = list_first_entry(&ctx->pending_frames, struct rs485_frame,
				   list);
	list_del(&request->list);

	uloop_timeout_cancel(&ctx->rs485_timeout);
	/* 3.5 characters at 9600 baud is about 3.6 milliseconds. Round up. */
	uloop_timeout_set(&ctx->interframe_gap, 4);
	ustream_consume(s, end - buf + sizeof(footer));
	rs485_send_next_frame(ctx);
	free(request);
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

	struct serial_rs485 rs485_cfg = {
		.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND,
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

	ret = ioctl (tty, TIOCSRS485, &rs485_cfg);
	if (ret) {
		ULOG_ERR("Can't set RS485 mode: %s\n", strerror(errno));
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
	struct device *dev;
	size_t i, frame_len;
	uint8_t buf[32];

	uint8_t probe[] = {0, AQUA_PROBE_REQUEST};

	for (i = 0; i < ARRAY_SIZE(ctx->slaves); i++) {
		dev = ctx->slaves + i;
		if (dev->connected)
			continue;

		if (dev->addr == 0)
			break;

		probe[0] = dev->addr;
		frame_len = aqualink_msg_to_frame(buf, probe, sizeof(probe));
		rs485_queue_frame(ctx, buf, frame_len);
	}

	uloop_timeout_set(t, 2 * 1000);
}

static int handsome_dev(struct aqua_ctx *ctx, struct device *dev)
{
	uint8_t msg_buf[16], buf[32];
	int len, frame_len;

	if (!dev->ops->get_next_request)
		return -EOPNOTSUPP;

	len = dev->ops->get_next_request(dev, msg_buf, sizeof(msg_buf));
	if (len < 0)
		return len;

	msg_buf[0] = dev->addr;
	frame_len = aqualink_msg_to_frame(buf, msg_buf, len);
	return rs485_queue_frame(ctx, buf, frame_len);
}

static void handle_connected_devices(struct uloop_timeout *t)
{
	struct aqua_ctx *ctx = container_of(t, struct aqua_ctx, device_work);
	struct device *dev;
	int i, ret;

	if (!list_empty(&ctx->pending_frames)) {
		ULOG_WARN("Bus contention. Delaying device work\n");
		uloop_timeout_set(t, 100);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(ctx->slaves); i++) {
		dev = ctx->slaves + i;

		if (dev->addr == 0)
			break;

		if (!dev->ops->get_next_request)
			continue;

		ret = handsome_dev(ctx, dev);
		if (ret < 0) {
			ULOG_ERR("Slave addr=0x%x next request error %d\n",
				 dev->addr, ret);
			continue;
		}
	}

	uloop_timeout_set(t, 500);
}

int main(int argc, char *argv[])
{
	char *tty_dev = "/dev/ttyS0";
	int opt, ret;

	const struct option options[] = {
		{"tty", required_argument, 0, 't'},
		{ }
	};

	struct aqua_ctx ctx = {
		.probe_again.cb = probe_bus,
		.device_work.cb = handle_connected_devices,
	};

	do {
		opt = getopt_long(argc, argv, "", options, NULL);
		switch (opt) {
		case 't':
			tty_dev = optarg;
			break;
		}
	} while (opt > 0);

	INIT_LIST_HEAD(&ctx.pending_frames);

	ret = add_slave(&ctx, 0x68, &jxi_heater_ops);
	if (ret) {
		ULOG_ERR("Internal error: %d\n", ret);
		return EXIT_FAILURE;
	}

	ulog_open(ULOG_STDIO | ULOG_SYSLOG, LOG_DAEMON, "aqua-control");
	ulog_threshold(LOG_INFO);
	ULOG_ERR("%s: Starting up\n", argv[0]);
	uloop_init();

	if (rs485_stream_open(tty_dev, &ctx.stream) < 0)
		return -1;

	uloop_timeout_set(&ctx.probe_again, 1000);
	uloop_timeout_set(&ctx.device_work, 1200);
	uloop_run();
	uloop_done();
}
