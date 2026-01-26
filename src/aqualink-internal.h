#ifndef AQUALINK_INTRERNAL_H
#define AQUALINK_INTRERNAL_H

#include <stddef.h>
#include <stdint.h>

struct device_ops;

struct device {
	const struct device_ops *ops;
	uint8_t addr;
	int connected : 1;
};

struct device_ops {
	int (*handle_reply)(struct device *dev, const uint8_t *reply, size_t len);
};

/* Unescape [10 00] to just [10] */
size_t aqualink_unpack(uint8_t *dest, const uint8_t *buf, size_t len);
size_t aqualink_pack(uint8_t *dest, const uint8_t *buf, size_t len);

size_t aqualink_msg_to_frame(uint8_t *dest, const uint8_t *msg, size_t len);
int aqualink_frame_to_msg(uint8_t *dest, const uint8_t *frame, size_t len);

extern const struct device_ops jxi_heater_ops;

static inline uint16_t read16_le(const uint8_t *raw)
{
	return (uint16_t)raw[1] << 8 | raw[0];
}

#endif /* AQUALINK_INTRERNAL_H */
