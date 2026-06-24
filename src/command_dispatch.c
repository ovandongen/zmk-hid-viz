#include "command_dispatch.h"

#include <raw_hid/events.h>

#include <zmk/event_manager.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(raw_hid_command_event);

/*
 * Shared inbound-command dispatcher.
 *
 * This is the single listener on raw_hid_received_event (which the transport
 * raises on the USB SetReport callback context — see command_dispatch.h). It
 * copies the report into a queue and returns immediately; the worker thread
 * below re-raises raw_hid_command_event so the actual command handlers run off
 * the callback, where their blocking sends are harmless. This makes "off the
 * USB callback" a structural property of one place, so every command — present
 * and future — is covered without each handler having to defer itself.
 */

/* Depth of the in-flight inbound queue. A handful of commands can land back to
 * back at connect time (device info, config id, manifest, an initial layer
 * set); 8 absorbs a burst while the worker drains. If it ever fills, the
 * listener drops the overflow rather than block the USB callback. */
#define CMD_QUEUE_DEPTH 8

struct cmd_item {
    uint8_t data[CONFIG_RAW_HID_REPORT_SIZE];
    uint8_t length;
};

K_MSGQ_DEFINE(cmd_msgq, sizeof(struct cmd_item), CMD_QUEUE_DEPTH, 4);

#define CMD_DISPATCH_THREAD_STACK_SIZE 2048

static K_THREAD_STACK_DEFINE(cmd_dispatch_thread_stack, CMD_DISPATCH_THREAD_STACK_SIZE);
static struct k_thread cmd_dispatch_thread_data;

static void cmd_dispatch_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        struct cmd_item item;
        k_msgq_get(&cmd_msgq, &item, K_FOREVER);

        /* Synchronous raise: every command handler runs to completion here, on
         * this worker thread, before the call returns — so `item` (and the
         * data pointer into it) stays valid for the whole dispatch. */
        raise_raw_hid_command_event(
            (struct raw_hid_command_event){.data = item.data, .length = item.length});
    }
}

static int cmd_dispatch_thread_init(void)
{
    /* One priority step above the manifest worker (K_LOWEST_APPLICATION_THREAD_PRIO)
     * so a ~580ms manifest stream can't delay an interactive command. Any overlap
     * is still safe: hid_viz_send()'s mutex serializes send_report(), so a
     * preempting command waits at most one report (<=30ms). */
    k_thread_create(&cmd_dispatch_thread_data, cmd_dispatch_thread_stack,
                    K_THREAD_STACK_SIZEOF(cmd_dispatch_thread_stack),
                    cmd_dispatch_thread_fn, NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO - 1, 0, K_NO_WAIT);
    k_thread_name_set(&cmd_dispatch_thread_data, "hid_viz_cmd");
    return 0;
}

SYS_INIT(cmd_dispatch_thread_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int inbound_dispatch_listener(const zmk_event_t *eh)
{
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event == NULL || event->length == 0 || event->data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Copy out now — `event->data` points at the transport's buffer, valid only
     * for the duration of this callback — then hand off and return immediately
     * so we never block (or send from) the USB SetReport callback context. */
    struct cmd_item item;
    item.length = MIN(event->length, (uint8_t)CONFIG_RAW_HID_REPORT_SIZE);
    memcpy(item.data, event->data, item.length);

    if (k_msgq_put(&cmd_msgq, &item, K_NO_WAIT) != 0) {
        LOG_WRN("hid_viz inbound queue full, dropping command 0x%02x", item.data[0]);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_cmd_dispatch, inbound_dispatch_listener);
ZMK_SUBSCRIPTION(hid_viz_cmd_dispatch, raw_hid_received_event);
