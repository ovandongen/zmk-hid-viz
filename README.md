# zmk-hid-viz

A ZMK firmware module that provides Raw HID communication between a keyboard and a host visualization app. Sends real-time layer-state changes and key press/release events, and accepts commands from the host.

## What it does

- **Layer notifications**: Every layer change (`&mo`, `&lt`, `&tog`, `&to`, combos, tap-dance — everything) is reported to the host in real time.
- **Key event notifications**: Every physical key press and release is reported with its matrix position.
- **Command handling**: The host app can query device info and control the active layer.
- **Dual BLE service**: Works on Windows, macOS, and Linux over both USB and Bluetooth.

## Why dual BLE services?

Windows' HoGP kernel driver claims the standard BLE HID service (UUID `0x1812`) exclusively, blocking user-mode access. This module registers a second GATT service with a custom vendor UUID that Windows doesn't touch. macOS and Linux use the standard HID service as before. Both services send the same data.

## Installation

Add to your `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: ovandongen
      url-base: https://github.com/ovandongen
  projects:
    - name: zmk-hid-viz
      remote: ovandongen
      revision: main
```

Add to your `.conf` file:

```
CONFIG_HID_VIZ=y
```

Add the `raw_hid_adapter` shield to your left/central half in `build.yaml`:

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
| `CONFIG_RAW_HID_USAGE_PAGE` | 0xFF60 | HID usage page |
| `CONFIG_RAW_HID_USAGE` | 0x61 | HID usage ID |
| `CONFIG_RAW_HID_REPORT_SIZE` | 32 | Report size in bytes |

## Message Protocol

### Outbound (keyboard → host)

**Layer state** (byte[0] = `0xFF`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFF` | Message type |
| 1 | `0x04` | Width of each bitmask field (bytes) |
| 2-5 | uint32 | Default layer bitmask |
| 6-9 | uint32 | Active layer bitmask |

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

**Set layer** (byte[0] = `0xFC`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFC` | Command type |
| 1 | uint8 | Layer index to activate |

## BLE Vendor Service UUIDs

For Windows BLE clients connecting via WinRT GATT:

| UUID | Purpose |
|------|---------|
| `4d4f4552-474f-5241-5748-49445f535643` | Service |
| `4d4f4552-474f-5241-5748-49445f545843` | TX (keyboard → host, subscribe for notifications) |
| `4d4f4552-474f-5241-5748-49445f525843` | RX (host → keyboard, write commands) |

## Credits

Transport layer based on [zmk-raw-hid](https://github.com/zzeneg/zmk-raw-hid) by zzeneg.
Notification approach inspired by [zmk-keypeek-layer-notifier](https://github.com/srwi/zmk-keypeek-layer-notifier) by srwi.
Windows BLE dual-service fix developed for [MoergoLayerViz](https://github.com/ovandongen/moergo-layer-viz).
