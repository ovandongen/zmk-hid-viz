#include <hid_viz/events.h>

#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Inbound command handler: receives commands from the host app over Raw HID
 * and responds or acts on them.
 *
 * Command protocol (host -> keyboard):
 *   byte[0] = command type
 *   byte[1..] = command-specific data
 *
 * Commands:
 *   0xFD - Get device info (responds with board ID and version)
 *   0xFC - Set layer state (bytes[1-4] = uint32 bitmask of layers to activate, LE)
 *   0xFB - Get config ID (responds with layout/config identifier string)
 *
 * Responses (keyboard -> host):
 *   0xFE - Device info response
 *   0xFA - Config ID response
 */

#define CMD_GET_DEVICE_INFO  0xFD
#define CMD_SET_LAYER        0xFC
#define CMD_GET_CONFIG_ID    0xFB
#define RSP_DEVICE_INFO      0xFE
#define RSP_CONFIG_ID        0xFA

#define HID_VIZ_PROTOCOL_VERSION 0x01

static uint8_t response_buf[CONFIG_RAW_HID_REPORT_SIZE];

static void send_device_info(void) {
    memset(response_buf, 0, sizeof(response_buf));
    response_buf[0] = RSP_DEVICE_INFO;
    response_buf[1] = HID_VIZ_PROTOCOL_VERSION;

    const char *board_id = CONFIG_ZMK_KEYBOARD_NAME;
    size_t id_len = strlen(board_id);
    if (id_len > CONFIG_RAW_HID_REPORT_SIZE - 2) {
        id_len = CONFIG_RAW_HID_REPORT_SIZE - 2;
    }
    memcpy(&response_buf[2], board_id, id_len);

    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event){.data = response_buf, .length = sizeof(response_buf)});
}

static void send_config_id(void) {
    memset(response_buf, 0, sizeof(response_buf));
    response_buf[0] = RSP_CONFIG_ID;

    const char *config_id = CONFIG_HID_VIZ_CONFIG_ID;
    size_t id_len = strlen(config_id);
    if (id_len > CONFIG_RAW_HID_REPORT_SIZE - 1) {
        id_len = CONFIG_RAW_HID_REPORT_SIZE - 1;
    }
    memcpy(&response_buf[1], config_id, id_len);

    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event){.data = response_buf, .length = sizeof(response_buf)});
}

static void handle_set_layer_state(uint32_t layer_state) {
    LOG_INF("Command: set layer state 0x%08x", layer_state);

    /* Deactivate all non-default layers first */
    for (uint8_t i = 1; i < 32; i++) {
        if (zmk_keymap_layer_active(i)) {
            zmk_keymap_layer_deactivate(i);
        }
    }

    /* Activate every layer requested by the bitmask (layer 0 stays active) */
    for (uint8_t i = 1; i < 32; i++) {
        if (layer_state & BIT(i)) {
            zmk_keymap_layer_activate(i);
        }
    }

    /* The layer changes will trigger zmk_layer_state_changed events,
     * which the notifier will pick up and send to the host automatically. */
}

static int command_handler(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t cmd = event->data[0];

    switch (cmd) {
    case CMD_GET_DEVICE_INFO:
        LOG_INF("Command: get device info");
        send_device_info();
        break;

    case CMD_GET_CONFIG_ID:
        LOG_INF("Command: get config ID");
        send_config_id();
        break;

    case CMD_SET_LAYER:
        if (event->length >= 5) {
            uint32_t state;
            memcpy(&state, &event->data[1], sizeof(uint32_t));
            handle_set_layer_state(state);
        }
        break;

    default:
        /* Unknown command — let other listeners handle it */
        LOG_DBG("Unknown command: 0x%02x", cmd);
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_command_handler, command_handler);
ZMK_SUBSCRIPTION(hid_viz_command_handler, raw_hid_received_event);
