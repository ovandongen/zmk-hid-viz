#define DT_DRV_COMPAT zmk_behavior_hid_viz_emit

#include <zephyr/device.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <raw_hid/events.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * &hid_viz_emit <action> <value>
 *
 * The originator ("triggers") path. On press it builds the target action's own
 * message — the SAME wire byte the fulfilling device's handler consumes — and
 * pushes it onto the bus over Raw HID. It carries NO ref: the hub assigns refs
 * on the hub->handler leg, and the originator is fire-and-forget. It never
 * learns the outcome directly; it observes the resulting *.changed event if it
 * cares. With no fulfiller present this is a silent no-op (originates into the
 * void), which is correct fire-and-forget behavior.
 *
 *   byte[0]    = action wire byte (param1, e.g. 0xE1 core.pointing.dpi.set)
 *   byte[1..4] = value (param2) as uint32 little-endian
 */
static uint8_t emit_buf[CONFIG_RAW_HID_REPORT_SIZE];

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint8_t action = (uint8_t)binding->param1;
    uint32_t value = binding->param2;

    memset(emit_buf, 0, sizeof(emit_buf));
    emit_buf[0] = action;
    emit_buf[1] = (uint8_t)(value & 0xFF);
    emit_buf[2] = (uint8_t)((value >> 8) & 0xFF);
    emit_buf[3] = (uint8_t)((value >> 16) & 0xFF);
    emit_buf[4] = (uint8_t)((value >> 24) & 0xFF);

    LOG_INF("hid_viz_emit: action=0x%02x value=%u", action, value);

    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event){.data = emit_buf, .length = sizeof(emit_buf)});

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* Fire-and-forget on press; nothing to do on release. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_hid_viz_emit_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_hid_viz_emit_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, behavior_hid_viz_emit_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_hid_viz_emit_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
