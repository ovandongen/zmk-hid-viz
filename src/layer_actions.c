#include <raw_hid/events.h>

#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"

/*
 * Layer action commands (M1 additions). A second listener on
 * raw_hid_received_event, alongside the existing command_handler.c; ZMK allows
 * multiple listeners per event and each one bubbles, so the two never conflict
 * (their command bytes are disjoint).
 *
 *   0xF4  core.layer.setBase   { byte[1] = layer index }
 *         Move to the target layer (ZMK's TO() behavior). Absolute, no confirm —
 *         the resulting 0xFF layer-state notification is the receipt.
 *
 *   0xF3  core.layer.activate  { byte[1..2] = ref (uint16 LE), byte[3] = index }
 *   0xF2  core.layer.deactivate{ byte[1..2] = ref (uint16 LE), byte[3] = index }
 *         Relative nudges. Confirm-flagged: reply 0xF7 { ref, ok } echoing the
 *         hub-assigned ref. No suppression is needed (each is a single op, unlike
 *         0xFC which loops deactivate+activate and would flood intermediate state).
 *
 * Layer indices are passed straight to the ZMK layer API, matching the existing
 * 0xFC handler in command_handler.c.
 */

#define HID_VIZ_OK   1
#define HID_VIZ_FAIL 0

/* Manifest self-registration — the layer-action handlers this file implements. */
HID_VIZ_CAP_REGISTER(cap_core_layer_setbase, CMD_LAYER_SET_BASE, ROLE_HANDLES, TIER_CORE, 0,
                     "core.layer.setBase");
HID_VIZ_CAP_REGISTER(cap_core_layer_activate, CMD_LAYER_ACTIVATE, ROLE_HANDLES, TIER_OPTIONAL, 1,
                     "core.layer.activate");
HID_VIZ_CAP_REGISTER(cap_core_layer_deactivate, CMD_LAYER_DEACTIVATE, ROLE_HANDLES, TIER_OPTIONAL, 1,
                     "core.layer.deactivate");

static uint8_t confirm_buf[CONFIG_RAW_HID_REPORT_SIZE];

static void send_confirm(uint16_t ref, uint8_t ok) {
    memset(confirm_buf, 0, sizeof(confirm_buf));
    confirm_buf[0] = RSP_CONFIRM;
    confirm_buf[1] = (uint8_t)(ref & 0xFF);
    confirm_buf[2] = (uint8_t)((ref >> 8) & 0xFF);
    confirm_buf[3] = ok;

    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event){.data = confirm_buf, .length = sizeof(confirm_buf)});
}

static void handle_set_base(uint8_t layer) {
    LOG_INF("Command: layer setBase -> %u", layer);
    /* zmk_keymap_layer_to() deactivates the app-managed layer stack and moves to
     * the target, like the TO() keymap behavior. Layers held by physical keys
     * (MO/MT) are unaffected. There is no setter for the compile-time default
     * layer in this fork, so this is the closest "set base" semantics. */
    zmk_keymap_layer_to(layer);
}

static void handle_activate(uint16_t ref, uint8_t layer) {
    LOG_INF("Command: layer activate ref=0x%04x index=%u", ref, layer);
    int ret = zmk_keymap_layer_activate(layer);
    send_confirm(ref, (ret == 0) ? HID_VIZ_OK : HID_VIZ_FAIL);
}

static void handle_deactivate(uint16_t ref, uint8_t layer) {
    LOG_INF("Command: layer deactivate ref=0x%04x index=%u", ref, layer);
    int ret = zmk_keymap_layer_deactivate(layer);
    send_confirm(ref, (ret == 0) ? HID_VIZ_OK : HID_VIZ_FAIL);
}

static int layer_actions_handler(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (event->data[0]) {
    case CMD_LAYER_SET_BASE:
        if (event->length >= 2) {
            handle_set_base(event->data[1]);
        }
        break;

    case CMD_LAYER_ACTIVATE:
        if (event->length >= 4) {
            uint16_t ref = (uint16_t)event->data[1] | ((uint16_t)event->data[2] << 8);
            handle_activate(ref, event->data[3]);
        }
        break;

    case CMD_LAYER_DEACTIVATE:
        if (event->length >= 4) {
            uint16_t ref = (uint16_t)event->data[1] | ((uint16_t)event->data[2] << 8);
            handle_deactivate(ref, event->data[3]);
        }
        break;

    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_layer_actions, layer_actions_handler);
ZMK_SUBSCRIPTION(hid_viz_layer_actions, raw_hid_received_event);
