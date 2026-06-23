#include "hid_viz_send.h"

#include <raw_hid/events.h>

#include <stdint.h>

#include <zephyr/kernel.h>

/* Serializes the raise -> transport send_report leg across all senders (the
 * manifest worker thread, the command/layer/rgb handlers running on the inbound
 * dispatch context, and the notifier on ZMK core-event contexts). See
 * hid_viz_send.h for why this is required. */
K_MUTEX_DEFINE(hid_viz_tx_mutex);

/* Gate for spontaneous layer-state notifications (see hid_viz_send.h). Lives
 * here, in the always-compiled funnel, so every consumer resolves it no matter
 * which optional sources are enabled. */
volatile bool hid_viz_suppress_notifications = false;

void hid_viz_send(const uint8_t *data, uint8_t len) {
    k_mutex_lock(&hid_viz_tx_mutex, K_FOREVER);
    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event){.data = (uint8_t *)data, .length = len});
    k_mutex_unlock(&hid_viz_tx_mutex);
}
