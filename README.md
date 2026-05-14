# zmk-hid-viz

A ZMK firmware module that sends real-time layer-state and key-event notifications to a host visualization app, and accepts commands from the host. Runs on top of the [zmk-raw-hid](https://github.com/zzeneg/zmk-raw-hid) transport module.

## What it does

- **Layer notifications**: Every layer change (`&mo`, `&lt`, `&tog`, `&to`, combos, tap-dance — everything) is reported to the host in real time.
- **Key event notifications**: Every physical key press and release is reported with its matrix position.
- **Command handling**: The host app can query device info and control the active layer state.

Transport (USB + BLE) is handled entirely by zmk-raw-hid; this module only consumes its `raw_hid_received_event` / `raw_hid_sent_event` events.

## Installation

Add both modules to your `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: zzeneg
      url-base: https://github.com/zzeneg
    - name: ovandongen
      url-base: https://github.com/ovandongen
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-raw-hid
      remote: zzeneg
      revision: main
    - name: zmk-hid-viz
      remote: ovandongen
      revision: main
  self:
    path: config
```

Add to your `.conf` file:

```
CONFIG_RAW_HID=y
CONFIG_HID_VIZ=y
```

Both lines are required — `CONFIG_RAW_HID` enables the transport (provided by `zmk-raw-hid`) and `CONFIG_HID_VIZ` enables the messaging layer on top of it.

Add the `raw_hid_adapter` shield (provided by zmk-raw-hid) to your left/central half in `build.yaml`:

```yaml
include:
  - board: your_board_lh
    shield: raw_hid_adapter
  - board: your_board_rh
```

## Configuration

All options in your `.conf` file:

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_HID_VIZ` | n | Enable the module |
| `CONFIG_HID_VIZ_LAYER_EVENTS` | y | Send layer state notifications |
| `CONFIG_HID_VIZ_KEY_EVENTS` | y | Send key press/release events |
| `CONFIG_HID_VIZ_COMMANDS` | y | Accept commands from host app |
| `CONFIG_HID_VIZ_CONFIG_ID` | "" | Layout/config identifier returned by the `0xFB` query |

Raw HID transport options (`CONFIG_RAW_HID_USAGE_PAGE`, `CONFIG_RAW_HID_USAGE`, `CONFIG_RAW_HID_REPORT_SIZE`, `CONFIG_RAW_HID_DEVICE`) are owned by zmk-raw-hid — see its README for defaults and tuning.

## Message Protocol

### Outbound (keyboard → host)

**Layer state** (byte[0] = `0xFF`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFF` | Message type |
| 1 | `0x04` | Width of each bitmask field (bytes) |
| 2-5 | uint32 (LE) | Default layer bitmask (layers 0-31) |
| 6-9 | uint32 (LE) | Active layer bitmask (layers 0-31) |

**Key event** (byte[0] = `0xF1`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xF1` | Message type |
| 2 | uint8 | Key matrix position |
| 3 | 0/1 | 0 = released, 1 = pressed |

**Device info response** (byte[0] = `0xFE`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFE` | Message type |
| 1 | uint8 | Protocol version |
| 2+ | string | Board/keyboard name (null-terminated) |

**Config ID response** (byte[0] = `0xFA`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFA` | Message type |
| 1+ | string | Layout/config ID from `CONFIG_HID_VIZ_CONFIG_ID` (null-terminated, empty if unset) |

### Inbound (host → keyboard)

**Get device info** (byte[0] = `0xFD`): Keyboard responds with `0xFE` device info message.

**Get config ID** (byte[0] = `0xFB`): Keyboard responds with `0xFA` config ID message. Host apps use this to auto-load the matching layout definition; an empty string means manual layout selection.

**Set layer state** (byte[0] = `0xFC`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFC` | Command type |
| 1-4 | uint32 (LE) | Layer state bitmask (bit N = layer N active, for layers 0-31) |

Layer 0 is always active regardless of the bitmask. The keyboard deactivates all
other layers and then activates every layer whose bit is set, allowing the host
to restore a complete layer stack rather than a single layer.

## Credits

Raw HID transport: [zmk-raw-hid](https://github.com/zzeneg/zmk-raw-hid) by zzeneg.
Notification approach inspired by [zmk-keypeek-layer-notifier](https://github.com/srwi/zmk-keypeek-layer-notifier) by srwi.
