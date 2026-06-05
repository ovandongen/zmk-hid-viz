#pragma once

#include <stdint.h>

/*
 * Shared originator ("triggers") sender. Builds a capability action's own wire
 * message and pushes it onto the Raw HID bus, fire-and-forget (no ref, no reply).
 * Used by both emit behaviors:
 *   - zmk,behavior-hid-viz-emit        (2-cell:  &hid_viz_emit <action> <value>)
 *   - zmk,behavior-hid-viz-emit-fixed  (0-cell:  &dpi_800, configured in devicetree)
 *
 *   byte[0]    = action wire byte (e.g. 0xE1 core.pointing.dpi.set)
 *   byte[1..4] = value as uint32 little-endian
 */
void hid_viz_emit_send(uint8_t action, uint32_t value);
