#pragma once

/*
 * ZMK API differences between the trees this module supports.
 *
 *   zmkfirmware v0.3  (Zephyr 3.5)
 *   moergo-sc main    (Zephyr 3.5)
 *   zmkfirmware main  (Zephyr 4.1)
 *
 * ZMK main still reports APP_VERSION 0.3.0, so its own version can't tell the
 * trees apart; the Zephyr major version can, and every API change below landed
 * together with the Zephyr 4.1 move. Re-check when a ZMK v0.4 is tagged or the
 * MoErgo fork moves to Zephyr 4.x.
 */

#include <stdbool.h>
#include <zmk/keymap.h>

/* Zephyr 3.5 generates <version.h>; 4.x moved it to <zephyr/version.h>. */
#if __has_include(<zephyr/version.h>)
#include <zephyr/version.h>
#else
#include <version.h>
#endif

/*
 * ZMK main added a `locking` argument to the layer API (for locking layer
 * behaviors). `false` keeps the old meaning: a normal, non-locking change. Note
 * that on main a non-locking deactivate leaves a layer that a keymap behavior
 * *locked* active — that is ZMK's intended semantics, not a bug here.
 */
#if KERNEL_VERSION_MAJOR >= 4
#define HID_VIZ_LAYER_ACTIVATE(layer)   zmk_keymap_layer_activate((layer), false)
#define HID_VIZ_LAYER_DEACTIVATE(layer) zmk_keymap_layer_deactivate((layer), false)
#define HID_VIZ_LAYER_TO(layer)         zmk_keymap_layer_to((layer), false)
#else
#define HID_VIZ_LAYER_ACTIVATE(layer)   zmk_keymap_layer_activate(layer)
#define HID_VIZ_LAYER_DEACTIVATE(layer) zmk_keymap_layer_deactivate(layer)
#define HID_VIZ_LAYER_TO(layer)         zmk_keymap_layer_to(layer)
#endif
