#ifndef AQUALINK_INTRERNAL_H
#define AQUALINK_INTRERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <libubox/uloop.h>
#include <stdbool.h>
#include <libubox/kvlist.h>
#include <libubox/ulog.h>

struct aqua_ctx;

struct property {
	enum {
		PROP_STRING,
		PROP_FLOAT,
		PROP_INT,
		PROP_BOOL,
	} type;

	union {
		const char *string;
		float fval;
		int ival;
		bool bval;
	};
};


struct device_ops;

struct device {
	struct uloop_timeout data_expired;
	struct kvlist properties;
	const struct device_ops *ops;
	struct kvlist *context_props;
	void *priv;
	const char *name;
	uint8_t addr;
	int data_valid : 1;
	int connected : 1;
};

struct device_ops {
	int (*init_properties)(struct device *dev);
	int (*handle_reply)(struct device *dev, const uint8_t *reply, size_t len);
	int (*get_next_request)(struct device *dev, uint8_t* msg, size_t len);
};

/* Unescape [10 00] to just [10] */
size_t aqualink_unpack(uint8_t *dest, const uint8_t *buf, size_t len);
size_t aqualink_pack(uint8_t *dest, const uint8_t *buf, size_t len);

size_t aqualink_msg_to_frame(uint8_t *dest, const uint8_t *msg, size_t len);
int aqualink_frame_to_msg(uint8_t *dest, const uint8_t *frame, size_t len);

extern const struct device_ops jxi_heater_ops;
extern const struct device_ops rs_panel_ops;

static inline uint16_t read16_le(const uint8_t *raw)
{
	return (uint16_t)raw[1] << 8 | raw[0];
}


// void kvlist_init(struct kvlist *kv, int (*get_len)(struct kvlist *kv, const void *data));
// void kvlist_free(struct kvlist *kv);
// void *kvlist_get(struct kvlist *kv, const char *name);
// bool kvlist_set(struct kvlist *kv, const char *name, const void *data);
// bool kvlist_delete(struct kvlist *kv, const char *name);
//
// int kvlist_strlen(struct kvlist *kv, const void *data);
// int kvlist_blob_len(struct kvlist *kv, const void *data);


static inline int _get_int(struct kvlist *kvl, const char *name)
{
	struct property *prop = kvlist_get(kvl, name);

	if (!prop || prop->type != PROP_INT) {
		ULOG_ERR("OOPS, can't find property %s\n", name);
		return -INT_MAX;
	}

	return prop->ival;
}

static inline int dev_get_int(struct device *dev, const char *name)
{
	return _get_int(&dev->properties, name);
}

static inline int prop_get_int(struct device *dev, const char *name)
{
	return _get_int(dev->context_props, name);
}

static inline int _set_int(struct kvlist *kvl, const char *name, int val)
{
	struct property *prop = kvlist_get(kvl, name);

	if (!prop || prop->type != PROP_INT) {
		ULOG_ERR("OOPS, can't find property %s\n", name);
		return -INT_MAX;
	}

	prop->ival = val;

	return 0;
}

static inline int dev_set_int(struct device *dev, const char *name, int val)
{
	return _set_int(&dev->properties, name, val);
}

static inline int prop_set_int(struct device *dev, const char *name, int val)
{
	return _set_int(dev->context_props, name, val);
}

#endif /* AQUALINK_INTRERNAL_H */
