#include "emit_send.h"

#include "hid_viz_send.h"

#include <raw_hid/events.h>

#include <string.h>
#include <stdint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * The originator path. With no fulfiller present this is a silent no-op (the
 * report goes onto the bus into the void), which is correct fire-and-forget
 * behavior. It carries NO ref: the hub assigns refs on the hub->handler leg; the
 * originator never learns the outcome directly and observes the resulting
 * *.changed event if it cares.
 */
static uint8_t emit_buf[CONFIG_RAW_HID_REPORT_SIZE];

void hid_viz_emit_send(uint8_t action, uint32_t value) {
    memset(emit_buf, 0, sizeof(emit_buf));
    emit_buf[0] = action;
    emit_buf[1] = (uint8_t)(value & 0xFF);
    emit_buf[2] = (uint8_t)((value >> 8) & 0xFF);
    emit_buf[3] = (uint8_t)((value >> 16) & 0xFF);
    emit_buf[4] = (uint8_t)((value >> 24) & 0xFF);

    LOG_INF("hid_viz_emit: action=0x%02x value=%u", action, value);

    hid_viz_send(emit_buf, sizeof(emit_buf));
}
