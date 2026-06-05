#define DT_DRV_COMPAT zmk_behavior_hid_viz_emit

#include <zephyr/device.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <stdint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "emit_send.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * &hid_viz_emit <action> <value>
 *
 * The originator ("triggers") path for hand-written keymaps. On press it emits
 * the target action's own message — the SAME wire byte the fulfilling device's
 * handler consumes — onto the bus via the shared hid_viz_emit_send(). The
 * devicetree-configured sibling (zmk,behavior-hid-viz-emit-fixed) shares that
 * sender; see emit_send.c for the fire-and-forget semantics.
 *
 *   param1 = action wire byte (e.g. 0xE1 core.pointing.dpi.set, from
 *            <dt-bindings/hid_viz/emit.h>)
 *   param2 = value, sent as uint32 little-endian
 */
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    hid_viz_emit_send((uint8_t)binding->param1, binding->param2);

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
