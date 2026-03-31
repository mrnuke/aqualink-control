#include "aqualink-internal.h"

#include <errno.h>
#include <libubox/utils.h>
#include <libubox/ulog.h>
#include <stdio.h>
#include <string.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define STRING_CHUNK_SIZE		16
#define STRING_MAX_CHUNKS		4
#define STRING_MAX_CHARS	(STRING_CHUNK_SIZE * STRING_MAX_CHUNKS)

enum comm_state {
	COMM_IDLE,
	COMM_MULTI_STRING,
};

enum panel_state {
	PANEL_IDLE,
	PANEL_SEND_STATUS,
	PANEL_SEND_HELLO,
	PANEL_SEND_TEMPERATURE,
};

enum panel_commands {
	RS_ACK_WITH_DATA = 0x01,
	RS_LED_STATE = 0x02,
	RS_STRING = 0x03,
	RS_MULTI_STRING = 0x04,
};

enum menu_state {
	MENU_IDLE,
	MENU_ENTERED,
	MENU_TEMPERED,
	MENU_LOST_TEMPER,
};

enum leds {
	LED_AUX7		= (1 << 0),
	LED_AUX3		= (1 << 4),
	LED_AUX2		= (1 << 6),
	LED_AUX1		= (1 << 8),
	LED_SPA			= (1 << 10),
	LED_PUMP		= (1 << 12),
	LED_AUX5		= (1 << 14),
	LED_AUX4		= (1 << 16),
	LED_AUX6		= (1 << 22),
	LED_POOL_HEAT		= (1 << 28),
	LED_SPA_HEAT		= ((uint64_t)1 << 32),
	LED_EXTRA_AUX		= ((uint64_t)1 << 36),
};

struct panel {
	struct device *dev;
	enum comm_state comm_state;
	enum panel_state pstate;
	enum panel_state b4;
	enum panel_state rite_now;
	enum menu_state ms;
	struct prop_watcher mode_changed;
	struct prop_watcher leds_changed;
	char str_buf[STRING_MAX_CHARS];
	uint64_t led_state;
	int str_msg_idx;
	uint8_t last_btn;
};

struct menulizer {
	const char *enamel;
	struct menulizer *submenus;
};

static struct panel bad_idea = {
	.comm_state = COMM_IDLE,
	.pstate = PANEL_IDLE,
	.ms = MENU_IDLE,
};

const char *button_names[] = {
	[0x01] = "spa",
	[0x02] = "pump",
	[0x05] = "aux1",
	[0x06] = "aux4",
	[0x09] = "menu",
	[0x0a] = "aux2",
	[0x0b] = "aux5",
	[0x0e] = "menu exit",
	[0x0f] = "aux3",
	[0x10] = "aux6",
	[0x12] = "pool heat",
	[0x15] = "aux7",
	[0x17] = "spa heat",
	[0x18] = "menu updn",
	[0x1c] = "aux extra",
	[0x1d] = "menu enter",
};

static struct device *panel_to_dev(struct panel *panel)
{
	return panel->dev;
}

static struct panel *dev_to_panel(struct device *dev)
{
	(void)dev;

	return &bad_idea;
}

static const char *btn_name_get(unsigned int btn_code)
{
	if (!btn_code || btn_code >= ARRAY_SIZE(button_names))
		return NULL;

	return button_names[btn_code];
}

static int panel_handle_ack(struct device *dev, const uint8_t *msg, size_t len)
{
	uint8_t ack_flags, btn_code;
	const char *btn_name;
	struct panel *panel = &bad_idea;

	if (len < 4)
		return -ENODATA;

	ack_flags = msg[2];
	btn_code = msg[3];

	if (ack_flags & 0x01) {
		ULOG_INFO("Panel wants atencion\n");
	}

	if (!btn_code)
		return 0;

	btn_name = btn_name_get(btn_code);
	ULOG_INFO("Button '%s' (0x%x) pressed\n", btn_name, btn_code);
	switch (btn_code) {
	case 0x09:
		panel->ms = MENU_ENTERED;
		break;
	case 0x0e:
		panel->ms = MENU_IDLE;
		break;
	default:
		prop_set_string(dev, "button_pressed", btn_name);
		break;
	}

	if (panel->comm_state == COMM_IDLE || panel->ms != MENU_IDLE) {
		panel->last_btn = btn_code;
	} else {
		ULOG_ERR("The heck? Was not ready for button event\n");
		panel->ms = MENU_IDLE;
	}

	return 0;
}

static int send_led_status(const struct panel *panel, uint8_t* buf, size_t len)
{
	buf[1] = RS_LED_STATE;
	buf[2] = (panel->led_state  >> 0) & 0xFF;
	buf[3] = (panel->led_state  >> 8) & 0xFF;
	buf[4] = (panel->led_state  >> 16) & 0xFF;
	buf[5] = (panel->led_state  >> 24) & 0xFF;
	buf[6] = (panel->led_state  >> 32) & 0xFF;

	return 7;
}

static int panel_handle_reply(struct device *dev, const uint8_t *reply,
			    size_t len)
{
	uint8_t cmd = reply[1];
	int ret;

	switch (cmd) {
	case RS_ACK_WITH_DATA:
		ret = panel_handle_ack(dev, reply, len);
		break;
	default:
		ret = -EBADRQC;
		break;
	}

	return ret;
}


static void led_happened(struct prop_watcher *pw, const char *name,
			       const struct property *prop)
{
	struct panel *panel = container_of(pw, struct panel, leds_changed);
	struct device *dev = panel_to_dev(panel);
	if (!dev) {
		ULOG_ERR("No es devos!\n");
		return;
	}
	int rmap = prop->datum.ival;

	int mask, i;
	const int led_bits[] = {
		LED_PUMP, LED_AUX1, LED_AUX2, LED_AUX3, LED_AUX4, LED_AUX5,
		LED_AUX6, LED_AUX7
	};

	mask = (LED_PUMP | LED_AUX1 | LED_AUX2 | LED_AUX3 | LED_AUX4 |
		LED_AUX5 | LED_AUX6 | LED_AUX7);

	panel->led_state &= ~mask;
	for (i = 0; i < ARRAY_SIZE(led_bits); i++) {
		if (rmap & (1 << i))
			panel->led_state |= led_bits[i];
	}
}

static void something_happened(struct prop_watcher *pw, const char *name,
			       const struct property *prop)
{
	struct panel *panel = container_of(pw, struct panel, mode_changed);

	panel->led_state &= ~(LED_POOL_HEAT | LED_SPA_HEAT);

	if (!strcmp(prop->datum.string, "spa")) {
		panel->led_state |= LED_SPA_HEAT;
	} else if (!strcmp(prop->datum.string, "pool")) {
		panel->led_state |= LED_POOL_HEAT;
	}

	ULOG_INFO("Polla cuchi LED=0x%lx\n", panel->led_state);
}


int panel_init_properties(struct device *dev)
{
	/* Talk about circular logic. */
	struct panel *panel = dev_to_panel(dev);
	panel->dev = dev;

	panel->mode_changed.notify_change = something_happened;
	panel->leds_changed.notify_change = led_happened;
	prop_add_watcher(dev, "heating_mode", &panel->mode_changed);
	prop_add_watcher(dev, "relay_map", &panel->leds_changed);

	return 0;
}

static int fill_string_packet(uint8_t* buf, size_t buf_len,
			      const char *str, size_t str_len, uint8_t index)
{
	if (buf_len < (str_len + 3))
		return -ENOSPC;

	buf[1] = RS_STRING;
	buf[2] = index;
	memcpy(buf + 3, str, str_len);

	return 3 + str_len;
}

static int fill_multipart_string(struct device *dev, uint8_t* msg, size_t len)
{
	/* Shouldn't we be sending all the strings from one function? */
	struct panel *panel = &bad_idea;
	int ret, start_idx, end_idx, str_len, how_much;

	str_len = strlen(panel->str_buf) + 1;
	start_idx = panel->str_msg_idx * STRING_CHUNK_SIZE;
	end_idx = start_idx + STRING_CHUNK_SIZE;
	if (end_idx >= str_len) {
		end_idx = str_len;
		/* This is the last packet */
		panel->comm_state = COMM_IDLE;
	}

	how_much = min(str_len - start_idx, STRING_CHUNK_SIZE);

	ret = fill_string_packet(msg, len, panel->str_buf + start_idx,
				 how_much, ++panel->str_msg_idx);
	msg[1] = RS_MULTI_STRING;
	return ret;
}

static int send_display_string(struct panel *panel, uint8_t* buf, size_t len,
			       const char *str)
{
	size_t num_chars = strlen(str);
	int ret;

	if (num_chars < STRING_CHUNK_SIZE) {
		return fill_string_packet(buf, len, str, num_chars, 0);
	} else if (num_chars >= STRING_MAX_CHARS) {
		return -E2BIG;
	}

	panel->comm_state = COMM_MULTI_STRING;
	strcpy(panel->str_buf, str);
	panel->str_msg_idx = 0;
	ret = fill_string_packet(buf, len, str, STRING_CHUNK_SIZE,
				 ++panel->str_msg_idx);
	buf[1] = RS_MULTI_STRING;
	return ret;
}

static int menu_while_menu(struct panel *panel, uint8_t* msg, size_t len)
{
	int ret = 0;

	switch (panel->last_btn) {
	case 0x09:
		ret = send_display_string(panel, msg, len, "PRESS ENTER* TO SELECT");
		break;
	case 0x18:
		ret = send_display_string(panel, msg, len, "SET TEMP");
		break;
	case 0x1d:
		ret = send_display_string(panel, msg, len, "SET POOL TEMP");
		break;
	default:
		ret = 0;
		break;
	}

	panel->last_btn = 0;

	return ret;
}

static int panel_get_next_request(struct device *dev, uint8_t *buf, size_t len)
{
	struct panel *panel = dev_to_panel(dev);
	int rtx, temperature;
	char moo[32];

	switch (panel->comm_state) {
	case COMM_MULTI_STRING:
		return fill_multipart_string(dev, buf, len);
	default:
		break;
	}

	switch (panel->pstate) {
	default: /* fall through */
	case PANEL_IDLE:
		panel->pstate = PANEL_SEND_HELLO;
		return send_display_string(panel, buf, len, "Aqua Master!");
	case PANEL_SEND_TEMPERATURE: /* fall through */
	case PANEL_SEND_HELLO:
		panel->pstate = PANEL_SEND_STATUS;
		return send_led_status(panel, buf, len);
	case PANEL_SEND_STATUS:
		panel->pstate = PANEL_SEND_TEMPERATURE;
		temperature = prop_get_int(dev, "water_temp");
		snprintf(moo, 32, "POOL TEMP %d°C", temperature);
		return send_display_string(panel, buf, len, moo);
	}

	switch (panel->ms) {
		case MENU_ENTERED:
		case MENU_TEMPERED:
		case MENU_LOST_TEMPER:
			rtx = menu_while_menu(panel, buf, len);
			if (rtx == 0)
				rtx = send_led_status(panel, buf, len);
		return rtx;
		default:
			/* Carry on my hayward son, don't you jandy more. */
	}

	return -ENODEV;
}

const struct device_ops rs_panel_ops = {
	.init_properties = panel_init_properties,
	.handle_reply = panel_handle_reply,
	.get_next_request = panel_get_next_request,
};
