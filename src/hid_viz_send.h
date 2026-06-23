#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Serialized Raw HID transmit path — the single outbound funnel for this module.
 *
 * Every report this module emits (layer/key notifications, command responses,
 * confirmations, manifest entries, emit/signal triggers) goes through
 * hid_viz_send() instead of raising raw_hid_sent_event directly. ZMK dispatches
 * the raise synchronously in the caller's context, straight into the transport's
 * send_report(), which is NOT thread-safe: it drives one shared IN endpoint
 * behind one semaphore, and on USB it blocks up to ~30ms per report waiting for
 * the host to drain the previous one.
 *
 * A single mutex makes the whole raise -> send_report leg atomic, so the
 * dedicated manifest thread (see manifest.c) and an inbound command handler can
 * never sit inside send_report at the same time. The lock is held for exactly
 * one report (<=30ms), so a concurrent sender waits at most one report — never
 * the whole manifest stream.
 *
 * This is what makes streaming the manifest off a worker thread safe: before,
 * the ~20-entry stream ran synchronously inside the USB SetReport callback
 * context and blocked it for ~580ms, killing the nRF52840 USB peripheral.
 */
void hid_viz_send(const uint8_t *data, uint8_t len);

/*
 * Gate for spontaneous outbound notifications. Set true to suppress layer-state
 * (0xFF) notifications while a burst of state changes is in flight — the
 * command handler's deactivate/activate loop (0xFC) and the manifest stream both
 * raise it so intermediate/interleaved reports don't flood the host. Defined in
 * hid_viz_send.c (always compiled) so it resolves regardless of which optional
 * sources are built in.
 */
extern volatile bool hid_viz_suppress_notifications;
