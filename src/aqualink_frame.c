/*
 * aqualink protocol packet framer
 *
 * Implements the encoding of RS-485 packets as used by Jandy pool equipment,
 * commonly marketed as "Aqualink".
 */
#include "aqualink-internal.h"

#include <errno.h>
#include <string.h>

static const uint8_t aq_header[] = {0x10, 0x02};
static const uint8_t aq_footer[] = {0x10, 0x03};

static uint8_t mod256_sum(const uint8_t *buf, size_t len)
{
	size_t i, csum = 0;

	for (i = 0; i < len; i++)
		csum = (csum + buf[i]) & 0xff;

	return csum;
}

size_t aqualink_msg_to_frame(uint8_t *dest, const uint8_t *msg, size_t len)
{
	uint8_t *const dest_start = dest;
	uint8_t sum;

	*dest++ = 0x10;
	*dest++ = 0x02;

	/* The checksum needs to be escaped too, ya evil bastards! */
	dest += aqualink_pack(dest, msg, len);
	sum = mod256_sum(dest_start, dest - dest_start);
	*dest++ = sum;
	if (sum == 0x10)
		*dest++ = 0x00;

	*dest++ = 0x10;
	*dest++ = 0x03;

	return dest - dest_start;
}

int aqualink_frame_to_msg(uint8_t *dest, const uint8_t *frame, size_t len)
{
	uint8_t calculated_sum, raw_sum;

	if (len < sizeof(aq_header) + sizeof(aq_footer) + 1)
		return -EINVAL;

	if (memcmp(frame + len - sizeof(aq_footer), aq_footer, sizeof(aq_footer)))
		return -EINVAL;

	/* Ignore the footer from now on. */
	len -= sizeof(aq_footer);

	if (memcmp(frame, aq_header, sizeof(aq_header)))
		return -EINVAL;

	frame += sizeof(aq_header);
	len -= sizeof(aq_header);

	len = aqualink_unpack(dest, frame, len);

	raw_sum = dest[len - 1];
	calculated_sum = mod256_sum(dest, len - 1) + mod256_sum(aq_header, 2);
	if ((calculated_sum & 0xff) != raw_sum)
		return -EPROTO;

	return len - 1;
}

static void *memfind(const uint8_t *buf, size_t len, const uint8_t needle[2])
{
	const uint8_t *next;
	uint8_t *start;

	if (len <= 1)
		return NULL;

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

/*
 * Unescape [10 00] to just [10]
 *
 * The more I am writing this in C, the more I am starting to want a better
 * language. How C treats bytes, chars, and integers the same is just insane.
 * Then the easy of dereferencing bad pointers in scary.
 */
size_t aqualink_unpack(uint8_t *dest, const uint8_t *buf, size_t len)
{
	const uint8_t escape_seq[] = { 0x10, 0x00 };
	const uint8_t *const end = buf + len;
	uint8_t *next, *src, *const start = dest;
	int move_len;

	src = memfind(buf, len, escape_seq);
	if (!src) {
		if (dest != buf)
			memmove(dest, buf, len);
		return len;
	}

	/* First byte stays, second byte is dropped */
	len = src - buf + 1;
	if (dest != buf)
		memmove(dest, buf, len);
	dest += len;
	src = src + 2;

	while (src < end) {
		len = end - src;
		next = memfind(src + 1, len - 1, escape_seq);
		move_len = next ? ((next - src) + 1) : len;

		memmove(dest, src, move_len);
		dest += move_len;
		/* Once again, second byte of the sequence is dropped */
		src += move_len + 1;
	}

	return dest - start;
}

/*
 * Escape [10] to [10 00]
 *
 * 'dest' is assumed to be long enough for the new buffer. Having 'dest' be at
 * least twice as long as 'src ' is enough for all cases.
 */
size_t aqualink_pack(uint8_t *dest, const uint8_t *src, size_t len)
{
	const uint8_t *const dest_buf = dest;
	const uint8_t escape_char = 0x10;
	const uint8_t *end = src + len;
	const uint8_t *next;
	size_t chunk_len;

	while (src < end) {
		len = end - src;
		next = memchr(src, escape_char, len);

		chunk_len = next ? ((next - src) + 1) : len;
		memcpy(dest, src, chunk_len);
		dest += chunk_len;
		if (next)
			*dest++ = 0x00;
		src += chunk_len;
	}

	return dest - dest_buf;
}
