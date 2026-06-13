#pragma once

#include <stdint.h>

/*
 * Shared signal sender. Fires a generic, host-defined trigger onto the Raw HID
 * bus, fire-and-forget (no ref, no reply). The id is opaque on the wire — all
 * meaning lives in the listening host's config (see docs/signal-capability-spec.md).
 * Used by both signal behaviors:
 *   - zmk,behavior-hid-signal        (1-cell:  &signal <id>, opaque)
 *   - zmk,behavior-hid-signal-fixed  (0-cell:  &signal_7, configured in devicetree)
 *
 *   byte[0] = SIGNAL_FIRE (0xC0)
 *   byte[1] = id (uint8)
 */
void hid_viz_send_signal(uint8_t id);
