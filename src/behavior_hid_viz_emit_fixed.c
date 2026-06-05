#define DT_DRV_COMPAT zmk_behavior_hid_viz_emit_fixed

#include <zephyr/device.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <stdint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"
#include "emit_send.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * &<label>  (zero-cell, devicetree-configured)
 *
 * Same originator path as the 2-cell &hid_viz_emit, but action + value live on
 * the node so the editor can place it:
 *
 *   dpi_800: dpi_800 {
 *       compatible = "zmk,behavior-hid-viz-emit-fixed";
 *       #binding-cells = <0>;
 *       action = "core.pointing.dpi.set";
 *       value  = <800>;
 *   };                                          // bind: &dpi_800
 *
 * The `action` string enum is resolved to its wire byte/tier at compile time via
 * DT_INST_ENUM_IDX + the HID_VIZ_ACTION_* table, and each instance also registers
 * a ROLE_TRIGGERS capability so the manifest advertises it automatically.
 */
struct hid_viz_emit_fixed_config {
    uint8_t  action;
    uint32_t value;
};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct hid_viz_emit_fixed_config *cfg = dev->config;

    hid_viz_emit_send(cfg->action, cfg->value);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* Fire-and-forget on press; nothing to do on release. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_hid_viz_emit_fixed_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_hid_viz_emit_fixed_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

/* Per-instance config + a triggers registration derived from the `action` enum. */
#define HID_VIZ_EMIT_FIXED_INST(n)                                                       \
    static const struct hid_viz_emit_fixed_config hid_viz_emit_fixed_config_##n = {      \
        .action = HID_VIZ_ACTION_BYTE(DT_INST_ENUM_IDX(n, action)),                      \
        .value  = (uint32_t)DT_INST_PROP(n, value),                                      \
    };                                                                                   \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_hid_viz_emit_fixed_init, NULL, NULL,             \
                            &hid_viz_emit_fixed_config_##n, POST_KERNEL,                 \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                          \
                            &behavior_hid_viz_emit_fixed_driver_api);                    \
    HID_VIZ_CAP_REGISTER(cap_emit_##n,                                                   \
                         HID_VIZ_ACTION_BYTE(DT_INST_ENUM_IDX(n, action)),               \
                         ROLE_TRIGGERS,                                                  \
                         HID_VIZ_ACTION_TIER(DT_INST_ENUM_IDX(n, action)),               \
                         0,                                                              \
                         HID_VIZ_ACTION_ID(DT_INST_ENUM_IDX(n, action)));

DT_INST_FOREACH_STATUS_OKAY(HID_VIZ_EMIT_FIXED_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
