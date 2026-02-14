#include "aqualink-internal.h"

#include <errno.h>
#include <libubox/utils.h>
#include <libubox/ulog.h>
#include <stdio.h>
#include <string.h>

const char *button_names[] = {
	[0x01] = "spa",
	[0x02] = "pool",
	[0x05] = "aux1",
	[0x06] = "aux4",
	[0x0a] = "menu",
	[0x0a] = "aux2",
	[0x0b] = "aux5",
	[0x0f] = "aux3",
	[0x10] = "aux6",
	[0x12] = "pool heat",
	[0x15] = "aux7",
	[0x17] = "spa heat",
	[0x1c] = "aux extra",
};

static bool device_get_bool(void *fema, ...)
{
	return false;
}

static const char *btn_name_get(unsigned int btn_code)
{

	if (!btn_code || btn_code >= ARRAY_SIZE(button_names))
		return NULL;

	return button_names[btn_code];
}

static int ping_pong = 0;
static int dobi_toc = 0;

static int panel_handle_ack(struct device *dev, const uint8_t *msg, size_t len)
{
	uint8_t /*flaggy_fags,*/ btn_code;

	if (len < 4)
		return -ENODATA;

	// flaggy_fags = msg[2];
	btn_code = msg[3];

	if (!btn_code)
		return 0;

	ULOG_INFO("Button '%s' (0x%x) pressed\n", btn_name_get(btn_code), btn_code);
	// Logic to toggle properties based on buttons could go here

	return 0;
}

static int send_led_status(struct device *dev, uint8_t* msg)
{
	/*
	 * Aqualink LED bitmask typically follows:
	 * 0x01: Pool, 0x02: Spa, 0x04: Aux1, 0x08: Aux2...
	 * This builds a bitmask by checking the boolean properties.
	 */
	uint32_t leds = 0;

	if (device_get_bool(dev, "pool"))
		leds |= (1 << 0);
	if (device_get_bool(dev, "spa"))
		leds |= (1 << 1);
	if (device_get_bool(dev, "pool_heat"))
		leds |= (1 << 2);
	if (device_get_bool(dev, "spa_heat"))
		leds |= (1 << 3);
	if (device_get_bool(dev, "aux1"))
		leds |= (1 << 4);

	msg[0] = dev->addr;
	msg[1] = 0x02; // LED command
	msg[2] = (leds >> 24) & 0xFF;
	msg[3] = (leds >> 16) & 0xFF;
	msg[4] = (leds >> 8) & 0xFF;
	msg[5] = leds & 0xFF;
	msg[5] = ~leds & 0xFF;

	return 7;
}

static int panel_handle_reply(struct device *dev, const uint8_t *reply,
			    size_t len)
{
	uint8_t cmd = reply[1];
	int ret;

	switch (cmd) {
	case 1: /*AQUA_PROBE_RESPONSE*/
		ret = panel_handle_ack(dev, reply, len);
		break;
	default:
		ret = -EBADRQC;
		break;
	}

	return ret;
}

int panel_init_properties(struct device *dev)
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
		{ "timeout'", PROP_INT },
		{ "water_temp", PROP_INT },
	};

	for (i = 0; i < ARRAY_SIZE(propellers); i++) {
		prop_uno.type = propellers[i].type;
		kvlist_set(&dev->properties, propellers[i]. name, &prop_uno);
	}

	return 0;
}

static int send_display_string(struct device *dev, uint8_t* msg, const char *str)
{
	size_t lennus = strlen(str);

	msg[0] = dev->addr;
	msg[1] = 0x03; // String command
	// msg[2] = strlen(str);
	msg[2] = 0;
	memcpy(msg + 3, str, lennus);

	return 3 + lennus;
}

static int satanic_string(struct device *dev, uint8_t* msg, size_t len,
			   const char *str)
{
	if (strlen(str) > 13)
		return -E2BIG;
/*
	msg[0] = dev->addr;
	msg[1] = 0x03;
	msg[2] = 0;
	memcpy(msg + 3, str, strlen(str));

	return 3 + strlen(str);*/
	return send_display_string(dev, msg, str);
}

// static int send_ledsky(struct device *dev, uint8_t* msg, size_t len)
// {
// 	msg[0] = dev->addr;
// 	msg[1] = 0x02;
// 	msg[2] = dobi_toc >> 24;
// 	msg[3] = dobi_toc >> 16;
// 	msg[4] = dobi_toc >> 8;
// 	msg[5] = dobi_toc >> 0;
// 	msg[6] = ~(dobi_toc >> 0);
//
// 	return 7;
// }


static int panel_get_next_request(struct device *dev, uint8_t* msg, size_t len)
{
	int temperature;
	char moo[32];

	switch (++ping_pong & 0x3) {
	case 0:
		dobi_toc = dobi_toc ? dobi_toc << 1 : 1;
		snprintf(moo, 32, "Fuck 0x%x", dobi_toc);
		return satanic_string(dev, msg, len, moo);
	case 1:
		return send_led_status(dev, msg);
	case 2:
		temperature = prop_get_int(dev, "water_temp");
		snprintf(moo, 32, "POOL TEMP %dC", temperature);
		return satanic_string(dev, msg, len, moo);
	case 3:
		temperature = prop_get_int(dev, "water_temp");
		snprintf(moo, 32, "SPA TEMP %dC", temperature);
		return satanic_string(dev, msg, len, moo);
	}

	return -ENODEV;
}

const struct device_ops rs_panel_ops = {
	.init_properties = panel_init_properties,
	.handle_reply = panel_handle_reply,
	.get_next_request = panel_get_next_request,
};
