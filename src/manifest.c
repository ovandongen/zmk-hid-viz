#include <raw_hid/events.h>

#include <zmk/event_manager.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"

/*
 * Manifest handler: responds to CMD_GET_MANIFEST (0xF9) with a stream of
 * RSP_MANIFEST_ENTRY (0xF8) reports, one logical entry at a time.
 *
 * The stream starts with identity entries (role = ROLE_IDENTITY) describing the
 * device, followed by one entry per enabled capability. The host queries this
 * once at connect time to discover the device's full capability surface; it is
 * not a hot path.
 *
 * Long strings (e.g. a GUID config ID) are split across several physical reports
 * sharing one sequence index — see send_manifest_entry().
 *
 * CONFIG_HID_VIZ_CONFIG_ID is only defined when CONFIG_HID_VIZ_COMMANDS is set
 * (its Kconfig `depends on`). This handler can compile with commands disabled,
 * so fall back to an empty identifier in that configuration.
 */
#if IS_ENABLED(CONFIG_HID_VIZ_COMMANDS)
#define HID_VIZ_CONFIG_ID_STR CONFIG_HID_VIZ_CONFIG_ID
#else
#define HID_VIZ_CONFIG_ID_STR ""
#endif

/*
 * v2: capabilities are no longer listed in a static table here. Each is declared
 * with HID_VIZ_CAP_REGISTER() in the file that implements it (notifier.c,
 * command_handler.c, layer_actions.c, behavior_hid_viz_emit_fixed.c), which lands
 * a `struct hid_viz_cap` in the ROM iterable section walked below. IDs longer
 * than one report (some pointing trigger IDs) are split across reports by
 * send_manifest_entry() like any other string. Emit nodes register one entry per
 * devicetree node, so several `dpi_*` nodes can share one action ID — those
 * duplicates are collapsed at emit time (see seen_id()).
 */

/* Upper bound on distinct capability IDs for the dedup scratch list. Far above
 * any real device's capability count; if somehow exceeded, dedup simply stops
 * suppressing further duplicates (a cosmetic manifest repeat, never a crash).
 * Sized with headroom for enumerated signal.fire/<id> entries (one slot per
 * distinct id) on top of the fixed core/RGB/pointing caps. Just pointers — 64 *
 * sizeof(char *) of stack scratch. */
#define MANIFEST_MAX_CAPS 64

/* Returns true if `id` was already seen in seen[0..*n); otherwise records it
 * (capacity permitting) and returns false. Compares by string value, since
 * identical IDs from different translation units are distinct string literals. */
static bool seen_id(const char *seen[], size_t *n, const char *id) {
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(seen[i], id) == 0) {
            return true;
        }
    }
    if (*n < MANIFEST_MAX_CAPS) {
        seen[(*n)++] = id;
    }
    return false;
}

/* Count distinct capability IDs registered in the section (post-dedup), so the
 * emit loop can flag the final logical entry with MANIFEST_FLAG_LAST. */
static uint8_t count_caps(void) {
    const char *seen[MANIFEST_MAX_CAPS];
    size_t n = 0;
    uint8_t count = 0;
    STRUCT_SECTION_FOREACH(hid_viz_cap, c) {
        if (!seen_id(seen, &n, c->id)) {
            count++;
        }
    }
    return count;
}

static uint8_t manifest_buf[CONFIG_RAW_HID_REPORT_SIZE];

/*
 * Send one logical manifest entry as one or more physical 0xF8 reports.
 * A string that does not fit MANIFEST_ENTRY_CHUNK_SIZE is split into chunks that
 * share seq_idx; every chunk but the last sets MANIFEST_FLAG_CONTINUES. The
 * role/tier/confirm header fields are carried only on the first chunk.
 */
static void send_manifest_entry(uint8_t seq_idx, bool is_last,
                                uint8_t role, uint8_t tier, uint8_t confirm,
                                const char *str)
{
    size_t remaining = strlen(str);
    size_t offset = 0;
    bool first = true;

    do {
        size_t chunk = MIN(remaining, (size_t)MANIFEST_ENTRY_CHUNK_SIZE);
        bool has_more = (remaining - chunk) > 0;

        memset(manifest_buf, 0, sizeof(manifest_buf));
        manifest_buf[0] = RSP_MANIFEST_ENTRY;
        manifest_buf[1] = seq_idx;
        manifest_buf[2] = (uint8_t)((is_last && !has_more ? MANIFEST_FLAG_LAST : 0) |
                                    (has_more ? MANIFEST_FLAG_CONTINUES : 0));
        manifest_buf[3] = first ? role : 0;
        manifest_buf[4] = first ? tier : 0;
        manifest_buf[5] = first ? confirm : 0;
        memcpy(&manifest_buf[MANIFEST_ENTRY_HEADER_SIZE], str + offset, chunk);
        /* On the final chunk the byte after the data is already 0 (null
         * terminator) from the memset above. */

        raise_raw_hid_sent_event(
            (struct raw_hid_sent_event){.data = manifest_buf, .length = sizeof(manifest_buf)});

        offset    += chunk;
        remaining -= chunk;
        first      = false;
    } while (remaining > 0);
}

static int manifest_command_handler(const zmk_event_t *eh)
{
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (event->data[0] != CMD_GET_MANIFEST) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Optional resumption: byte[1] = first logical entry to (re)send. */
    uint8_t start_idx = (event->length >= 2) ? event->data[1] : 0;
    LOG_INF("Command: get manifest (start=%u)", start_idx);

    /* Compose the ordered logical sequence:
     *   0:      identity — board name (always present)
     *   1:      identity — config ID  (only if non-empty)
     *   2..:    one entry per registered capability (link order, sorted by the
     *           section variable name; triggers deduped by ID)
     */
    bool has_config_id = (strlen(HID_VIZ_CONFIG_ID_STR) > 0);
    uint8_t cap_count  = count_caps();
    uint8_t total      = (uint8_t)(1 + (has_config_id ? 1 : 0) + cap_count);
    uint8_t last_idx   = (total > 0) ? (uint8_t)(total - 1) : 0;
    uint8_t seq = 0;

    /* identity: board name */
    if (seq >= start_idx) {
        send_manifest_entry(seq, seq == last_idx,
                            ROLE_IDENTITY, ID_TIER_NAME, 0,
                            CONFIG_ZMK_KEYBOARD_NAME);
    }
    seq++;

    /* identity: config ID (conditional) */
    if (has_config_id) {
        if (seq >= start_idx) {
            send_manifest_entry(seq, seq == last_idx,
                                ROLE_IDENTITY, ID_TIER_CONFIG, 0,
                                HID_VIZ_CONFIG_ID_STR);
        }
        seq++;
    }

    /* capability entries (from the linker section; triggers deduped by ID) */
    const char *seen[MANIFEST_MAX_CAPS];
    size_t seen_n = 0;
    STRUCT_SECTION_FOREACH(hid_viz_cap, c) {
        if (seen_id(seen, &seen_n, c->id)) {
            continue;
        }
        if (seq >= start_idx) {
            send_manifest_entry(seq, seq == last_idx,
                                c->role, c->tier, c->confirm, c->id);
        }
        seq++;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_manifest, manifest_command_handler);
ZMK_SUBSCRIPTION(hid_viz_manifest, raw_hid_received_event);
