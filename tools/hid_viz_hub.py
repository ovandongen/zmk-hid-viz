#!/usr/bin/env python3
"""
Host-hub prototype for the zmk-hid-viz capability bus (M4 matchmaking loop).

The keyboard and a pointing device are two *separate* USB devices; they never see
each other. This script is the broker in the middle: it enumerates every hid-viz
device, reads each one's manifest to learn what it `triggers` vs `handles`, builds
a routing table, then listens on all of them and forwards an emitted trigger to
whichever device handles the same capability ID.

  [ZMK keyboard]  --0xE1 dpi.set 800-->  [ this hub ]  --0xE1-->  [QMK trackball]
     triggers                              router                    handles

It is a throwaway test/prototype (lives next to hid_viz_test.py under the
gitignored tools/), meant to validate the routing design before it graduates into
the real host (zmk-hid-protocol). With only the keyboard plugged in there is no
handler for the pointing triggers, so emits log as "no handler" — that already
exercises enumeration, manifest aggregation, routing-table construction, and
trigger detection. Plug in a device that `handles` the action and the same run
starts forwarding, no code change.

Setup:
    pip install hidapi
    python3 tools/hid_viz_hub.py --list     # show discovered devices + manifests
    python3 tools/hid_viz_hub.py            # build routes, then broker until Ctrl-C

Notes:
  * Forwarding is fire-and-forget (no ref / no 0xF7 confirm) — matches the current
    emit semantics. The confirm plane (M2) would slot into the hub->handler leg.
  * The manifest carries the capability *ID*, not the wire byte, so the hub owns
    the canonical ID<->byte map below (same table hid_viz_test.py decodes with).
    An emitted trigger report is forwarded *verbatim*: by design the emit byte is
    the same byte the handler consumes, so no re-encoding is needed.
  * Manifests are read once at startup (no hot-plug). Re-run after plugging in a
    new device.
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
RSP_MANIFEST_ENTRY = 0xF8
CMD_GET_MANIFEST = 0xF9
RSP_CONFIRM = 0xF7

# --- Canonical capability-ID <-> wire-byte map for routable actions -----------
# The manifest does not transmit the wire byte, so the host owns this mapping.
# Mirrors dt-bindings/hid_viz/emit.h. Keyed both ways for decode + (future) encode.
ACTION_ID_BY_BYTE = {
    0xE1: "core.pointing.dpi.set",
    0xE2: "core.pointing.dpi.setIndex",
    0xE9: "core.pointing.dragScroll.set",
    0xEB: "core.pointing.snipe.set",
}

# --- Manifest framing ---------------------------------------------------------
MANIFEST_HEADER = 6
MANIFEST_FLAG_LAST = 0x01
MANIFEST_FLAG_CONTINUES = 0x02

ROLE_IDENTITY, ROLE_NOTIFIES, ROLE_HANDLES, ROLE_TRIGGERS = 0, 1, 2, 3
ROLE_NAMES = {0: "identity", 1: "notifies", 2: "handles", 3: "triggers"}
TIER_NAMES = {0: "core", 1: "fw-specific", 2: "profile", 3: "optional"}
ID_TIER_NAMES = {0: "name", 1: "configId"}


# ------------------------------------------------------------------- discovery
def _rank(d):
    """Prefer the macOS combined interface (-1), which accepts read+write of the
    vendor reports; otherwise the lowest interface number."""
    n = d.get("interface_number")
    n = 999 if n is None else n
    return (n != -1, n)


def find_devices(usage_page, usage, vid, pid):
    """Return one enumerate() entry per physical hid-viz device.

    A device exposes its raw-HID endpoint as several enumerate() rows (the macOS
    combined interface -1 plus per-interface rows). Opening two handles to the
    same endpoint corrupts the manifest reads, so we must keep exactly one row per
    device. Those rows do NOT reliably share a serial (macOS gives the -1 and the
    per-interface rows different serials), so group by (vid, pid) and keep the
    best-ranked interface (prefer -1, like hid_viz_test.py). Two genuinely
    distinct devices (a keyboard + a trackball) have different vid/pid and stay
    separate; the only thing this merges is two identical-vid/pid units, which is
    fine for a test tool."""
    groups = {}
    for d in hid.enumerate():
        if vid and d["vendor_id"] != vid:
            continue
        if pid and d["product_id"] != pid:
            continue
        if d.get("usage_page") == usage_page and d.get("usage") == usage:
            groups.setdefault((d["vendor_id"], d["product_id"]), []).append(d)
    chosen = []
    for entries in groups.values():
        entries.sort(key=_rank)
        chosen.append(entries[0])
    return chosen


# ------------------------------------------------------------------------- io
def send(dev, payload):
    """Write one unnumbered report: leading 0x00 report-id byte + 32-byte body."""
    body = bytes(payload) + bytes(REPORT_SIZE - len(payload))
    dev.write(bytes([0x00]) + body)


def drain(dev, settle_ms=600, idle_ms=120):
    """Collect inbound reports until the device goes quiet for idle_ms.

    Non-blocking reads (set_nonblocking(1) is set on open): the blocking
    read(size, timeout_ms) form throws on macOS hidapi, so we poll."""
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
            break
        else:
            time.sleep(0.01)
    return reports


# -------------------------------------------------------------------- manifest
def query_manifest(dev):
    """Send 0xF9, reassemble the 0xF8 stream. Returns (name, config_id, caps)
    where caps is a list of {role, tier, confirm, id} for the non-identity rows."""
    send(dev, [CMD_GET_MANIFEST])
    reports = drain(dev, settle_ms=600)

    entries = {}
    order = []
    for r in reports:
        if r[0] != RSP_MANIFEST_ENTRY:
            continue
        seq = r[1]
        chunk = r[MANIFEST_HEADER:]
        if seq not in entries:
            entries[seq] = {"role": r[3], "tier": r[4], "confirm": r[5],
                            "buf": bytearray()}
            order.append(seq)
        entries[seq]["buf"] += chunk

    name = None
    config_id = None
    caps = []
    for seq in sorted(order):
        e = entries[seq]
        s = bytes(e["buf"]).split(b"\x00", 1)[0].decode(errors="replace")
        if e["role"] == ROLE_IDENTITY:
            if e["tier"] == 0:
                name = s
            elif e["tier"] == 1:
                config_id = s
        else:
            caps.append({"role": e["role"], "tier": e["tier"],
                         "confirm": e["confirm"], "id": s})
    return name, config_id, caps


class Device:
    def __init__(self, dev, meta, name, config_id, caps):
        self.dev = dev
        self.meta = meta
        self.name = name or (meta.get("product_string") or "?").strip()
        self.config_id = config_id
        self.caps = caps

    def caps_with_role(self, role):
        return [c for c in self.caps if c["role"] == role]


# ---------------------------------------------------------------------- wiring
def build_routes(devices):
    """Map capability ID -> list of devices that `handles` it."""
    handlers_by_id = {}
    for dev in devices:
        for c in dev.caps_with_role(ROLE_HANDLES):
            handlers_by_id.setdefault(c["id"], []).append(dev)
    return handlers_by_id


def print_inventory(devices, handlers_by_id):
    print("\n== Devices (%d) ==" % len(devices))
    for d in devices:
        cfg = ("  configId=%s" % d.config_id) if d.config_id else ""
        print("  %s%s" % (d.name, cfg))
        for c in sorted(d.caps, key=lambda c: (c["role"], c["id"])):
            print("      %-9s %-11s %s%s"
                  % (ROLE_NAMES.get(c["role"], "?"),
                     TIER_NAMES.get(c["tier"], "?"),
                     "c " if c["confirm"] else "  ",
                     c["id"]))

    print("\n== Routing table (triggers -> handlers) ==")
    any_trigger = False
    any_route = False
    for d in devices:
        for c in d.caps_with_role(ROLE_TRIGGERS):
            any_trigger = True
            targets = [h for h in handlers_by_id.get(c["id"], []) if h is not d]
            if targets:
                any_route = True
                dest = ", ".join(h.name for h in targets)
            else:
                dest = "(no handler registered)"
            print("  %-30s %s  ->  %s" % (c["id"], d.name, dest))
    if not any_trigger:
        print("  (no device advertises any triggers)")
    elif not any_route:
        print("  -- nothing to route yet: no device handles these actions.")
        print("     Plug in a device that `handles` them (e.g. a QMK trackball)")
        print("     and re-run; emits will forward automatically.")


# ------------------------------------------------------------------------ hub
def handle_report(src, report, handlers_by_id, verbose):
    t = report[0]
    if t in ACTION_ID_BY_BYTE:
        cap_id = ACTION_ID_BY_BYTE[t]
        value = int.from_bytes(report[1:5], "little")
        targets = [h for h in handlers_by_id.get(cap_id, []) if h is not src]
        if targets:
            for h in targets:
                send(h.dev, report)  # forward verbatim; emit byte == handle byte
            print("ROUTE  %-28s value=%-6d  %s -> %s"
                  % (cap_id, value, src.name, ", ".join(h.name for h in targets)))
        else:
            print("DROP   %-28s value=%-6d  %s -> (no handler)"
                  % (cap_id, value, src.name))
    elif t == RSP_CONFIRM:
        ref = report[1] | (report[2] << 8)
        print("CONFIRM ref=%d ok=%d  from %s" % (ref, report[3], src.name))
    elif verbose and t == MSG_LAYER_STATE:
        active = int.from_bytes(report[6:10], "little")
        print("  .  layer.changed  active=0x%08x  %s" % (active, src.name))
    elif verbose and t == MSG_KEY_EVENT:
        print("  .  key  pos=%d pressed=%d  %s" % (report[2], report[3], src.name))


def run_hub(devices, handlers_by_id, seconds, verbose):
    span = "until Ctrl-C" if seconds == 0 else ("%ds" % seconds)
    print("\n== Brokering (%s) — press an emit key on the keyboard ==" % span)
    deadline = None if seconds == 0 else time.time() + seconds
    try:
        while deadline is None or time.time() < deadline:
            idle = True
            for d in devices:
                try:
                    data = d.dev.read(REPORT_SIZE)
                except OSError:
                    continue
                if data:
                    idle = False
                    handle_report(d, bytes(data), handlers_by_id, verbose)
            if idle:
                time.sleep(0.01)
    except KeyboardInterrupt:
        print("\nStopped.")


# ------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true",
                    help="discover devices, print manifests + routing table, exit")
    ap.add_argument("--usage-page", type=lambda x: int(x, 0), default=DEFAULT_USAGE_PAGE)
    ap.add_argument("--usage", type=lambda x: int(x, 0), default=DEFAULT_USAGE)
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--seconds", type=int, default=0,
                    help="broker for N seconds then exit (0 = until Ctrl-C)")
    ap.add_argument("--verbose", action="store_true",
                    help="also print layer/key notifications, not just routing")
    args = ap.parse_args()

    metas = find_devices(args.usage_page, args.usage, args.vid, args.pid)
    if not metas:
        sys.exit("No hid-viz devices found (usage_page=%04x usage=%02x). "
                 "Is the keyboard plugged in and flashed?"
                 % (args.usage_page, args.usage))

    devices = []
    for meta in metas:
        dev = hid.device()
        try:
            dev.open_path(meta["path"])
            dev.set_nonblocking(1)
        except OSError as e:
            print("  !! could not open %r: %s"
                  % ((meta.get("product_string") or "?").strip(), e))
            continue
        name, config_id, caps = query_manifest(dev)
        if not caps and name is None:
            print("  !! %r returned no manifest (CONFIG_HID_VIZ_MANIFEST=y?)"
                  % ((meta.get("product_string") or "?").strip()))
        devices.append(Device(dev, meta, name, config_id, caps))

    handlers_by_id = build_routes(devices)
    print_inventory(devices, handlers_by_id)

    try:
        if not args.list:
            run_hub(devices, handlers_by_id, args.seconds, args.verbose)
    finally:
        for d in devices:
            d.dev.close()


if __name__ == "__main__":
    main()
