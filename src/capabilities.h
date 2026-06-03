#pragma once

#include <stdint.h>

/*
 * Capability registry — single source of truth for the wire constants and the
 * capability descriptor used by the manifest. New code (manifest.c,
 * layer_actions.c) includes this header; the pre-existing notifier.c and
 * command_handler.c keep their own local #defines (identical values), so this
 * header is purely additive and breaks nothing.
 */

/* ============================================================
 * Roles — what a device does with a capability
 * ============================================================ */
#define ROLE_IDENTITY    0   /* Not a capability; identity/discovery metadata */
#define ROLE_NOTIFIES    1   /* Device emits this event to the host */
#define ROLE_HANDLES     2   /* Device accepts this command from the host */
#define ROLE_TRIGGERS    3   /* Device originates this action (fire-and-forget) */

/* ============================================================
 * Tiers — availability / stability of the capability
 * ============================================================ */
#define TIER_CORE        0   /* Always present when the module is enabled */
#define TIER_FW_SPECIFIC 1   /* Present only if the firmware implements it */
#define TIER_PROFILE     2   /* Optional profile (RGB, pointing, etc.) */
#define TIER_OPTIONAL    3   /* Opt-in; may be absent even when module is on */

/* ============================================================
 * Identity sub-types (carried in the tier field when role == ROLE_IDENTITY)
 * ============================================================ */
#define ID_TIER_NAME    0   /* Board name   (CONFIG_ZMK_KEYBOARD_NAME) */
#define ID_TIER_CONFIG  1   /* Config ID    (CONFIG_HID_VIZ_CONFIG_ID) */

/* ============================================================
 * Wire type bytes — outbound notifications (keyboard -> host)
 * ============================================================ */
#define MSG_LAYER_STATE      0xFF   /* Active layer bitmask */
#define MSG_KEY_EVENT        0xF1   /* Key press/release */

/* ============================================================
 * Wire type bytes — existing inbound commands / responses
 * ============================================================ */
#define CMD_GET_DEVICE_INFO  0xFD
#define RSP_DEVICE_INFO      0xFE
#define CMD_GET_CONFIG_ID    0xFB
#define RSP_CONFIG_ID        0xFA
#define CMD_SET_LAYER        0xFC   /* core.layer.set (full bitmask) */

/* ============================================================
 * Wire type bytes — M1 additions
 * ============================================================ */
#define CMD_GET_MANIFEST     0xF9   /* Request manifest stream */
#define RSP_MANIFEST_ENTRY   0xF8   /* One manifest entry per report */
#define CMD_LAYER_SET_BASE   0xF4   /* core.layer.setBase   { index } */
#define CMD_LAYER_ACTIVATE   0xF3   /* core.layer.activate  { ref[2], index } */
#define CMD_LAYER_DEACTIVATE 0xF2   /* core.layer.deactivate{ ref[2], index } */
#define RSP_CONFIRM          0xF7   /* Action confirmation  { ref[2], ok } */

/*
 * RSP_MANIFEST_ENTRY report layout
 *   byte[0]   = RSP_MANIFEST_ENTRY (0xF8)
 *   byte[1]   = sequence index (uint8, 0-based logical entry index)
 *   byte[2]   = flags
 *                 bit0 = MANIFEST_FLAG_LAST      (last logical entry in the stream)
 *                 bit1 = MANIFEST_FLAG_CONTINUES (string continues in next physical report)
 *   byte[3]   = role    (ROLE_*;  0 on continuation reports)
 *   byte[4]   = tier    (TIER_* or ID_TIER_*;  0 on continuation reports)
 *   byte[5]   = confirm (0/1;  0 on continuation reports)
 *   byte[6..] = string chunk (null-terminated on the final chunk only)
 *
 * Strings longer than one report (e.g. a 36-char GUID config ID) are split across
 * several physical reports that share the same sequence index. All but the last
 * carry MANIFEST_FLAG_CONTINUES; the host concatenates chunks until it is clear.
 */
#define MANIFEST_ENTRY_HEADER_SIZE  6
#define MANIFEST_FLAG_LAST          0x01
#define MANIFEST_FLAG_CONTINUES     0x02
/* Usable bytes for string data in one physical report */
#define MANIFEST_ENTRY_CHUNK_SIZE   (CONFIG_RAW_HID_REPORT_SIZE - MANIFEST_ENTRY_HEADER_SIZE)

/* ============================================================
 * Capability descriptor — one row per capability in the manifest table
 * ============================================================ */
struct hid_viz_cap {
    uint8_t      type;      /* Wire byte (CMD_*/MSG_* constant) */
    uint8_t      role;      /* ROLE_* */
    uint8_t      tier;      /* TIER_* */
    uint8_t      confirm;   /* 0 or 1 */
    const char  *id;        /* Null-terminated capability ID string */
};
