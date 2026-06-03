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

/* Static capability table — rows gated by the same Kconfig flags that compile in
 * the code implementing each capability. This is the single place edited when a
 * capability is added. */
static const struct hid_viz_cap capabilities[] = {
#if IS_ENABLED(CONFIG_HID_VIZ_LAYER_EVENTS)
    { MSG_LAYER_STATE,      ROLE_NOTIFIES, TIER_CORE,     0, "core.layer.changed" },
#endif
#if IS_ENABLED(CONFIG_HID_VIZ_COMMANDS)
    { CMD_SET_LAYER,        ROLE_HANDLES,  TIER_CORE,     0, "core.layer.set" },
    { CMD_LAYER_SET_BASE,   ROLE_HANDLES,  TIER_CORE,     0, "core.layer.setBase" },
    { CMD_LAYER_ACTIVATE,   ROLE_HANDLES,  TIER_OPTIONAL, 1, "core.layer.activate" },
    { CMD_LAYER_DEACTIVATE, ROLE_HANDLES,  TIER_OPTIONAL, 1, "core.layer.deactivate" },
#endif
#if IS_ENABLED(CONFIG_HID_VIZ_KEY_EVENTS)
    { MSG_KEY_EVENT,        ROLE_NOTIFIES, TIER_OPTIONAL, 0, "core.keyboard.key.event" },
#endif
};

/* Every capability ID must fit in a single report so the manifest stays compact
 * (only host-supplied identity strings ever use multi-report continuation). This
 * fails the build if a too-long ID is ever added. */
#define ASSERT_CAP_FITS(str) \
    BUILD_ASSERT(sizeof(str) <= MANIFEST_ENTRY_CHUNK_SIZE, \
                 "Capability ID '" str "' does not fit one manifest report")
ASSERT_CAP_FITS("core.layer.changed");
ASSERT_CAP_FITS("core.layer.set");
ASSERT_CAP_FITS("core.layer.setBase");
ASSERT_CAP_FITS("core.layer.activate");
ASSERT_CAP_FITS("core.layer.deactivate");
ASSERT_CAP_FITS("core.keyboard.key.event");

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
     *   2..:    one entry per enabled capability
     */
    bool has_config_id = (strlen(HID_VIZ_CONFIG_ID_STR) > 0);
    uint8_t cap_count  = ARRAY_SIZE(capabilities);
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

    /* capability entries */
    for (uint8_t i = 0; i < cap_count; i++, seq++) {
        if (seq >= start_idx) {
            send_manifest_entry(seq, seq == last_idx,
                                capabilities[i].role,
                                capabilities[i].tier,
                                capabilities[i].confirm,
                                capabilities[i].id);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_manifest, manifest_command_handler);
ZMK_SUBSCRIPTION(hid_viz_manifest, raw_hid_received_event);
