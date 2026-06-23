#include <raw_hid/events.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"
#include "hid_viz_send.h"

/*
 * Outbound notifications: subscribes to ZMK core events and sends
 * layer-state and key-event messages over Raw HID.
 *
 * Message types:
 *   0xFF - Layer state change (byte 6-9 = active layer bitmask)
 *   0xF1 - Key press/release (byte 2 = position, byte 3 = state)
 */

#define MSG_TYPE_LAYER_STATE 0xFF
#define MSG_TYPE_KEY_EVENT   0xF1
#define MAX_LAYER_CHECK      32

/* Manifest self-registration — one notifies entry per enabled notification.
 * Gated by the same flags that compile this file in (see CMakeLists.txt). */
#if IS_ENABLED(CONFIG_HID_VIZ_LAYER_EVENTS)
HID_VIZ_CAP_REGISTER(cap_core_layer_changed, MSG_LAYER_STATE, ROLE_NOTIFIES, TIER_CORE, 0,
                     "core.layer.changed");
#endif
#if IS_ENABLED(CONFIG_HID_VIZ_KEY_EVENTS)
HID_VIZ_CAP_REGISTER(cap_core_keyboard_key_event, MSG_KEY_EVENT, ROLE_NOTIFIES, TIER_OPTIONAL, 0,
                     "core.keyboard.key.event");
#endif

/* hid_viz_suppress_notifications is defined in hid_viz_send.c (always compiled)
 * and declared in hid_viz_send.h. */
static uint8_t layer_buf[CONFIG_RAW_HID_REPORT_SIZE];
static uint8_t key_buf[CONFIG_RAW_HID_REPORT_SIZE];

void send_layer_state(void) {
    uint32_t layer_state = 0;
    for (uint8_t i = 0; i < MAX_LAYER_CHECK; i++) {
        if (zmk_keymap_layer_active(i)) {
            layer_state |= BIT(i);
        }
    }

    uint32_t default_layer_state = BIT(0);

    memset(layer_buf, 0, sizeof(layer_buf));
    layer_buf[0] = MSG_TYPE_LAYER_STATE;
    layer_buf[1] = sizeof(uint32_t);
    memcpy(&layer_buf[2], &default_layer_state, sizeof(uint32_t));
    memcpy(&layer_buf[2 + sizeof(uint32_t)], &layer_state, sizeof(uint32_t));

    hid_viz_send(layer_buf, sizeof(layer_buf));
}

static void send_key_event(uint32_t position, bool pressed) {
    if (position > UINT8_MAX) {
        LOG_WRN("Position %u exceeds packet format", position);
        return;
    }

    memset(key_buf, 0, sizeof(key_buf));
    key_buf[0] = MSG_TYPE_KEY_EVENT;
    key_buf[1] = 0;
    key_buf[2] = (uint8_t)position;
    key_buf[3] = pressed ? 1 : 0;

    hid_viz_send(key_buf, sizeof(key_buf));
}

static int layer_state_changed_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    if (!hid_viz_suppress_notifications) {
        send_layer_state();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    send_key_event(ev->position, ev->state);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_layer_notifier, layer_state_changed_listener);
ZMK_SUBSCRIPTION(hid_viz_layer_notifier, zmk_layer_state_changed);

ZMK_LISTENER(hid_viz_key_notifier, position_state_changed_listener);
ZMK_SUBSCRIPTION(hid_viz_key_notifier, zmk_position_state_changed);
