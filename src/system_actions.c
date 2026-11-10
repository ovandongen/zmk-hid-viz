#include "command_dispatch.h"

#include <zmk/event_manager.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h> /* ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL */
#include <zmk/usb.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"
#include "hid_viz_send.h"

/*
 * core.system.* — put a half into its UF2 bootloader, or reset it, from the host
 * (CONFIG_HID_VIZ_SYSTEM, default off). Lets a flashing tool start a flash
 * without anyone pressing a bootloader key.
 *
 *   0xB0  core.system.bootloader
 *   0xB1  core.system.reset
 *     byte[0]     = command type
 *     byte[1..2]  = ref (uint16 LE), echoed in the 0xF7 confirm
 *     byte[3]     = target: 0xFF = this (central) half, 0..n-1 = peripheral slot
 *     byte[4..9]  = magic: "HVBOOT" for 0xB0, "HVRSET" for 0xB1
 *     byte[10]    = CRC-8 (poly 0x07, init 0x00, no reflection) over byte[0..9]
 *     byte[11..]  = must be zero
 *
 *   Reply: 0xF7 { ref, ok }. ok=1 means "accepted": for the central the reset
 *   follows ~100ms after the reply; for a peripheral it means the command was
 *   queued on the split link — ZMK can't tell us it arrived, so the host should
 *   treat the bootloader drive appearing as the real success signal.
 *   ok=0: target slot doesn't exist, USB isn't up, or the relay refused.
 *
 * GUARD (accidents, not attackers — the magic is in public source): a report
 * that fails the length / magic / CRC / zero-padding checks is ignored without
 * any reply, since it most likely wasn't meant for us. The two commands use
 * different magics so a damaged reset can never become a bootloader and vice
 * versa. The real security boundary is the Kconfig switch being off by default.
 *
 * USB GATE: zmk-raw-hid raises the same inbound event for BLE writes as for USB,
 * with no transport tag. Refusing unless USB HID is up on this half keeps a
 * BLE-only host from triggering it (a UF2 flash needs the cable anyway).
 *
 * ROUTING: &bootloader / &sys_reset have BEHAVIOR_LOCALITY_EVENT_SOURCE, so
 * event.source picks the half: ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL runs it
 * here, any other value relays it to that peripheral slot over ZMK's own split
 * link — the peripheral needs no module. NOTE: unlike rgb_actions.c, source MUST
 * be set explicitly; left at 0 it would target peripheral slot 0. The field only
 * exists under CONFIG_ZMK_SPLIT.
 *
 * MANIFEST: advertised as "core.system.bootloader/<n>" and "core.system.reset/<n>"
 * where <n> is the number of peripheral slots (0 on non-split), following the
 * signal.fire/<id> suffix convention. Strip at the last '/' to get the ID.
 */

/* ---- Behaviors to invoke, by their device names (not hard-coded strings) ---- */

#if !DT_NODE_HAS_STATUS(DT_NODELABEL(bootloader), okay) ||                       \
    !DT_NODE_HAS_STATUS(DT_NODELABEL(sys_reset), okay)
#error "CONFIG_HID_VIZ_SYSTEM needs ZMK's &bootloader and &sys_reset behavior nodes"
#endif

#define SYS_BOOTLOADER_DEV DEVICE_DT_NAME(DT_NODELABEL(bootloader))
#define SYS_RESET_DEV      DEVICE_DT_NAME(DT_NODELABEL(sys_reset))

/* ---- Peripheral slot count (compile-time, as a literal for the manifest ID) ----
 * ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT is a MAX() expression, which STRINGIFY can't
 * turn into "1", so derive it from the plain Kconfig ints and assert it agrees. */

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#define SYS_PERIPHERAL_SLOTS CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_WIRED)
#define SYS_PERIPHERAL_SLOTS 1
#else
#define SYS_PERIPHERAL_SLOTS 0
#endif
BUILD_ASSERT(SYS_PERIPHERAL_SLOTS == ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT,
             "hid_viz system: peripheral slot count disagrees with ZMK");
#else
#define SYS_PERIPHERAL_SLOTS 0
#endif

/* The BLE split payload carries the behavior name in a 9-byte field and older
 * trees truncate it silently, so a renamed/longer node would relay garbage. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#include <zmk/split/bluetooth/service.h>
BUILD_ASSERT(sizeof(SYS_BOOTLOADER_DEV) <= ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN,
             "&bootloader node name too long for the BLE split payload");
BUILD_ASSERT(sizeof(SYS_RESET_DEV) <= ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN,
             "&sys_reset node name too long for the BLE split payload");
#endif

/* Manifest self-registration — gated by CONFIG_HID_VIZ_SYSTEM via CMakeLists. */
HID_VIZ_CAP_REGISTER(cap_core_system_bootloader, CMD_SYSTEM_BOOTLOADER, ROLE_HANDLES,
                     TIER_OPTIONAL, 1, "core.system.bootloader/" STRINGIFY(SYS_PERIPHERAL_SLOTS));
HID_VIZ_CAP_REGISTER(cap_core_system_reset, CMD_SYSTEM_RESET, ROLE_HANDLES, TIER_OPTIONAL, 1,
                     "core.system.reset/" STRINGIFY(SYS_PERIPHERAL_SLOTS));

/* ---- Wire format ---- */

#define SYS_TARGET_CENTRAL 0xFF
#define SYS_MAGIC_OFFSET   4
#define SYS_MAGIC_LEN      6
#define SYS_CRC_OFFSET     (SYS_MAGIC_OFFSET + SYS_MAGIC_LEN) /* 10 */
#define SYS_PACKET_LEN     (SYS_CRC_OFFSET + 1)               /* 11 */

BUILD_ASSERT(CONFIG_RAW_HID_REPORT_SIZE >= SYS_PACKET_LEN,
             "raw HID report too small for core.system commands");

static const uint8_t magic_bootloader[SYS_MAGIC_LEN] = {'H', 'V', 'B', 'O', 'O', 'T'};
static const uint8_t magic_reset[SYS_MAGIC_LEN] = {'H', 'V', 'R', 'S', 'E', 'T'};

#define HID_VIZ_OK   1
#define HID_VIZ_FAIL 0

/* Delay between sending the confirm and rebooting this half, so the 0xF7 report
 * has left the IN endpoint and been read by the host. */
#define SYS_LOCAL_RESET_DELAY_MS 100

/* CRC-8, poly 0x07, init 0x00 (a.k.a. CRC-8/SMBUS; same as Zephyr's
 * crc8_ccitt(0, ...)). Inlined to avoid depending on CONFIG_CRC across trees. */
static uint8_t sys_crc8(const uint8_t *p, size_t n) {
    uint8_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static uint8_t confirm_buf[CONFIG_RAW_HID_REPORT_SIZE];

static void send_confirm(uint16_t ref, uint8_t ok) {
    memset(confirm_buf, 0, sizeof(confirm_buf));
    confirm_buf[0] = RSP_CONFIRM;
    confirm_buf[1] = (uint8_t)(ref & 0xFF);
    confirm_buf[2] = (uint8_t)((ref >> 8) & 0xFF);
    confirm_buf[3] = ok;

    hid_viz_send(confirm_buf, sizeof(confirm_buf));
}

/* Press the behavior on the half chosen by `source`. Press only: the reset
 * behaviors act on press and their release is a no-op. On the local half a
 * successful call does not return. */
static int invoke_on(const char *behavior_dev, uint8_t source) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_dev,
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = source,
#endif
    };
    ARG_UNUSED(source); /* non-split: there is only this half */
    return zmk_behavior_invoke_binding(&binding, event, true);
}

/* ---- Deferred local reset ---- */

static const char *local_reset_dev;
static atomic_t local_reset_pending = ATOMIC_INIT(0);

static void local_reset_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    int ret = invoke_on(local_reset_dev, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
    /* Only reached if the reboot didn't happen. */
    LOG_ERR("system: local %s failed (%d)", local_reset_dev, ret);
    atomic_clear(&local_reset_pending);
}

static K_WORK_DELAYABLE_DEFINE(local_reset_work, local_reset_work_fn);

/* ---- Command handling ---- */

/* True if `data` is a well-formed packet for this command. */
static bool packet_valid(const uint8_t *data, uint8_t length, const uint8_t *magic) {
    if (length < SYS_PACKET_LEN) {
        return false;
    }
    if (memcmp(&data[SYS_MAGIC_OFFSET], magic, SYS_MAGIC_LEN) != 0) {
        return false;
    }
    if (sys_crc8(data, SYS_CRC_OFFSET) != data[SYS_CRC_OFFSET]) {
        return false;
    }
    for (uint8_t i = SYS_PACKET_LEN; i < length; i++) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

static void handle_system(const uint8_t *data, uint8_t length, const char *behavior_dev,
                          const uint8_t *magic, const char *what) {
    if (!packet_valid(data, length, magic)) {
        LOG_WRN("system: ignoring malformed %s command", what);
        return;
    }

    uint16_t ref = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    uint8_t target = data[3];

    if (atomic_get(&local_reset_pending)) {
        LOG_WRN("system: %s ignored, this half is already resetting", what);
        return;
    }

    if (!zmk_usb_is_hid_ready()) {
        LOG_WRN("system: %s refused, USB HID not active", what);
        send_confirm(ref, HID_VIZ_FAIL);
        return;
    }

    if (target == SYS_TARGET_CENTRAL) {
        LOG_INF("system: %s this half (ref=0x%04x)", what, ref);
        atomic_set(&local_reset_pending, 1);
        local_reset_dev = behavior_dev;
        /* Reply first, then reboot, so the confirm reaches the host. */
        send_confirm(ref, HID_VIZ_OK);
        k_work_schedule(&local_reset_work, K_MSEC(SYS_LOCAL_RESET_DELAY_MS));
        return;
    }

    if (target >= SYS_PERIPHERAL_SLOTS) {
        LOG_WRN("system: %s target %u out of range (%u slots)", what, target,
                SYS_PERIPHERAL_SLOTS);
        send_confirm(ref, HID_VIZ_FAIL);
        return;
    }

    int ret = invoke_on(behavior_dev, target);
    LOG_INF("system: %s peripheral slot %u (ref=0x%04x) -> %d", what, target, ref, ret);
    send_confirm(ref, (ret == 0) ? HID_VIZ_OK : HID_VIZ_FAIL);
}

static int system_actions_handler(const zmk_event_t *eh) {
    struct raw_hid_command_event *event = as_raw_hid_command_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (event->data[0]) {
    case CMD_SYSTEM_BOOTLOADER:
        handle_system(event->data, event->length, SYS_BOOTLOADER_DEV, magic_bootloader,
                      "bootloader");
        break;
    case CMD_SYSTEM_RESET:
        handle_system(event->data, event->length, SYS_RESET_DEV, magic_reset, "reset");
        break;
    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_system_actions, system_actions_handler);
ZMK_SUBSCRIPTION(hid_viz_system_actions, raw_hid_command_event);
