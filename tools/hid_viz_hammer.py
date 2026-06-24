#!/usr/bin/env python3
"""
Hammer the Glove80's raw-HID inbound command path to try to reproduce the
SetLayerState (0xFC) USB hang / de-enumeration.

Run with /usr/bin/python3 (arm64) so hidapi loads. USB, left half plugged in.

The crash signature we're hunting: an inbound write (esp. 0xFC) wedges the
nRF52840 USB peripheral -> it drops off the bus and does not come back. We
detect that as: a write raising OSError AND the device disappearing from
hid.enumerate().

Usage:
    /usr/bin/python3 hid_viz_hammer.py [--count N] [--delay-ms M]
                                       [--check-every K] [--only-fc] [--quiet]
"""
import argparse
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing dependency. Install with:  pip install hidapi  (use /usr/bin/python3)")

VID, PID = 0x16C0, 0x27DB
USAGE_PAGE, USAGE = 0xFF60, 0x61
REPORT_SIZE = 32

CMD_SET_LAYER = 0xFC
CMD_GET_DEVICE_INFO = 0xFD
CMD_GET_CONFIG_ID = 0xFB
CMD_RGB_SET = 0xD1
CMD_LAYER_SET_BASE = 0xF4
CMD_LAYER_ACTIVATE = 0xF3
CMD_LAYER_DEACTIVATE = 0xF2

# RGB field-mask bits
RGB_FIELD_ON = 0x01
RGB_FIELD_H = 0x02
RGB_FIELD_S = 0x04
RGB_FIELD_V = 0x08


def find():
    for d in hid.enumerate():
        if (d["vendor_id"] == VID and d["product_id"] == PID
                and d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE):
            return d
    return None


def present():
    return find() is not None


def open_dev():
    d = find()
    if not d:
        return None
    dev = hid.device()
    dev.open_path(d["path"])
    dev.set_nonblocking(1)
    return dev


def w(dev, payload):
    body = bytes(payload) + bytes(REPORT_SIZE - len(payload))
    # hidapi write returns bytes written, or -1 / raises OSError on failure
    n = dev.write(bytes([0x00]) + body)
    if n < 0:
        raise OSError("write returned %d" % n)
    return n


def set_layer(dev, mask):
    w(dev, [CMD_SET_LAYER, mask & 0xFF, (mask >> 8) & 0xFF,
            (mask >> 16) & 0xFF, (mask >> 24) & 0xFF])


# A rotation of layer bitmasks: none(base), single layers, and combos. Each one
# makes 0xFC churn its deactivate-all then activate-set loop and emit a final
# 0xFF — the exact sequence whose trailing IN report races the next OUT write.
FC_PATTERNS = [
    0,
    1 << 1,
    1 << 2,
    (1 << 1) | (1 << 2),
    1 << 3,
    (1 << 2) | (1 << 3),
    (1 << 1) | (1 << 3),
    0,
]


def mixed_cmds(dev, i):
    """Occasionally interleave the other inbound commands to stress every path."""
    m = i % 17
    if m == 5:
        w(dev, [CMD_GET_DEVICE_INFO])
    elif m == 9:
        w(dev, [CMD_GET_CONFIG_ID])
    elif m == 11:
        # rgb set on, hue rotating, sat/val mid — benign, reverts at the end
        h = (i * 7) % 360
        w(dev, [CMD_RGB_SET, RGB_FIELD_ON | RGB_FIELD_H | RGB_FIELD_S | RGB_FIELD_V,
                1, h & 0xFF, (h >> 8) & 0xFF, 100, 40])
    elif m == 13:
        w(dev, [CMD_LAYER_SET_BASE, (i % 4) & 0xFF])
    elif m == 15:
        ref = i & 0xFFFF
        w(dev, [CMD_LAYER_ACTIVATE, ref & 0xFF, (ref >> 8) & 0xFF, 3])
        w(dev, [CMD_LAYER_DEACTIVATE, ref & 0xFF, (ref >> 8) & 0xFF, 3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=5000)
    ap.add_argument("--delay-ms", type=float, default=0.0,
                    help="sleep between iterations (0 = as fast as possible)")
    ap.add_argument("--check-every", type=int, default=100,
                    help="verify the device is still enumerated every N iters")
    ap.add_argument("--only-fc", action="store_true",
                    help="hammer ONLY 0xFC SetLayerState (no mixed commands)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not present():
        sys.exit("Glove80 raw-HID interface not found. Plug in the LEFT half over USB.")

    dev = open_dev()
    print("Opened Glove80. Hammering %d iterations (delay=%.1fms, only_fc=%s)..."
          % (args.count, args.delay_ms, args.only_fc))
    print("Watching for de-enumeration (write error + device gone). Ctrl+C to stop.\n")

    t0 = time.time()
    last_print = t0
    fc_sent = 0
    other_sent = 0

    for i in range(1, args.count + 1):
        mask = FC_PATTERNS[i % len(FC_PATTERNS)]
        try:
            set_layer(dev, mask)
            fc_sent += 1
            if not args.only_fc:
                before = other_sent
                mixed_cmds(dev, i)
                # mixed_cmds may send 0/1/2 writes; count roughly
                other_sent = before + 1
        except OSError as e:
            # A write just failed. Distinguish a real de-enum from transient backpressure.
            time.sleep(0.3)
            gone = not present()
            print("\n!!! WRITE FAILED at iter %d (fc_sent=%d): %s" % (i, fc_sent, e))
            if gone:
                print("!!! DEVICE IS GONE FROM THE USB BUS — de-enumeration / firmware hang REPRODUCED.")
                print("    Replug the keyboard. This is the baseline crash to verify against after reflash.")
            else:
                print("    Device still enumerated — likely transient backpressure, not the hang.")
                print("    Trying to reopen and continue...")
                try:
                    dev.close()
                except Exception:
                    pass
                dev = open_dev()
                if dev is None:
                    print("!!! Reopen failed — treating as de-enumeration.")
                    sys.exit(2)
                continue
            sys.exit(2)

        if args.delay_ms:
            time.sleep(args.delay_ms / 1000.0)

        if i % args.check_every == 0:
            if not present():
                print("\n!!! DEVICE VANISHED after iter %d (no write error yet) — de-enum / hang." % i)
                sys.exit(2)

        now = time.time()
        if not args.quiet and (now - last_print) > 1.0:
            rate = i / (now - t0)
            print("  ... %d/%d  (%.0f cmd/s)  fc_sent=%d" % (i, args.count, rate, fc_sent))
            last_print = now

    # Settle the keyboard back to base layer / lights as a courtesy.
    try:
        set_layer(dev, 0)
    except OSError:
        pass

    dt = time.time() - t0
    print("\nSURVIVED: %d iterations in %.1fs (%.0f cmd/s), fc_sent=%d. "
          "No de-enumeration observed." % (args.count, dt, args.count / dt, fc_sent))
    print("(The original crash was rare — not reproducing once isn't proof; "
          "re-run with higher --count if you want more confidence.)")
    dev.close()


if __name__ == "__main__":
    main()
