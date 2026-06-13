#define DT_DRV_COMPAT zmk_behavior_hid_signal_fixed

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
 * &<label>  (zero-cell, devicetree-configured)
 *
 * Same originator path as the 1-cell &signal, but the id lives on the node so the
 * editor can place it, AND each instance self-registers an enumerated capability:
 *
 *   signal_7: signal_7 {
 *       compatible = "zmk,behavior-hid-signal-fixed";
 *       #binding-cells = <0>;
 *       id = <7>;
 *   };                                          // bind: &signal_7
 *
 * Each instance registers a ROLE_TRIGGERS capability with the id baked into the
 * string ("signal.fire/7"), so a host scanning the manifest sees exactly which
 * ids the board fires — without the id ever carrying meaning on the wire (that
 * stays host config). Reuses the manifest's existing per-entry string encoding;
 * no wire-format change.
 */
struct hid_viz_signal_fixed_config {
    uint8_t id;
};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct hid_viz_signal_fixed_config *cfg = dev->config;

    hid_viz_send_signal(cfg->id);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* Fire-and-forget on press; nothing to do on release. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_hid_signal_fixed_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_hid_signal_fixed_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

/* Per-instance config + an enumerated triggers registration derived from `id`.
 * STRINGIFY(DT_INST_PROP(n, id)) folds the id into a compile-time string literal
 * ("signal.fire/7"), usable both in the registration and its BUILD_ASSERT. */
#define HID_VIZ_SIGNAL_FIXED_INST(n)                                                     \
    static const struct hid_viz_signal_fixed_config hid_viz_signal_fixed_config_##n = {  \
        .id = (uint8_t)DT_INST_PROP(n, id),                                              \
    };                                                                                   \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_hid_signal_fixed_init, NULL, NULL,               \
                            &hid_viz_signal_fixed_config_##n, POST_KERNEL,               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                          \
                            &behavior_hid_signal_fixed_driver_api);                      \
    HID_VIZ_CAP_REGISTER(cap_signal_##n, SIGNAL_FIRE, ROLE_TRIGGERS, TIER_CORE, 0,       \
                         "signal.fire/" STRINGIFY(DT_INST_PROP(n, id)));

DT_INST_FOREACH_STATUS_OKAY(HID_VIZ_SIGNAL_FIXED_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
