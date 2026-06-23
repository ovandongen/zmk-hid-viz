#include <raw_hid/events.h>

#include <zmk/event_manager.h>
#include <zmk/rgb_underglow.h>
#include <zmk/behavior.h>
#include <dt-bindings/zmk/rgb.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"
#include "hid_viz_send.h"

/*
 * RGB profile (M5): `core.rgb.*` mapped onto stock ZMK underglow. Global
 * on/off + HSB + effect only — the portable surface shared with QMK. Per-key
 * colour is NOT exposed here; on ZMK the per-key picture is locally derived
 * from layer state, so hosts change it via `core.layer.set`
 * (see docs/rgb-capability-implementation.md).
 *
 *   0xD1  core.rgb.set  { on?, h?, s?, v?, effect? }  (absolute, idempotent)
 *     byte[1]   = field mask: bit0=on, bit1=h, bit2=s, bit3=v, bit4=effect
 *     byte[2]   = on (0/1)
 *     byte[3-4] = h, uint16 LE (0-359)
 *     byte[5]   = s (0-100)
 *     byte[6]   = v (0-100, maps to ZMK's brightness `b`)
 *     byte[7]   = effect index (into the device's effect ring)
 *     Bytes whose mask bit is clear are ignored (positions are fixed).
 *     An empty mask applies nothing but still gets the 0xD0 receipt, so it
 *     doubles as a state query.
 *
 *   0xD0  core.rgb.changed  { on, h, s, v, effect }  (receipt, latched)
 *     byte[1]   = on (0/1)
 *     byte[2-3] = h, uint16 LE
 *     byte[4]   = s
 *     byte[5]   = v
 *     byte[6]   = effect index
 *     `effect` is always present: when it selects a per-key/layer mode
 *     (fork territory), the reported h/s are dormant and the host must treat
 *     them as advisory.
 *
 * `speed` is omitted in v1 — ZMK only has a relative change_spd(), which
 * doesn't fit an absolute, idempotent set.
 *
 * SPLIT NOTE: changes are applied by invoking the `rgb_ug` behavior via
 * zmk_behavior_invoke_binding(), NOT by calling the zmk_rgb_underglow_* API
 * directly. The behavior has BEHAVIOR_LOCALITY_GLOBAL, so the invocation is
 * relayed to every split peripheral (each half runs its own underglow
 * instance) before running locally — same path as a `&rgb_ug` keymap key.
 * A direct API call would only light the central half. The MoErgo fork has
 * absolute commands for the whole surface: RGB_ON/OFF_CMD,
 * RGB_COLOR_HSB_CMD, and RGB_EFS_CMD (absolute effect select).
 */

/* Manifest self-registration — gated by CONFIG_HID_VIZ_RGB via CMakeLists. */
HID_VIZ_CAP_REGISTER(cap_core_rgb_changed, MSG_RGB_CHANGED, ROLE_NOTIFIES, TIER_PROFILE, 0,
                     "core.rgb.changed");
HID_VIZ_CAP_REGISTER(cap_core_rgb_set, CMD_RGB_SET, ROLE_HANDLES, TIER_PROFILE, 0,
                     "core.rgb.set");

#define RGB_SET_FIELD_ON     BIT(0)
#define RGB_SET_FIELD_H      BIT(1)
#define RGB_SET_FIELD_S      BIT(2)
#define RGB_SET_FIELD_V      BIT(3)
#define RGB_SET_FIELD_EFFECT BIT(4)

/* Full fixed layout: type + mask + on + h(2) + s + v + effect */
#define RGB_SET_MIN_LENGTH   8

#define RGB_HUE_MAX 359
#define RGB_SAT_MAX 100
#define RGB_BRT_MAX 100

/* Last values written through this module. zmk_rgb_underglow_get_state()
 * reports on/off only — there is no public HSB/effect getter — so 0xD0
 * receipts source these statics. Seeded with the compile-time start values;
 * until the first set they can disagree with state restored from flash, and
 * keymap &rgb_ug behaviours (and Magic+T on the Glove80) call the underglow
 * API directly, bypassing these. Documented desync; honest reporting needs a
 * fork-side getter + change event. */
static uint16_t cur_h = CONFIG_ZMK_RGB_UNDERGLOW_HUE_START;
static uint8_t cur_s = CONFIG_ZMK_RGB_UNDERGLOW_SAT_START;
static uint8_t cur_v = CONFIG_ZMK_RGB_UNDERGLOW_BRT_START;
static uint8_t cur_effect = CONFIG_ZMK_RGB_UNDERGLOW_EFF_START;

static uint8_t rgb_buf[CONFIG_RAW_HID_REPORT_SIZE];

static void send_rgb_changed(void) {
    bool on = false;
    zmk_rgb_underglow_get_state(&on);

    memset(rgb_buf, 0, sizeof(rgb_buf));
    rgb_buf[0] = MSG_RGB_CHANGED;
    rgb_buf[1] = on ? 1 : 0;
    rgb_buf[2] = (uint8_t)(cur_h & 0xFF);
    rgb_buf[3] = (uint8_t)((cur_h >> 8) & 0xFF);
    rgb_buf[4] = cur_s;
    rgb_buf[5] = cur_v;
    rgb_buf[6] = cur_effect;

    hid_viz_send(rgb_buf, sizeof(rgb_buf));
}

/* Invoke the rgb_ug behavior (press only — its release handler is a no-op).
 * Returns the local invocation's result; peripheral relays are
 * fire-and-forget inside zmk_behavior_invoke_binding(). */
static int rgb_behavior(uint32_t cmd, uint32_t param) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "rgb_ug",
        .param1 = cmd,
        .param2 = param,
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
    };
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static void handle_rgb_set(const uint8_t *data) {
    uint8_t fields = data[1];

    LOG_INF("Command: rgb set mask=0x%02x", fields);

    if (fields & (RGB_SET_FIELD_H | RGB_SET_FIELD_S | RGB_SET_FIELD_V)) {
        /* Absent fields keep their last-set value so the full HSB triple the
         * behavior takes doesn't clobber them. */
        uint16_t h = (fields & RGB_SET_FIELD_H)
                         ? MIN((uint16_t)data[3] | ((uint16_t)data[4] << 8), RGB_HUE_MAX)
                         : cur_h;
        uint8_t s = (fields & RGB_SET_FIELD_S) ? MIN(data[5], RGB_SAT_MAX) : cur_s;
        uint8_t v = (fields & RGB_SET_FIELD_V) ? MIN(data[6], RGB_BRT_MAX) : cur_v;
        if (rgb_behavior(RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(h, s, v)) == 0) {
            cur_h = h;
            cur_s = s;
            cur_v = v;
        } else {
            LOG_WRN("rgb set: hsb(%u,%u,%u) rejected", h, s, v);
        }
    }

    if (fields & RGB_SET_FIELD_EFFECT) {
        if (rgb_behavior(RGB_EFS_CMD, data[7]) == 0) {
            cur_effect = data[7];
        } else {
            LOG_WRN("rgb set: effect %u out of range", data[7]);
        }
    }

    /* on/off last, so switching on reveals the colour/effect just applied
     * instead of one frame of the old state. */
    if (fields & RGB_SET_FIELD_ON) {
        rgb_behavior(data[2] ? RGB_ON_CMD : RGB_OFF_CMD, 0);
    }

    /* Receipt — also serves as the read path for an empty mask. */
    send_rgb_changed();
}

static int rgb_actions_handler(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->data[0] == CMD_RGB_SET && event->length >= RGB_SET_MIN_LENGTH) {
        handle_rgb_set(event->data);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_rgb_actions, rgb_actions_handler);
ZMK_SUBSCRIPTION(hid_viz_rgb_actions, raw_hid_received_event);
