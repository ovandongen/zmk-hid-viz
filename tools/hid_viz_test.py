#!/usr/bin/env python3
"""
Standalone host-side test harness for zmk-hid-viz M1.

Exercises the new firmware wire messages directly over Raw HID, with no
dependency on the full host visualization app:

  * 0xF9  get manifest        -> stream of 0xF8 entries (incl. GUID continuation)
  * 0xF4  core.layer.setBase  -> resulting 0xFF layer-state notification
  * 0xF3  core.layer.activate -> 0xF7 confirm, then 0xFF
  * 0xF2  core.layer.deactivate -> 0xF7 confirm, then 0xFF
  * 0xC0  signal.fire         -> received when a &signal / &signal_<id> key is pressed
          (originated by the keyboard, fire-and-forget; the host only listens).
          Run `--only signals` and press your signal keys.

It finds the keyboard's raw-HID interface by the usage page / usage that
zmk-raw-hid advertises (defaults 0xFF60 / 0x61), so you do NOT need to know the
Glove80's VID/PID. Override with --usage-page/--usage/--vid/--pid/--path if your
.conf changes the defaults.

Setup:
    pip install hidapi          # provides the `hid` module
    python3 tools/hid_viz_test.py --list      # see candidate interfaces
    python3 tools/hid_viz_test.py             # run the full test sequence

To exercise the long-string (GUID) manifest continuation, set a 36-char GUID in
your firmware .conf before flashing:
    CONFIG_HID_VIZ_CONFIG_ID="123e4567-e89b-12d3-a456-426614174000"
With an empty config id the configId entry is skipped entirely.

NOTE: connect over USB. zmk-raw-hid runs on the central (left) half, so flash and
plug in the left half.
"""

import argparse
import sys
import time

try:
    import hid  # cython-hidapi: `pip install hidapi`
except ImportError:
    sys.exit("Missing dependency. Install with:  pip install hidapi")

# --- zmk-raw-hid transport defaults (override via CLI if your .conf changes them)
DEFAULT_USAGE_PAGE = 0xFF60
DEFAULT_USAGE = 0x61
REPORT_SIZE = 32  # CONFIG_RAW_HID_REPORT_SIZE

# --- Wire type bytes (mirror of src/capabilities.h) ---------------------------
MSG_LAYER_STATE = 0xFF
MSG_KEY_EVENT = 0xF1
RSP_DEVICE_INFO = 0xFE
RSP_CONFIG_ID = 0xFA
RSP_MANIFEST_ENTRY = 0xF8
CMD_GET_MANIFEST = 0xF9
CMD_LAYER_SET_BASE = 0xF4
CMD_LAYER_ACTIVATE = 0xF3
CMD_LAYER_DEACTIVATE = 0xF2
RSP_CONFIRM = 0xF7
MSG_RGB_CHANGED = 0xD0
CMD_RGB_SET = 0xD1
SIGNAL_FIRE = 0xC0  # signal.fire { id:uint8 } — keyboard-originated, fire-and-forget

# CMD_RGB_SET field-mask bits (byte[1]; field positions are fixed)
RGB_FIELD_ON = 0x01      # byte[2]
RGB_FIELD_H = 0x02       # byte[3-4] uint16 LE, 0-359
RGB_FIELD_S = 0x04       # byte[5], 0-100
RGB_FIELD_V = 0x08       # byte[6], 0-100
RGB_FIELD_EFFECT = 0x10  # byte[7]

# --- Emitted action bytes (&hid_viz_emit; mirror of dt-bindings/hid_viz/emit.h)
EMIT_ACTION_NAMES = {
    0xE1: "core.pointing.dpi.set",
    0xE2: "core.pointing.dpi.setIndex",
    0xE9: "core.pointing.dragScroll.set",
    0xEB: "core.pointing.snipe.set",
}

# --- Manifest framing ---------------------------------------------------------
MANIFEST_HEADER = 6
MANIFEST_FLAG_LAST = 0x01
MANIFEST_FLAG_CONTINUES = 0x02

ROLE_NAMES = {0: "identity", 1: "notifies", 2: "handles", 3: "triggers"}
TIER_NAMES = {0: "core", 1: "fw-specific", 2: "profile", 3: "optional"}
ID_TIER_NAMES = {0: "name", 1: "configId"}


# ------------------------------------------------------------------- discovery
def find_candidates(usage_page, usage, vid, pid):
    out = []
    for d in hid.enumerate():
        if vid and d["vendor_id"] != vid:
            continue
        if pid and d["product_id"] != pid:
            continue
        # On macOS/Windows usage_page/usage are populated by enumerate(); on
        # Linux they may be 0, in which case we fall back to matching any
        # vendor-defined page (>= 0xFF00) when an explicit page wasn't found.
        if d.get("usage_page") == usage_page and d.get("usage") == usage:
            out.append(d)
    return out


def print_interfaces(devs, header):
    print(header)
    if not devs:
        print("  (none)")
        return
    for d in devs:
        print(
            "  vid=%04x pid=%04x  usage_page=%04x usage=%02x  "
            "iface=%s  %r"
            % (
                d["vendor_id"],
                d["product_id"],
                d.get("usage_page") or 0,
                d.get("usage") or 0,
                d.get("interface_number"),
                (d.get("product_string") or "").strip(),
            )
        )
        print("     path=%s" % d["path"].decode(errors="replace"))


def open_device(args):
    if args.path:
        dev = hid.device()
        dev.open_path(args.path.encode())
        return dev

    cands = find_candidates(args.usage_page, args.usage, args.vid, args.pid)
    if not cands:
        print_interfaces(list(hid.enumerate()), "All HID interfaces seen:")
        sys.exit(
            "\nNo interface matched usage_page=%04x usage=%02x. "
            "Pick one above and pass --path, or set --usage-page/--usage."
            % (args.usage_page, args.usage)
        )
    # enumerate() order is not stable across calls. Prefer the macOS combined
    # interface (interface_number == -1), which reliably accepts read+write of
    # the vendor reports; otherwise take the lowest interface number.
    def rank(d):
        n = d.get("interface_number")
        n = 999 if n is None else n
        return (n != -1, n)
    cands.sort(key=rank)
    if len(cands) > 1:
        print_interfaces(cands, "Multiple matches; using the first:")
    dev = hid.device()
    dev.open_path(cands[0]["path"])
    return dev


# ------------------------------------------------------------------------- io
def send(dev, payload):
    """Write one unnumbered report: leading 0x00 report-id byte + 32-byte body."""
    body = bytes(payload) + bytes(REPORT_SIZE - len(payload))
    dev.write(bytes([0x00]) + body)


def drain(dev, settle_ms=600, idle_ms=120):
    """Collect inbound reports until the device goes quiet for idle_ms.

    Uses non-blocking reads (set_nonblocking(1) in main): the blocking
    read(size, timeout_ms) form throws "read error" on macOS hidapi, so we
    poll instead. settle_ms bounds the total wait; each report is REPORT_SIZE
    bytes with the type byte in data[0]."""
    reports = []
    start = time.time()
    last_rx = start
    while True:
        now = time.time()
        if now - start > settle_ms / 1000.0:
            break
        try:
            data = dev.read(REPORT_SIZE)
        except OSError:
            break
        if data:
            reports.append(bytes(data))
            last_rx = now
        elif reports and (now - last_rx) > idle_ms / 1000.0:
            break  # had something, now quiet -> done
        else:
            time.sleep(0.01)
    return reports


# -------------------------------------------------------------------- parsing
def fmt_layer_state(r):
    width = r[1]
    default_mask = int.from_bytes(r[2:6], "little")
    active_mask = int.from_bytes(r[6:10], "little")
    active = [i for i in range(32) if active_mask & (1 << i)]
    return "0xFF layer.changed  width=%d default=0x%08x active=0x%08x %s" % (
        width,
        default_mask,
        active_mask,
        active,
    )


def fmt_confirm(r):
    ref = r[1] | (r[2] << 8)
    return "0xF7 confirm        ref=%d ok=%d" % (ref, r[3])


def fmt_emit(r):
    value = int.from_bytes(r[1:5], "little")
    return "0x%02X %-28s value=%d (emitted trigger)" % (
        r[0],
        EMIT_ACTION_NAMES[r[0]],
        value,
    )


def parse_rgb_changed(r):
    return {
        "on": r[1],
        "h": r[2] | (r[3] << 8),
        "s": r[4],
        "v": r[5],
        "effect": r[6],
    }


def fmt_rgb_changed(r):
    st = parse_rgb_changed(r)
    return "0xD0 rgb.changed    on=%d h=%d s=%d v=%d effect=%d" % (
        st["on"], st["h"], st["s"], st["v"], st["effect"],
    )


def fmt_signal(r):
    return "0xC0 signal.fire     id=%d (emitted trigger)" % r[1]


def fmt_other(r):
    return "0x%02X (%d bytes) %s" % (r[0], len(r), r[:8].hex(" "))


def print_reports(reports, label):
    print("  <- %s: %d report(s)" % (label, len(reports)))
    for r in reports:
        t = r[0]
        if t == MSG_LAYER_STATE:
            print("     " + fmt_layer_state(r))
        elif t == RSP_CONFIRM:
            print("     " + fmt_confirm(r))
        elif t == MSG_KEY_EVENT:
            print("     0xF1 key.event      pos=%d pressed=%d" % (r[2], r[3]))
        elif t == MSG_RGB_CHANGED:
            print("     " + fmt_rgb_changed(r))
        elif t in EMIT_ACTION_NAMES:
            print("     " + fmt_emit(r))
        elif t == SIGNAL_FIRE:
            print("     " + fmt_signal(r))
        else:
            print("     " + fmt_other(r))


# ------------------------------------------------------------------ test cases
def test_manifest(dev):
    print("\n== Manifest (0xF9) ==")
    send(dev, [CMD_GET_MANIFEST])
    reports = drain(dev, settle_ms=600)

    entries = {}  # seq -> dict
    order = []
    saw_last = False
    for r in reports:
        if r[0] != RSP_MANIFEST_ENTRY:
            print("     (ignored non-manifest) " + fmt_other(r))
            continue
        seq = r[1]
        flags = r[2]
        chunk = r[MANIFEST_HEADER:]
        if seq not in entries:
            entries[seq] = {
                "role": r[3],
                "tier": r[4],
                "confirm": r[5],
                "buf": bytearray(),
            }
            order.append(seq)
        # accumulate raw chunk; first chunk header fields already captured
        entries[seq]["buf"] += chunk
        if flags & MANIFEST_FLAG_LAST:
            saw_last = True

    if not entries:
        print("  !! no 0xF8 entries received "
              "(is CONFIG_HID_VIZ_MANIFEST=y and the firmware flashed?)")
        return

    print("  seq role        tier         confirm  id/value")
    for seq in sorted(order):
        e = entries[seq]
        s = bytes(e["buf"]).split(b"\x00", 1)[0].decode(errors="replace")
        role = e["role"]
        role_name = ROLE_NAMES.get(role, "?%d" % role)
        if role == 0:  # identity: tier field is an ID_TIER_*
            tier_name = ID_TIER_NAMES.get(e["tier"], "?%d" % e["tier"])
        else:
            tier_name = TIER_NAMES.get(e["tier"], "?%d" % e["tier"])
        nchunks = (len(e["buf"]) + REPORT_SIZE - MANIFEST_HEADER - 1) // (
            REPORT_SIZE - MANIFEST_HEADER
        )
        cont = "  (reassembled from %d reports)" % nchunks if nchunks > 1 else ""
        print(
            "  %3d %-10s %-11s %s        %s%s"
            % (seq, role_name, tier_name, e["confirm"], s, cont)
        )
    if not saw_last:
        print("  !! never saw MANIFEST_FLAG_LAST — stream may be truncated")


def test_setbase(dev, layer):
    print("\n== setBase (0xF4) -> layer %d ==" % layer)
    send(dev, [CMD_LAYER_SET_BASE, layer & 0xFF])
    print_reports(drain(dev), "after setBase")


def test_activate(dev, ref, layer):
    print("\n== activate (0xF3) ref=%d layer=%d ==" % (ref, layer))
    send(dev, [CMD_LAYER_ACTIVATE, ref & 0xFF, (ref >> 8) & 0xFF, layer & 0xFF])
    reports = drain(dev)
    print_reports(reports, "after activate")
    if not any(r[0] == RSP_CONFIRM for r in reports):
        print("  !! no 0xF7 confirm seen")


def test_deactivate(dev, ref, layer):
    print("\n== deactivate (0xF2) ref=%d layer=%d ==" % (ref, layer))
    send(dev, [CMD_LAYER_DEACTIVATE, ref & 0xFF, (ref >> 8) & 0xFF, layer & 0xFF])
    reports = drain(dev)
    print_reports(reports, "after deactivate")
    if not any(r[0] == RSP_CONFIRM for r in reports):
        print("  !! no 0xF7 confirm seen")


def rgb_set(dev, mask, on=0, h=0, s=0, v=0, effect=0, label="rgb.set"):
    """Send one 0xD1 and return the parsed 0xD0 receipt (None if absent)."""
    send(dev, [
        CMD_RGB_SET, mask, on & 0xFF,
        h & 0xFF, (h >> 8) & 0xFF,
        s & 0xFF, v & 0xFF, effect & 0xFF,
    ])
    reports = drain(dev)
    print_reports(reports, label)
    for r in reports:
        if r[0] == MSG_RGB_CHANGED:
            return parse_rgb_changed(r)
    return None


def test_rgb(dev):
    print("\n== RGB (0xD1 core.rgb.set / 0xD0 core.rgb.changed) ==")

    # 1. Empty mask = apply nothing, receipt only -> the state-query path.
    print("  -> query (mask=0x00)")
    orig = rgb_set(dev, 0x00, label="query receipt")
    if orig is None:
        print("  !! no 0xD0 receipt (is CONFIG_HID_VIZ_RGB=y and the firmware flashed?)")
        return
    print("     NOTE: h/s/v/effect are module-tracked statics; they desync from")
    print("     flash-restored state and &rgb_ug/Magic+T changes until first set.")

    # 2. Visible set: on, green-ish, half brightness.
    print("  -> set on=1 h=120 s=100 v=50 (should show green)")
    st = rgb_set(dev, RGB_FIELD_ON | RGB_FIELD_H | RGB_FIELD_S | RGB_FIELD_V,
                 on=1, h=120, s=100, v=50, label="set receipt")
    if st is None:
        print("  !! no 0xD0 receipt after set")
        return
    if (st["on"], st["h"], st["s"], st["v"]) != (1, 120, 100, 50):
        print("  !! receipt mismatch: expected on=1 h=120 s=100 v=50")

    time.sleep(5)  # leave the colour visible for a moment

    # 3. Restore what the query reported (best effort, given the desync note).
    print("  -> restore on=%d h=%d s=%d v=%d effect=%d" % (
        orig["on"], orig["h"], orig["s"], orig["v"], orig["effect"]))
    rgb_set(dev, RGB_FIELD_ON | RGB_FIELD_H | RGB_FIELD_S | RGB_FIELD_V | RGB_FIELD_EFFECT,
            on=orig["on"], h=orig["h"], s=orig["s"], v=orig["v"],
            effect=orig["effect"], label="restore receipt")


def test_signals(dev, seconds):
    print("\n== Signals (0xC0 signal.fire) ==")
    print("  Signals are keyboard-originated: press the keys bound to "
          "&signal_1 / &signal_2 now.")
    print("  Listening %ds..." % seconds)
    seen = {}
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = dev.read(REPORT_SIZE)
        except OSError:
            break
        if not data:
            time.sleep(0.02)
            continue
        r = bytes(data)
        if r[0] == SIGNAL_FIRE:
            seen[r[1]] = seen.get(r[1], 0) + 1
            print("     " + fmt_signal(r))
        else:
            print_reports([r], "other")
    if seen:
        summary = ", ".join("id=%d x%d" % (k, v) for k, v in sorted(seen.items()))
        print("  fired ids: %s" % summary)
    else:
        print("  !! no 0xC0 signal.fire reports seen — check CONFIG_HID_VIZ_SIGNAL=y, "
              "the &signal_<id> keymap binding, and that you pressed the signal keys")


def listen(dev, seconds):
    print("\n== Listening for %ds (press keys / change layers) ==" % seconds)
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = dev.read(REPORT_SIZE)
        except OSError:
            break
        if data:
            print_reports([bytes(data)], "event")
        else:
            time.sleep(0.02)


# ------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true",
                    help="list matching raw-HID interfaces and exit")
    ap.add_argument("--list-all", action="store_true",
                    help="list every HID interface and exit")
    ap.add_argument("--usage-page", type=lambda x: int(x, 0), default=DEFAULT_USAGE_PAGE)
    ap.add_argument("--usage", type=lambda x: int(x, 0), default=DEFAULT_USAGE)
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--path", help="open this hidapi path directly")
    ap.add_argument("--layer", type=int, default=2, help="layer index for setBase")
    ap.add_argument("--act-layer", type=int, default=3,
                    help="layer index for activate/deactivate")
    ap.add_argument("--listen", type=int, metavar="SECONDS",
                    help="passively print notifications for N seconds, then exit")
    ap.add_argument("--only",
                    choices=["manifest", "setbase", "activate", "deactivate", "rgb", "signals"],
                    help="run a single test instead of the full sequence "
                         "(rgb and signals are never part of the full sequence — "
                         "run them explicitly)")
    ap.add_argument("--signal-seconds", type=int, default=15,
                    help="how long `--only signals` listens for key-pressed signals")
    args = ap.parse_args()

    if args.list_all:
        print_interfaces(list(hid.enumerate()), "All HID interfaces:")
        return
    if args.list:
        cands = find_candidates(args.usage_page, args.usage, args.vid, args.pid)
        print_interfaces(cands, "Matching raw-HID interfaces:")
        return

    dev = open_device(args)
    try:
        dev.set_nonblocking(1)
        if args.listen is not None:
            listen(dev, args.listen)
            return
        if args.only == "manifest":
            test_manifest(dev)
        elif args.only == "setbase":
            test_setbase(dev, args.layer)
        elif args.only == "activate":
            test_activate(dev, 1, args.act_layer)
        elif args.only == "deactivate":
            test_deactivate(dev, 2, args.act_layer)
        elif args.only == "rgb":
            test_rgb(dev)
        elif args.only == "signals":
            test_signals(dev, args.signal_seconds)
        else:
            test_manifest(dev)
            test_setbase(dev, args.layer)
            test_activate(dev, 1, args.act_layer)
            test_deactivate(dev, 2, args.act_layer)
        print("\nDone.")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
