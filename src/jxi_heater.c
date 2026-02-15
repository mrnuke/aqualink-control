#include "aqualink-internal.h"

#include <errno.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

enum jxi_commands {
	JXI_COMMAND = 0x0c,
	JXI_COMMAND_REPLY = 0x0d,
	JXI_GET_MEASUREMENTS = 0x25,
};

static int jxi_handle_control_response(struct device *dev,
				       const uint8_t *msg, size_t len)
{
	uint8_t status, huh, errors;

	if (len < 5)
		return -ENODATA;

	status = msg[2];
	huh = msg[3];
	errors = msg[4];

	ULOG_INFO("sflags=%x, unknown=%x, eflags=%x\n", status, huh, errors);

	if (status & 0x08)
		ULOG_INFO("Heater is on or in the process of igniting");
	if (status & 0x10)
		ULOG_INFO("Remote RS-485 is disabled at the panel");
	if (errors & 0x08)
		ULOG_ERR("heater no burno\n");

	return 0;
}

/*
 * Cousin AI suggested:
 * // Inside jxi_heater_handle_measurements
 * prop_set_int(dev, "gv_hours", gv_on_time);
 * prop_set_int(dev, "ignition_cycles", cycles);
 * prop_set_int(dev, "water_temp", temperature);
 */
static int jxi_handle_measurements(struct device *dev,
				   const uint8_t *msg, size_t len)
{
	if (len < 9)
		return -ENODATA;

	prop_set_int(dev, "water_temp", (int)msg[8] - 20);
	dev_set_int(dev, "gv_on_time", read16_le(msg + 2));
	dev_set_int(dev, "ignition_cycles", read16_le(msg + 4));

	return 0;
}


static int jxi_handle_reply(struct device *dev, const uint8_t *reply,
			    size_t len)
{
	uint8_t cmd = reply[1];
	int ret;

	switch (cmd) {
		case JXI_COMMAND_REPLY:
			ret = jxi_handle_control_response(dev, reply, len);
			break;
		case JXI_GET_MEASUREMENTS:
			ret = jxi_handle_measurements(dev, reply, len);
			break;
		default:
			ret = -EBADRQC;
			break;
	}

	return ret;
}

/*
 * Cousin AI:
 * // Inside jxi_heater_get_next_request
 * uint8_t flags = 0x08; // Always "Enable Heater" if we are in this state
 * if (prop_get_bool(dev, "pool_enabled")) flags |= 0x01;
 * if (prop_get_bool(dev, "spa_enabled"))  flags |= 0x02;
 *
 * buf[0] = 0x0c;
 * buf[1] = flags;
 * buf[2] = (uint8_t)prop_get_int(dev, "pool_setpoint");
 * buf[3] = (uint8_t)prop_get_int(dev, "spa_setpoint");
 * buf[4] = 0xff; // The "Aquadick" External Temp Null
 */
static int jxi_get_next_request(struct device *dev, uint8_t* msg, size_t len)
{
	msg[0] = 0x68;
	msg[1] = JXI_GET_MEASUREMENTS;

	return 2;
}

int jxi_init_properties(struct device *dev)
{
	struct property prop_uno;
	int i;

	const struct {
		const char *name;
		int type;
	} propellers[] = {
		{ "celsius", PROP_BOOL},
		// { "control_flags': 0,
		{ "cycles", PROP_INT },
		// { "error_flags': 0,
		{ "ext_temp_valid", PROP_BOOL},
		{ "external_temp_reading", PROP_INT },
		{ "gv_on_time", PROP_INT },
		{ "ignition_cycles", PROP_INT },
		{ "heater_error", PROP_BOOL},
		{ "heater_on", PROP_BOOL},
		{ "last_fault", PROP_INT },
		{ "pool", PROP_BOOL},
		{ "prev_fault", PROP_INT },
		{ "remote_rs485_disabled", PROP_BOOL},
		{ "setpoint_pool", PROP_INT },
		{ "setpoint_spa", PROP_INT },
		{ "spa", PROP_BOOL},
		// { "status_flags': 0,
		{ "water_temp", PROP_INT },
	};

	for (i = 0; i < ARRAY_SIZE(propellers); i++) {
		prop_uno.type = propellers[i].type;
		kvlist_set(&dev->properties, propellers[i]. name, &prop_uno);
	}

	return 0;
}

const struct device_ops jxi_heater_ops = {
	.init_properties = jxi_init_properties,
	.handle_reply = jxi_handle_reply,
	.get_next_request = jxi_get_next_request,
};
