/*
 * Aqualink control - An software aqualink master implementation
 *
 * SPDX-License-Identifier: GPL-3.0
 */

#define ULOG_DEBUG(fmt, ...) ulog(LOG_DEBUG, fmt, ## __VA_ARGS__)

#include "aqualink-internal.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

static void dump_sump(const uint8_t *buf, size_t len)
{
	size_t i;
	char hex[256];
	char *next = hex;

	for (i = 0; i < len; i++) {
		next += snprintf(next, sizeof(hex), " %02x", buf[i]);
	}

	printf("%s\n", hex);
}

static int test_frame_encoding(const uint8_t *frame, size_t frame_len,
			       const uint8_t *message, size_t msg_len,
			       uint8_t *buf)
{
	int ret, len;

	dump_sump(message, msg_len);
	len = aqualink_msg_to_frame(buf, message, msg_len);
	ret = memcmp(frame, buf, len);
	assert(len == frame_len);
	dump_sump(buf, len);
	printf("Encoding: %s\n", ret ? "FAIL" : "PASS");
	if (ret)
		return ret;

	dump_sump(frame, frame_len);
	len = aqualink_frame_to_msg(buf, frame, frame_len);
	if (len >  0)
		dump_sump(buf, len);
	assert(len == msg_len);
	ret = memcmp(message, buf, len);
	printf("Decoding: %s (ret=%d)\n", ret ? "FAIL" : "PASS", len);

	return ret;
}

static int test_framer(void)
{
	const uint8_t frame1[] = {0x10, 0x02, 0x68, 0x10, 0x00, 0xbe, 0x10, 0x00, 0x58, 0x10, 0x03};
	const uint8_t message1[] = {0x68, 0x10, 0xbe, 0x10};
	const uint8_t frame2[] = {0x10, 0x02, 0x00, 0x25, 0x15, 0x00, 0x56, 0x01, 0xf5, 0x00, 0x23, 0xbb, 0x10, 0x03};
	const uint8_t message2[] = {0x00, 0x25, 0x15, 0x00, 0x56, 0x01, 0xf5, 0x00, 0x23};

	uint8_t buf[sizeof(frame2)];
	int ret;

	ret = test_frame_encoding(frame1, sizeof(frame1),
				  message1, sizeof(message1), buf);
	if (ret)
		return ret;

	ret = test_frame_encoding(frame2, sizeof(frame2),
				  message2, sizeof(message2), buf);
	if (ret)
		return ret;

	/* Now check if frames can be unpacked in-place.  */
	memcpy(buf, frame2, sizeof(frame2));
	return test_frame_encoding(buf, sizeof(frame2),
				   message2, sizeof(message2), buf);
}

static int test_packet_escape(void)
{
	const uint8_t expected[] = "\x68\x10\x00\xbe\x10\x00\x9f";
	const uint8_t message[] = "\x68\x10\xbe\x10\x9f";
	uint8_t buf[2 * sizeof(message)];
	int ret, len;

	dump_sump(message, sizeof(message));
	len = aqualink_pack(buf, message, sizeof(message));
	ret = memcmp(expected, buf, len);
	dump_sump(buf, len);
	printf("Escaping: %s\n", ret ? "FAIL" : "PASS");

	return ret;
}

static int test_packet_unescape(void)
{
	uint8_t packet[] = "\x10\x02\x68\x10\x00\xbe\x10\x00\x9f\x10\x03";
	const uint8_t stripd[] = "\x10\x02\x68\x10\xbe\x10\x9f\x10\x03";
	int ret, len;

	dump_sump(packet, sizeof(packet));
	len = aqualink_unpack(packet, packet, sizeof(packet));
	ret = memcmp(packet, stripd, len);
	dump_sump(packet, len);
	printf("Unescaping: %s\n", ret ? "FAIL" : "PASS");

	return ret;
}

int main(int argc, char *argv[])
{
	int num_fail = 0;

	num_fail += test_framer();
	num_fail += test_packet_escape();
	num_fail += test_packet_unescape();

	return num_fail;
}
