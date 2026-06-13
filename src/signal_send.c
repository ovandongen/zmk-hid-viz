#include "signal_send.h"

#include "capabilities.h"

#include <raw_hid/events.h>

#include <string.h>
#include <stdint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * The signal originator path. With no listener present this is a silent no-op
 * (the report goes onto the bus into the void), which is correct fire-and-forget
 * behaviour. A signal is a momentary "id N happened" edge — it carries NO ref and
 * there is no ack; where feedback is wanted the host drives it (e.g. core.rgb.set)
 * from the outcome it observed. See docs/signal-capability-spec.md.
 */
static uint8_t signal_buf[CONFIG_RAW_HID_REPORT_SIZE];

void hid_viz_send_signal(uint8_t id) {
    memset(signal_buf, 0, sizeof(signal_buf));
    signal_buf[0] = SIGNAL_FIRE;
    signal_buf[1] = id;

    LOG_INF("hid_viz_signal: id=%u", id);

    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event){.data = signal_buf, .length = sizeof(signal_buf)});
}
