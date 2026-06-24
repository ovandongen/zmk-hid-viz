#pragma once

#include <zmk/event_manager.h>

#include <stdint.h>

/*
 * Internal inbound-command event.
 *
 * The transport (zmk-raw-hid) raises raw_hid_received_event synchronously inside
 * the USB SetReport (OUT) control-transfer callback. ZMK dispatches events in
 * the caller's context, so a listener on raw_hid_received_event runs *on that
 * callback*. Any report it then sends goes through the USB send_report(), which
 * blocks up to ~30ms on the IN endpoint — a blocking IN wait inside the OUT
 * callback, which can wedge and de-enumerate the nRF52840 USB peripheral.
 *
 * command_dispatch.c is the single listener on raw_hid_received_event: it only
 * copies the report and hands it to a worker thread, then returns immediately.
 * The worker re-raises THIS event, so every command handler runs off the USB
 * callback context where a blocking send is harmless.
 *
 * INVARIANT: inbound command handlers must subscribe to raw_hid_command_event,
 * never to raw_hid_received_event directly. The only sanctioned direct
 * subscribers to raw_hid_received_event are this dispatcher and the manifest
 * streamer (manifest.c), which already self-defers to its own worker.
 */
struct raw_hid_command_event {
    uint8_t *data;
    uint8_t length;
};

ZMK_EVENT_DECLARE(raw_hid_command_event);
