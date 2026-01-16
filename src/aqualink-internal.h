#ifndef AQUALINK_INTRERNAL_H
#define AQUALINK_INTRERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Unescape [10 00] to just [10] */
size_t aqualink_unpack(uint8_t *dest, const uint8_t *buf, size_t len);
size_t aqualink_pack(uint8_t *dest, const uint8_t *buf, size_t len);

size_t aqualink_msg_to_frame(uint8_t *dest, const uint8_t *msg, size_t len);
int aqualink_frame_to_msg(uint8_t *dest, const uint8_t *frame, size_t len);


#endif /* AQUALINK_INTRERNAL_H */
