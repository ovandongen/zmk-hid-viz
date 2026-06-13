#define DT_DRV_COMPAT zmk_behavior_hid_signal

#include <zephyr/device.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <stdint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"
#include "signal_send.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * &signal <id>
 *
 * The flexible keymap form of a generic host-defined trigger. On press it fires
 * the opaque id onto the bus via the shared hid_viz_send_signal(); the host maps
 * id -> action (see docs/signal-capability-spec.md). The id is a keymap binding
 * parameter, so it is NOT statically enumerable — this form advertises the bare
 * "signal.fire" capability ("I fire signals"). Use the devicetree-configured
 * sibling (zmk,behavior-hid-signal-fixed) when you want each id enumerated in the
 * manifest.
 *
 *   param1 = id (uint8), sent in byte[1]
 */
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    hid_viz_send_signal((uint8_t)binding->param1);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* Fire-and-forget on press; nothing to do on release. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_hid_signal_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_hid_signal_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, behavior_hid_signal_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_hid_signal_driver_api);

/* Opaque capability: present when a &signal node exists, regardless of which ids
 * the keymap binds (they can't be enumerated from binding params). */
HID_VIZ_CAP_REGISTER(cap_signal_fire, SIGNAL_FIRE,
                     ROLE_TRIGGERS, TIER_CORE, 0, "signal.fire");

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
