#pragma once

/*
 * Action wire bytes for the &hid_viz_emit keymap behavior (its first parameter).
 *
 * These are the SAME type bytes the fulfilling device's handler consumes — the
 * keyboard emits one as a "trigger", a pointing device "handles" it. See the
 * type-byte allocation table in docs/capability-bus-implementation-plan.md.
 *
 * Usage in a .keymap:
 *   #include <dt-bindings/hid_viz/emit.h>
 *   ...
 *   &hid_viz_emit POINTING_DPI_SET 800
 */

#define POINTING_DPI_SET        0xE1  /* core.pointing.dpi.set        { cpi }   */
#define POINTING_DPI_SETINDEX   0xE2  /* core.pointing.dpi.setIndex   { index } */
#define POINTING_DRAGSCROLL_SET 0xE9  /* core.pointing.dragScroll.set { 0|1 }   */
#define POINTING_SNIPE_SET      0xEB  /* core.pointing.snipe.set      { 0|1 }   */
